#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include "usbip_protocol.h"
#include "usb_descriptors.h"
#include <stdbool.h>

typedef struct usb_device usb_device_t;

typedef struct {
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
} usb_device_interface_info_t;

#define USB_DEVICE_MAX_INTERFACES 32

struct usb_device {
    char     busid[SYSFS_BUS_ID_SIZE];
    char     path[SYSFS_PATH_MAX];
    uint32_t speed;
    
    usb_device_descriptor_t desc;
    uint8_t  current_config;
    uint8_t  num_interfaces;
    usb_device_interface_info_t interfaces[USB_DEVICE_MAX_INTERFACES];
    uint8_t  idle_rate;
    bool     attached;
    bool     reserved;          /* CUSTOM_BIND accepted; awaiting USB/IP IMPORT */
    bool     detach_requested;  /* explicit CUSTOM_DETACH requested */
    //uintptr_t active_socket;    /* owning USB/IP data socket while attached */
    uint64_t reserved_at_ms;    /* reservation timeout guard */

    /* Backend-specific cleanup. */
    void (*destroy)(usb_device_t *dev);

    /* Device operations */
    int (*handle_control_transfer)(usb_device_t *dev, const usb_setup_packet_t *setup,
                                   const uint8_t *out_data, uint32_t out_len,
                                   uint8_t *in_data, uint32_t max_in_len,
                                   uint32_t *actual_in_len);

    int (*handle_data_transfer)(usb_device_t *dev, uint32_t ep, uint32_t direction,
                                const uint8_t *out_data, uint32_t out_len,
                                uint8_t *in_data, uint32_t max_in_len,
                                uint32_t *actual_in_len);
};

/* Virtual HID Mouse/Keyboard device constructor */
usb_device_t *usb_device_create_virtual_mouse(const char *busid);
void usb_device_destroy(usb_device_t *dev);

void usb_device_fill_op_info(const usb_device_t *dev, usbip_op_device_info_t *info);
void usb_device_fill_interface_info(const usb_device_t *dev, uint8_t intf_idx, usbip_op_interface_info_t *info);

#endif /* USB_DEVICE_H */
