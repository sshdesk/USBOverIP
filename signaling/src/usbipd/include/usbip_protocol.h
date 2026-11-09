#ifndef USBIP_PROTOCOL_H
#define USBIP_PROTOCOL_H

#include <stdint.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <arpa/inet.h>
#endif

#define USBIP_VERSION               0x0111
#define USBIP_DEFAULT_PORT          3240

/* Custom TCP control commands (32-bit network-order words). */
#define USBIP_CMD_CUSTOM_DETACH      0x5555FFFFu
#define USBIP_CMD_CUSTOM_LIST        0x5555FEEFu
#define USBIP_CMD_CUSTOM_BIND        0x5555FDDFu
#define SYSFS_BUS_ID_SIZE           32
#define SYSFS_PATH_MAX              256

/* Management OpCodes */
#define OP_REQ_DEVLIST              0x8005
#define OP_REP_DEVLIST              0x0005
#define OP_REQ_IMPORT               0x8003
#define OP_REP_IMPORT               0x0003

/* Management Status Codes */
#define ST_OK                       0x00000000
#define ST_NA                       0x00000001
#define ST_DEV_BUSY                 0x00000002
#define ST_DEV_ERR                  0x00000003
#define ST_NODEV                    0x00000004
#define ST_ERROR                    0x00000005

/* URB Command Codes */
#define USBIP_CMD_SUBMIT            0x00000001
#define USBIP_CMD_UNLINK            0x00000002
#define USBIP_RET_SUBMIT            0x00000003
#define USBIP_RET_UNLINK            0x00000004

/* Direction */
#define USBIP_DIR_OUT               0
#define USBIP_DIR_IN                1

/* Device Speeds */
#define USB_SPEED_UNKNOWN           0
#define USB_SPEED_LOW               1
#define USB_SPEED_FULL              2
#define USB_SPEED_HIGH              3
#define USB_SPEED_WIRELESS          4
#define USB_SPEED_SUPER             5

#pragma pack(push, 1)

/* --- Management Phase Packets (Big-Endian on Wire) --- */

typedef struct {
    uint16_t version;
    uint16_t command;
    uint32_t status;
} usbip_op_common_t;

typedef struct {
    uint16_t version;
    uint16_t command;
    uint32_t status;
} usbip_op_req_devlist_t;

typedef struct {
    uint16_t version;
    uint16_t command;
    uint32_t status;
    uint32_t ndev;
} usbip_op_rep_devlist_hdr_t;

typedef struct {
    char     path[SYSFS_PATH_MAX];
    char     busid[SYSFS_BUS_ID_SIZE];
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;

    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;

    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bConfigurationValue;
    uint8_t  bNumConfigurations;
    uint8_t  bNumInterfaces;
} usbip_op_device_info_t;

typedef struct {
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t padding;
} usbip_op_interface_info_t;

typedef struct {
    uint16_t version;
    uint16_t command;
    uint32_t status;
    char     busid[SYSFS_BUS_ID_SIZE];
} usbip_op_req_import_t;

typedef struct {
    uint16_t version;
    uint16_t command;
    uint32_t status;
    usbip_op_device_info_t udev;
} usbip_op_rep_import_t;

/* --- URB Transfer Phase Packets (Big-Endian on Wire) --- */

typedef struct {
    uint32_t command;               /* USBIP_CMD_SUBMIT (0x01) */
    uint32_t seqnum;                /* Sequential packet id */
    uint32_t devid;
    uint32_t direction;            /* 0: OUT, 1: IN */
    uint32_t ep;                   /* Endpoint number */
    uint32_t transfer_flags;
    uint32_t transfer_buffer_length;
    int32_t  start_frame;
    int32_t  number_of_packets;
    int32_t  interval;
    uint8_t  setup[8];              /* Setup Packet (Standard USB Control) */
} usbip_header_cmd_submit_t;

typedef struct {
    uint32_t command;               /* USBIP_RET_SUBMIT (0x03) */
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
    uint32_t status;                /* 0 for success */
    uint32_t actual_length;
    int32_t  start_frame;
    int32_t  number_of_packets;
    int32_t  error_count;
    uint8_t  setup[8];              /* 8 bytes setup/padding to match Linux kernel 48-byte header */
} usbip_header_ret_submit_t;

typedef struct {
    uint32_t command;               /* USBIP_CMD_UNLINK (0x02) */
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
    uint32_t unlink_seqnum;         /* Seqnum of URB to cancel */
    uint8_t  padding[24];
} usbip_header_cmd_unlink_t;

typedef struct {
    uint32_t command;               /* USBIP_RET_UNLINK (0x04) */
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
    uint32_t status;
    uint8_t  padding[24];
} usbip_header_ret_unlink_t;

#pragma pack(pop)

/* Endianness conversion helpers */
static inline uint64_t usbip_ntohll(uint64_t val) {
    return (((uint64_t)ntohl((uint32_t)(val & 0xFFFFFFFF))) << 32) |
           (uint64_t)ntohl((uint32_t)(val >> 32));
}

static inline uint64_t usbip_htonll(uint64_t val) {
    return usbip_ntohll(val);
}

#endif /* USBIP_PROTOCOL_H */
