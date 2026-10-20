#include "usbip_server.h"
#include "usbip_log.h"
#include "usb_device_passthrough.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  #include <process.h>
  typedef int socklen_t;
  #define CLOSE_SOCKET(s) closesocket(s)
#else
  #include <unistd.h>
  #include <pthread.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #define SOCKET int
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  #define CLOSE_SOCKET(s) close(s)
#endif

/* Linux errno-style USB/IP status used when a request exceeds the bounded
 * transfer buffer supported by this server. */
#define USBIP_STATUS_EOVERFLOW (-75)
#define USBIP_MAX_TRANSFER_SIZE 262144u

#ifdef _WIN32
static SRWLOCK g_devices_lock = SRWLOCK_INIT;
static void lock_devices(void) { AcquireSRWLockExclusive(&g_devices_lock); }
static void unlock_devices(void) { ReleaseSRWLockExclusive(&g_devices_lock); }
#else
static void lock_devices(void) { }
static void unlock_devices(void) { }
#endif

typedef struct {
    usbip_server_t *server;
    SOCKET          client_sock;
} client_thread_arg_t;

static int send_exact(SOCKET sock, const void *buf, size_t len) {
    size_t total_sent = 0;
    const char *ptr = (const char *)buf;
    while (total_sent < len) {
        int sent = send(sock, ptr + total_sent, (int)(len - total_sent), 0);
        if (sent <= 0) return -1;
        total_sent += sent;
    }
    return 0;
}

static int recv_exact(SOCKET sock, void *buf, size_t len) {
    size_t total_received = 0;
    char *ptr = (char *)buf;
    while (total_received < len) {
        int received = recv(sock, ptr + total_received, (int)(len - total_received), 0);
        if (received <= 0) return -1;
        total_received += received;
    }
    return 0;
}

static usb_device_t *find_device_by_busid(usbip_server_t *server, const char *busid) {
    for (int i = 0; i < server->num_devices; ++i) {
        if (strcmp(server->devices[i]->busid, busid) == 0) {
            return server->devices[i];
        }
    }
    return NULL;
}

static void sleep_milliseconds(unsigned int milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    usleep(milliseconds * 1000u);
#endif
}

static int send_custom_status(SOCKET sock, uint32_t command, uint32_t status) {
    uint32_t reply[2];
    reply[0] = htonl(command);
    reply[1] = htonl(status);
    return send_exact(sock, reply, sizeof(reply));
}

static bool dynamic_busid_exists(const char *busid) {
    usb_physical_device_info_t physical[USB_PHYSICAL_DEVICE_MAX];
    int count = usb_device_enumerate_physical_devices(physical, USB_PHYSICAL_DEVICE_MAX);
    for (int i = 0; i < count; ++i) {
        if (strcmp(physical[i].busid, busid) == 0) return true;
    }
    return false;
}

/* Locate a session by BUSID while holding the device lock. */
static int find_import_session_slot(usbip_server_t *server, const char *busid) {
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (server->active_import_busids[i][0] &&
            strcmp(server->active_import_busids[i], busid) == 0) return i;
    }
    return -1;
}

static int find_free_import_session_slot(usbip_server_t *server) {
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (!server->active_import_busids[i][0]) return i;
    }
    return -1;
}

/* Release only this import's session record and capture. Never clear another
 * device's socket/session when multiple imports are active. */
static void cleanup_import_session(usbip_server_t *server, usb_device_t *dev, SOCKET sock) {
    char session_busid[SYSFS_BUS_ID_SIZE];
    strncpy(session_busid, dev->busid, sizeof(session_busid) - 1);
    session_busid[sizeof(session_busid) - 1] = '\0';
    bool destroy_device = server->dynamic_devices;
    lock_devices();
    dev->attached = false;
    if (server->dynamic_devices) {
        for (int i = 0; i < server->num_devices; ++i) {
            if (server->devices[i] == dev) {
                for (int j = i + 1; j < server->num_devices; ++j)
                    server->devices[j - 1] = server->devices[j];
                server->devices[--server->num_devices] = NULL;
                break;
            }
        }
    }
    unlock_devices();

    if (destroy_device) {
        usb_device_destroy(dev);
        printf("[USB/IP] Capture for device released after session detach.\n");
    }

    lock_devices();
    int slot = find_import_session_slot(server, session_busid);
    if (slot >= 0 && server->active_import_sockets[slot] == sock) {
        server->active_import_sockets[slot] = INVALID_SOCKET;
        server->active_import_busids[slot][0] = '\0';
    }
    unlock_devices();
}

static uint32_t handle_custom_control(usbip_server_t *server, SOCKET sock,
                                      uint32_t command, const char *busid) {
    if (command == USBIP_CMD_CUSTOM_BIND) {
        bool busy = false;
        lock_devices();
        usb_device_t *current = find_device_by_busid(server, busid);
        if (current && current->attached) busy = true;
        unlock_devices();
        if (busy) return ST_DEV_BUSY;
        if (server->dynamic_devices)
            return dynamic_busid_exists(busid) ? ST_OK : ST_NODEV;
        return current ? ST_OK : ST_NODEV;
    }

    if (command == USBIP_CMD_CUSTOM_DETACH) {
        SOCKET import_socket = INVALID_SOCKET;
        lock_devices();
        int slot = find_import_session_slot(server, busid);
        if (slot >= 0) import_socket = server->active_import_sockets[slot];
        unlock_devices();

        if (slot < 0) {
            /* Idempotent detach: this BUSID has already cleaned up. */
            return ST_OK;
        }

        if (import_socket != INVALID_SOCKET) {
#ifdef _WIN32
            (void)shutdown(import_socket, SD_BOTH);
#else
            (void)shutdown(import_socket, SHUT_RDWR);
#endif
        }

        /* Wait for this BUSID's owning import thread to release its capture. */
        for (unsigned int elapsed = 0; elapsed < 5000; elapsed += 10) {
            bool still_active;
            lock_devices();
            still_active = find_import_session_slot(server, busid) >= 0;
            unlock_devices();
            if (!still_active) return ST_OK;
            sleep_milliseconds(10);
        }
        fprintf(stderr, "[USB/IP] Timed out waiting for BUSID %s capture release.\n", busid);
        return ST_ERROR;
    }

    return ST_ERROR;
}

static int handle_op_devlist(usbip_server_t *server, SOCKET sock) {
    usb_physical_device_info_t physical[USB_PHYSICAL_DEVICE_MAX];
    int physical_count = 0;
    if (server->dynamic_devices) {
        physical_count = usb_device_enumerate_physical_devices(physical, USB_PHYSICAL_DEVICE_MAX);
        /* Don't advertise devices that are currently attached through this server. */
        lock_devices();
        int write_index = 0;
        for (int i = 0; i < physical_count; ++i) {
            bool busy = false;
            for (int j = 0; j < server->num_devices; ++j) {
                if (server->devices[j] && server->devices[j]->attached &&
                    strcmp(server->devices[j]->busid, physical[i].busid) == 0) {
                    busy = true;
                    break;
                }
            }
            if (!busy) physical[write_index++] = physical[i];
        }
        physical_count = write_index;
        /* A captured device may no longer appear under its original Windows
         * hardware ID. Keep cached, detached exports visible in the list. */
        for (int j = 0; j < server->num_devices && physical_count < USB_PHYSICAL_DEVICE_MAX; ++j) {
            usb_device_t *cached = server->devices[j];
            if (!cached || cached->attached) continue;
            bool present = false;
            for (int i = 0; i < physical_count; ++i) {
                if (strcmp(physical[i].busid, cached->busid) == 0) { present = true; break; }
            }
            if (!present) {
                strncpy(physical[physical_count].busid, cached->busid,
                        sizeof(physical[physical_count].busid) - 1);
                physical[physical_count].vid = cached->desc.idVendor;
                physical[physical_count].pid = cached->desc.idProduct;
                ++physical_count;
            }
        }
        unlock_devices();
    }

    int exported_count = server->dynamic_devices ? physical_count : server->num_devices;
    printf("[USB/IP] Handling OP_REQ_DEVLIST (devices count: %d, dynamic=%s)\n",
           exported_count, server->dynamic_devices ? "yes" : "no");

    usbip_op_rep_devlist_hdr_t rep;
    memset(&rep, 0, sizeof(rep));
    rep.version = htons(USBIP_VERSION);
    rep.command = htons(OP_REP_DEVLIST);
    rep.status  = htonl(ST_OK);
    rep.ndev    = htonl((uint32_t)exported_count);
    if (send_exact(sock, &rep, sizeof(rep)) != 0) return -1;

    if (server->dynamic_devices) {
        for (int i = 0; i < physical_count; ++i) {
            usbip_op_device_info_t info;
            memset(&info, 0, sizeof(info));
            snprintf(info.path, sizeof(info.path), "/sys/bus/usb/devices/%s", physical[i].busid);
            strncpy(info.busid, physical[i].busid, sizeof(info.busid) - 1);
            unsigned int bus = 1, port = 1;
            if (sscanf(physical[i].busid, "%u-%u", &bus, &port) != 2) { bus = 1; port = 1; }
            info.busnum = htonl(bus);
            info.devnum = htonl(port);
            info.speed = htonl(USB_SPEED_HIGH); /* Actual speed is reported by the import reply. */
            info.idVendor = htons(physical[i].vid);
            info.idProduct = htons(physical[i].pid);
            /* The standard USB/IP device-list packet has no string fields.
             * Interface details are returned accurately in OP_REP_IMPORT after capture. */
            info.bNumInterfaces = 0;
            if (send_exact(sock, &info, sizeof(info)) != 0) return -1;
        }
        return 0;
    }

    lock_devices();
    for (int i = 0; i < server->num_devices; ++i) {
        usb_device_t *dev = server->devices[i];
        usbip_op_device_info_t dev_info;
        usb_device_fill_op_info(dev, &dev_info);
        if (send_exact(sock, &dev_info, sizeof(dev_info)) != 0) {
            unlock_devices();
            return -1;
        }
        for (int intf = 0; intf < dev_info.bNumInterfaces; ++intf) {
            usbip_op_interface_info_t intf_info;
            usb_device_fill_interface_info(dev, (uint8_t)intf, &intf_info);
            if (send_exact(sock, &intf_info, sizeof(intf_info)) != 0) {
                unlock_devices();
                return -1;
            }
        }
    }
    unlock_devices();
    return 0;
}

static int handle_urb_transfer_loop(usbip_server_t *server, SOCKET sock, usb_device_t *dev) {
    (void)server;
    printf("[USB/IP] Attached to device %s. Entering URB transfer loop.\n", dev->busid);
    uint8_t *payload_in  = (uint8_t *)malloc(USBIP_MAX_TRANSFER_SIZE);
    uint8_t *payload_out = (uint8_t *)malloc(USBIP_MAX_TRANSFER_SIZE);

    if (!payload_in || !payload_out) {
        printf("[USB/IP ERROR] Failed to allocate URB payload buffers.\n");
        free(payload_in);
        free(payload_out);
        dev->attached = false;
        return -1;
    }

    while (server->running) {
        uint32_t cmd_code_net = 0;
        if (recv_exact(sock, &cmd_code_net, sizeof(cmd_code_net)) != 0) {
            printf("[USB/IP] Client disconnected or socket read failed.\n");
            break;
        }
        uint32_t cmd_code = ntohl(cmd_code_net);

        if (cmd_code == USBIP_CMD_SUBMIT) {
            usbip_header_cmd_submit_t cmd;
            cmd.command = cmd_code;
            if (recv_exact(sock, ((char *)&cmd) + sizeof(uint32_t), sizeof(cmd) - sizeof(uint32_t)) != 0) {
                printf("[USB/IP ERROR] Failed to read full USBIP_CMD_SUBMIT header.\n");
                break;
            }

            uint32_t seqnum      = ntohl(cmd.seqnum);
            uint32_t devid       = ntohl(cmd.devid);
            uint32_t direction   = ntohl(cmd.direction);
            uint32_t ep          = ntohl(cmd.ep);
            uint32_t buf_len     = ntohl(cmd.transfer_buffer_length);

            USBIP_LOG(3, "[URB Submit #%u] DevID=0x%08X EP=%u Dir=%s TransferLen=%u\n",
                   seqnum, devid, ep, direction == USBIP_DIR_IN ? "IN" : "OUT", buf_len);

            /* Read OUT payload if sent from client */
            if (direction == USBIP_DIR_OUT && buf_len > 0) {
                uint32_t read_len = (buf_len < USBIP_MAX_TRANSFER_SIZE) ? buf_len : USBIP_MAX_TRANSFER_SIZE;
                if (recv_exact(sock, payload_out, read_len) != 0) {
                    printf("[USB/IP ERROR] Failed to receive OUT payload data.\n");
                    break;
                }
                if (buf_len > read_len) {
                    uint32_t rem = buf_len - read_len;
                    while (rem > 0) {
                        char discard[256];
                        uint32_t chunk = (rem < sizeof(discard)) ? rem : sizeof(discard);
                        if (recv_exact(sock, discard, chunk) != 0) break;
                        rem -= chunk;
                    }
                }
            }

            uint32_t actual_length = 0;
            int status = 0;

            if (buf_len > USBIP_MAX_TRANSFER_SIZE) {
                /* Never pass a wire-provided length larger than our backing
                 * buffers to the device backend. OUT payload has already been
                 * consumed above so the USB/IP stream remains aligned. */
                status = USBIP_STATUS_EOVERFLOW;
                actual_length = 0;
            } else if (ep == 0) {
                /* Control transfer on Endpoint 0 */
                usb_setup_packet_t *setup = (usb_setup_packet_t *)cmd.setup;
                USBIP_LOG(3, "  [EP0 Setup] bmRequestType=0x%02X bRequest=0x%02X wValue=0x%04X wIndex=0x%04X wLength=%u\n",
                       setup->bmRequestType, setup->bRequest, setup->wValue, setup->wIndex, setup->wLength);
                status = dev->handle_control_transfer(dev, setup, payload_out, buf_len,
                                                      payload_in, 262144, &actual_length);

                USBIP_LOG(3, "  [EP0 Result] status=%d actual_len=%u\n", status, actual_length);
            } else {
                /* Data transfer on Bulk / Interrupt endpoint */
                status = dev->handle_data_transfer(dev, ep, direction, payload_out, buf_len,
                                                   payload_in, 262144, &actual_length);

                USBIP_LOG(3, "  [EP%u %s Result] status=%d actual_len=%u\n", ep, direction == USBIP_DIR_IN ? "IN" : "OUT", status, actual_length);
            }

            /* Send USBIP_RET_SUBMIT (Exact 48 bytes) */
            usbip_header_ret_submit_t ret;
            memset(&ret, 0, sizeof(ret));
            ret.command       = htonl(USBIP_RET_SUBMIT);
            ret.seqnum        = htonl(seqnum);
            ret.devid         = htonl(devid);
            ret.direction     = htonl(direction);
            ret.ep            = htonl(ep);
            /* USB/IP uses the signed errno-style URB status returned by the
             * host backend (e.g. -EPIPE for STALL).  Do not collapse it to 1. */
            ret.status        = htonl((uint32_t)status);
            ret.actual_length = htonl(status == 0 ? actual_length : 0);
            ret.start_frame   = htonl(0);
            ret.number_of_packets = htonl(0);
            ret.error_count   = htonl(0);

            if (send_exact(sock, &ret, sizeof(ret)) != 0) {
                printf("[USB/IP ERROR] Failed to send USBIP_RET_SUBMIT header to socket.\n");
                break;
            }

            /* Send IN payload back if requested */
            if (status == 0 && direction == USBIP_DIR_IN && actual_length > 0) {
                if (send_exact(sock, payload_in, actual_length) != 0) {
                    printf("[USB/IP ERROR] Failed to send IN payload to socket.\n");
                    break;
                }
            }
        }
        else if (cmd_code == USBIP_CMD_UNLINK) {
            usbip_header_cmd_unlink_t unlink_cmd;
            unlink_cmd.command = cmd_code;
            if (recv_exact(sock, ((char *)&unlink_cmd) + sizeof(uint32_t), sizeof(unlink_cmd) - sizeof(uint32_t)) != 0) {
                printf("[USB/IP ERROR] Failed to read USBIP_CMD_UNLINK header.\n");
                break;
            }

            USBIP_LOG(2, "  [URB Unlink] seqnum=%u unlink_seqnum=%u\n", ntohl(unlink_cmd.seqnum), ntohl(unlink_cmd.unlink_seqnum));

            usbip_header_ret_unlink_t unlink_ret;
            memset(&unlink_ret, 0, sizeof(unlink_ret));
            unlink_ret.command   = htonl(USBIP_RET_UNLINK);
            unlink_ret.seqnum    = unlink_cmd.seqnum;
            unlink_ret.devid     = unlink_cmd.devid;
            unlink_ret.direction = unlink_cmd.direction;
            unlink_ret.ep        = unlink_cmd.ep;
            unlink_ret.status    = htonl(0);

            if (send_exact(sock, &unlink_ret, sizeof(unlink_ret)) != 0) {
                printf("[USB/IP ERROR] Failed to send USBIP_RET_UNLINK header to socket.\n");
                break;
            }
        }
        else {
            printf("[USB/IP ERROR] Unknown URB command code: 0x%08X\n", cmd_code);
            break;
        }
    }

    free(payload_in);
    free(payload_out);
    printf("[USB/IP] Transfer loop ended for device %s.\n", dev->busid);
    return 0;
}

static int handle_op_import(usbip_server_t *server, SOCKET sock, const usbip_op_common_t *hdr) {
    (void)hdr;
    char busid[SYSFS_BUS_ID_SIZE] = {0};
    if (recv_exact(sock, busid, sizeof(busid)) != 0) return -1;
    busid[sizeof(busid) - 1] = '\0';
    printf("[USB/IP] Received OP_REQ_IMPORT for busid: '%s'\n", busid);

    usbip_op_rep_import_t rep;
    memset(&rep, 0, sizeof(rep));
    rep.version = htons(USBIP_VERSION);
    rep.command = htons(OP_REP_IMPORT);

    lock_devices();
    usb_device_t *dev = find_device_by_busid(server, busid);
    if (!dev && server->dynamic_devices) {
        usb_physical_device_info_t physical[USB_PHYSICAL_DEVICE_MAX];
        int count = usb_device_enumerate_physical_devices(physical, USB_PHYSICAL_DEVICE_MAX);
        uint16_t vid = 0, pid = 0;
        bool found = false;
        for (int i = 0; i < count; ++i) {
            if (strcmp(physical[i].busid, busid) == 0) {
                vid = physical[i].vid;
                pid = physical[i].pid;
                found = true;
                break;
            }
        }
        if (found) {
            printf("[USB/IP] Client selected %s (%04X:%04X); capturing now.\n", busid, vid, pid);
            dev = usb_device_create_passthrough(busid, vid, pid, INVALID_HANDLE_VALUE);
            if (dev) {
                if (server->num_devices < MAX_DEVICES) {
                    server->devices[server->num_devices++] = dev;
                } else {
                    fprintf(stderr, "[USB/IP] Device registry is full; releasing newly captured device.\n");
                    usb_device_destroy(dev);
                    dev = NULL;
                }
            }
        }
    }

    if (!dev) {
        unlock_devices();
        printf("[USB/IP] Device '%s' not found or no longer available.\n", busid);
        rep.status = htonl(ST_NODEV);
        (void)send_exact(sock, &rep, sizeof(rep));
        return -1;
    }
    if (dev->attached) {
        unlock_devices();
        printf("[USB/IP] Device '%s' is already busy/attached.\n", busid);
        rep.status = htonl(ST_DEV_BUSY);
        (void)send_exact(sock, &rep, sizeof(rep));
        return -1;
    }

    /* Reserve it before releasing the lock so simultaneous IMPORT requests
     * cannot both attach the same device. Track the data socket separately
     * from short-lived LIST/BIND/DETACH control connections. */
    int session_slot = find_free_import_session_slot(server);
    if (session_slot < 0) {
        unlock_devices();
        rep.status = htonl(ST_DEV_BUSY);
        (void)send_exact(sock, &rep, sizeof(rep));
        return -1;
    }
    dev->attached = true;
    server->active_import_sockets[session_slot] = sock;
    strncpy(server->active_import_busids[session_slot], busid,
            sizeof(server->active_import_busids[session_slot]) - 1);
    server->active_import_busids[session_slot][sizeof(server->active_import_busids[session_slot]) - 1] = '\0';
    usb_device_fill_op_info(dev, &rep.udev);
    rep.status = htonl(ST_OK);
    unlock_devices();

    if (send_exact(sock, &rep, sizeof(rep)) != 0) {
        cleanup_import_session(server, dev, sock);
        return -1;
    }

    /* The import thread owns cleanup. Explicit DETACH only shuts down this
     * socket and waits for this thread to release the physical capture. */
    int transfer_result = handle_urb_transfer_loop(server, sock, dev);
    cleanup_import_session(server, dev, sock);
    return transfer_result;
}

#ifdef _WIN32
static unsigned __stdcall client_thread_proc(void *arg) {
#else
static void *client_thread_proc(void *arg) {
#endif
    client_thread_arg_t *carg = (client_thread_arg_t *)arg;
    usbip_server_t *server = carg->server;
    SOCKET sock = carg->client_sock;
    free(carg);

    while (server->running) {
        usbip_op_common_t op_hdr;
        uint32_t first_word = 0;
        if (recv_exact(sock, &first_word, sizeof(first_word)) != 0) break;
        uint32_t custom_command = ntohl(first_word);

        if (custom_command == USBIP_CMD_CUSTOM_LIST) {
            printf("[USB/IP] Received CUSTOM_LIST.\n");
            (void)handle_op_devlist(server, sock);
            break; /* LIST uses one request/reply connection. */
        }
        if (custom_command == USBIP_CMD_CUSTOM_BIND ||
            custom_command == USBIP_CMD_CUSTOM_DETACH) {
            char busid[SYSFS_BUS_ID_SIZE] = {0};
            if (recv_exact(sock, busid, sizeof(busid)) != 0) break;
            busid[sizeof(busid) - 1] = '\0';
            printf("[USB/IP] Received %s for BUSID '%s'.\n",
                   custom_command == USBIP_CMD_CUSTOM_BIND ? "CUSTOM_BIND" : "CUSTOM_DETACH",
                   busid);
            uint32_t status = handle_custom_control(server, sock, custom_command, busid);
            if (send_custom_status(sock, custom_command, status) != 0) break;
            printf("[USB/IP] %s BUSID %s: status=%u\n",
                   custom_command == USBIP_CMD_CUSTOM_BIND ? "CUSTOM_BIND" : "CUSTOM_DETACH",
                   busid, status);
            continue;
        }

        /* Otherwise this is the first half of a standard 8-byte USB/IP
         * management header. Reconstruct it and parse normally. */
        memcpy(&op_hdr, &first_word, sizeof(first_word));
        if (recv_exact(sock, ((char *)&op_hdr) + sizeof(first_word),
                       sizeof(op_hdr) - sizeof(first_word)) != 0) break;

        uint16_t version = ntohs(op_hdr.version);
        uint16_t command = ntohs(op_hdr.command);
        if (version != USBIP_VERSION) {
            printf("[USB/IP] Version mismatch: 0x%04x (expected 0x%04x)\n", version, USBIP_VERSION);
            break;
        }

        if (command == OP_REQ_DEVLIST) {
            if (handle_op_devlist(server, sock) != 0) break;
        }
        else if (command == OP_REQ_IMPORT) {
            (void)handle_op_import(server, sock, &op_hdr);
            break; /* Import thread owns socket and capture cleanup. */
        }
        else {
            printf("[USB/IP] Unknown management command: 0x%04x\n", command);
            break;
        }
    }

    CLOSE_SOCKET(sock);
    if (server->active_client_socket == sock)
        server->active_client_socket = INVALID_SOCKET;
#ifdef _WIN32
    if (server->client_done_event)
        SetEvent(server->client_done_event);
    return 0;
#else
    return NULL;
#endif
}

usbip_server_t *usbip_server_create(uint16_t port) {
    usbip_server_t *server = (usbip_server_t *)calloc(1, sizeof(usbip_server_t));
    if (!server) return NULL;

    server->port = (port != 0) ? port : USBIP_DEFAULT_PORT;
    server->running = false;
    server->listen_socket = INVALID_SOCKET;
    server->active_client_socket = INVALID_SOCKET;
    for (int i = 0; i < MAX_DEVICES; ++i) {
        server->active_import_sockets[i] = INVALID_SOCKET;
        server->active_import_busids[i][0] = '\0';
    }
#ifdef _WIN32
    server->client_done_event = CreateEventA(NULL, TRUE, TRUE, NULL);
    if (!server->client_done_event) {
        free(server);
        return NULL;
    }
#endif
    return server;
}

void usbip_server_destroy(usbip_server_t *server) {
    if (server) {
        usbip_server_stop(server);
        for (int i = 0; i < server->num_devices; ++i) {
            usb_device_destroy(server->devices[i]);
        }
#ifdef _WIN32
        if (server->client_done_event)
            CloseHandle(server->client_done_event);
#endif
        free(server);
    }
}

int usbip_server_register_device(usbip_server_t *server, usb_device_t *dev) {
    if (!server || !dev || server->num_devices >= MAX_DEVICES) return -1;
    server->devices[server->num_devices++] = dev;
    return 0;
}

void usbip_server_enable_dynamic_devices(usbip_server_t *server, bool enabled) {
    if (server) server->dynamic_devices = enabled;
}

int usbip_server_run(usbip_server_t *server) {
    if (!server) return -1;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[Error] WSAStartup failed.\n");
        return -1;
    }
#endif

    server->listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->listen_socket == INVALID_SOCKET) {
        fprintf(stderr, "[Error] Failed to create TCP socket.\n");
        return -1;
    }

    int opt = 1;
    setsockopt(server->listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(server->port);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server->listen_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[Error] Failed to bind to port %u.\n", server->port);
        CLOSE_SOCKET(server->listen_socket);
        return -1;
    }

    if (listen(server->listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        fprintf(stderr, "[Error] Failed to listen on socket.\n");
        CLOSE_SOCKET(server->listen_socket);
        return -1;
    }

    server->running = true;
    printf("==================================================\n");
    printf(" USB/IP Daemon (C Implementation) Listening on %u\n", server->port);
    printf(" Exported devices: %d\n", server->num_devices);
    for (int i = 0; i < server->num_devices; ++i) {
        printf("   - BusID: %s (%04x:%04x)\n",
               server->devices[i]->busid,
               server->devices[i]->desc.idVendor,
               server->devices[i]->desc.idProduct);
    }
    printf("==================================================\n");

    while (server->running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        SOCKET client_sock = accept(server->listen_socket, (struct sockaddr *)&client_addr, &addr_len);

        if (client_sock == INVALID_SOCKET) {
            if (!server->running) break;
            continue;
        }

        /* USB/IP sends many small command/response headers. Disable Nagle on
         * the accepted stream and raise socket buffers for bulk transfers. */
        {
            int no_delay = 1;
            int socket_buffer = 1024 * 1024;
            (void)setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY,
                             (const char *)&no_delay, sizeof(no_delay));
            (void)setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF,
                             (const char *)&socket_buffer, sizeof(socket_buffer));
            (void)setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF,
                             (const char *)&socket_buffer, sizeof(socket_buffer));
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("[USB/IP] Incoming connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        client_thread_arg_t *arg = (client_thread_arg_t *)malloc(sizeof(client_thread_arg_t));
        arg->server = server;
        arg->client_sock = client_sock;
        server->active_client_socket = client_sock;

#ifdef _WIN32
        if (server->client_done_event)
            ResetEvent(server->client_done_event);
        uintptr_t th = _beginthreadex(NULL, 0, client_thread_proc, arg, 0, NULL);
        if (th) {
            CloseHandle((HANDLE)th);
        } else {
            fprintf(stderr, "[USB/IP ERROR] Failed to create client thread.\n");
            free(arg);
            CLOSE_SOCKET(client_sock);
            server->active_client_socket = INVALID_SOCKET;
            if (server->client_done_event) SetEvent(server->client_done_event);
        }
#else
        pthread_t th;
        pthread_create(&th, NULL, client_thread_proc, arg);
        pthread_detach(th);
#endif
    }

#ifdef _WIN32
    if (server->client_done_event)
        WaitForSingleObject(server->client_done_event, 5000);
    WSACleanup();
#endif

    return 0;
}

void usbip_server_stop(usbip_server_t *server) {
    if (server && server->running) {
        server->running = false;
        if (server->listen_socket != INVALID_SOCKET) {
            CLOSE_SOCKET(server->listen_socket);
            server->listen_socket = INVALID_SOCKET;
        }
        /* Wake an active USB/IP client thread. The client thread owns the
         * socket close; wait for it before the device is destroyed. */
        if (server->active_client_socket != INVALID_SOCKET) {
#ifdef _WIN32
            shutdown(server->active_client_socket, SD_BOTH);
#else
            shutdown(server->active_client_socket, SHUT_RDWR);
#endif
        }
#ifdef _WIN32
        if (server->client_done_event)
            WaitForSingleObject(server->client_done_event, 5000);
#endif
    }
}
