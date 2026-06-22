#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

#include "firmware_storage.h"

#define USB_VID 0xCAFE
#define USB_BCD 0x0200

static const tusb_desc_device_t cdc_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = 0x4001,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_t msc_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = 0x4002,
    .bcdDevice = 0x0200,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *) (firmware_storage_has_binary()
        ? &cdc_device_descriptor
        : &msc_device_descriptor);
}

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
};

#define ITF_NUM_MSC 0

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82
#define EPNUM_MSC_OUT 0x03
#define EPNUM_MSC_IN 0x83

#define CDC_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
#define MSC_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t cdc_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CDC_CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

static const uint8_t msc_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, MSC_CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;
    return firmware_storage_has_binary()
        ? cdc_configuration_descriptor
        : msc_configuration_descriptor;
}

static const char *const string_descriptors[] = {
    (const char[]) {0x09, 0x04},
    "AVR Pico Programmer",
    NULL,
    NULL,
    "AVR Programmer Serial",
    "AVR Firmware Storage",
};

static uint16_t descriptor_buffer[33];
static char serial_number[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;

    size_t count;
    if (index == 0) {
        memcpy(&descriptor_buffer[1], string_descriptors[0], 2);
        count = 1;
    } else {
        if (index >= sizeof(string_descriptors) / sizeof(string_descriptors[0])) {
            return NULL;
        }

        const char *string = string_descriptors[index];
        if (index == 2) {
            string = firmware_storage_has_binary()
                ? "AVR Serial Programmer"
                : "AVR Firmware Storage";
        } else if (index == 3) {
            if (serial_number[0] == '\0') {
                pico_get_unique_board_id_string(serial_number, sizeof(serial_number));
            }
            string = serial_number;
        }

        count = strlen(string);
        if (count > 32) {
            count = 32;
        }
        for (size_t i = 0; i < count; ++i) {
            descriptor_buffer[i + 1] = (uint8_t) string[i];
        }
    }

    descriptor_buffer[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * count + 2));
    return descriptor_buffer;
}
