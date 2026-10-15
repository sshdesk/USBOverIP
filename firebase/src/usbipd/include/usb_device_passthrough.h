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

void usb_device_list_physical_devices(void);

#endif
