#include "usbip_server.h"
#include "usbip_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Linux errno-style USB/IP status used when a request exceeds the bounded
 * transfer buffer supported by this server. */
#define USBIP_STATUS_EOVERFLOW (-75)
#define USBIP_MAX_TRANSFER_SIZE 262144u

// Forward declaration for WebRTC data channel send API if available externally
extern "C" void rtc_data_channel_send(rtc_data_channel_t *channel, const uint8_t *data, size_t size);

struct ring_buffer {
    uint8_t *buffer;
    size_t   capacity;
    size_t   size;
};

static ring_buffer_t *ring_buffer_create(size_t capacity) {
    ring_buffer_t *rb = (ring_buffer_t *)malloc(sizeof(ring_buffer_t));
    if (!rb) return NULL;
    rb->buffer = (uint8_t *)malloc(capacity);
    rb->capacity = capacity;
    rb->size = 0;
    if (!rb->buffer) {
        free(rb);
        return NULL;
    }
    return rb;
}

static void ring_buffer_destroy(ring_buffer_t *rb) {
    if (rb) {
        free(rb->buffer);
        free(rb);
    }
}

static void ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t size) {
    if (rb->size + size <= rb->capacity) {
        memcpy(rb->buffer + rb->size, data, size);
        rb->size += size;
    } else {
        size_t excess = (rb->size + size) - rb->capacity;
        if (excess < rb->size) {
            size_t move_len = rb->size - excess;
            memmove(rb->buffer, rb->buffer + excess, move_len);
            rb->size = move_len;
            memcpy(rb->buffer + rb->size, data, size);
            rb->size += size;
        } else {
            memcpy(rb->buffer, data, size > rb->capacity ? rb->capacity : size);
            rb->size = size > rb->capacity ? rb->capacity : size;
        }
    }
}

static size_t ring_buffer_available(ring_buffer_t *rb) {
    return rb->size;
}

static int ring_buffer_read(ring_buffer_t *rb, uint8_t *dest, size_t size) {
    if (rb->size < size) return -1;
    memcpy(dest, rb->buffer, size);
    size_t remaining = rb->size - size;
    memmove(rb->buffer, rb->buffer + size, remaining);
    rb->size = remaining;
    return 0;
}

static int send_exact_rtc(usbip_server_t *server, const void *buf, size_t len) {
    if (!server->data_channel || !server->client_connected) return -1;
    rtc_data_channel_send(server->data_channel, (const uint8_t *)buf, len);
    return 0;
}

static int recv_exact_rtc(usbip_server_t *server, void *buf, size_t len) {
    char *ptr = (char *)buf;
    size_t total_received = 0;
    
    while (server->running && server->client_connected) {
        EnterCriticalSection(&server->lock);
        size_t available = ring_buffer_available(server->rx_buffer);
        size_t needed = len - total_received;
        
        if (available >= needed) {
            ring_buffer_read(server->rx_buffer, (uint8_t *)(ptr + total_received), needed);
            LeaveCriticalSection(&server->lock);
            total_received += needed;
            return 0;
        } else if (available > 0) {
            ring_buffer_read(server->rx_buffer, (uint8_t *)(ptr + total_received), available);
            total_received += available;
        }
        LeaveCriticalSection(&server->lock);
        
        Sleep(1);
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

static int handle_op_devlist(usbip_server_t *server) {
    printf("[USB/IP WebRTC] Handling OP_REQ_DEVLIST (Exported devices count: %d)\n", server->num_devices);

    usbip_op_rep_devlist_hdr_t rep;
    rep.version = htons(USBIP_VERSION);
    rep.command = htons(OP_REP_DEVLIST);
    rep.status  = htonl(ST_OK);
    rep.ndev    = htonl((uint32_t)server->num_devices);

    if (send_exact_rtc(server, &rep, sizeof(rep)) != 0) {
        return -1;
    }

    for (int i = 0; i < server->num_devices; ++i) {
        usb_device_t *dev = server->devices[i];
        usbip_op_device_info_t dev_info;
        usb_device_fill_op_info(dev, &dev_info);

        if (send_exact_rtc(server, &dev_info, sizeof(dev_info)) != 0) {
            return -1;
        }

        for (int intf = 0; intf < dev_info.bNumInterfaces; ++intf) {
            usbip_op_interface_info_t intf_info;
            usb_device_fill_interface_info(dev, (uint8_t)intf, &intf_info);
            if (send_exact_rtc(server, &intf_info, sizeof(intf_info)) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int handle_urb_transfer_loop(usbip_server_t *server, usb_device_t *dev) {
    printf("[USB/IP WebRTC] Attached to device %s. Entering URB transfer loop.\n", dev->busid);
    dev->attached = true;

    uint8_t *payload_in  = (uint8_t *)malloc(USBIP_MAX_TRANSFER_SIZE);
    uint8_t *payload_out = (uint8_t *)malloc(USBIP_MAX_TRANSFER_SIZE);

    if (!payload_in || !payload_out) {
        printf("[USB/IP ERROR] Failed to allocate URB payload buffers.\n");
        free(payload_in);
        free(payload_out);
        return -1;
    }

    while (server->running && server->client_connected) {
        uint32_t cmd_code_net = 0;
        if (recv_exact_rtc(server, &cmd_code_net, sizeof(cmd_code_net)) != 0) {
            break;
        }
        uint32_t cmd_code = ntohl(cmd_code_net);

        if (cmd_code == USBIP_CMD_SUBMIT) {
            usbip_header_cmd_submit_t cmd;
            cmd.command = cmd_code;
            if (recv_exact_rtc(server, ((char *)&cmd) + sizeof(uint32_t), sizeof(cmd) - sizeof(uint32_t)) != 0) {
                break;
            }

            uint32_t seqnum      = ntohl(cmd.seqnum);
            uint32_t devid       = ntohl(cmd.devid);
            uint32_t direction   = ntohl(cmd.direction);
            uint32_t ep          = ntohl(cmd.ep);
            uint32_t buf_len     = ntohl(cmd.transfer_buffer_length);

            USBIP_LOG(3, "[URB Submit #%u] DevID=0x%08X EP=%u Dir=%s TransferLen=%u\n",
                   seqnum, devid, ep, direction == USBIP_DIR_IN ? "IN" : "OUT", buf_len);

            if (direction == USBIP_DIR_OUT && buf_len > 0) {
                uint32_t read_len = (buf_len < USBIP_MAX_TRANSFER_SIZE) ? buf_len : USBIP_MAX_TRANSFER_SIZE;
                if (recv_exact_rtc(server, payload_out, read_len) != 0) {
                    break;
                }
                if (buf_len > read_len) {
                    uint32_t rem = buf_len - read_len;
                    while (rem > 0) {
                        char discard[256];
                        uint32_t chunk = (rem < sizeof(discard)) ? rem : sizeof(discard);
                        if (recv_exact_rtc(server, discard, chunk) != 0) break;
                        rem -= chunk;
                    }
                }
            }

            uint32_t actual_length = 0;
            int status = 0;

            if (buf_len > USBIP_MAX_TRANSFER_SIZE) {
                status = USBIP_STATUS_EOVERFLOW;
                actual_length = 0;
            } else if (ep == 0) {
                usb_setup_packet_t *setup = (usb_setup_packet_t *)cmd.setup;
                status = dev->handle_control_transfer(dev, setup, payload_out, buf_len,
                                                      payload_in, 262144, &actual_length);
            } else {
                status = dev->handle_data_transfer(dev, ep, direction, payload_out, buf_len,
                                                   payload_in, 262144, &actual_length);
            }

            /* Send USBIP_RET_SUBMIT header (48 bytes) - exact match */
            usbip_header_ret_submit_t ret;
            memset(&ret, 0, sizeof(ret));
            ret.command       = htonl(USBIP_RET_SUBMIT);
            ret.seqnum        = htonl(seqnum);
            ret.devid         = htonl(devid);
            ret.direction     = htonl(direction);
            ret.ep            = htonl(ep);
            ret.status        = htonl((uint32_t)status);
            ret.actual_length = htonl(status == 0 ? actual_length : 0);

            if (send_exact_rtc(server, &ret, sizeof(ret)) != 0) {
                break;
            }

            /* Send IN payload back if requested - exact match */
            if (status == 0 && direction == USBIP_DIR_IN && actual_length > 0) {
                if (send_exact_rtc(server, payload_in, actual_length) != 0) {
                    break;
                }
            }
        }
        else if (cmd_code == USBIP_CMD_UNLINK) {
            usbip_header_cmd_unlink_t unlink_cmd;
            unlink_cmd.command = cmd_code;
            if (recv_exact_rtc(server, ((char *)&unlink_cmd) + sizeof(uint32_t), sizeof(unlink_cmd) - sizeof(uint32_t)) != 0) {
                break;
            }

            usbip_header_ret_unlink_t unlink_ret;
            memset(&unlink_ret, 0, sizeof(unlink_ret));
            unlink_ret.command   = htonl(USBIP_RET_UNLINK);
            unlink_ret.seqnum    = unlink_cmd.seqnum;
            unlink_ret.devid     = unlink_cmd.devid;
            unlink_ret.direction = unlink_cmd.direction;
            unlink_ret.ep        = unlink_cmd.ep;
            unlink_ret.status    = htonl(0);

            if (send_exact_rtc(server, &unlink_ret, sizeof(unlink_ret)) != 0) {
                break;
            }
        }
        else {
            break;
        }
    }

    free(payload_in);
    free(payload_out);
    dev->attached = false;
    printf("[USB/IP WebRTC] Device %s detached.\n", dev->busid);
    return 0;
}

static int handle_op_import(usbip_server_t *server, const usbip_op_common_t *hdr) {
    (void)hdr;
    char busid[SYSFS_BUS_ID_SIZE] = {0};
    if (recv_exact_rtc(server, busid, sizeof(busid)) != 0) {
        return -1;
    }

    printf("[USB/IP WebRTC] Received OP_REQ_IMPORT for busid: '%s'\n", busid);
    usb_device_t *dev = find_device_by_busid(server, busid);

    usbip_op_rep_import_t rep;
    memset(&rep, 0, sizeof(rep));
    rep.version = htons(USBIP_VERSION);
    rep.command = htons(OP_REP_IMPORT);

    if (!dev) {
        rep.status = htonl(ST_NODEV);
        send_exact_rtc(server, &rep, sizeof(rep));
        return -1;
    }

    if (dev->attached) {
        rep.status = htonl(ST_DEV_BUSY);
        send_exact_rtc(server, &rep, sizeof(rep));
        return -1;
    }

    rep.status = htonl(ST_OK);
    usb_device_fill_op_info(dev, &rep.udev);

    if (send_exact_rtc(server, &rep, sizeof(rep)) != 0) {
        return -1;
    }

    return handle_urb_transfer_loop(server, dev);
}

static DWORD WINAPI web_rtc_worker_thread(LPVOID lpParam) {
    usbip_server_t *server = (usbip_server_t *)lpParam;
    
    while (server->worker_running && server->running) {
        if (!server->client_connected) {
            Sleep(50);
            continue;
        }

        usbip_op_common_t op_hdr;
        if (recv_exact_rtc(server, &op_hdr, sizeof(op_hdr)) != 0) {
            server->client_connected = false;
            break;
        }

        uint16_t version = ntohs(op_hdr.version);
        uint16_t command = ntohs(op_hdr.command);

        if (version != USBIP_VERSION) {
            break;
        }

        if (command == OP_REQ_DEVLIST) {
            if (handle_op_devlist(server) != 0) break;
        }
        else if (command == OP_REQ_IMPORT) {
            if (handle_op_import(server, &op_hdr) != 0) break;
            break;
        }
        else {
            break;
        }
    }
    return 0;
}

usbip_server_t *usbip_server_create(void) {
    usbip_server_t *server = (usbip_server_t *)calloc(1, sizeof(usbip_server_t));
    if (!server) return NULL;

    server->running = false;
    server->client_connected = false;
    server->rx_buffer = ring_buffer_create(1024 * 1024);
    InitializeCriticalSection(&server->lock);
    
    return server;
}

void usbip_server_destroy(usbip_server_t *server) {
    if (server) {
        usbip_server_stop(server);
        for (int i = 0; i < server->num_devices; ++i) {
            usb_device_destroy(server->devices[i]);
        }
        DeleteCriticalSection(&server->lock);
        ring_buffer_destroy(server->rx_buffer);
        free(server);
    }
}

int usbip_server_register_device(usbip_server_t *server, usb_device_t *dev) {
    if (!server || !dev || server->num_devices >= MAX_DEVICES) return -1;
    server->devices[server->num_devices++] = dev;
    return 0;
}

void usbip_server_stop(usbip_server_t *server) {
    if (server) {
        server->running = false;
        server->client_connected = false;
        server->worker_running = false;
        if (server->worker_thread) {
            WaitForSingleObject(server->worker_thread, 3000);
            CloseHandle(server->worker_thread);
            server->worker_thread = NULL;
        }
    }
}

void usbip_server_on_data_received(usbip_server_t *server, const uint8_t *data, size_t size) {
    if (!server) return;
    EnterCriticalSection(&server->lock);
    ring_buffer_write(server->rx_buffer, data, size);
    LeaveCriticalSection(&server->lock);
}

void usbip_server_on_channel_open(usbip_server_t *server) {
    if (!server) return;
    printf("[USB/IP WebRTC] Data channel opened.\n");
    EnterCriticalSection(&server->lock);
    server->client_connected = true;
    LeaveCriticalSection(&server->lock);

    server->running = true;
    server->worker_running = true;
    server->worker_thread = CreateThread(NULL, 0, web_rtc_worker_thread, server, 0, NULL);
}

void usbip_server_on_channel_closed(usbip_server_t *server) {
    if (!server) return;
    printf("[USB/IP WebRTC] Data channel closed.\n");
    EnterCriticalSection(&server->lock);
    server->client_connected = false;
    LeaveCriticalSection(&server->lock);
    server->worker_running = false;
}
