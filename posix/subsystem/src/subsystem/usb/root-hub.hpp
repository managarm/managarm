#pragma once

#include <array>
#include <cstdint>

namespace usb_subsystem::root_hub {

constexpr std::array<uint8_t, 18> descUsb3_1 = {
    0x12, /* bLength */
    0x01, /* bDescriptorType */
    0x10,
    0x03, /* bcdUsb */

    0x09, /* bDeviceClass */
    0x00, /* bDeviceSubClass */
    0x03, /* bDeviceProtocol */
    0x09, /* bMaxPacketSize0 */

    0x6b,
    0x1d, /* idVendor */
    0x03,
    0x00, /* idProduct */
    0x00,
    0x00, /* bcdDevice */

    0x03, /* iManufacturer */
    0x02, /* iProduct */
    0x01, /* iSerialNumber */
    0x01, /* bNumConfigurations */
};

constexpr std::array<uint8_t, 18> descUsb3_0 = {
    0x12, /* bLength */
    0x01, /* bDescriptorType */
    0x00,
    0x03, /* bcdUsb */

    0x09, /* bDeviceClass */
    0x00, /* bDeviceSubClass */
    0x03, /* bDeviceProtocol */
    0x09, /* bMaxPacketSize0 */

    0x6b,
    0x1d, /* idVendor */
    0x03,
    0x00, /* idProduct */
    0x00,
    0x00, /* bcdDevice */

    0x03, /* iManufacturer */
    0x02, /* iProduct */
    0x01, /* iSerialNumber */
    0x01, /* bNumConfigurations */
};

constexpr std::array<uint8_t, 18> descUsb2_0 = {
    0x12, /* bLength */
    0x01, /* bDescriptorType */
    0x00,
    0x02, /* bcdUsb */

    0x09, /* bDeviceClass */
    0x00, /* bDeviceSubClass */
    0x00, /* bDeviceProtocol */
    0x40, /* bMaxPacketSize0 */

    0x6b,
    0x1d, /* idVendor */
    0x02,
    0x00, /* idProduct */
    0x00,
    0x00, /* bcdDevice */

    0x03, /* iManufacturer */
    0x02, /* iProduct */
    0x01, /* iSerialNumber */
    0x01, /* bNumConfigurations */
};

constexpr std::array<uint8_t, 18> descUsb1_1 = {
    0x12, /* bLength */
    0x01, /* bDescriptorType */
    0x10,
    0x01, /* bcdUsb */

    0x09, /* bDeviceClass */
    0x00, /* bDeviceSubClass */
    0x00, /* bDeviceProtocol */
    0x40, /* bMaxPacketSize0 */

    0x6b,
    0x1d, /* idVendor */
    0x01,
    0x00, /* idProduct */
    0x00,
    0x00, /* bcdDevice */

    0x03, /* iManufacturer */
    0x02, /* iProduct */
    0x01, /* iSerialNumber */
    0x01, /* bNumConfigurations */
};

constexpr std::array<uint8_t, 25> descFullSpeed = {
    /* configuration descriptor */
    0x09, /* bLength */
    0x02, /* bDescriptorType */
    0x19,
    0x00, /* wTotalLength */
    0x01, /* bNumInterfaces */
    0x01, /* bConfigurationValue */
    0x00, /* iConfiguration */
    0xC0, /* bmAttributes */
    0x00, /* MaxPower */

    /* interface descriptor */
    0x09, /* bLength */
    0x04, /* bDescriptorType */
    0x00, /* bInterfaceNumber */
    0x00, /* bAlternateSetting */
    0x01, /* bNumEndpoints */
    0x09, /* bInterfaceClass */
    0x00, /* bInterfaceSubClass */
    0x00, /* bInterfaceProtocol */
    0x00, /* iInterface */

    /* endpoint descriptor */
    0x07, /* bLength */
    0x05, /* bDescriptorType */
    0x81, /* bEndpointAddress */
    0x03, /* bmAttributes */
    0x02,
    0x00, /* wMaxPacketSize */
    0xFF, /* bInterval */
};

constexpr std::array<uint8_t, 25> descHighSpeed = {
    /* configuration descriptor */
    0x09, /* bLength */
    0x02, /* bDescriptorType */
    0x19,
    0x00, /* wTotalLength */
    0x01, /* bNumInterfaces */
    0x01, /* bConfigurationValue */
    0x00, /* iConfiguration */
    0xC0, /* bmAttributes */
    0x00, /* MaxPower */

    /* interface descriptor */
    0x09, /* bLength */
    0x04, /* bDescriptorType */
    0x00, /* bInterfaceNumber */
    0x00, /* bAlternateSetting */
    0x01, /* bNumEndpoints */
    0x09, /* bInterfaceClass */
    0x00, /* bInterfaceSubClass */
    0x00, /* bInterfaceProtocol */
    0x00, /* iInterface */

    /* endpoint descriptor */
    0x07, /* bLength */
    0x05, /* bDescriptorType */
    0x81, /* bEndpointAddress */
    0x03, /* bmAttributes */
    0x04,
    0x00, /* wMaxPacketSize */
    0x0c, /* bInterval */
};

constexpr std::array<uint8_t, 31> descSuperSpeed = {
    /* configuration descriptor */
    0x09, /* bLength */
    0x02, /* bDescriptorType */
    0x1f,
    0x00, /* wTotalLength */
    0x01, /* bNumInterfaces */
    0x01, /* bConfigurationValue */
    0x00, /* iConfiguration */
    0xC0, /* bmAttributes */
    0x00, /* MaxPower */

    /* interface descriptor */
    0x09, /* bLength */
    0x04, /* bDescriptorType */
    0x00, /* bInterfaceNumber */
    0x00, /* bAlternateSetting */
    0x01, /* bNumEndpoints */
    0x09, /* bInterfaceClass */
    0x00, /* bInterfaceSubClass */
    0x00, /* bInterfaceProtocol */
    0x00, /* iInterface */

    /* endpoint descriptor */
    0x07, /* bLength */
    0x05, /* bDescriptorType */
    0x81, /* bEndpointAddress */
    0x03, /* bmAttributes */
    0x04,
    0x00, /* wMaxPacketSize */
    0x0c, /* bInterval */

    /* SuperSpeed endpoint companion descriptor */
    0x06, /* bLength */
    0x30, /* bDescriptorType */
    0x00, /* bMaxBurst */
    0x00, /* bmAttributes */
    0x02,
    0x00, /* wBytesPerInterval */
};

} // namespace usb_subsystem::root_hub
