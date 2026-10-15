#include "usbip_server.h"
#include "usb_device_passthrough.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef _WIN32
#include <process.h>
#endif

static usbip_server_t *g_server = NULL;

#ifdef _WIN32
static HANDLE g_force_release_event = NULL;
static HANDLE g_force_release_thread = NULL;
static char g_force_release_event_name[256] = {0};

static unsigned __stdcall force_release_waiter(void *arg) {
    (void)arg;
    if (g_force_release_event &&
        WaitForSingleObject(g_force_release_event, INFINITE) == WAIT_OBJECT_0) {
        printf("\n[USB/IP] FORCE RELEASE requested. Stopping server and releasing USB...\n");
        if (g_server) usbip_server_stop(g_server);
    }
    return 0;
}

static void force_release_cleanup(void) {
    if (g_force_release_thread) {
        WaitForSingleObject(g_force_release_thread, 2000);
        CloseHandle(g_force_release_thread);
        g_force_release_thread = NULL;
    }
    if (g_force_release_event) {
        CloseHandle(g_force_release_event);
        g_force_release_event = NULL;
    }
}

static bool force_release_create_endpoint(const char *busid) {
    _snprintf_s(g_force_release_event_name, sizeof(g_force_release_event_name),
                _TRUNCATE, "Local\\USBIP_C_FORCE_RELEASE_%s", busid);
    g_force_release_event = CreateEventA(NULL, TRUE, FALSE, g_force_release_event_name);
    if (!g_force_release_event) {
        fprintf(stderr, "[USB/IP] Failed to create force-release event (WinError=%lu).\n",
                GetLastError());
        return false;
    }

    uintptr_t th = _beginthreadex(NULL, 0, force_release_waiter, NULL, 0, NULL);
    if (!th) {
        fprintf(stderr, "[USB/IP] Failed to create force-release watcher.\n");
        CloseHandle(g_force_release_event);
        g_force_release_event = NULL;
        return false;
    }
    g_force_release_thread = (HANDLE)th;
    return true;
}

static int force_release_signal(const char *busid) {
    char name[256];
    _snprintf_s(name, sizeof(name), _TRUNCATE,
                "Local\\USBIP_C_FORCE_RELEASE_%s", busid);
    HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    if (!ev) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) {
            fprintf(stderr, "[USB/IP] No running server found for BUSID %s.\n", busid);
        } else {
            fprintf(stderr, "[USB/IP] Could not open force-release endpoint (WinError=%lu).\n", err);
        }
        return 1;
    }
    BOOL ok = SetEvent(ev);
    CloseHandle(ev);
    if (!ok) {
        fprintf(stderr, "[USB/IP] Failed to signal force-release endpoint (WinError=%lu).\n",
                GetLastError());
        return 1;
    }
    printf("[USB/IP] Force-release signal sent for BUSID %s.\n", busid);
    return 0;
}
#endif

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    (void)ctrl_type;
    printf("\n[USB/IP] Termination signal received. Stopping server...\n");
    if (g_server) usbip_server_stop(g_server);
    return TRUE;
}
#else
static void sigint_handler(int sig) {
    (void)sig;
    printf("\n[USB/IP] Termination signal received. Stopping server...\n");
    if (g_server) usbip_server_stop(g_server);
}
#endif

static void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s --list\n", prog);
    printf("      List USB devices visible to Windows.\n\n");
    printf("  %s [port] --passthrough <busid> <vid_hex> <pid_hex>\n", prog);
    printf("      Capture the real USB device through VBoxUSBMon/VBoxUSB.\n");
    printf("      Example: %s 3240 --passthrough 3-2 0951 16A1\n\n", prog);
#ifdef _WIN32
    printf("  %s --force-release <busid>\n", prog);
    printf("      Force the running server to end the USB/IP session, release VBoxUSB, and exit.\n");
    printf("      Example: %s --force-release 3-2\n\n", prog);
#endif
    printf("IMPORTANT: Run elevated. The usbipd-win VBoxUSB/VBoxUSBMon drivers\n");
    printf("must be installed. The Windows filesystem driver must not use the\n");
    printf("device while it is exported.\n");
}

int main(int argc, char *argv[]) {
    uint16_t port = USBIP_DEFAULT_PORT;
    const char *busid = NULL;
    uint16_t vid = 0, pid = 0;
    bool have_passthrough = false;
#ifdef _WIN32
    const char *force_release_busid = NULL;
#endif

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "--list") || !strcmp(argv[i], "-l")) {
            usb_device_list_physical_devices();
            return 0;
        }
#ifdef _WIN32
        if (!strcmp(argv[i], "--force-release")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--force-release requires BUSID\n");
                print_usage(argv[0]);
                return 2;
            }
            force_release_busid = argv[++i];
            continue;
        }
#endif
        if (!strcmp(argv[i], "--passthrough")) {
            if (i + 3 >= argc) {
                fprintf(stderr, "--passthrough requires BUSID VID PID\n");
                print_usage(argv[0]);
                return 2;
            }
            busid = argv[++i];
            vid = (uint16_t)strtoul(argv[++i], NULL, 16);
            pid = (uint16_t)strtoul(argv[++i], NULL, 16);
            have_passthrough = true;
            continue;
        }
        int p = atoi(argv[i]);
        if (p > 0 && p <= 65535) port = (uint16_t)p;
    }

#ifdef _WIN32
    if (force_release_busid) {
        if (have_passthrough) {
            fprintf(stderr, "--force-release cannot be combined with --passthrough.\n");
            return 2;
        }
        return force_release_signal(force_release_busid);
    }
#endif

    if (!have_passthrough) {
        print_usage(argv[0]);
        return 2;
    }

    printf("==================================================\n");
    printf(" USB/IP C Server - REAL Windows USB Passthrough\n");
    printf(" VBoxUSB/VBoxUSBMon compatible architecture\n");
    printf("==================================================\n");
    printf("BusID: %s  VID:PID=%04X:%04X  TCP=%u\n", busid, vid, pid, port);

    g_server = usbip_server_create(port);
    if (!g_server) {
        fprintf(stderr, "Failed to create USB/IP server.\n");
        return 1;
    }

    usb_device_t *dev = usb_device_create_passthrough(busid, vid, pid, INVALID_HANDLE_VALUE);
    if (!dev) {
        fprintf(stderr, "Failed to capture USB device.\n");
        fprintf(stderr, "Check that VBoxUSBMon/VBoxUSB from usbipd-win is installed,\n");
        fprintf(stderr, "the process is Administrator, and the device is not busy.\n");
        usbip_server_destroy(g_server);
        g_server = NULL;
        return 1;
    }

    if (usbip_server_register_device(g_server, dev) != 0) {
        fprintf(stderr, "Failed to register USB device.\n");
        usb_device_destroy(dev);
        usbip_server_destroy(g_server);
        g_server = NULL;
        return 1;
    }

#ifdef _WIN32
    if (!force_release_create_endpoint(busid)) {
        usbip_server_destroy(g_server);
        g_server = NULL;
        return 1;
    }
    printf("[USB/IP] Force-release endpoint ready: %s\n", g_force_release_event_name);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
#endif

    int ret = usbip_server_run(g_server);
#ifdef _WIN32
    force_release_cleanup();
#endif
    usbip_server_destroy(g_server);
    g_server = NULL;
    printf("[USB/IP] Server exited cleanly.\n");
    return ret;
}
