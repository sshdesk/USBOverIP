#ifndef USBIP_SERVER_H
#define USBIP_SERVER_H

#include "usbip_protocol.h"
#include "usb_device.h"
#include <stdbool.h>
#include <windows.h>

#define MAX_DEVICES 64

typedef struct rtc_peer_connection rtc_peer_connection_t;
typedef struct rtc_data_channel    rtc_data_channel_t;
typedef struct ring_buffer         ring_buffer_t;

struct usbip_server_t {
    usb_device_t         *devices[MAX_DEVICES];
    int                   num_devices;
    bool                  dynamic_devices; /* enumerate on DEVLIST; capture on IMPORT */
    volatile bool         running;

    rtc_peer_connection_t *peer_connection;
    rtc_data_channel_t    *data_channel;
    bool                  client_connected;

    ring_buffer_t         *rx_buffer;
    CRITICAL_SECTION       lock;
    HANDLE                 worker_thread;
    volatile bool          worker_running;

    char                  active_import_busids[MAX_DEVICES][SYSFS_BUS_ID_SIZE];
};

typedef struct usbip_server_t usbip_server_t;

usbip_server_t *usbip_server_create(void);
void usbip_server_destroy(usbip_server_t *server);
void usbip_server_enable_dynamic_devices(usbip_server_t *server, bool enabled);
int  usbip_server_register_device(usbip_server_t *server, usb_device_t *dev);
void usbip_server_stop(usbip_server_t *server);

void usbip_server_on_data_received(usbip_server_t *server, const uint8_t *data, size_t size);
void usbip_server_on_channel_open(usbip_server_t *server);
void usbip_server_on_channel_closed(usbip_server_t *server);

#endif /* USBIP_SERVER_H */