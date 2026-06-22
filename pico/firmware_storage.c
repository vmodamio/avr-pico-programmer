#include "firmware_storage.h"

#include <ctype.h>
#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include "tusb.h"

#define DISK_BLOCK_SIZE 512u
#define DISK_BLOCK_COUNT 1024u
#define DISK_SIZE (DISK_BLOCK_SIZE * DISK_BLOCK_COUNT)

#define FAT1_BLOCK 1u
#define FAT_BLOCK_COUNT 3u
#define FAT2_BLOCK (FAT1_BLOCK + FAT_BLOCK_COUNT)
#define ROOT_BLOCK (FAT2_BLOCK + FAT_BLOCK_COUNT)
#define ROOT_BLOCK_COUNT 2u
#define ROOT_ENTRY_COUNT 32u
#define DATA_BLOCK (ROOT_BLOCK + ROOT_BLOCK_COUNT)
#define DATA_CLUSTER_COUNT (DISK_BLOCK_COUNT - DATA_BLOCK)
#define MAX_FILE_CLUSTERS (AVR_FILE_MAX_SIZE / DISK_BLOCK_SIZE)

#define STORAGE_SETTLE_MS 1000u
#define STORAGE_RESERVED_SIZE (520u * 1024u)
#define STORAGE_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - STORAGE_RESERVED_SIZE)

#define README_CONTENTS \
    "AVR Pico Programmer\r\n" \
    "===================\r\n\r\n" \
    "Copy one .BIN or Intel HEX file to this drive.\r\n" \
    "BIN images may be 128 KiB; HEX files may be 500 KiB.\r\n" \
    "After the copy completes, the Pico restarts as a serial programmer.\r\n" \
    "Open its serial port and type: flash\r\n\r\n" \
    "In serial mode, type 'delete' to remove the file and return here.\r\n" \
    "Do not format this drive.\r\n"

typedef struct {
    uint32_t offset;
    uint32_t erase_size;
    const uint8_t *data;
    size_t data_size;
} flash_operation_t;

static uint8_t flash_sector_cache[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));
static int32_t cached_sector = -1;
static bool cache_dirty;
static bool disk_dirty;
static bool force_sync;
static bool storage_available;
static bool binary_confirmed;
static bool binary_activation_pending;
static uint32_t binary_activation_ms;
static uint32_t last_write_ms;

static uint16_t file_clusters[MAX_FILE_CLUSTERS];
static size_t file_cluster_count;
static size_t file_size;
static char file_name[13];
static storage_bin_result_t last_sync_result = STORAGE_BIN_NONE;

static void write_u16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t) value;
    destination[1] = (uint8_t) (value >> 8);
}

static void write_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t) value;
    destination[1] = (uint8_t) (value >> 8);
    destination[2] = (uint8_t) (value >> 16);
    destination[3] = (uint8_t) (value >> 24);
}

static uint16_t read_u16(const uint8_t *source) {
    return (uint16_t) source[0] | ((uint16_t) source[1] << 8);
}

static uint32_t read_u32(const uint8_t *source) {
    return (uint32_t) source[0] |
           ((uint32_t) source[1] << 8) |
           ((uint32_t) source[2] << 16) |
           ((uint32_t) source[3] << 24);
}

static void perform_flash_operation(void *parameter) {
    const flash_operation_t *operation = parameter;
    flash_range_erase(operation->offset, operation->erase_size);
    if (operation->data != NULL && operation->data_size > 0) {
        flash_range_program(operation->offset, operation->data, operation->data_size);
    }
}

static bool binary_fits_before_storage(void) {
    extern char __flash_binary_end;
    uintptr_t binary_end = (uintptr_t) &__flash_binary_end - XIP_BASE;
    return binary_end <= STORAGE_FLASH_OFFSET;
}

static const uint8_t *storage_xip_address(size_t offset) {
    return (const uint8_t *) (XIP_BASE + STORAGE_FLASH_OFFSET + offset);
}

static bool flush_cache(void) {
    if (!cache_dirty || cached_sector < 0) {
        return true;
    }

    uint32_t flash_offset = STORAGE_FLASH_OFFSET +
                            (uint32_t) cached_sector * FLASH_SECTOR_SIZE;
    flash_operation_t operation = {
        .offset = flash_offset,
        .erase_size = FLASH_SECTOR_SIZE,
        .data = flash_sector_cache,
        .data_size = FLASH_SECTOR_SIZE,
    };
    if (flash_safe_execute(perform_flash_operation, &operation, 1000) != PICO_OK) {
        return false;
    }
    cache_dirty = false;
    return true;
}

static bool select_cache_sector(uint32_t sector) {
    if (cached_sector == (int32_t) sector) {
        return true;
    }
    if (!flush_cache()) {
        return false;
    }
    memcpy(flash_sector_cache,
           storage_xip_address((size_t) sector * FLASH_SECTOR_SIZE),
           FLASH_SECTOR_SIZE);
    cached_sector = (int32_t) sector;
    return true;
}

static bool disk_read_bytes(size_t offset, void *destination, size_t length) {
    if (!storage_available || offset + length > DISK_SIZE) {
        return false;
    }

    uint8_t *output = destination;
    while (length > 0) {
        uint32_t sector = (uint32_t) (offset / FLASH_SECTOR_SIZE);
        size_t within_sector = offset % FLASH_SECTOR_SIZE;
        size_t amount = FLASH_SECTOR_SIZE - within_sector;
        if (amount > length) {
            amount = length;
        }

        const uint8_t *source = cached_sector == (int32_t) sector
            ? flash_sector_cache + within_sector
            : storage_xip_address(offset);
        memcpy(output, source, amount);
        output += amount;
        offset += amount;
        length -= amount;
    }
    return true;
}

static bool disk_write_bytes(size_t offset, const void *source, size_t length) {
    if (!storage_available || offset + length > DISK_SIZE) {
        return false;
    }

    const uint8_t *input = source;
    while (length > 0) {
        uint32_t sector = (uint32_t) (offset / FLASH_SECTOR_SIZE);
        size_t within_sector = offset % FLASH_SECTOR_SIZE;
        size_t amount = FLASH_SECTOR_SIZE - within_sector;
        if (amount > length) {
            amount = length;
        }
        if (!select_cache_sector(sector)) {
            return false;
        }
        memcpy(flash_sector_cache + within_sector, input, amount);
        cache_dirty = true;
        input += amount;
        offset += amount;
        length -= amount;
    }
    return true;
}

static void set_fat12_entry(uint8_t *fat, uint16_t cluster, uint16_t value) {
    size_t offset = cluster + cluster / 2u;
    value &= 0x0fffu;
    if (cluster & 1u) {
        fat[offset] = (uint8_t) ((fat[offset] & 0x0fu) | (value << 4));
        fat[offset + 1] = (uint8_t) (value >> 4);
    } else {
        fat[offset] = (uint8_t) value;
        fat[offset + 1] = (uint8_t) ((fat[offset + 1] & 0xf0u) | (value >> 8));
    }
}

static uint16_t get_fat12_entry(uint16_t cluster) {
    uint8_t packed[2];
    size_t offset = cluster + cluster / 2u;
    if (offset + 1 >= FAT_BLOCK_COUNT * DISK_BLOCK_SIZE ||
        !disk_read_bytes(FAT1_BLOCK * DISK_BLOCK_SIZE + offset, packed, sizeof(packed))) {
        return 0;
    }
    uint16_t value = (uint16_t) packed[0] | ((uint16_t) packed[1] << 8);
    return (cluster & 1u) ? (value >> 4) : (value & 0x0fffu);
}

static void create_root_entry(uint8_t *entry, const char name[11], uint8_t attributes,
                              uint16_t first_cluster, uint32_t size) {
    memset(entry, 0, 32);
    memcpy(entry, name, 11);
    entry[11] = attributes;
    write_u16(entry + 26, first_cluster);
    write_u32(entry + 28, size);
}

static bool format_disk(void) {
    memset(flash_sector_cache, 0, sizeof(flash_sector_cache));

    uint8_t *boot = flash_sector_cache;
    boot[0] = 0xeb;
    boot[1] = 0x3c;
    boot[2] = 0x90;
    memcpy(boot + 3, "AVRPROG ", 8);
    write_u16(boot + 11, DISK_BLOCK_SIZE);
    boot[13] = 1;
    write_u16(boot + 14, 1);
    boot[16] = 2;
    write_u16(boot + 17, ROOT_ENTRY_COUNT);
    write_u16(boot + 19, DISK_BLOCK_COUNT);
    boot[21] = 0xf8;
    write_u16(boot + 22, FAT_BLOCK_COUNT);
    write_u16(boot + 24, 1);
    write_u16(boot + 26, 1);
    boot[36] = 0x80;
    boot[38] = 0x29;
    write_u32(boot + 39, 0x41565250u);
    memcpy(boot + 43, "AVRPROG    ", 11);
    memcpy(boot + 54, "FAT12   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xaa;

    uint8_t *fat1 = flash_sector_cache + FAT1_BLOCK * DISK_BLOCK_SIZE;
    fat1[0] = 0xf8;
    fat1[1] = 0xff;
    fat1[2] = 0xff;
    set_fat12_entry(fat1, 2, 0x0fff);
    memcpy(flash_sector_cache + FAT2_BLOCK * DISK_BLOCK_SIZE,
           fat1, FAT_BLOCK_COUNT * DISK_BLOCK_SIZE);

    uint8_t *root = flash_sector_cache + ROOT_BLOCK * DISK_BLOCK_SIZE;
    create_root_entry(root, "AVRPROG    ", 0x08, 0, 0);
    const char readme[] = README_CONTENTS;
    create_root_entry(root + 32, "README  TXT", 0x20, 2, sizeof(readme) - 1);

    flash_operation_t operation = {
        .offset = STORAGE_FLASH_OFFSET,
        .erase_size = STORAGE_RESERVED_SIZE,
        .data = flash_sector_cache,
        .data_size = FLASH_SECTOR_SIZE,
    };
    if (flash_safe_execute(perform_flash_operation, &operation, 2000) != PICO_OK) {
        return false;
    }

    // The README data begins in the second 4 KiB flash sector.
    memset(flash_sector_cache, 0, sizeof(flash_sector_cache));
    size_t readme_offset = DATA_BLOCK * DISK_BLOCK_SIZE - FLASH_SECTOR_SIZE;
    memcpy(flash_sector_cache + readme_offset, readme, sizeof(readme) - 1);
    operation.offset = STORAGE_FLASH_OFFSET + FLASH_SECTOR_SIZE;
    operation.erase_size = FLASH_SECTOR_SIZE;
    if (flash_safe_execute(perform_flash_operation, &operation, 1000) != PICO_OK) {
        return false;
    }

    cached_sector = -1;
    cache_dirty = false;
    disk_dirty = false;
    return true;
}

static bool disk_is_formatted(void) {
    const uint8_t *boot = storage_xip_address(0);
    return memcmp(boot + 3, "AVRPROG ", 8) == 0 &&
           read_u16(boot + 11) == DISK_BLOCK_SIZE &&
           read_u16(boot + 19) == DISK_BLOCK_COUNT &&
           boot[510] == 0x55 && boot[511] == 0xaa;
}

static void format_short_filename(const uint8_t *entry, char filename[13]) {
    size_t position = 0;
    for (size_t i = 0; i < 8 && entry[i] != ' '; ++i) {
        filename[position++] = (char) entry[i];
    }
    if (entry[8] != ' ') {
        filename[position++] = '.';
        for (size_t i = 8; i < 11 && entry[i] != ' '; ++i) {
            filename[position++] = (char) entry[i];
        }
    }
    filename[position] = '\0';
}

static bool has_extension(const uint8_t *entry, const char extension[3]) {
    return toupper((unsigned char) entry[8]) == extension[0] &&
           toupper((unsigned char) entry[9]) == extension[1] &&
           toupper((unsigned char) entry[10]) == extension[2];
}

static bool is_supported_firmware_file(const uint8_t *entry) {
    return has_extension(entry, "BIN") || has_extension(entry, "HEX");
}

static storage_bin_result_t refresh_file_info(void) {
    uint8_t root[ROOT_BLOCK_COUNT * DISK_BLOCK_SIZE];
    if (!disk_read_bytes(ROOT_BLOCK * DISK_BLOCK_SIZE, root, sizeof(root))) {
        return STORAGE_FLASH_ERROR;
    }

    const uint8_t *selected = NULL;
    unsigned bin_count = 0;
    for (size_t i = 0; i < ROOT_ENTRY_COUNT; ++i) {
        const uint8_t *entry = root + i * 32;
        if (entry[0] == 0x00) {
            break;
        }
        if (entry[0] == 0xe5 || entry[11] == 0x0f) {
            continue;
        }
        uint8_t attributes = entry[11];
        if (attributes & (0x02 | 0x04 | 0x08 | 0x10)) {
            continue;
        }
        if (is_supported_firmware_file(entry)) {
            selected = entry;
            ++bin_count;
        }
    }

    file_size = 0;
    file_cluster_count = 0;
    file_name[0] = '\0';
    if (bin_count == 0) {
        return STORAGE_BIN_NONE;
    }
    if (bin_count > 1) {
        return STORAGE_BIN_MULTIPLE;
    }

    uint32_t size = read_u32(selected + 28);
    if (size == 0) {
        return STORAGE_BIN_EMPTY;
    }
    bool is_hex = has_extension(selected, "HEX");
    if ((!is_hex && size > AVR_BIN_MAX_SIZE) ||
        (is_hex && size > AVR_HEX_MAX_SIZE)) {
        return STORAGE_BIN_TOO_LARGE;
    }
    if (!is_hex && (size & 1u)) {
        return STORAGE_BIN_ODD_SIZE;
    }

    size_t clusters_needed = (size + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE;
    uint16_t cluster = read_u16(selected + 26);
    for (size_t i = 0; i < clusters_needed; ++i) {
        if (cluster < 2 || cluster >= DATA_CLUSTER_COUNT + 2 || i >= MAX_FILE_CLUSTERS) {
            return STORAGE_BIN_CORRUPT;
        }
        file_clusters[i] = cluster;
        if (i + 1 < clusters_needed) {
            cluster = get_fat12_entry(cluster);
            if (cluster >= 0x0ff8u) {
                return STORAGE_BIN_CORRUPT;
            }
        }
    }

    file_size = size;
    file_cluster_count = clusters_needed;
    format_short_filename(selected, file_name);
    return STORAGE_BIN_READY;
}

void firmware_storage_init(void) {
    storage_available = binary_fits_before_storage();
    cached_sector = -1;
    cache_dirty = false;
    disk_dirty = false;
    force_sync = false;
    binary_confirmed = false;
    binary_activation_pending = false;

    if (!storage_available) {
        last_sync_result = STORAGE_FLASH_ERROR;
        return;
    }
    if (!disk_is_formatted() && !format_disk()) {
        storage_available = false;
        last_sync_result = STORAGE_FLASH_ERROR;
        return;
    }
    last_sync_result = refresh_file_info();
    binary_confirmed = last_sync_result == STORAGE_BIN_READY;
}

bool firmware_storage_is_settled(void) {
    return !disk_dirty ||
           (uint32_t) (to_ms_since_boot(get_absolute_time()) - last_write_ms) >= STORAGE_SETTLE_MS;
}

void firmware_storage_poll(void) {
    if ((disk_dirty && firmware_storage_is_settled()) || force_sync) {
        if (!flush_cache()) {
            last_sync_result = STORAGE_FLASH_ERROR;
        } else {
            cached_sector = -1;
            last_sync_result = refresh_file_info();
            binary_confirmed = false;
            if (last_sync_result == STORAGE_BIN_READY) {
                binary_activation_pending = true;
                binary_activation_ms = to_ms_since_boot(get_absolute_time());
            } else {
                binary_activation_pending = false;
            }
        }
        disk_dirty = false;
        force_sync = false;
    }
}

bool firmware_storage_has_binary(void) {
    if (binary_activation_pending &&
        (uint32_t) (to_ms_since_boot(get_absolute_time()) - binary_activation_ms) >=
            STORAGE_SETTLE_MS) {
        binary_activation_pending = false;
        binary_confirmed = true;
    }
    return binary_confirmed && last_sync_result == STORAGE_BIN_READY && file_size > 0;
}

storage_bin_result_t firmware_storage_info(size_t *firmware_size, char filename[13]) {
    if (firmware_size != NULL) {
        *firmware_size = file_size;
    }
    if (filename != NULL) {
        memcpy(filename, file_name, sizeof(file_name));
    }
    return last_sync_result;
}

bool firmware_storage_read(size_t offset, void *destination, size_t length) {
    if (!firmware_storage_has_binary() || offset + length > file_size) {
        return false;
    }

    uint8_t *output = destination;
    while (length > 0) {
        size_t cluster_index = offset / DISK_BLOCK_SIZE;
        size_t within_cluster = offset % DISK_BLOCK_SIZE;
        if (cluster_index >= file_cluster_count) {
            return false;
        }
        size_t amount = DISK_BLOCK_SIZE - within_cluster;
        if (amount > length) {
            amount = length;
        }
        size_t disk_offset = (DATA_BLOCK + file_clusters[cluster_index] - 2u) *
                             DISK_BLOCK_SIZE + within_cluster;
        if (!disk_read_bytes(disk_offset, output, amount)) {
            return false;
        }
        output += amount;
        offset += amount;
        length -= amount;
    }
    return true;
}

bool firmware_storage_delete(void) {
    if (!storage_available || !format_disk()) {
        last_sync_result = STORAGE_FLASH_ERROR;
        return false;
    }
    last_sync_result = refresh_file_info();
    binary_confirmed = false;
    binary_activation_pending = false;
    return last_sync_result == STORAGE_BIN_NONE;
}

storage_bin_result_t firmware_storage_last_sync_result(void) {
    return last_sync_result;
}

const char *firmware_storage_result_string(storage_bin_result_t result) {
    switch (result) {
        case STORAGE_BIN_READY: return "firmware ready";
        case STORAGE_BIN_NONE: return "no .BIN or .HEX file stored";
        case STORAGE_BIN_MULTIPLE: return "more than one firmware file; keep only one";
        case STORAGE_BIN_EMPTY: return "the firmware file is empty";
        case STORAGE_BIN_TOO_LARGE: return "the BIN or HEX file exceeds its storage limit";
        case STORAGE_BIN_ODD_SIZE: return "the .BIN size must be a multiple of two bytes";
        case STORAGE_BIN_CORRUPT: return "the firmware file allocation is incomplete or corrupt";
        case STORAGE_FLASH_ERROR: return "Pico flash storage is unavailable";
        default: return "unknown storage error";
    }
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
    (void) lun;
    memcpy(vendor_id, "AVRPICO ", 8);
    memcpy(product_id, "FW STORAGE      ", 16);
    memcpy(product_rev, "2.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (!storage_available) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void) lun;
    *block_count = DISK_BLOCK_COUNT;
    *block_size = DISK_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                           bool start, bool load_eject) {
    (void) lun;
    (void) power_condition;
    if (load_eject && !start) {
        force_sync = true;
    }
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void) lun;
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t buffer_size) {
    (void) lun;
    if (lba >= DISK_BLOCK_COUNT || offset + buffer_size > DISK_BLOCK_SIZE ||
        !disk_read_bytes(lba * DISK_BLOCK_SIZE + offset, buffer, buffer_size)) {
        return -1;
    }
    return (int32_t) buffer_size;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t buffer_size) {
    (void) lun;
    if (lba >= DISK_BLOCK_COUNT || offset + buffer_size > DISK_BLOCK_SIZE ||
        !disk_write_bytes(lba * DISK_BLOCK_SIZE + offset, buffer, buffer_size)) {
        return -1;
    }
    disk_dirty = true;
    binary_confirmed = false;
    binary_activation_pending = false;
    last_write_ms = to_ms_since_boot(get_absolute_time());
    return (int32_t) buffer_size;
}

void tud_msc_write10_complete_cb(uint8_t lun) {
    (void) lun;
    last_write_ms = to_ms_since_boot(get_absolute_time());
}

int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_command[16],
                        void *buffer, uint16_t buffer_size) {
    (void) buffer;
    (void) buffer_size;
    if (scsi_command[0] == 0x35) {
        force_sync = true;
        return 0;
    }
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}
