#define _CRT_SECURE_NO_WARNINGS
#include "usbip_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// External bindings to your C++ Firebase REST/SDK and libwebrtc implementation layer
extern "C" {
    bool firebase_init(const char *database_url);
    void firebase_publish_offer(const char *busid, const char *sdp);
    void firebase_listen_for_answer(const char *busid, void (*on_answer)(const char *sdp));
    bool webrtc_init_peer_connection(usbip_server_t *server);
    bool webrtc_data_channel_send(rtc_data_channel_t *dc, const void *data, size_t size);
}

static int send_via_webrtc(usbip_server_t *server, const void *buf, size_t len) {
    if (!server->data_channel || !server->client_connected) return -1;
    if (webrtc_data_channel_send(server->data_channel, buf, len)) {
        return 0;
    }
    return -1;
}

static usb_device_t *find_device_by_busid(usbip_server_t *server, const char *busid) {
    for (int i = 0; i < server->num_devices; ++i) {
        if (strcmp(server->devices[i]->busid, busid) == 0) {
            return server->devices[i];
        }
    }
    return NULL;
}

static usb_device_t *get_attached_device(usbip_server_t *server) {
    for (int i = 0; i < server->num_devices; ++i) {
        if (server->devices[i]->attached) {
            return server->devices[i];
        }
    }
    return NULL;
}

// Core USB/IP packet router triggered when binary data frames arrive via WebRTC DataChannel
void usbip_server_on_data_received(usbip_server_t *server, const uint8_t *data, size_t size) {
    if (!server->running || !server->client_connected) return;

    if (size < sizeof(uint32_t)) return;

    uint32_t cmd_code_net = 0;
    memcpy(&cmd_code_net, data, sizeof(uint32_t));
    uint32_t cmd_code = ntohl(cmd_code_net);

    if (cmd_code == OP_REQ_DEVLIST) {
        printf("[USB/IP] Handling OP_REQ_DEVLIST via WebRTC DataChannel (Devices: %d)\n", server->num_devices);

        usbip_op_rep_devlist_hdr_t rep;
        rep.version = htons(USBIP_VERSION);
        rep.command = htons(OP_REP_DEVLIST);
        rep.status  = htonl(ST_OK);
        rep.ndev    = htonl((uint32_t)server->num_devices);

        if (send_via_webrtc(server, &rep, sizeof(rep)) != 0) return;

        for (int i = 0; i < server->num_devices; ++i) {
            usb_device_t *dev = server->devices[i];
            usbip_op_device_info_t dev_info;
            usb_device_fill_op_info(dev, &dev_info);

            if (send_via_webrtc(server, &dev_info, sizeof(dev_info)) != 0) return;

            for (int intf = 0; intf < dev_info.bNumInterfaces; ++intf) {
                usbip_op_interface_info_t intf_info;
                usb_device_fill_interface_info(dev, (uint8_t)intf, &intf_info);
                if (send_via_webrtc(server, &intf_info, sizeof(intf_info)) != 0) return;
            }
        }
    }
    else if (cmd_code == OP_REQ_IMPORT) {
        if (size < sizeof(usbip_op_req_import_t)) return;
        
        char busid[SYSFS_BUS_ID_SIZE] = {0};
        memcpy(busid, data + sizeof(usbip_op_common_t), SYSFS_BUS_ID_SIZE);

        printf("[USB/IP] Received OP_REQ_IMPORT for busid: '%s'\n", busid);
        usb_device_t *dev = find_device_by_busid(server, busid);

        usbip_op_rep_import_t rep;
        memset(&rep, 0, sizeof(rep));
        rep.version = htons(USBIP_VERSION);
        rep.command = htons(OP_REP_IMPORT);

        if (!dev) {
            printf("[USB/IP] Device '%s' not found.\n", busid);
            rep.status = htonl(ST_NODEV);
            send_via_webrtc(server, &rep, sizeof(rep));
            return;
        }

        if (dev->attached) {
            printf("[USB/IP] Device '%s' is already busy/attached.\n", busid);
            rep.status = htonl(ST_DEV_BUSY);
            send_via_webrtc(server, &rep, sizeof(rep));
            return;
        }

        rep.status = htonl(ST_OK);
        usb_device_fill_op_info(dev, &rep.udev);
        send_via_webrtc(server, &rep, sizeof(rep));
        
        dev->attached = true;
        printf("[USB/IP] Device %s attached to client via WebRTC.\n", dev->busid);
    }
    else if (cmd_code == USBIP_CMD_SUBMIT) {
        if (size < sizeof(usbip_header_cmd_submit_t)) return;

        usbip_header_cmd_submit_t cmd;
        memcpy(&cmd, data, sizeof(cmd));

        uint32_t seqnum      = ntohl(cmd.seqnum);
        uint32_t devid       = ntohl(cmd.devid);
        uint32_t direction   = ntohl(cmd.direction);
        uint32_t ep          = ntohl(cmd.ep);
        uint32_t buf_len     = ntohl(cmd.transfer_buffer_length);

        usb_device_t *dev = get_attached_device(server);
        if (!dev) return;

        const uint8_t *payload_out = NULL;
        uint8_t *temp_out_buf = NULL;
        if (direction == USBIP_DIR_OUT && buf_len > 0) {
            if (size >= sizeof(cmd) + buf_len) {
                payload_out = data + sizeof(cmd);
            } else {
                temp_out_buf = (uint8_t *)malloc(buf_len);
                if (temp_out_buf) {
                    size_t available = size > sizeof(cmd) ? size - sizeof(cmd) : 0;
                    if (available > 0) {
                        memcpy(temp_out_buf, data + sizeof(cmd), available > buf_len ? buf_len : available);
                    }
                    payload_out = temp_out_buf;
                }
            }
        }

        uint8_t *payload_in = (uint8_t *)malloc(262144);
        if (!payload_in) {
            if (temp_out_buf) free(temp_out_buf);
            return;
        }

        uint32_t actual_length = 0;
        int status = 0;

        if (ep == 0) {
            usb_setup_packet_t *setup = (usb_setup_packet_t *)cmd.setup;
            status = dev->handle_control_transfer(dev, setup, payload_out, buf_len,
                                                  payload_in, 262144, &actual_length);
        } else {
            status = dev->handle_data_transfer(dev, ep, direction, payload_out, buf_len,
                                               payload_in, 262144, &actual_length);
        }

        if (temp_out_buf) free(temp_out_buf);

        usbip_header_ret_submit_t ret;
        memset(&ret, 0, sizeof(ret));
        ret.command       = htonl(USBIP_RET_SUBMIT);
        ret.seqnum        = htonl(seqnum);
        ret.devid         = htonl(devid);
        ret.direction     = htonl(direction);
        ret.ep            = htonl(ep);
        ret.status        = htonl((uint32_t)status);
        ret.actual_length = htonl(status == 0 ? actual_length : 0);
        ret.start_frame   = htonl(0);
        ret.number_of_packets = htonl(0);
        ret.error_count   = htonl(0);

        if (send_via_webrtc(server, &ret, sizeof(ret)) != 0) {
            free(payload_in);
            return;
        }

        if (direction == USBIP_DIR_IN && actual_length > 0) {
            send_via_webrtc(server, payload_in, actual_length);
        }

        free(payload_in);
    }
    else if (cmd_code == USBIP_CMD_UNLINK) {
        if (size < sizeof(usbip_header_cmd_unlink_t)) return;

        usbip_header_cmd_unlink_t unlink_cmd;
        memcpy(&unlink_cmd, data, sizeof(unlink_cmd));

        printf("  [URB Unlink] seqnum=%u unlink_seqnum=%u\n", 
               ntohl(unlink_cmd.seqnum), ntohl(unlink_cmd.unlink_seqnum));

        usbip_header_ret_unlink_t unlink_ret;
        memset(&unlink_ret, 0, sizeof(unlink_ret));
        unlink_ret.command   = htonl(USBIP_RET_UNLINK);
        unlink_ret.seqnum    = unlink_cmd.seqnum;
        unlink_ret.devid     = unlink_cmd.devid;
        unlink_ret.direction = unlink_cmd.direction;
        unlink_ret.ep        = unlink_cmd.ep;
        unlink_ret.status    = htonl(0);

        send_via_webrtc(server, &unlink_ret, sizeof(unlink_ret));
    }
}

void usbip_server_on_channel_open(usbip_server_t *server) {
    printf("[WebRTC] Data Channel connected via Firebase signaling. Ready for USB/IP traffic.\n");
    server->client_connected = true;
}

void usbip_server_on_channel_closed(usbip_server_t *server) {
    printf("[WebRTC] Data Channel disconnected.\n");
    server->client_connected = false;
    for (int i = 0; i < server->num_devices; ++i) {
        server->devices[i]->attached = false;
    }
}

usbip_server_t *usbip_server_create(const char *firebase_url) {
    usbip_server_t *server = (usbip_server_t *)calloc(1, sizeof(usbip_server_t));
    if (!server) return NULL;

    strncpy(server->firebase_url, firebase_url ? firebase_url : "", sizeof(server->firebase_url) - 1);
    server->running = false;
    server->client_connected = false;
    return server;
}

void usbip_server_destroy(usbip_server_t *server) {
    if (server) {
        usbip_server_stop(server);
        for (int i = 0; i < server->num_devices; ++i) {
            usb_device_destroy(server->devices[i]);
        }
        free(server);
    }
}

int usbip_server_register_device(usbip_server_t *server, usb_device_t *dev) {
    if (!server || !dev || server->num_devices >= MAX_DEVICES) return -1;
    server->devices[server->num_devices++] = dev;
    return 0;
}

int usbip_server_run(usbip_server_t *server) {
    if (!server) return -1;

    if (!firebase_init(server->firebase_url)) {
        fprintf(stderr, "[Error] Failed to connect to Firebase database endpoint.\n");
        return -1;
    }

    if (!webrtc_init_peer_connection(server)) {
        fprintf(stderr, "[Error] Failed to initialize libwebrtc peer connection context.\n");
        return -1;
    }

    server->running = true;
    printf("==================================================\n");
    printf(" USB/IP Daemon (Firebase + LibWebRTC DataChannel)\n");
    printf(" Exported devices: %d\n", server->num_devices);
    for (int i = 0; i < server->num_devices; ++i) {
        printf("   - BusID: %s (%04x:%04x)\n",
               server->devices[i]->busid,
               server->devices[i]->desc.idVendor,
               server->devices[i]->desc.idProduct);
    }
    printf("==================================================\n");

    while (server->running) {
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    }

    return 0;
}

void usbip_server_stop(usbip_server_t *server) {
    if (server && server->running) {
        server->running = false;
        server->client_connected = false;
    }
}