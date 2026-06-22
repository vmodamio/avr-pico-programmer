#include "firmware_image.h"

#include <ctype.h>
#include <string.h>

#include "firmware_storage.h"

#define READER_BUFFER_SIZE 256u

typedef struct {
    size_t file_offset;
    size_t file_size;
    size_t buffer_offset;
    size_t buffer_length;
    uint8_t buffer[READER_BUFFER_SIZE];
} file_reader_t;

typedef struct {
    uint8_t length;
    uint16_t address;
    uint8_t type;
    uint8_t data[255];
} hex_record_t;

static file_reader_t reader;
static bool image_is_hex;
static size_t decoded_image_size;
static size_t expected_page_address;
static uint32_t address_base;
static hex_record_t pending_record;
static uint32_t pending_address;
static size_t pending_index;
static bool pending_valid;
static bool stream_finished;

static bool filename_has_hex_extension(const char *filename) {
    size_t length = strlen(filename);
    return length >= 4 && filename[length - 4] == '.' &&
           toupper((unsigned char) filename[length - 3]) == 'H' &&
           toupper((unsigned char) filename[length - 2]) == 'E' &&
           toupper((unsigned char) filename[length - 1]) == 'X';
}

static void reader_reset(size_t file_size) {
    memset(&reader, 0, sizeof(reader));
    reader.file_size = file_size;
}

static bool reader_get(uint8_t *character) {
    if (reader.file_offset >= reader.file_size) {
        return false;
    }
    if (reader.buffer_offset >= reader.buffer_length) {
        size_t amount = reader.file_size - reader.file_offset;
        if (amount > sizeof(reader.buffer)) {
            amount = sizeof(reader.buffer);
        }
        if (!firmware_storage_read(reader.file_offset, reader.buffer, amount)) {
            return false;
        }
        reader.buffer_offset = 0;
        reader.buffer_length = amount;
    }
    *character = reader.buffer[reader.buffer_offset++];
    ++reader.file_offset;
    return true;
}

static int hex_nibble(uint8_t character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    character = (uint8_t) toupper(character);
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static bool read_hex_byte(uint8_t *value) {
    uint8_t high_character;
    uint8_t low_character;
    if (!reader_get(&high_character) || !reader_get(&low_character)) {
        return false;
    }
    int high = hex_nibble(high_character);
    int low = hex_nibble(low_character);
    if (high < 0 || low < 0) {
        return false;
    }
    *value = (uint8_t) ((high << 4) | low);
    return true;
}

static firmware_image_result_t read_record(hex_record_t *record) {
    uint8_t character;
    do {
        if (!reader_get(&character)) {
            return FIRMWARE_IMAGE_MISSING_EOF;
        }
    } while (isspace((unsigned char) character));

    if (character != ':') {
        return FIRMWARE_IMAGE_FORMAT_ERROR;
    }

    uint8_t address_high;
    uint8_t address_low;
    uint8_t checksum;
    if (!read_hex_byte(&record->length) ||
        !read_hex_byte(&address_high) ||
        !read_hex_byte(&address_low) ||
        !read_hex_byte(&record->type)) {
        return FIRMWARE_IMAGE_FORMAT_ERROR;
    }
    record->address = (uint16_t) ((uint16_t) address_high << 8) | address_low;

    uint8_t sum = (uint8_t) (record->length + address_high + address_low + record->type);
    for (size_t i = 0; i < record->length; ++i) {
        if (!read_hex_byte(&record->data[i])) {
            return FIRMWARE_IMAGE_FORMAT_ERROR;
        }
        sum = (uint8_t) (sum + record->data[i]);
    }
    if (!read_hex_byte(&checksum)) {
        return FIRMWARE_IMAGE_FORMAT_ERROR;
    }
    if ((uint8_t) (sum + checksum) != 0) {
        return FIRMWARE_IMAGE_CHECKSUM_ERROR;
    }
    return FIRMWARE_IMAGE_OK;
}

static firmware_image_result_t apply_address_record(const hex_record_t *record,
                                                     uint32_t *base) {
    if (record->type == 2) {
        if (record->length != 2) {
            return FIRMWARE_IMAGE_FORMAT_ERROR;
        }
        *base = ((uint32_t) record->data[0] << 12) |
                ((uint32_t) record->data[1] << 4);
    } else if (record->type == 4) {
        if (record->length != 2) {
            return FIRMWARE_IMAGE_FORMAT_ERROR;
        }
        *base = ((uint32_t) record->data[0] << 24) |
                ((uint32_t) record->data[1] << 16);
    }
    return FIRMWARE_IMAGE_OK;
}

static firmware_image_result_t validate_hex(size_t file_size, size_t *image_size) {
    reader_reset(file_size);
    uint32_t base = 0;
    uint32_t previous_end = 0;
    uint32_t highest_address = 0;
    bool saw_data = false;

    while (true) {
        hex_record_t record;
        firmware_image_result_t result = read_record(&record);
        if (result != FIRMWARE_IMAGE_OK) {
            return result;
        }

        if (record.type == 0) {
            uint32_t address = base + record.address;
            uint32_t end = address + record.length;
            if (end < address || end > AVR_BIN_MAX_SIZE) {
                return FIRMWARE_IMAGE_ADDRESS_ERROR;
            }
            if (saw_data && address < previous_end) {
                return FIRMWARE_IMAGE_ORDER_ERROR;
            }
            if (record.length > 0) {
                saw_data = true;
                previous_end = end;
                if (end > highest_address) {
                    highest_address = end;
                }
            }
        } else if (record.type == 1) {
            if (record.length != 0) {
                return FIRMWARE_IMAGE_FORMAT_ERROR;
            }
            if (!saw_data) {
                return FIRMWARE_IMAGE_FORMAT_ERROR;
            }
            *image_size = (highest_address + 1u) & ~1u;
            return FIRMWARE_IMAGE_OK;
        } else if (record.type == 2 || record.type == 4) {
            firmware_image_result_t address_result = apply_address_record(&record, &base);
            if (address_result != FIRMWARE_IMAGE_OK) {
                return address_result;
            }
        } else if (record.type == 3 || record.type == 5) {
            if (record.length != 4) {
                return FIRMWARE_IMAGE_FORMAT_ERROR;
            }
        } else {
            return FIRMWARE_IMAGE_FORMAT_ERROR;
        }
    }
}

static void reset_hex_stream(size_t file_size) {
    reader_reset(file_size);
    expected_page_address = 0;
    address_base = 0;
    pending_index = 0;
    pending_valid = false;
    stream_finished = false;
}

firmware_image_result_t firmware_image_open(firmware_image_info_t *info) {
    size_t file_size;
    char filename[13];
    if (firmware_storage_info(&file_size, filename) != STORAGE_BIN_READY) {
        return FIRMWARE_IMAGE_STORAGE_ERROR;
    }

    bool is_hex = filename_has_hex_extension(filename);
    size_t image_size = file_size;
    if (is_hex) {
        firmware_image_result_t result = validate_hex(file_size, &image_size);
        if (result != FIRMWARE_IMAGE_OK) {
            return result;
        }
        reset_hex_stream(file_size);
    } else if (file_size > AVR_BIN_MAX_SIZE || (file_size & 1u)) {
        return FIRMWARE_IMAGE_FORMAT_ERROR;
    }

    image_is_hex = is_hex;
    decoded_image_size = image_size;
    expected_page_address = 0;
    if (info != NULL) {
        memcpy(info->filename, filename, sizeof(info->filename));
        info->file_size = file_size;
        info->image_size = image_size;
        info->is_hex = is_hex;
    }
    return FIRMWARE_IMAGE_OK;
}

static firmware_image_result_t load_next_data_record(void) {
    while (!stream_finished) {
        hex_record_t record;
        firmware_image_result_t result = read_record(&record);
        if (result != FIRMWARE_IMAGE_OK) {
            return result;
        }
        if (record.type == 0) {
            if (record.length == 0) {
                continue;
            }
            pending_record = record;
            pending_address = address_base + record.address;
            pending_index = 0;
            pending_valid = true;
            return FIRMWARE_IMAGE_OK;
        }
        if (record.type == 1) {
            stream_finished = true;
            return FIRMWARE_IMAGE_OK;
        }
        if (record.type == 2 || record.type == 4) {
            result = apply_address_record(&record, &address_base);
            if (result != FIRMWARE_IMAGE_OK) {
                return result;
            }
        }
    }
    return FIRMWARE_IMAGE_OK;
}

firmware_image_result_t firmware_image_read_page(size_t address, uint8_t *buffer,
                                                 size_t length, bool *has_data) {
    if (address + length > decoded_image_size || address != expected_page_address) {
        return FIRMWARE_IMAGE_ADDRESS_ERROR;
    }

    memset(buffer, 0xff, length);
    size_t page_end = address + length;
    if (image_is_hex) {
        while (true) {
            if (!pending_valid) {
                firmware_image_result_t result = load_next_data_record();
                if (result != FIRMWARE_IMAGE_OK) {
                    return result;
                }
                if (!pending_valid) {
                    break;
                }
            }

            size_t record_position = pending_address + pending_index;
            size_t record_end = pending_address + pending_record.length;
            if (record_position >= page_end) {
                break;
            }
            if (record_end <= address) {
                pending_valid = false;
                continue;
            }

            size_t copy_start = record_position < address ? address : record_position;
            size_t copy_end = record_end < page_end ? record_end : page_end;
            size_t source_offset = copy_start - pending_address;
            memcpy(buffer + copy_start - address,
                   pending_record.data + source_offset,
                   copy_end - copy_start);
            pending_index = copy_end - pending_address;
            if (pending_index >= pending_record.length) {
                pending_valid = false;
            }
            if (copy_end >= page_end) {
                break;
            }
        }
    } else {
        if (!firmware_storage_read(address, buffer, length)) {
            return FIRMWARE_IMAGE_STORAGE_ERROR;
        }
    }

    bool contains_data = false;
    for (size_t i = 0; i < length; ++i) {
        if (buffer[i] != 0xff) {
            contains_data = true;
            break;
        }
    }
    if (has_data != NULL) {
        *has_data = contains_data;
    }
    expected_page_address = page_end;
    return FIRMWARE_IMAGE_OK;
}

const char *firmware_image_result_string(firmware_image_result_t result) {
    switch (result) {
        case FIRMWARE_IMAGE_OK: return "firmware image ready";
        case FIRMWARE_IMAGE_STORAGE_ERROR: return "stored firmware could not be read";
        case FIRMWARE_IMAGE_FORMAT_ERROR: return "invalid Intel HEX record format";
        case FIRMWARE_IMAGE_CHECKSUM_ERROR: return "Intel HEX checksum mismatch";
        case FIRMWARE_IMAGE_ADDRESS_ERROR: return "firmware address exceeds 128 KiB";
        case FIRMWARE_IMAGE_ORDER_ERROR: return "Intel HEX data records are overlapping or out of order";
        case FIRMWARE_IMAGE_MISSING_EOF: return "Intel HEX end-of-file record is missing";
        default: return "unknown firmware image error";
    }
}
