#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AVR_BIN_MAX_SIZE (128u * 1024u)
#define AVR_HEX_MAX_SIZE (500u * 1024u)
#define AVR_FILE_MAX_SIZE AVR_HEX_MAX_SIZE

typedef enum {
    STORAGE_BIN_READY,
    STORAGE_BIN_NONE,
    STORAGE_BIN_MULTIPLE,
    STORAGE_BIN_EMPTY,
    STORAGE_BIN_TOO_LARGE,
    STORAGE_BIN_ODD_SIZE,
    STORAGE_BIN_CORRUPT,
    STORAGE_FLASH_ERROR,
} storage_bin_result_t;

void firmware_storage_init(void);
void firmware_storage_poll(void);
bool firmware_storage_is_settled(void);
bool firmware_storage_has_binary(void);

storage_bin_result_t firmware_storage_info(size_t *firmware_size, char filename[13]);
bool firmware_storage_read(size_t offset, void *destination, size_t length);
bool firmware_storage_delete(void);

storage_bin_result_t firmware_storage_last_sync_result(void);
const char *firmware_storage_result_string(storage_bin_result_t result);
