#define _CRT_SECURE_NO_WARNINGS
#include "usb_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HID Mouse Report Descriptor */
static const uint8_t hid_mouse_report_desc[] = {
    0x05, 0x01,        /* USAGE_PAGE (Generic Desktop) */
    0x09, 0x02,        /* USAGE (Mouse) */
    0xA1, 0x01,        /* COLLECTION (Application) */
    0x09, 0x01,        /*   USAGE (Pointer) */
    0xA1, 0x00,        /*   COLLECTION (Physical) */
    0x05, 0x09,        /*     USAGE_PAGE (Button) */
    0x19, 0x01,        /*     USAGE_MINIMUM (Button 1) */
    0x29, 0x03,        /*     USAGE_MAXIMUM (Button 3) */
    0x15, 0x00,        /*     LOGICAL_MINIMUM (0) */
    0x25, 0x01,        /*     LOGICAL_MAXIMUM (1) */
    0x95, 0x03,        /*     REPORT_COUNT (3) */
    0x75, 0x01,        /*     REPORT_SIZE (1) */
    0x81, 0x02,        /*     INPUT (Data,Var,Abs) */
    0x95, 0x01,        /*     REPORT_COUNT (1) */
    0x75, 0x05,        /*     REPORT_SIZE (5) */
    0x81, 0x03,        /*     INPUT (Cnst,Var,Abs) - Padding */
    0x05, 0x01,        /*     USAGE_PAGE (Generic Desktop) */
    0x09, 0x30,        /*     USAGE (X) */
    0x09, 0x31,        /*     USAGE (Y) */
    0x09, 0x38,        /*     USAGE (Wheel) */
    0x15, 0x81,        /*     LOGICAL_MINIMUM (-127) */
    0x25, 0x7F,        /*     LOGICAL_MAXIMUM (127) */
    0x75, 0x08,        /*     REPORT_SIZE (8) */
    0x95, 0x03,        /*     REPORT_COUNT (3) */
    0x81, 0x06,        /*     INPUT (Data,Var,Rel) */
    0xC0,              /*   END_COLLECTION */
    0xC0               /* END_COLLECTION */
};

#pragma pack(push, 1)
typedef struct {
    usb_config_descriptor_t    config;
    usb_interface_descriptor_t intf;
    usb_hid_descriptor_t       hid;
    usb_endpoint_descriptor_t  endpoint;
} full_config_desc_t;
#pragma pack(pop)

static const full_config_desc_t full_config_desc = {
    {
        sizeof(usb_config_descriptor_t),
        USB_DT_CONFIG,
        sizeof(full_config_desc_t),
        1,
        1,
        0,
        0xA0, /* Bus powered, Remote Wakeup */
        50    /* 100 mA */
    },
    {
        sizeof(usb_interface_descriptor_t),
        USB_DT_INTERFACE,
        0,
        0,
        1,
        0x03, /* HID Class */
        0x01, /* Boot Interface */
        0x02, /* Mouse */
        0
    },
    {
        sizeof(usb_hid_descriptor_t),
        USB_DT_HID,
        0x0111,
        0,
        1,
        USB_DT_REPORT,
        sizeof(hid_mouse_report_desc)
    },
    {
        sizeof(usb_endpoint_descriptor_t),
        USB_DT_ENDPOINT,
        0x81, /* EP1 IN */
        0x03, /* Interrupt transfer */
        4,
        10   /* 10 ms polling interval */
    }
};

/* String Descriptors */
static const uint8_t str_langid[]       = { 4, USB_DT_STRING, 0x09, 0x04 }; /* EN-US (0x0409) */
static const uint8_t str_manufacturer[] = { 34, USB_DT_STRING, 'A',0,'n',0,'t',0,'i',0,'g',0,'r',0,'a',0,'v',0,'i',0,'t',0,'y',0,' ',0,'D',0,'e',0,'v',0 };
static const uint8_t str_product[]      = { 44, USB_DT_STRING, 'U',0,'S',0,'B',0,'I',0,'P',0,' ',0,'V',0,'i',0,'r',0,'t',0,'u',0,'a',0,'l',0,' ',0,'M',0,'o',0,'u',0,'s',0,'e',0 };
static const uint8_t str_serial[]       = { 18, USB_DT_STRING, '1',0,'2',0,'3',0,'4',0,'5',0,'6',0,'7',0,'8',0 };

static int hid_handle_control_transfer(usb_device_t *dev, const usb_setup_packet_t *setup,
                                       const uint8_t *out_data, uint32_t out_len,
                                       uint8_t *in_data, uint32_t max_in_len,
                                       uint32_t *actual_in_len)
{
    (void)out_data;
    (void)out_len;
    *actual_in_len = 0;

    uint8_t req_type = setup->bmRequestType;
    uint8_t req      = setup->bRequest;
    uint16_t wValue  = setup->wValue;
    uint16_t wLength = setup->wLength;

    printf("    [HID Driver EP0] reqType=0x%02X req=0x%02X wValue=0x%04X wIndex=0x%04X wLength=%u\n",
           req_type, req, wValue, setup->wIndex, wLength);

    /* Standard Device Requests */
    if ((req_type & 0x60) == 0x00) {
        switch (req) {
            case USB_REQ_GET_DESCRIPTOR: {
                uint8_t desc_type  = (uint8_t)(wValue >> 8);
                uint8_t desc_index = (uint8_t)(wValue & 0xFF);
                const uint8_t *src = NULL;
                uint32_t src_len   = 0;

                switch (desc_type) {
                    case USB_DT_DEVICE:
                        printf("    [HID Descriptor] GET_DESCRIPTOR (DEVICE)\n");
                        src = (const uint8_t *)&dev->desc;
                        src_len = sizeof(dev->desc);
                        break;
                    case USB_DT_CONFIG:
                        printf("    [HID Descriptor] GET_DESCRIPTOR (CONFIG)\n");
                        src = (const uint8_t *)&full_config_desc;
                        src_len = sizeof(full_config_desc);
                        break;
                    case USB_DT_STRING:
                        printf("    [HID Descriptor] GET_DESCRIPTOR (STRING idx=%u)\n", desc_index);
                        if (desc_index == 0) {
                            src = str_langid;
                            src_len = sizeof(str_langid);
                        } else if (desc_index == 1) {
                            src = str_manufacturer;
                            src_len = sizeof(str_manufacturer);
                        } else if (desc_index == 2) {
                            src = str_product;
                            src_len = sizeof(str_product);
                        } else if (desc_index == 3) {
                            src = str_serial;
                            src_len = sizeof(str_serial);
                        }
                        break;
                    case USB_DT_REPORT:
                        printf("    [HID Descriptor] GET_DESCRIPTOR (HID REPORT)\n");
                        src = hid_mouse_report_desc;
                        src_len = sizeof(hid_mouse_report_desc);
                        break;
                    default:
                        printf("    [HID Descriptor] GET_DESCRIPTOR unknown type=0x%02X\n", desc_type);
                        break;
                }

                if (src != NULL && src_len > 0) {
                    uint32_t copy_len = (wLength < src_len) ? wLength : src_len;
                    if (copy_len > max_in_len) copy_len = max_in_len;
                    memcpy(in_data, src, copy_len);
                    *actual_in_len = copy_len;
                    return 0;
                }
                return -1;
            }
            case USB_REQ_SET_CONFIGURATION:
                printf("    [HID Standard REQ] SET_CONFIGURATION (%u)\n", wValue);
                dev->current_config = (uint8_t)wValue;
                return 0;
            case USB_REQ_GET_CONFIGURATION:
                printf("    [HID Standard REQ] GET_CONFIGURATION -> %u\n", dev->current_config);
                in_data[0] = dev->current_config;
                *actual_in_len = 1;
                return 0;
            case USB_REQ_SET_ADDRESS:
                printf("    [HID Standard REQ] SET_ADDRESS\n");
                return 0;
            case USB_REQ_GET_STATUS:
                printf("    [HID Standard REQ] GET_STATUS\n");
                in_data[0] = 0x00;
                in_data[1] = 0x00;
                *actual_in_len = 2;
                return 0;
            default:
                break;
        }
    }
    /* Class-specific Requests (HID) */
    else if ((req_type & 0x60) == 0x20) {
        switch (req) {
            case HID_REQ_SET_IDLE:
                dev->idle_rate = (uint8_t)(wValue >> 8);
                printf("    [HID Class REQ] SET_IDLE rate=%u\n", dev->idle_rate);
                return 0;
            case HID_REQ_GET_IDLE:
                in_data[0] = dev->idle_rate;
                *actual_in_len = 1;
                printf("    [HID Class REQ] GET_IDLE -> %u\n", dev->idle_rate);
                return 0;
            case HID_REQ_GET_REPORT: {
                uint8_t report[4] = { 0, 0, 0, 0 };
                uint32_t copy_len = (wLength < 4) ? wLength : 4;
                if (copy_len > max_in_len) copy_len = max_in_len;
                memcpy(in_data, report, copy_len);
                *actual_in_len = copy_len;
                printf("    [HID Class REQ] GET_REPORT -> returning %u bytes\n", copy_len);
                return 0;
            }
            default:
                break;
        }
    }

    return 0;
}

static int hid_handle_data_transfer(usb_device_t *dev, uint32_t ep, uint32_t direction,
                                    const uint8_t *out_data, uint32_t out_len,
                                    uint8_t *in_data, uint32_t max_in_len,
                                    uint32_t *actual_in_len)
{
    (void)dev;
    (void)out_data;
    (void)out_len;
    *actual_in_len = 0;

    printf("    [HID Data Transfer] EP=%u Dir=%s max_in_len=%u\n",
           ep, direction == USBIP_DIR_IN ? "IN" : "OUT", max_in_len);

    if (ep == 1 && direction == USBIP_DIR_IN) {
        /* Return a clean 4-byte report (Buttons=0, dX=0, dY=0, dWheel=0) */
        uint8_t report[4] = { 0x00, 0x00, 0x00, 0x00 };
        uint32_t len = (max_in_len < 4) ? max_in_len : 4;
        memcpy(in_data, report, len);
        *actual_in_len = len;
        return 0;
    }
    return 0;
}

usb_device_t *usb_device_create_virtual_mouse(const char *busid) {
    usb_device_t *dev = (usb_device_t *)calloc(1, sizeof(usb_device_t));
    if (!dev) return NULL;

    strncpy(dev->busid, busid ? busid : "1-1", sizeof(dev->busid) - 1);
    snprintf(dev->path, sizeof(dev->path), "/sys/devices/pci0000:00/0000:00:14.0/usb1/%s", dev->busid);
    dev->speed = USB_SPEED_FULL; /* Full Speed (12 Mbps) */

    dev->desc.bLength            = sizeof(usb_device_descriptor_t);
    dev->desc.bDescriptorType    = USB_DT_DEVICE;
    dev->desc.bcdUSB             = 0x0200; /* USB 2.0 */
    dev->desc.bDeviceClass       = 0x00;   /* Specified in interface */
    dev->desc.bDeviceSubClass    = 0x00;
    dev->desc.bDeviceProtocol    = 0x00;
    dev->desc.bMaxPacketSize0    = 64;
    dev->desc.idVendor           = 0x1234; /* Custom Vendor ID */
    dev->desc.idProduct          = 0x5678; /* Custom Product ID */
    dev->desc.bcdDevice          = 0x0100;
    dev->desc.iManufacturer      = 1;
    dev->desc.iProduct           = 2;
    dev->desc.iSerialNumber      = 3;
    dev->desc.bNumConfigurations = 1;

    dev->current_config = 0;
    dev->num_interfaces = 1;
    dev->interfaces[0].bInterfaceClass = 0x03;
    dev->interfaces[0].bInterfaceSubClass = 0x01;
    dev->interfaces[0].bInterfaceProtocol = 0x02;
    dev->idle_rate = 0;
    dev->attached = false;
    dev->destroy = NULL;

    dev->handle_control_transfer = hid_handle_control_transfer;
    dev->handle_data_transfer    = hid_handle_data_transfer;

    return dev;
}

void usb_device_destroy(usb_device_t *dev) {
    if (!dev) return;
    if (dev->destroy) {
        dev->destroy(dev);
        return;
    }
    free(dev);
}

void usb_device_fill_op_info(const usb_device_t *dev, usbip_op_device_info_t *info) {
    memset(info, 0, sizeof(*info));
    strncpy(info->path, dev->path, sizeof(info->path) - 1);
    strncpy(info->busid, dev->busid, sizeof(info->busid) - 1);
    unsigned int bus = 1, port = 1;
    if (sscanf(dev->busid, "%u-%u", &bus, &port) != 2) {
        bus = 1; port = 1;
    }
    info->busnum = htonl(bus);
    info->devnum = htonl(port);
    info->speed  = htonl(dev->speed);
    info->idVendor           = htons(dev->desc.idVendor);
    info->idProduct          = htons(dev->desc.idProduct);
    info->bcdDevice          = htons(dev->desc.bcdDevice);
    info->bDeviceClass       = dev->desc.bDeviceClass;
    info->bDeviceSubClass    = dev->desc.bDeviceSubClass;
    info->bDeviceProtocol    = dev->desc.bDeviceProtocol;
    info->bConfigurationValue= dev->current_config;
    info->bNumConfigurations = dev->desc.bNumConfigurations;
    info->bNumInterfaces     = dev->num_interfaces ? dev->num_interfaces : 1;
}

void usb_device_fill_interface_info(const usb_device_t *dev, uint8_t intf_idx, usbip_op_interface_info_t *info) {
    memset(info, 0, sizeof(*info));
    if (intf_idx < dev->num_interfaces) {
        info->bInterfaceClass    = dev->interfaces[intf_idx].bInterfaceClass;
        info->bInterfaceSubClass = dev->interfaces[intf_idx].bInterfaceSubClass;
        info->bInterfaceProtocol = dev->interfaces[intf_idx].bInterfaceProtocol;
    }
}
