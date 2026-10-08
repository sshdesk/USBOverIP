#ifndef VBOX_USB_H
#define VBOX_USB_H

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif
#include <stdint.h>
#include <stddef.h>

#define USBMON_DEVICE_NAME L"\\\\.\\VBoxUSBMon"
#define USBMON_MAGIC 0x19670408u

/* VBoxUSB / VBoxUSBMon IOCTLs used by usbipd-win. */
#define SUPUSB_IOCTL_SEND_URB             CTL_CODE(FILE_DEVICE_UNKNOWN, 0x607, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define SUPUSB_IOCTL_USB_SELECT_INTERFACE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x609, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define SUPUSB_IOCTL_USB_SET_CONFIG       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x60A, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define SUPUSB_IOCTL_USB_CLAIM_DEVICE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x60B, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define SUPUSB_IOCTL_USB_CLEAR_ENDPOINT   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x60E, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define SUPUSB_IOCTL_GET_VERSION          CTL_CODE(FILE_DEVICE_UNKNOWN, 0x60F, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define SUPUSB_IOCTL_USB_ABORT_ENDPOINT   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x610, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/* Same numeric function as USB_ABORT_ENDPOINT, but sent to VBoxUSBMon. */
#define SUPUSBMON_IOCTL_GET_VERSION       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x610, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define SUPUSBMON_IOCTL_ADD_FILTER        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x611, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define SUPUSBMON_IOCTL_REMOVE_FILTER     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x612, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define VBOXUSB_DRIVER_MAJOR 5u

/* USBSUP_TRANSFER_TYPE */
enum {
    VBOXUSB_TRANSFER_CONTROL   = 0,
    VBOXUSB_TRANSFER_ISO       = 1,
    VBOXUSB_TRANSFER_BULK      = 2,
    VBOXUSB_TRANSFER_INTERRUPT = 3,
    VBOXUSB_TRANSFER_MESSAGE   = 4
};

/* USBSUP_DIRECTION */
enum {
    VBOXUSB_DIR_SETUP = 0,
    VBOXUSB_DIR_IN    = 1,
    VBOXUSB_DIR_OUT   = 2
};

/* USBSUP_XFER_FLAG */
#define VBOXUSB_XFER_FLAG_SHORT_OK 0x00000001u

/* USBSUP_ERROR */
enum {
    VBOXUSB_ERR_OK = 0,
    VBOXUSB_ERR_STALL,
    VBOXUSB_ERR_DEVICE_NOT_RESPONDING,
    VBOXUSB_ERR_CRC,
    VBOXUSB_ERR_NAC,
    VBOXUSB_ERR_UNDERRUN,
    VBOXUSB_ERR_OVERRUN
};

#pragma pack(push, 4)
typedef struct {
    uint16_t cb;
    uint16_t off;
    uint32_t stat;
} vbox_usb_iso_packet_t;

typedef struct {
    uint32_t uType;          /* offset 0 */
    uint32_t uEndpoint;      /* offset 4, endpoint number only */
    uint32_t enmDir;         /* offset 8 */
    uint32_t fFlags;         /* offset 12 */
    uint32_t enmStatus;      /* offset 16, returned by driver */
    uint64_t cbData;         /* offset 20 */
    uint64_t pvData;         /* offset 28, native user pointer */
    uint32_t cIsocPkts;      /* offset 36 */
    vbox_usb_iso_packet_t aIsoPkts[8]; /* offset 40, total struct 104 */
} vbox_usb_urb_t;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    uint8_t bInterfaceNumber;
    uint8_t fClaimed;
} vbox_usb_claim_dev_t;
#pragma pack(pop)

/* USBFILTER / VBoxUSBMon ADD_FILTER layout is 312 bytes. */
#pragma pack(push, 1)
typedef struct {
    uint16_t enmMatch;
    uint16_t u16Value;
} vbox_usb_filter_field_t;

typedef struct {
    uint32_t u32Magic;
    uint32_t enmType;
    vbox_usb_filter_field_t aFields[11];
    uint32_t offCurEnd;
    uint8_t achStrTab[256];
} vbox_usb_filter_t;

/* ADD_FILTER output: uId + rc. */
typedef struct {
    uint64_t uId;
    int32_t rc;
} vbox_usb_filter_add_out_t;
#pragma pack(pop)

#define VBOXUSB_FILTER_TYPE_CAPTURE 4u /* UsbFilterType.CAPTURE */
#define VBOXUSB_FILTER_MATCH_IGNORE 1u
#define VBOXUSB_FILTER_MATCH_NUM_EXACT 3u

enum {
    VBOXUSB_FILTER_VENDOR = 0,
    VBOXUSB_FILTER_PRODUCT = 1,
    VBOXUSB_FILTER_DEVICE_REV = 2,
    VBOXUSB_FILTER_DEVICE_CLASS = 3,
    VBOXUSB_FILTER_DEVICE_SUBCLASS = 4,
    VBOXUSB_FILTER_DEVICE_PROTOCOL = 5,
    VBOXUSB_FILTER_BUS = 6,
    VBOXUSB_FILTER_PORT = 7
};

#ifdef __cplusplus
static_assert(sizeof(vbox_usb_urb_t) == 104, "VBox USBSUP_URB must be 104 bytes on x64");
static_assert(sizeof(vbox_usb_filter_t) == 312, "VBox USBFILTER must be 312 bytes");
#endif

#endif
