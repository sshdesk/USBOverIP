#ifndef USBIP_SERVER_H
#define USBIP_SERVER_H

#include "usbip_protocol.h"
#include "usb_device.h"

#define MAX_DEVICES 64

typedef struct {
    uint16_t      port;
    usb_device_t *devices[MAX_DEVICES];
    int           num_devices;
    bool          dynamic_devices; /* enumerate on DEVLIST; capture on IMPORT */
    volatile bool running;
#ifdef _WIN32
    SOCKET        listen_socket;
    SOCKET        active_client_socket;
    HANDLE        client_done_event;
#else
    int           listen_socket;
    int           active_client_socket;
#endif
} usbip_server_t;

usbip_server_t *usbip_server_create(uint16_t port);
void usbip_server_destroy(usbip_server_t *server);

int  usbip_server_register_device(usbip_server_t *server, usb_device_t *dev);
void usbip_server_enable_dynamic_devices(usbip_server_t *server, bool enabled);
int  usbip_server_run(usbip_server_t *server);
void usbip_server_stop(usbip_server_t *server);

#endif /* USBIP_SERVER_H */
