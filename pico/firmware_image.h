#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FIRMWARE_IMAGE_OK,
    FIRMWARE_IMAGE_STORAGE_ERROR,
    FIRMWARE_IMAGE_FORMAT_ERROR,
    FIRMWARE_IMAGE_CHECKSUM_ERROR,
    FIRMWARE_IMAGE_ADDRESS_ERROR,
    FIRMWARE_IMAGE_ORDER_ERROR,
    FIRMWARE_IMAGE_MISSING_EOF,
} firmware_image_result_t;

typedef struct {
    char filename[13];
    size_t file_size;
    size_t image_size;
    bool is_hex;
} firmware_image_info_t;

firmware_image_result_t firmware_image_open(firmware_image_info_t *info);
firmware_image_result_t firmware_image_read_page(size_t address, uint8_t *buffer,
                                                 size_t length, bool *has_data);
const char *firmware_image_result_string(firmware_image_result_t result);

