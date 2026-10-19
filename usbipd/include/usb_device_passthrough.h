#ifndef USB_DEVICE_PASSTHROUGH_H
#define USB_DEVICE_PASSTHROUGH_H

#include "usb_device.h"
#include <windows.h>
#include <stdbool.h>

/*
 * Real Windows USB/IP passthrough using the VBoxUSB/VBoxUSBMon host-side
 * driver stack used by usbipd-win. The caller must run elevated and the
 * usbipd-win VBoxUSB drivers must be installed.
 */
usb_device_t *usb_device_create_passthrough(const char *busid,
                                             uint16_t vid,
                                             uint16_t pid,
                                             HANDLE ignored_driver);

#define USB_PHYSICAL_DEVICE_MAX 64

typedef struct {
    char busid[SYSFS_BUS_ID_SIZE];
    uint16_t vid;
    uint16_t pid;
} usb_physical_device_info_t;

/* Enumerate physical USB devices without capturing them. Returns the number
 * of records written (up to capacity). Devices without a derivable BUSID are
 * omitted because USB/IP IMPORT selects devices by BUSID. */
int usb_device_enumerate_physical_devices(usb_physical_device_info_t *out,
                                          int capacity);
void usb_device_list_physical_devices(void);

#endif
