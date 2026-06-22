#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "avrprog.h"
#include "firmware_image.h"
#include "firmware_storage.h"

#define MAX_AVR_PAGE_SIZE 256u

typedef struct {
    uint8_t signature[3];
    const char *name;
    size_t flash_size;
    size_t page_size;
} avr_device_t;

static const avr_device_t supported_devices[] = {
    {{0x1e, 0x93, 0x0c}, "ATtiny84A", 8u * 1024u, 64},
    {{0x1e, 0x93, 0x07}, "ATmega8/8A", 8u * 1024u, 64},
    {{0x1e, 0x94, 0x03}, "ATmega16/16A", 16u * 1024u, 128},
    {{0x1e, 0x94, 0x0a}, "ATmega164P", 16u * 1024u, 128},
    {{0x1e, 0x94, 0x0f}, "ATmega164A/PA", 16u * 1024u, 128},
    {{0x1e, 0x95, 0x02}, "ATmega32/32A", 32u * 1024u, 128},
    {{0x1e, 0x95, 0x08}, "ATmega324P", 32u * 1024u, 128},
    {{0x1e, 0x95, 0x15}, "ATmega324A", 32u * 1024u, 128},
    {{0x1e, 0x95, 0x11}, "ATmega324PA", 32u * 1024u, 128},
    {{0x1e, 0x95, 0x17}, "ATmega324PB", 32u * 1024u, 128},
    {{0x1e, 0x95, 0x0f}, "ATmega328P", 32u * 1024u, 128},
    {{0x1e, 0x95, 0x14}, "ATmega328", 32u * 1024u, 128},
    {{0x1e, 0x95, 0x16}, "ATmega328PB", 32u * 1024u, 128},
    {{0x1e, 0x96, 0x02}, "ATmega64/64A", 64u * 1024u, 256},
    {{0x1e, 0x96, 0x09}, "ATmega644", 64u * 1024u, 256},
    {{0x1e, 0x96, 0x0a}, "ATmega644P/A/PA", 64u * 1024u, 256},
    {{0x1e, 0x97, 0x02}, "ATmega128/128A", 128u * 1024u, 256},
    {{0x1e, 0x97, 0x03}, "ATmega1280", 128u * 1024u, 256},
    {{0x1e, 0x97, 0x05}, "ATmega1284P", 128u * 1024u, 256},
    {{0x1e, 0x97, 0x06}, "ATmega1284", 128u * 1024u, 256},
};

static uint8_t page_buffer[MAX_AVR_PAGE_SIZE] __attribute__((aligned(4)));
static char command_line[32];
static size_t command_length;
static bool flash_requested;
static bool banner_requested;

static const avr_device_t *find_avr_device(const uint8_t signature[3]) {
    for (size_t i = 0; i < sizeof(supported_devices) / sizeof(supported_devices[0]); ++i) {
        if (memcmp(signature, supported_devices[i].signature, 3) == 0) {
            return &supported_devices[i];
        }
    }
    return NULL;
}

static void print_help(void) {
    printf("AVR Pico Programmer\n");
    printf("  help   - show these commands\n");
    printf("  status - show the stored .BIN file\n");
    printf("  flash  - program and verify the stored .BIN\n");
    printf("  delete - remove the firmware file and restart as USB storage\n");
}

static void print_storage_status(void) {
    storage_bin_result_t storage_result = firmware_storage_info(NULL, NULL);
    if (storage_result != STORAGE_BIN_READY) {
        printf("Storage: %s.\n", firmware_storage_result_string(storage_result));
        return;
    }

    firmware_image_info_t image;
    firmware_image_result_t image_result = firmware_image_open(&image);
    if (image_result != FIRMWARE_IMAGE_OK) {
        printf("Image error: %s. Use 'delete' to replace it.\n",
               firmware_image_result_string(image_result));
        return;
    }
    printf("Ready: %s (%u-byte %s, %u program bytes).\n",
           image.filename, (unsigned) image.file_size,
           image.is_hex ? "Intel HEX" : "binary",
           (unsigned) image.image_size);
}

static bool program_avr(size_t firmware_size) {
    if (firmware_size == 0 || firmware_size > AVR_BIN_MAX_SIZE || (firmware_size & 1u)) {
        printf("Refusing invalid firmware length: %u bytes.\n", (unsigned) firmware_size);
        return false;
    }

    printf("Starting AVR programming (%u bytes)...\n", (unsigned) firmware_size);
    avr_spi_init();
    avr_reset();

    if (!avr_enter_programming_mode()) {
        printf("Error: AVR did not acknowledge programming mode. Check power, RESET, and SPI wiring.\n");
        avr_leave_programming_mode();
        return false;
    }

    uint8_t signature[3];
    for (size_t i = 0; i < sizeof(signature); ++i) {
        signature[i] = avr_read_signature_byte((uint8_t) i);
    }
    const avr_device_t *device = find_avr_device(signature);
    if (device == NULL) {
        printf("Unsupported AVR signature: %02x %02x %02x.\n",
               signature[0], signature[1], signature[2]);
        avr_leave_programming_mode();
        return false;
    }
    if (firmware_size > device->flash_size) {
        printf("Error: image is larger than the %s flash (%u bytes).\n",
               device->name, (unsigned) device->flash_size);
        avr_leave_programming_mode();
        return false;
    }

    printf("Detected %s; page size %u bytes. Erasing program memory.\n",
           device->name, (unsigned) device->page_size);
    avr_erase_memory();

    size_t page_count = (firmware_size + device->page_size - 1) / device->page_size;
    for (size_t page = 0; page < page_count; ++page) {
        size_t page_offset = page * device->page_size;
        size_t page_bytes = firmware_size - page_offset;
        if (page_bytes > device->page_size) {
            page_bytes = device->page_size;
        }
        bool has_data;
        firmware_image_result_t read_result = firmware_image_read_page(
            page_offset, page_buffer, page_bytes, &has_data);
        if (read_result != FIRMWARE_IMAGE_OK) {
            printf("Error reading image at offset %u: %s.\n",
                   (unsigned) page_offset,
                   firmware_image_result_string(read_result));
            avr_leave_programming_mode();
            return false;
        }

        if (!has_data) {
            tud_task();
            continue;
        }

        if (page == 0 || page + 1 == page_count || (page % 16u) == 0) {
            printf("Writing page %u/%u...\n",
                   (unsigned) (page + 1), (unsigned) page_count);
        }
        size_t word_count = page_bytes / 2;
        for (size_t word = 0; word < word_count; ++word) {
            size_t byte_index = word * 2;
            avr_write_temporary_buffer((uint16_t) word,
                                       page_buffer[byte_index],
                                       page_buffer[byte_index + 1]);
        }

        uint16_t page_word_address = (uint16_t) (page_offset / 2);
        avr_flash_program_memory(page_word_address);
        if (!avr_verify_program_memory_page(page_word_address, page_buffer, word_count)) {
            printf("Error: verification failed on page %u.\n", (unsigned) (page + 1));
            avr_leave_programming_mode();
            return false;
        }
        tud_task();
    }

    avr_leave_programming_mode();
    printf("Finished: %s flash verified successfully.\n", device->name);
    return true;
}

static void delete_firmware(void) {
    printf("Deleting stored firmware and switching to USB storage...\n");
    stdio_flush();
    if (!firmware_storage_delete()) {
        printf("Delete failed: %s.\n",
               firmware_storage_result_string(firmware_storage_last_sync_result()));
        return;
    }
    printf("Done. Restarting now.\n");
    stdio_flush();
    sleep_ms(100);
    watchdog_reboot(0, 0, 10);
    while (true) {
        tight_loop_contents();
    }
}

static void execute_command(void) {
    while (command_length > 0 && isspace((unsigned char) command_line[command_length - 1])) {
        --command_length;
    }
    command_line[command_length] = '\0';

    char *command = command_line;
    while (*command != '\0' && isspace((unsigned char) *command)) {
        ++command;
    }
    for (char *character = command; *character != '\0'; ++character) {
        *character = (char) tolower((unsigned char) *character);
    }

    if (*command == '\0') {
        return;
    }
    if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0) {
        print_help();
    } else if (strcmp(command, "status") == 0) {
        print_storage_status();
    } else if (strcmp(command, "flash") == 0) {
        flash_requested = true;
    } else if (strcmp(command, "delete") == 0 || strcmp(command, "remove") == 0) {
        delete_firmware();
    } else {
        printf("Unknown command '%s'. Type 'help'.\n", command);
    }
}

static void serial_task(void) {
    while (tud_cdc_available()) {
        char character;
        if (tud_cdc_read(&character, 1) != 1) {
            break;
        }
        if (character == '\r' || character == '\n') {
            if (command_length > 0) {
                printf("\n");
                execute_command();
                command_length = 0;
            }
        } else if ((character == '\b' || character == 0x7f) && command_length > 0) {
            --command_length;
            printf("\b \b");
        } else if (isprint((unsigned char) character) &&
                   command_length + 1 < sizeof(command_line)) {
            command_line[command_length++] = character;
            putchar(character);
        }
    }
}

static void service_flash_request(void) {
    if (!flash_requested) {
        return;
    }
    flash_requested = false;

    storage_bin_result_t storage_result = firmware_storage_info(NULL, NULL);
    if (storage_result != STORAGE_BIN_READY) {
        printf("Cannot flash: %s.\n", firmware_storage_result_string(storage_result));
        return;
    }

    firmware_image_info_t image;
    firmware_image_result_t image_result = firmware_image_open(&image);
    if (image_result != FIRMWARE_IMAGE_OK) {
        printf("Cannot flash: %s.\n", firmware_image_result_string(image_result));
        return;
    }
    printf("Using %s (%s).\n", image.filename,
           image.is_hex ? "Intel HEX" : "binary");
    program_avr(image.image_size);
}

void tud_cdc_line_state_cb(uint8_t interface, bool dtr, bool rts) {
    (void) interface;
    (void) rts;
    if (dtr) {
        banner_requested = true;
    }
}

int main(void) {
    firmware_storage_init();
    bool programmer_mode = firmware_storage_has_binary();
    tusb_init();
    stdio_init_all();

    while (true) {
        tud_task();
        firmware_storage_poll();

        if (!programmer_mode && firmware_storage_has_binary()) {
            sleep_ms(100);
            watchdog_reboot(0, 0, 10);
            while (true) {
                tight_loop_contents();
            }
        }

        if (programmer_mode) {
            serial_task();
            if (banner_requested) {
                banner_requested = false;
                printf("\n");
                print_help();
                print_storage_status();
            }
            service_flash_request();
        }
        sleep_ms(1);
    }
}
