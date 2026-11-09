
#include "usb_device_passthrough.h"
#include "usbip_log.h"
#include "vbox_usb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <usbiodef.h>
#include <winioctl.h>
#include <usbioctl.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

/*
 * This backend follows the host-side architecture used by usbipd-win:
 *
 *   physical USB device
 *        -> VBoxUSBMon capture filter
 *        -> VBoxUSB.sys
 *        -> user-mode USB/IP server
 *        -> TCP/3240
 *
 * The old implementation in this project incorrectly treated the VBoxUSBMon
 * monitor handle as a per-device VBoxUSB handle and used an obsolete URB
 * structure.  VBoxUSBMon is only used to install/remove capture filters.
 * URBs are submitted to the captured device's VBoxUSB interface.
 */

/*
 * usbiodef.h declares GUID_DEVINTERFACE_USB_DEVICE as an extern GUID.
 * This project is intentionally self-contained and does not define GUIDs
 * through initguid.h, so provide the documented GUID locally.  Keep it
 * mutable because Configuration Manager APIs take LPGUID (non-const).
 */
static GUID GUID_DEVINTERFACE_USB_DEVICE_LOCAL =
    {0xA5DCBF10,0x6530,0x11D2,{0x90,0x1F,0x00,0xC0,0x4F,0xB9,0x51,0xED}};

/* VBoxUSB per-device device class GUID used by usbipd-win.  VBoxUSBMon is
 * only the capture/filter interface; actual URBs use an interface from this
 * class after the device is re-enumerated. */
static GUID GUID_CLASS_VBOXUSB_LOCAL =
    {0x00873FDF,0xCAFE,0x80EE,{0xAA,0x5E,0x00,0xC0,0x4F,0xB1,0x72,0x0B}};

/* USB hub device-interface class. usbipd-win cycles the physical port after
 * switching the binding; merely calling CM_Setup_DevNode is not sufficient. */
static GUID GUID_DEVINTERFACE_USB_HUB_LOCAL =
    {0xF18A0E88,0xC30C,0x11D0,{0x88,0x15,0x00,0xA0,0xC9,0x06,0xBE,0xD8}};

/* Configuration Manager device-interface properties used by usbipd-win's
 * WindowsDevice enumeration.  We deliberately use CM_* rather than guessing
 * a device-interface path. */
static DEVPROPKEY DEVPKEY_Device_InstanceId_LOCAL =
    {{0x78c34fc8,0x104a,0x4aca,{0x9e,0xa4,0x52,0x4d,0x52,0x99,0x6e,0x57}},256};
static DEVPROPKEY DEVPKEY_Device_LocationInfo_LOCAL =
    {{0x4340a6c5,0x93fa,0x4706,{0x97,0x2c,0x7b,0x64,0x80,0x08,0xa5,0xa7}},6};

#define USBIP_STATUS_EPIPE       (-32)
#define USBIP_STATUS_ENODEV      (-19)
#define USBIP_STATUS_EPROTO      (-71)
#define USBIP_STATUS_EOVERFLOW   (-75)
#define USBIP_STATUS_EIO          (-5)
#define USBIP_STATUS_ENOENT      (-2)


typedef struct {
    usb_device_t base;
    HANDLE hDevice;
    HANDLE hIoCompletion;
    HANDLE hMonitor;
    uint64_t filter_id;
    char instance_id[512];
    uint32_t port;
    uint16_t vid;
    uint16_t pid;
    bool captured;
} usb_device_passthrough_t;

typedef struct {
    HDEVINFO set;
    SP_DEVINFO_DATA data;
    char instance_id[512];
    char location[512];
    char service[128];
    char interface_path[1024];
} usb_found_device_t;

static void print_winerr(const char *where) {
    DWORD e = GetLastError();
    char *msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0,
                   (LPSTR)&msg, 0, NULL);
    if (msg) {
        size_t n = strlen(msg);
        while (n && (msg[n-1] == '\r' || msg[n-1] == '\n')) msg[--n] = 0;
    }
    printf("[%s] WinError=%lu%s%s%s\n", where, (unsigned long)e,
           msg ? " (" : "", msg ? msg : "", msg ? ")" : "");
    if (msg) {
        LocalFree(msg);
    }
}

static bool get_dev_property_string(HDEVINFO set, SP_DEVINFO_DATA *data,
                                    DWORD property, char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = 0;
    WCHAR wbuf[1024];
    DWORD type = 0, needed = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, data, property, &type,
                                           (PBYTE)wbuf, sizeof(wbuf), &needed)) {
        return false;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) return false;
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)out_size, NULL, NULL);
    out[out_size - 1] = 0;
    return true;
}

static bool get_instance_id(HDEVINFO set, SP_DEVINFO_DATA *data,
                            char *out, size_t out_size) {
    WCHAR wbuf[1024];
    DWORD needed = 0;
    if (!SetupDiGetDeviceInstanceIdW(set, data, wbuf,
                                     sizeof(wbuf) / sizeof(wbuf[0]), &needed)) {
        return false;
    }
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)out_size, NULL, NULL);
    out[out_size - 1] = 0;
    return true;
}

static bool get_interface_path(HDEVINFO set, SP_DEVICE_INTERFACE_DATA *if_data,
                               char *out, size_t out_size) {
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(set, if_data, NULL, 0, &needed, NULL);
    if (!needed) return false;

    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
        (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)calloc(1, needed);
    if (!detail) return false;
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

    bool ok = false;
    if (SetupDiGetDeviceInterfaceDetailW(set, if_data, detail, needed,
                                         NULL, NULL)) {
        WideCharToMultiByte(CP_UTF8, 0, detail->DevicePath, -1,
                            out, (int)out_size, NULL, NULL);
        out[out_size - 1] = 0;
        ok = true;
    }
    free(detail);
    return ok;
}

static bool parse_busid_port(const char *busid, uint32_t *out_bus, uint32_t *out_port) {
    if (!busid || !out_bus || !out_port) return false;
    unsigned long bus = 0, port = 0;
    if (sscanf(busid, "%lu-%lu", &bus, &port) != 2 || port == 0 || bus > 255 || port > 255)
        return false;
    *out_bus = (uint32_t)bus;
    *out_port = (uint32_t)port;
    return true;
}

static bool location_get_bus_port(const char *location, uint32_t *out_bus, uint32_t *out_port) {
    if (!location || !out_bus || !out_port) return false;
    const char *pp = strstr(location, "Port_#");
    const char *hp = strstr(location, "Hub_#");
    if (!pp || !hp) {
        char upper[512];
        strncpy(upper, location, sizeof(upper) - 1);
        upper[sizeof(upper) - 1] = 0;
        for (char *p = upper; *p; ++p) *p = (char)toupper((unsigned char)*p);
        pp = strstr(upper, "PORT_#");
        hp = strstr(upper, "HUB_#");
        if (!pp || !hp) return false;
        unsigned long port = strtoul(pp + 6, NULL, 10);
        unsigned long bus = strtoul(hp + 5, NULL, 10);
        if (!port || !bus) return false;
        *out_bus = (uint32_t)bus;
        *out_port = (uint32_t)port;
        return true;
    }
    unsigned long port = strtoul(pp + 6, NULL, 10);
    unsigned long bus = strtoul(hp + 5, NULL, 10);
    if (!port || !bus) return false;
    *out_bus = (uint32_t)bus;
    *out_port = (uint32_t)port;
    return true;
}

static bool location_has_bus_port(const char *location, uint32_t bus, uint32_t port) {
    if (!location || !bus || !port) return false;
    char port_needle[32];
    char hub_needle[32];
    snprintf(port_needle, sizeof(port_needle), "Port_#%04lu", (unsigned long)port);
    snprintf(hub_needle, sizeof(hub_needle), "Hub_#%04lu", (unsigned long)bus);
    for (char *p = (char *)location; *p; ++p) *p = (char)toupper((unsigned char)*p);
    for (char *p = port_needle; *p; ++p) *p = (char)toupper((unsigned char)*p);
    for (char *p = hub_needle; *p; ++p) *p = (char)toupper((unsigned char)*p);
    return strstr(location, port_needle) != NULL && strstr(location, hub_needle) != NULL;
}

static bool hardware_id_matches(const char *hardware_id, uint16_t vid, uint16_t pid) {
    if (!hardware_id) return false;
    char needle[64];
    snprintf(needle, sizeof(needle), "VID_%04X&PID_%04X", vid, pid);
    char upper[1024];
    strncpy(upper, hardware_id, sizeof(upper) - 1);
    upper[sizeof(upper) - 1] = 0;
    for (char *p = upper; *p; ++p) *p = (char)toupper((unsigned char)*p);
    return strstr(upper, needle) != NULL;
}

static bool enumerate_usb_device(uint16_t vid, uint16_t pid, uint32_t bus, uint32_t port,
                                 bool require_vbox,
                                 usb_found_device_t *found) {
    memset(found, 0, sizeof(*found));
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE_LOCAL, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        print_winerr("SetupDiGetClassDevs(USB)");
        return false;
    }

    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA if_data;
        memset(&if_data, 0, sizeof(if_data));
        if_data.cbSize = sizeof(if_data);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_USB_DEVICE_LOCAL,
                                         i, &if_data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }

        SP_DEVINFO_DATA data;
        memset(&data, 0, sizeof(data));
        data.cbSize = sizeof(data);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &if_data, NULL, 0, NULL, &data)) {
            /* The first call is expected to return INSUFFICIENT_BUFFER, but it
             * also fills SP_DEVINFO_DATA only in some Windows versions. */
        }
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &if_data, NULL, 0, &needed, &data);
        if (!needed) continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)calloc(1, needed);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &if_data, detail, needed,
                                              NULL, &data)) {
            free(detail);
            continue;
        }

        char hwid[1024] = {0};
        char location[512] = {0};
        char service[128] = {0};
        char instance[512] = {0};
        WideCharToMultiByte(CP_UTF8, 0, detail->DevicePath, -1,
                            found->interface_path, sizeof(found->interface_path), NULL, NULL);
        get_dev_property_string(set, &data, SPDRP_HARDWAREID, hwid, sizeof(hwid));
        get_dev_property_string(set, &data, SPDRP_LOCATION_INFORMATION, location, sizeof(location));
        get_dev_property_string(set, &data, SPDRP_SERVICE, service, sizeof(service));
        get_instance_id(set, &data, instance, sizeof(instance));

        bool vidpid = hardware_id_matches(hwid, vid, pid);
        bool port_match = location_has_bus_port(location, bus, port);
        bool vbox = (_stricmp(service, "VBoxUSB") == 0);

        if (port_match && (require_vbox ? vbox : vidpid)) {
            found->set = set;
            found->data = data;
            strncpy(found->instance_id, instance, sizeof(found->instance_id)-1);
            strncpy(found->location, location, sizeof(found->location)-1);
            strncpy(found->service, "VBoxUSB", sizeof(found->service)-1);
            free(detail);
            return true;
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(set);
    return false;
}

static void close_found(usb_found_device_t *f) {
    if (f && f->set != INVALID_HANDLE_VALUE && f->set != NULL) {
        SetupDiDestroyDeviceInfoList(f->set);
        f->set = NULL;
    }
}

typedef struct {
    DEVINST devinst;
    uint32_t port;
    bool valid;
} vbox_device_restart_t;

static bool open_parent_hub_for_devinst(DEVINST child, HANDLE *out_hub) {
    *out_hub = INVALID_HANDLE_VALUE;
    DEVINST parent = 0;
    CONFIGRET cr = CM_Get_Parent(&parent, child, 0);
    if (cr != CR_SUCCESS) {
        printf("[PnP] CM_Get_Parent failed: CR=%lu\n", (unsigned long)cr);
        return false;
    }

    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_HUB_LOCAL,
                                        NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        print_winerr("SetupDiGetClassDevs(USB_HUB)");
        return false;
    }

    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA if_data;
        memset(&if_data, 0, sizeof(if_data));
        if_data.cbSize = sizeof(if_data);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_USB_HUB_LOCAL,
                                         i, &if_data)) {
            DWORD e = GetLastError();
            if (e == ERROR_NO_MORE_ITEMS) break;
            printf("[PnP] SetupDiEnumDeviceInterfaces(USB_HUB) #%lu failed: %lu\n",
                   (unsigned long)i, (unsigned long)e);
            continue;
        }

        DWORD needed = 0;
        SP_DEVINFO_DATA data;
        memset(&data, 0, sizeof(data));
        data.cbSize = sizeof(data);
        SetupDiGetDeviceInterfaceDetailW(set, &if_data, NULL, 0, &needed, &data);
        if (!needed) continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)calloc(1, needed);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &if_data, detail, needed, NULL, &data)) {
            free(detail);
            continue;
        }

        DEVINST hub_inst = data.DevInst;
        char inst[512] = {0};
        get_instance_id(set, &data, inst, sizeof(inst));
        printf("[PnP] USB hub interface #%lu: DevInst=%lu parent=%lu instance=%s\n",
               (unsigned long)i, (unsigned long)hub_inst, (unsigned long)parent,
               inst[0] ? inst : "<unknown>");

        if (hub_inst == parent) {
            WCHAR path[2048];
            wcsncpy(path, detail->DevicePath, ARRAYSIZE(path) - 1);
            path[ARRAYSIZE(path) - 1] = L'\0';
            HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                print_winerr("open parent USB hub");
                free(detail);
                SetupDiDestroyDeviceInfoList(set);
                return false;
            }
            printf("[PnP] Opened parent USB hub interface: %ls\n", path);
            *out_hub = h;
            free(detail);
            SetupDiDestroyDeviceInfoList(set);
            return true;
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(set);
    printf("[PnP] Could not find USB hub interface for parent DevInst=%lu.\n",
           (unsigned long)parent);
    return false;
}

static bool cycle_parent_usb_port(DEVINST child, uint32_t port) {
    HANDLE hub = INVALID_HANDLE_VALUE;
    if (!open_parent_hub_for_devinst(child, &hub)) return false;

    USB_CYCLE_PORT_PARAMS params;
    memset(&params, 0, sizeof(params));
    params.ConnectionIndex = port;
    DWORD returned = 0;
    /* usbipd-win sends the same USB_CYCLE_PORT_PARAMS buffer as both
     * input and output. Although the WDK documents no output payload, the
     * VBox/usbipd implementation expects the full structure-sized buffer
     * to be supplied on the DeviceIoControl call. Passing NULL/0 here causes
     * ERROR_INSUFFICIENT_BUFFER (122) on this driver stack. */
    BOOL ok = DeviceIoControl(hub, IOCTL_USB_HUB_CYCLE_PORT,
                              &params, sizeof(params),
                              &params, sizeof(params),
                              &returned, NULL);
    if (!ok) {
        print_winerr("IOCTL_USB_HUB_CYCLE_PORT");
        CloseHandle(hub);
        return false;
    }
    printf("[PnP] USB hub port %lu cycled successfully (StatusReturned=0x%08lX).\n",
           (unsigned long)port, (unsigned long)params.StatusReturned);
    CloseHandle(hub);
    return true;
}

static bool get_usb_device_descriptor(DEVINST child, uint32_t port,
                                       USB_DEVICE_DESCRIPTOR *out_desc) {
    if (!out_desc) return false;
    memset(out_desc, 0, sizeof(*out_desc));

    HANDLE hub = INVALID_HANDLE_VALUE;
    if (!open_parent_hub_for_devinst(child, &hub)) return false;

    /* IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION is the documented way for
     * user mode to read a device descriptor through its parent hub.  We do
     * this before removing the device so the capture filter can contain the
     * complete descriptor match used by usbipd-win. */
    const DWORD cb = (DWORD)(sizeof(USB_DESCRIPTOR_REQUEST) + sizeof(USB_DEVICE_DESCRIPTOR));
    USB_DESCRIPTOR_REQUEST *req =
        (USB_DESCRIPTOR_REQUEST *)calloc(1, cb);
    if (!req) {
        CloseHandle(hub);
        return false;
    }
    req->ConnectionIndex = port;
    req->SetupPacket.bmRequest = 0x80; /* device-to-host, standard, device */
    req->SetupPacket.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    req->SetupPacket.wValue = (USHORT)((USB_DEVICE_DESCRIPTOR_TYPE << 8) | 0);
    req->SetupPacket.wIndex = 0;
    req->SetupPacket.wLength = sizeof(USB_DEVICE_DESCRIPTOR);

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(hub,
                              IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
                              req, cb, req, cb, &returned, NULL);
    if (!ok) {
        print_winerr("IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION");
        free(req);
        CloseHandle(hub);
        return false;
    }

    if (returned < sizeof(USB_DESCRIPTOR_REQUEST) + 8) {
        printf("[PnP] USB descriptor response too short: %lu bytes.\n",
               (unsigned long)returned);
        free(req);
        CloseHandle(hub);
        return false;
    }

    memcpy(out_desc, req->Data, sizeof(*out_desc));
    printf("[PnP] Device descriptor: VID=%04X PID=%04X bcdDevice=%04X class=%02X subclass=%02X protocol=%02X.\n",
           out_desc->idVendor, out_desc->idProduct, out_desc->bcdDevice,
           out_desc->bDeviceClass, out_desc->bDeviceSubClass,
           out_desc->bDeviceProtocol);
    free(req);
    CloseHandle(hub);
    return out_desc->idVendor != 0 && out_desc->idProduct != 0;
}

static bool begin_device_restart(const char *instance_id, uint32_t port,
                                 vbox_device_restart_t *r) {
    memset(r, 0, sizeof(*r));
    WCHAR wid[512];
    if (!MultiByteToWideChar(CP_UTF8, 0, instance_id, -1, wid, 512)) return false;
    if (CM_Locate_DevNodeW(&r->devinst, wid, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        printf("[VBoxUSB] CM_Locate_DevNode failed.\n");
        return false;
    }
    PNP_VETO_TYPE veto_type = PNP_VetoTypeUnknown;
    WCHAR veto_name[MAX_PATH] = {0};
    CONFIGRET cr = CM_Query_And_Remove_SubTreeW(
        r->devinst, &veto_type, veto_name, MAX_PATH,
        CM_REMOVE_UI_NOT_OK | CM_REMOVE_NO_RESTART);
    if (cr != CR_SUCCESS) {
        printf("[VBoxUSB] CM_Query_And_Remove_SubTree failed: CR=%lu veto=%lu\n",
               (unsigned long)cr, (unsigned long)veto_type);
        return false;
    }
    r->port = port;
    r->valid = true;
    printf("[PnP] Device removed without restart; DevInst=%lu port=%lu.\n",
           (unsigned long)r->devinst, (unsigned long)r->port);
    return true;
}

static void finish_device_restart(vbox_device_restart_t *r) {
    if (!r || !r->valid) return;
    printf("[PnP] Waiting 100 ms before USB port cycle...\n");
    Sleep(100);
    bool cycled = cycle_parent_usb_port(r->devinst, r->port);
    if (!cycled) {
        printf("[PnP] WARNING: USB port cycle failed; attempting CM_Setup_DevNode anyway.\n");
    }
    /* The old DEVINST is commonly invalid after the physical cycle because
     * PnP has already replaced it with a newly enumerated node.  usbipd-win
     * deliberately ignores this race; the important operation is the port
     * cycle followed by fresh device-interface enumeration. */
    CONFIGRET cr = CM_Setup_DevNode(r->devinst, CM_SETUP_DEVNODE_READY);
    if (cr != CR_SUCCESS) {
        printf("[VBoxUSB] CM_Setup_DevNode returned CR=%lu (ignored; PnP may have replaced the DevNode).\n",
               (unsigned long)cr);
    }
    Sleep(200);
    r->valid = false;
}

static bool monitor_get_version(HANDLE monitor, uint32_t *major, uint32_t *minor) {
    uint32_t out[2] = {0,0};
    DWORD returned = 0;
    if (!DeviceIoControl(monitor, SUPUSBMON_IOCTL_GET_VERSION,
                         NULL, 0, out, sizeof(out), &returned, NULL)) {
        return false;
    }
    *major = out[0]; *minor = out[1];
    return true;
}

static bool monitor_add_filter(HANDLE monitor, const USB_DEVICE_DESCRIPTOR *desc,
                               uint32_t port, uint64_t *filter_id) {
    vbox_usb_filter_t f;
    memset(&f, 0, sizeof(f));
    f.u32Magic = USBMON_MAGIC;
    f.enmType = VBOXUSB_FILTER_TYPE_CAPTURE;
    for (int i = 0; i < 11; ++i) {
        f.aFields[i].enmMatch = VBOXUSB_FILTER_MATCH_IGNORE;
    }
    /* Match the same descriptor fields as usbipd-win/VirtualBox.  Matching
     * only VID/PID/port is accepted by the monitor, but can leave a filter
     * that does not bind to the re-enumerated device on newer VBoxUSB builds. */
    f.aFields[VBOXUSB_FILTER_VENDOR].enmMatch = VBOXUSB_FILTER_MATCH_NUM_EXACT;
    f.aFields[VBOXUSB_FILTER_VENDOR].u16Value = desc->idVendor;
    f.aFields[VBOXUSB_FILTER_PRODUCT].enmMatch = VBOXUSB_FILTER_MATCH_NUM_EXACT;
    f.aFields[VBOXUSB_FILTER_PRODUCT].u16Value = desc->idProduct;
    if (desc->bLength >= 18) {
        f.aFields[VBOXUSB_FILTER_DEVICE_REV].enmMatch = VBOXUSB_FILTER_MATCH_NUM_EXACT;
        f.aFields[VBOXUSB_FILTER_DEVICE_REV].u16Value = desc->bcdDevice;
        f.aFields[VBOXUSB_FILTER_DEVICE_CLASS].enmMatch = VBOXUSB_FILTER_MATCH_NUM_EXACT;
        f.aFields[VBOXUSB_FILTER_DEVICE_CLASS].u16Value = desc->bDeviceClass;
        f.aFields[VBOXUSB_FILTER_DEVICE_SUBCLASS].enmMatch = VBOXUSB_FILTER_MATCH_NUM_EXACT;
        f.aFields[VBOXUSB_FILTER_DEVICE_SUBCLASS].u16Value = desc->bDeviceSubClass;
        f.aFields[VBOXUSB_FILTER_DEVICE_PROTOCOL].enmMatch = VBOXUSB_FILTER_MATCH_NUM_EXACT;
        f.aFields[VBOXUSB_FILTER_DEVICE_PROTOCOL].u16Value = desc->bDeviceProtocol;
    }
    f.aFields[VBOXUSB_FILTER_PORT].enmMatch = VBOXUSB_FILTER_MATCH_NUM_EXACT;
    f.aFields[VBOXUSB_FILTER_PORT].u16Value = (uint16_t)port;

    printf("[VBoxUSBMon] Filter: type=%lu size=%zu VID=%04X PID=%04X REV=%04X class=%02X subclass=%02X protocol=%02X port=%lu\n",
           (unsigned long)f.enmType, sizeof(f), desc->idVendor, desc->idProduct,
           desc->bcdDevice, desc->bDeviceClass, desc->bDeviceSubClass,
           desc->bDeviceProtocol, (unsigned long)port);

    vbox_usb_filter_add_out_t out;
    memset(&out, 0, sizeof(out));
    DWORD returned = 0;
    if (!DeviceIoControl(monitor, SUPUSBMON_IOCTL_ADD_FILTER,
                         &f, sizeof(f), &out, sizeof(out), &returned, NULL)) {
        print_winerr("VBoxUSBMon ADD_FILTER");
        return false;
    }
    if (out.rc < 0) {
        printf("[VBoxUSBMon] ADD_FILTER returned rc=%ld\n", (long)out.rc);
        return false;
    }
    *filter_id = out.uId;
    return true;
}

static bool monitor_remove_filter(HANDLE monitor, uint64_t filter_id) {
    if (!monitor || monitor == INVALID_HANDLE_VALUE || !filter_id) return true;
    DWORD returned = 0;
    int32_t rc = -1;
    if (!DeviceIoControl(monitor, SUPUSBMON_IOCTL_REMOVE_FILTER,
                         &filter_id, sizeof(filter_id), &rc, sizeof(rc),
                         &returned, NULL)) {
        return false;
    }
    return rc == 0;
}

static int map_vbox_error(uint32_t e) {
    switch (e) {
        case VBOXUSB_ERR_OK: return 0;
        case VBOXUSB_ERR_STALL: return USBIP_STATUS_EPIPE;
        case VBOXUSB_ERR_DEVICE_NOT_RESPONDING: return USBIP_STATUS_ENODEV;
        case VBOXUSB_ERR_CRC:
        case VBOXUSB_ERR_NAC: return USBIP_STATUS_EPROTO;
        case VBOXUSB_ERR_UNDERRUN:
        case VBOXUSB_ERR_OVERRUN: return USBIP_STATUS_EOVERFLOW;
        default: return USBIP_STATUS_EIO;
    }
}

/*
 * usbipd-win opens the VBoxUSB interface with FILE_FLAG_OVERLAPPED and
 * submits DeviceIoControl calls through an OVERLAPPED operation. VBoxUSB
 * SEND_URB may complete asynchronously, so the URB's pvData target must
 * remain valid until the operation has completed. We wait here so the rest
 * of the C backend can keep its simple synchronous API.
 */
static bool vbox_device_io_control_sync(usb_device_passthrough_t *pt,
                                        DWORD code,
                                        void *in_buf, DWORD in_len,
                                        void *out_buf, DWORD out_len,
                                        DWORD *returned_out) {
    if (returned_out) *returned_out = 0;

    /* Match usbipd-win's DeviceFile more closely: the VBoxUSB handle is an
     * OVERLAPPED handle bound to an I/O completion port, and synchronous
     * success is explicitly skipped from the completion port.  VBoxUSB
     * SEND_URB is one of the operations for which this distinction matters
     * because the URB contains a native user-space pvData pointer. */
    if (pt->hIoCompletion == NULL || pt->hIoCompletion == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(pt->hDevice, code,
                              in_buf, in_len,
                              out_buf, out_len,
                              &returned, &ov);

    if (ok) {
        if (returned_out) *returned_out = returned;
        return true;
    }

    DWORD e = GetLastError();
    if (e != ERROR_IO_PENDING) {
        SetLastError(e);
        return false;
    }

    DWORD completed_bytes = 0;
    ULONG_PTR completion_key = 0;
    LPOVERLAPPED completed_ov = NULL;

    BOOL got = GetQueuedCompletionStatus(pt->hIoCompletion,
                                          &completed_bytes,
                                          &completion_key,
                                          &completed_ov,
                                          INFINITE);
    (void)completion_key;

    if (!got) {
        DWORD qe = GetLastError();
        SetLastError(qe);
        return false;
    }

    if (completed_ov != &ov) {
        printf("[VBoxUSB] Unexpected IOCP completion packet.\n");
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }

    if (returned_out) *returned_out = completed_bytes;
    return true;
}

static int vbox_ioctl(usb_device_passthrough_t *pt, DWORD code,
                      void *in_buf, DWORD in_len, void *out_buf, DWORD out_len) {
    DWORD returned = 0;
    if (!vbox_device_io_control_sync(pt, code, in_buf, in_len,
                                     out_buf, out_len, &returned)) {
        print_winerr("VBoxUSB DeviceIoControl");
        return USBIP_STATUS_EIO;
    }
    return 0;
}

static int vbox_get_version(usb_device_passthrough_t *pt) {
    uint32_t version[2] = {0,0};
    int rc = vbox_ioctl(pt, SUPUSB_IOCTL_GET_VERSION, NULL, 0,
                        version, sizeof(version));
    if (rc) return rc;
    printf("[VBoxUSB] Device driver version %u.%u\n", version[0], version[1]);
    if (version[0] != VBOXUSB_DRIVER_MAJOR) {
        printf("[VBoxUSB] Unsupported major version %u (expected %u).\n",
               version[0], VBOXUSB_DRIVER_MAJOR);
        return USBIP_STATUS_EIO;
    }
    return 0;
}

static int vbox_claim(usb_device_passthrough_t *pt) {
    vbox_usb_claim_dev_t claim;
    memset(&claim, 0, sizeof(claim));
    int rc = vbox_ioctl(pt, SUPUSB_IOCTL_USB_CLAIM_DEVICE,
                        &claim, sizeof(claim), &claim, sizeof(claim));
    if (rc) return rc;
    if (claim.fClaimed == 0) {
        printf("[VBoxUSB] Device is already claimed.\n");
        return -16; /* -EBUSY */
    }
    return 0;
}

static int vbox_set_config(usb_device_passthrough_t *pt, uint8_t config) {
    return vbox_ioctl(pt, SUPUSB_IOCTL_USB_SET_CONFIG,
                      &config, sizeof(config), NULL, 0);
}

static int vbox_select_interface(usb_device_passthrough_t *pt, uint8_t intf, uint8_t alt) {
    uint8_t in[2] = {intf, alt};
    return vbox_ioctl(pt, SUPUSB_IOCTL_USB_SELECT_INTERFACE,
                      in, sizeof(in), NULL, 0);
}

static int vbox_clear_endpoint(usb_device_passthrough_t *pt, uint8_t endpoint) {
    return vbox_ioctl(pt, SUPUSB_IOCTL_USB_CLEAR_ENDPOINT,
                      &endpoint, sizeof(endpoint), NULL, 0);
}

static int vbox_send_urb(usb_device_passthrough_t *pt, vbox_usb_urb_t *urb) {
    /* Match usbipd-win: serialize USBSUP_URB into a separate byte buffer and
     * use that same buffer as DeviceIoControl input and output. */
    uint8_t bytes[sizeof(*urb)];
    memcpy(bytes, urb, sizeof(bytes));

    USBIP_LOG(3, "[VBoxUSB] SEND_URB: type=%lu ep=%lu dir=%lu flags=0x%08lX cbData=%llu pvData=0x%llX\n",
           (unsigned long)urb->uType,
           (unsigned long)urb->uEndpoint,
           (unsigned long)urb->enmDir,
           (unsigned long)urb->fFlags,
           (unsigned long long)urb->cbData,
           (unsigned long long)urb->pvData);

    DWORD returned = 0;
    if (!vbox_device_io_control_sync(pt, SUPUSB_IOCTL_SEND_URB,
                                     bytes, (DWORD)sizeof(bytes),
                                     bytes, (DWORD)sizeof(bytes),
                                     &returned)) {
        DWORD e = GetLastError();
        printf("[VBoxUSB] SEND_URB failed: WinError=%lu\n", (unsigned long)e);
        return USBIP_STATUS_EIO;
    }

    if (returned != sizeof(bytes)) {
        printf("[VBoxUSB] SEND_URB returned unexpected byte count=%lu (expected=%zu)\n",
               (unsigned long)returned, sizeof(bytes));
        return USBIP_STATUS_EIO;
    }

    memcpy(urb, bytes, sizeof(*urb));

    USBIP_LOG(3, "[VBoxUSB] SEND_URB returned=%lu status=%lu cbData=%llu\n",
           (unsigned long)returned,
           (unsigned long)urb->enmStatus,
           (unsigned long long)urb->cbData);

    return map_vbox_error(urb->enmStatus);
}

static void dump_bytes(const char *label, const uint8_t *data, size_t len) {
    if (usbip_log_level() < 3) return;
    if (!data || !len) {
        printf("[VBoxUSB] %s: <empty>\n", label);
        return;
    }

    size_t dump_len = len > 128 ? 128 : len;
    printf("[VBoxUSB] %s (%zu bytes, dumping %zu):\n", label, len, dump_len);
    for (size_t i = 0; i < dump_len; i += 16) {
        printf("  %04zX: ", i);
        size_t end = i + 16;
        if (end > dump_len) end = dump_len;
        for (size_t j = i; j < end; ++j)
            printf("%02X ", data[j]);
        printf("\n");
    }
}

static int vbox_control(usb_device_passthrough_t *pt,
                        const usb_setup_packet_t *setup,
                        const uint8_t *out_data, uint32_t out_len,
                        uint8_t *in_data, uint32_t max_in_len,
                        uint32_t *actual_in_len) {
    *actual_in_len = 0;

    /* These requests mutate VBoxUSB's pipe table.  usbipd-win serializes
     * them through dedicated IOCTLs instead of racing SEND_URB. */
    if (setup->bmRequestType == 0x00 && setup->bRequest == USB_REQ_SET_CONFIGURATION) {
        int rc = vbox_set_config(pt, (uint8_t)setup->wValue);
        if (rc == 0) pt->base.current_config = (uint8_t)setup->wValue;
        return rc;
    }
    if (setup->bmRequestType == 0x01 && setup->bRequest == USB_REQ_SET_INTERFACE) {
        return vbox_select_interface(pt, (uint8_t)setup->wIndex,
                                     (uint8_t)setup->wValue);
    }
    if (setup->bmRequestType == 0x02 && setup->bRequest == USB_REQ_CLEAR_FEATURE &&
        setup->wValue == 0) {
        return vbox_clear_endpoint(pt, (uint8_t)setup->wIndex);
    }

    uint32_t requested = setup->wLength;
    if (requested > max_in_len) requested = max_in_len;

    /* Match usbipd-win's non-ISO transfer construction. The MESSAGE buffer
     * contains the 8-byte setup packet followed by the payload. */
    uint32_t total = 8 + ((setup->bmRequestType & 0x80) ? requested : out_len);
    uint8_t *buffer = (uint8_t *)calloc(1, total ? total : 8);
    if (!buffer) return USBIP_STATUS_EIO;

    memcpy(buffer, setup, 8);
    if (!(setup->bmRequestType & 0x80) && out_len) {
        memcpy(buffer + 8, out_data, out_len);
    }

    vbox_usb_urb_t urb;
    memset(&urb, 0, sizeof(urb));
    USBIP_LOG(3, "[VBoxUSB] MESSAGE ABI: sizeof(URB)=%zu off(type)=%zu off(ep)=%zu off(dir)=%zu off(flags)=%zu off(status)=%zu off(len)=%zu off(buf)=%zu\n",
           sizeof(urb), offsetof(vbox_usb_urb_t, uType),
           offsetof(vbox_usb_urb_t, uEndpoint), offsetof(vbox_usb_urb_t, enmDir),
           offsetof(vbox_usb_urb_t, fFlags), offsetof(vbox_usb_urb_t, enmStatus),
           offsetof(vbox_usb_urb_t, cbData), offsetof(vbox_usb_urb_t, pvData));

    urb.uType = VBOXUSB_TRANSFER_MESSAGE;
    urb.uEndpoint = 0;

    /* IMPORTANT: MESSAGE is the transfer type, not the transfer direction.
     * usbipd-win sets dir from the USB/IP basic.direction even for EP0
     * MESSAGE requests. GET_DESCRIPTOR therefore uses DIR_IN (1), not
     * DIR_SETUP (0). Using DIR_SETUP makes VBoxUSB accept the URB but does
     * not perform the IN data stage, which leaves the payload unchanged. */
    urb.enmDir = (setup->bmRequestType & 0x80) ? VBOXUSB_DIR_IN : VBOXUSB_DIR_OUT;

    /* Normal USB/IP IN transfers allow short packets unless the client set
     * URB_SHORT_NOT_OK. The current device callback does not expose
     * transfer_flags, so use the normal SHORT_OK behavior for control IN. */
    urb.fFlags = (setup->bmRequestType & 0x80) ? VBOXUSB_XFER_FLAG_SHORT_OK : 0;
    urb.cbData = total;
    urb.pvData = (uint64_t)(uintptr_t)buffer;

    dump_bytes("MESSAGE buffer before SEND_URB", buffer, total);

    int rc = vbox_send_urb(pt, &urb);

    /* MESSAGE transfers contain the setup packet followed by the actual
     * transfer data. */
    if (rc == 0) {
        uint64_t returned_total = urb.cbData;
        if (returned_total > total) returned_total = total;
        dump_bytes("MESSAGE buffer after SEND_URB", buffer, (size_t)returned_total);
    }

    if (rc == 0 && (setup->bmRequestType & 0x80)) {
        uint64_t actual = urb.cbData;
        if (actual >= 8) actual -= 8; else actual = 0;
        if (actual > requested) actual = requested;
        if (actual) memcpy(in_data, buffer + 8, (size_t)actual);
        *actual_in_len = (uint32_t)actual;
        USBIP_LOG(3, "[VBoxUSB] Control IN actual payload=%u bytes\n", *actual_in_len);
    } else if (rc == 0) {
        uint64_t actual = urb.cbData >= 8 ? urb.cbData - 8 : 0;
        if (actual > out_len) actual = out_len;
        *actual_in_len = (uint32_t)actual;
    }
    free(buffer);
    return rc;
}

static int vbox_data(usb_device_passthrough_t *pt, uint32_t ep,
                     uint32_t direction, const uint8_t *out_data, uint32_t out_len,
                     uint8_t *in_data, uint32_t max_in_len,
                     uint32_t *actual_in_len) {
    *actual_in_len = 0;
    /*
     * For an IN transfer, max_in_len is the capacity of our backing buffer,
     * not the length requested by the USB/IP client. VBoxUSB's cbData must
     * contain the actual USB transfer length requested by the client.
     *
     * v22 accidentally used max_in_len (262144) here, so a 36-byte USB/IP
     * request was submitted to VBoxUSB as a 256 KiB transfer.
     */
    uint32_t len = out_len;
    if (direction == USBIP_DIR_IN && len > max_in_len)
        len = max_in_len;

    vbox_usb_urb_t urb;
    memset(&urb, 0, sizeof(urb));
    urb.uType = VBOXUSB_TRANSFER_BULK;
    urb.uEndpoint = ep & 0x0F;
    urb.enmDir = (direction == USBIP_DIR_IN) ? VBOXUSB_DIR_IN : VBOXUSB_DIR_OUT;
    urb.fFlags = (direction == USBIP_DIR_IN) ? VBOXUSB_XFER_FLAG_SHORT_OK : 0;
    urb.cbData = len;
    urb.pvData = (uint64_t)(uintptr_t)((direction == USBIP_DIR_IN) ? in_data : out_data);

    int rc = vbox_send_urb(pt, &urb);
    if (rc == 0) {
        /* VBoxUSB returns the actual number of bytes transferred in cbData. */
        if (urb.cbData > len)
            urb.cbData = len;
        *actual_in_len = (uint32_t)urb.cbData;
    }
    return rc;
}

static bool fetch_device_descriptor(usb_device_passthrough_t *pt) {
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x80;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)(USB_DT_DEVICE << 8);
    setup.wLength = sizeof(pt->base.desc);
    uint32_t actual = 0;
    int rc = vbox_control(pt, &setup, NULL, 0,
                          (uint8_t *)&pt->base.desc, sizeof(pt->base.desc), &actual);
    if (rc || actual < 8) {
        printf("[VBoxUSB] GET_DESCRIPTOR(Device) failed rc=%d actual=%u\n", rc, actual);
        return false;
    }

    printf("[VBoxUSB] Device descriptor bytes: ");
    const uint8_t *d = (const uint8_t *)&pt->base.desc;
    for (size_t i = 0; i < sizeof(pt->base.desc); ++i)
        printf("%02X%s", d[i], ((i + 1) % 16) ? " " : "\n");
    if (sizeof(pt->base.desc) % 16) printf("\n");

    if (pt->base.desc.bLength != 18 || pt->base.desc.bDescriptorType != USB_DT_DEVICE ||
        pt->base.desc.idVendor == 0 || pt->base.desc.idProduct == 0) {
        printf("[VBoxUSB] Invalid device descriptor: bLength=%u type=%u VID=%04X PID=%04X\n",
               pt->base.desc.bLength, pt->base.desc.bDescriptorType,
               pt->base.desc.idVendor, pt->base.desc.idProduct);
        return false;
    }
    return true;
}

static bool fetch_config_descriptor(usb_device_passthrough_t *pt) {
    uint8_t head[9] = {0};
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x80;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)(USB_DT_CONFIG << 8);
    setup.wLength = sizeof(head);
    uint32_t actual = 0;
    if (vbox_control(pt, &setup, NULL, 0, head, sizeof(head), &actual) != 0 || actual < 9)
        return false;

    uint16_t total = (uint16_t)head[2] | ((uint16_t)head[3] << 8);
    if (total < 9 || total > 65535) return false;
    uint8_t *cfg = (uint8_t *)malloc(total);
    if (!cfg) return false;
    setup.wLength = total;
    bool ok = false;
    actual = 0;
    if (vbox_control(pt, &setup, NULL, 0, cfg, total, &actual) == 0 && actual >= 9) {
        pt->base.num_interfaces = 0;
        size_t off = 0;
        while (off + 2 <= actual) {
            uint8_t len = cfg[off];
            uint8_t type = cfg[off + 1];
            if (len < 2 || off + len > actual) break;
            if (type == USB_DT_INTERFACE && len >= 9 &&
                pt->base.num_interfaces < USB_DEVICE_MAX_INTERFACES) {
                usb_device_interface_info_t *ii =
                    &pt->base.interfaces[pt->base.num_interfaces++];
                ii->bInterfaceNumber = cfg[off + 2];
                ii->bAlternateSetting = cfg[off + 3];
                ii->bInterfaceClass = cfg[off + 5];
                ii->bInterfaceSubClass = cfg[off + 6];
                ii->bInterfaceProtocol = cfg[off + 7];
            }
            off += len;
        }
        pt->base.current_config = 0; /* device is unconfigured until Linux sends SET_CONFIGURATION */
        ok = true;
    }
    free(cfg);
    return ok;
}


static bool cm_interface_property_string(const WCHAR *interface_path,
                                          const DEVPROPKEY *key,
                                          char *out, size_t out_size) {
    if (!interface_path || !key || !out || out_size == 0) return false;
    out[0] = 0;
    DEVPROPTYPE type = 0;
    ULONG cb = 0;
    CONFIGRET cr = CM_Get_Device_Interface_PropertyW(interface_path, key,
                                                      &type, NULL, &cb, 0);
    if (cr != CR_BUFFER_SMALL && cr != CR_SUCCESS) return false;
    if (!cb || cb > 64 * 1024) return false;
    BYTE *buf = (BYTE *)calloc(1, cb + sizeof(WCHAR));
    if (!buf) return false;
    cr = CM_Get_Device_Interface_PropertyW(interface_path, key,
                                           &type, buf, &cb, 0);
    bool ok = false;
    if (cr == CR_SUCCESS && type == DEVPROP_TYPE_STRING) {
        WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)buf, -1,
                            out, (int)out_size, NULL, NULL);
        out[out_size - 1] = 0;
        ok = out[0] != 0;
    }
    free(buf);
    return ok;
}

static bool cm_devnode_property_string(DEVINST devinst,
                                       const DEVPROPKEY *key,
                                       char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = 0;
    DEVPROPTYPE type = 0;
    ULONG cb = 0;
    CONFIGRET cr = CM_Get_DevNode_PropertyW(devinst, key, &type, NULL, &cb, 0);
    if (cr != CR_BUFFER_SMALL && cr != CR_SUCCESS) return false;
    if (!cb || cb > 64 * 1024) return false;
    BYTE *buf = (BYTE *)calloc(1, cb + sizeof(WCHAR));
    if (!buf) return false;
    cr = CM_Get_DevNode_PropertyW(devinst, key, &type, buf, &cb, 0);
    bool ok = false;
    if (cr == CR_SUCCESS && type == DEVPROP_TYPE_STRING) {
        WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)buf, -1,
                            out, (int)out_size, NULL, NULL);
        out[out_size - 1] = 0;
        ok = out[0] != 0;
    }
    free(buf);
    return ok;
}

/* Equivalent of usbipd-win's WindowsDevice.GetAll(GUID_CLASS_VBOXUSB)
 * followed by FirstOrDefault(di => di.BusId == busid).  The important detail
 * is that the VBoxUSB interface is enumerated globally; it is not necessarily
 * enumerable as a normal GUID_DEVINTERFACE_USB_DEVICE for the original USB
 * devnode.  Never construct a VBoxUSB path from VID/PID strings. */
static const char *instance_suffix(const char *instance_id) {
    if (!instance_id || !*instance_id) return NULL;
    const char *p = strrchr(instance_id, '\\');
    return p ? p + 1 : instance_id;
}

static bool instance_suffix_matches(const char *captured, const char *original) {
    const char *a = instance_suffix(captured);
    const char *b = instance_suffix(original);
    if (!a || !b || !*a || !*b) return false;
    return _stricmp(a, b) == 0;
}

/* VBoxUSB changes the VID/PID portion of the devnode to 80EE:Cafe, but
 * preserves the device-specific instance suffix.  The VBoxUSB interface
 * also does not expose the physical USB LocationInfo, so matching by
 * BUS-PORT is not reliable after capture.
 *
 * Example:
 *   original: USB\\VID_0951&PID_16A1\\D850E64D135E1060592C00D8
 *   captured: USB\\Vid_80EE&Pid_CAFE\\D850E64D135E1060592C00D8
 *                                      ^^^^^^^^^^^^^^^^^^^^^^^^
 *                                      same physical device
 */
static bool find_vbox_interface_for_instance(const char *original_instance,
                                             usb_found_device_t *found) {
    memset(found, 0, sizeof(*found));

    ULONG chars = 0;
    CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(
        &chars, &GUID_CLASS_VBOXUSB_LOCAL, NULL,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS || chars < 2) {
        printf("[VBoxUSB] CM_Get_Device_Interface_List_Size(global) CR=%lu chars=%lu.\n",
               (unsigned long)cr, (unsigned long)chars);
        return false;
    }

    WCHAR *list = (WCHAR *)calloc(chars, sizeof(WCHAR));
    if (!list) return false;

    cr = CM_Get_Device_Interface_ListW(&GUID_CLASS_VBOXUSB_LOCAL, NULL, list,
                                       chars, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS) {
        printf("[VBoxUSB] CM_Get_Device_Interface_List(global) CR=%lu.\n",
               (unsigned long)cr);
        free(list);
        return false;
    }

    unsigned int count = 0;
    for (WCHAR *q = list; *q; q += wcslen(q) + 1) {
        ++count;

        char path[1024] = {0};
        char instance[512] = {0};
        char location[512] = {0};

        WideCharToMultiByte(CP_UTF8, 0, q, -1, path, sizeof(path), NULL, NULL);

        if (!cm_interface_property_string(q, &DEVPKEY_Device_InstanceId_LOCAL,
                                          instance, sizeof(instance))) {
            printf("[VBoxUSB] interface #%u: cannot get InstanceId: %s\n",
                   count, path);
            continue;
        }

        WCHAR winst[512];
        if (!MultiByteToWideChar(CP_UTF8, 0, instance, -1,
                                 winst, ARRAYSIZE(winst))) {
            continue;
        }

        DEVINST devinst = 0;
        if (CM_Locate_DevNodeW(&devinst, winst, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            continue;

        cm_devnode_property_string(devinst, &DEVPKEY_Device_LocationInfo_LOCAL,
                                   location, sizeof(location));

        printf("[VBoxUSB] interface #%u: instance=%s location=%s path=%s\n",
               count, instance, location[0] ? location : "<none>", path);

        if (!instance_suffix_matches(instance, original_instance))
            continue;

        strncpy(found->interface_path, path,
                sizeof(found->interface_path) - 1);
        strncpy(found->instance_id, instance,
                sizeof(found->instance_id) - 1);
        strncpy(found->location, location,
                sizeof(found->location) - 1);
        strncpy(found->service, "VBoxUSB",
                sizeof(found->service) - 1);

        printf("[VBoxUSB] Matched VBoxUSB interface by instance suffix: %s\n",
               instance_suffix(instance));
        free(list);
        return true;
    }

    printf("[VBoxUSB] No VBoxUSB interface matched original instance: %s\n",
           original_instance ? original_instance : "<none>");
    free(list);
    return false;
}

static bool find_captured_device(const char *original_instance,
                                 usb_found_device_t *found) {
    return find_vbox_interface_for_instance(original_instance, found);
}

static bool open_monitor(HANDLE *out) {
    *out = INVALID_HANDLE_VALUE;
    HANDLE h = CreateFileW(USBMON_DEVICE_NAME,
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        printf("[VBoxUSBMon] CreateFile(%ls) failed: WinError=%lu\n",
               USBMON_DEVICE_NAME, (unsigned long)e);
        if (e == ERROR_FILE_NOT_FOUND) {
            printf("[VBoxUSBMon] VBoxUSBMon.sys is loaded, but its control-device symbolic link is absent.\n");
            printf("[VBoxUSBMon] This is a driver initialization/interface problem, not a USB/IP protocol problem.\n");
        }
        SetLastError(e);
        return false;
    }
    uint32_t major = 0, minor = 0;
    if (!monitor_get_version(h, &major, &minor)) {
        print_winerr("VBoxUSBMon GET_VERSION");
        CloseHandle(h);
        return false;
    }
    printf("[VBoxUSBMon] Driver version %u.%u\n", major, minor);
    if (major != VBOXUSB_DRIVER_MAJOR) {
        printf("[VBoxUSBMon] Unsupported major version; usbipd-win uses version %u.\n",
               VBOXUSB_DRIVER_MAJOR);
        CloseHandle(h);
        return false;
    }
    *out = h;
    return true;
}

static int passthrough_handle_control_transfer(usb_device_t *dev,
                                               const usb_setup_packet_t *setup,
                                               const uint8_t *out_data, uint32_t out_len,
                                               uint8_t *in_data, uint32_t max_in_len,
                                               uint32_t *actual_in_len) {
    usb_device_passthrough_t *pt = (usb_device_passthrough_t *)dev;
    return vbox_control(pt, setup, out_data, out_len, in_data, max_in_len, actual_in_len);
}

static int passthrough_handle_data_transfer(usb_device_t *dev, uint32_t ep,
                                            uint32_t direction,
                                            const uint8_t *out_data, uint32_t out_len,
                                            uint8_t *in_data, uint32_t max_in_len,
                                            uint32_t *actual_in_len) {
    usb_device_passthrough_t *pt = (usb_device_passthrough_t *)dev;
    return vbox_data(pt, ep, direction, out_data, out_len,
                     in_data, max_in_len, actual_in_len);
}

static void passthrough_destroy(usb_device_t *dev) {
    usb_device_passthrough_t *pt = (usb_device_passthrough_t *)dev;
    printf("[VBoxUSB] Releasing device %s\n", pt->base.busid);

    if (pt->hIoCompletion != NULL && pt->hIoCompletion != INVALID_HANDLE_VALUE) {
        CloseHandle(pt->hIoCompletion);
        pt->hIoCompletion = INVALID_HANDLE_VALUE;
    }

    if (pt->hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(pt->hDevice);
        pt->hDevice = INVALID_HANDLE_VALUE;
    }

    /* Mirror usbipd-win's release order: remove the captured devnode first,
     * then remove the capture filter, then cycle/re-enable the port. If the
     * filter is removed after the device has already been re-enumerated, the
     * device can be captured again immediately. */
    vbox_device_restart_t restart;
    bool have_restart = false;
    if (pt->instance_id[0] && pt->captured) {
        have_restart = begin_device_restart(pt->instance_id, pt->port, &restart);
    }

    if (pt->hMonitor != INVALID_HANDLE_VALUE && pt->filter_id) {
        if (!monitor_remove_filter(pt->hMonitor, pt->filter_id)) {
            print_winerr("VBoxUSBMon REMOVE_FILTER");
        }
    }

    if (pt->hMonitor != INVALID_HANDLE_VALUE) {
        CloseHandle(pt->hMonitor);
        pt->hMonitor = INVALID_HANDLE_VALUE;
    }

    if (have_restart) finish_device_restart(&restart);

    free(pt);
}

usb_device_t *usb_device_create_passthrough(const char *busid,
                                            uint16_t vid, uint16_t pid,
                                            HANDLE ignored_driver) {
    (void)ignored_driver;

    uint32_t bus = 0, port = 0;
    if (!parse_busid_port(busid, &bus, &port)) {
        printf("[VBoxUSB] Invalid busid '%s' (expected BUS-PORT).\n", busid ? busid : "(null)");
        return NULL;
    }
    (void)bus;

    usb_found_device_t original;
    if (!enumerate_usb_device(vid, pid, bus, port, false, &original)) {
        printf("[VBoxUSB] Could not locate USB device VID=%04X PID=%04X on port %lu.\n",
               vid, pid, (unsigned long)port);
        return NULL;
    }

    usb_device_passthrough_t *pt =
        (usb_device_passthrough_t *)calloc(1, sizeof(*pt));
    if (!pt) {
        close_found(&original);
        return NULL;
    }
    pt->hDevice = INVALID_HANDLE_VALUE;
    pt->hIoCompletion = INVALID_HANDLE_VALUE;
    pt->hMonitor = INVALID_HANDLE_VALUE;
    pt->vid = vid;
    pt->pid = pid;
    pt->port = port;
    strncpy(pt->instance_id, original.instance_id, sizeof(pt->instance_id)-1);

    printf("[VBoxUSB] Found instance: %s\n", pt->instance_id);
    printf("[VBoxUSB] Location: %s\n", original.location);

    USB_DEVICE_DESCRIPTOR filter_desc;
    bool have_filter_desc = get_usb_device_descriptor(original.data.DevInst, port, &filter_desc);
    if (!have_filter_desc) {
        printf("[VBoxUSB] WARNING: could not read the live USB descriptor; falling back to VID/PID/port filter.\n");
        memset(&filter_desc, 0, sizeof(filter_desc));
        filter_desc.idVendor = vid;
        filter_desc.idProduct = pid;
    }
    char original_instance_for_capture[512] = {0};
    strncpy(original_instance_for_capture, original.instance_id, sizeof(original_instance_for_capture) - 1);
    close_found(&original);

    if (!open_monitor(&pt->hMonitor)) {
        free(pt);
        return NULL;
    }

    /* usbipd-win deliberately begins the PnP restart BEFORE installing the
     * capture filter. VBoxUSBMon only changes the binding while enumeration
     * is in progress. */
    vbox_device_restart_t restart;
    if (!begin_device_restart(pt->instance_id, port, &restart)) {
        printf("[VBoxUSB] Could not begin PnP restart for capture.\n");
        CloseHandle(pt->hMonitor);
        free(pt);
        return NULL;
    }

    if (!monitor_add_filter(pt->hMonitor, &filter_desc, port, &pt->filter_id)) {
        finish_device_restart(&restart);
        CloseHandle(pt->hMonitor);
        free(pt);
        return NULL;
    }
    printf("[VBoxUSBMon] Capture filter installed: id=%llu\n",
           (unsigned long long)pt->filter_id);
    pt->captured = true;

    /* IMPORTANT: usbipd-win finishes the PnP restart immediately after
     * ADD_FILTER.  VBoxUSBMon captures the device during the resulting
     * re-enumeration and rewrites its identity so VBoxUSB.sys can bind.
     * ClaimDevice() then polls for the resulting VBoxUSB interface. */
    finish_device_restart(&restart);
    printf("[VBoxUSB] PnP re-enumeration completed; waiting for VBoxUSB interface.\n");

    usb_found_device_t captured;
    bool found = false;
    for (int attempt = 0; attempt < 100 && !found; ++attempt) {
        Sleep(100);
        found = find_captured_device(original_instance_for_capture, &captured);
    }
    if (!found) {
        printf("[VBoxUSB] VBoxUSB device interface did not appear after capture.\n");
        passthrough_destroy((usb_device_t *)pt);
        return NULL;
    }

    printf("[VBoxUSB] Captured interface: %s\n", captured.interface_path);
    printf("[VBoxUSB] Captured instance: %s\n", captured.instance_id);
    /* The capture stack rewrites the instance ID to the VBoxUSB stub.
     * Track the captured devnode so cleanup can restart that devnode after
     * removing the filter, allowing Windows to reclaim the physical device. */
    strncpy(pt->instance_id, captured.instance_id, sizeof(pt->instance_id)-1);

    WCHAR wpath[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, captured.interface_path, -1, wpath, 2048)) {
        print_winerr("UTF-8 -> UTF-16 VBoxUSB path");
        close_found(&captured);
        passthrough_destroy((usb_device_t *)pt);
        return NULL;
    }

    pt->hDevice = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (pt->hDevice == INVALID_HANDLE_VALUE) {
        print_winerr("open captured VBoxUSB device");
        passthrough_destroy((usb_device_t *)pt);
        return NULL;
    }

    /* Match usbipd-win DeviceFile: successful synchronous completions are
     * skipped from the completion port, while pending IOCTLs arrive through
     * the port. */
    if (!SetFileCompletionNotificationModes(pt->hDevice,
                                            FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)) {
        print_winerr("SetFileCompletionNotificationModes(VBoxUSB)");
        passthrough_destroy((usb_device_t *)pt);
        return NULL;
    }

    pt->hIoCompletion = CreateIoCompletionPort(pt->hDevice, NULL,
                                               (ULONG_PTR)pt, 1);
    if (pt->hIoCompletion == NULL) {
        print_winerr("CreateIoCompletionPort(VBoxUSB)");
        passthrough_destroy((usb_device_t *)pt);
        return NULL;
    }

    pt->captured = true;
    printf("[VBoxUSB] Device handle bound to IO completion port.\n");

    if (vbox_get_version(pt) != 0 || vbox_claim(pt) != 0) {
        passthrough_destroy((usb_device_t *)pt);
        return NULL;
    }

    /* Give VBoxUSB a short settling interval after CLAIM_DEVICE.  This is
     * intentionally outside the URB ABI and does not alter the request. */
    Sleep(100);

    memset(&pt->base, 0, sizeof(pt->base));
    strncpy(pt->base.busid, busid, sizeof(pt->base.busid)-1);
    snprintf(pt->base.path, sizeof(pt->base.path), "/sys/bus/usb/devices/%s", busid);
    pt->base.speed = USB_SPEED_HIGH;
    pt->base.current_config = 0;
    pt->base.destroy = passthrough_destroy;
    pt->base.handle_control_transfer = passthrough_handle_control_transfer;
    pt->base.handle_data_transfer = passthrough_handle_data_transfer;

    if (!fetch_device_descriptor(pt)) {
        passthrough_destroy((usb_device_t *)pt);
        return NULL;
    }
    if (pt->base.desc.idVendor != vid || pt->base.desc.idProduct != pid) {
        printf("[VBoxUSB] Warning: descriptor reports %04X:%04X (requested %04X:%04X).\n",
               pt->base.desc.idVendor, pt->base.desc.idProduct, vid, pid);
    }
    fetch_config_descriptor(pt);
    if (!pt->base.num_interfaces) pt->base.num_interfaces = 1;

    printf("[VBoxUSB] REAL passthrough ready: %04X:%04X busid=%s interfaces=%u\n",
           pt->base.desc.idVendor, pt->base.desc.idProduct,
           pt->base.busid, pt->base.num_interfaces);
    return (usb_device_t *)pt;
}

int usb_device_enumerate_physical_devices(usb_physical_device_info_t *out, int capacity) {
    if (!out || capacity <= 0) return 0;
    int count = 0;
#ifdef _WIN32
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE_LOCAL, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        print_winerr("SetupDiGetClassDevs(enumerate)");
        return 0;
    }
    for (DWORD i = 0; i < 4096 && count < capacity; ++i) {
        SP_DEVICE_INTERFACE_DATA if_data;
        memset(&if_data, 0, sizeof(if_data));
        if_data.cbSize = sizeof(if_data);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_USB_DEVICE_LOCAL, i, &if_data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }
        DWORD needed = 0;
        SP_DEVINFO_DATA data;
        memset(&data, 0, sizeof(data));
        data.cbSize = sizeof(data);
        SetupDiGetDeviceInterfaceDetailW(set, &if_data, NULL, 0, &needed, &data);
        if (!needed) continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)calloc(1, needed);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &if_data, detail, needed, NULL, &data)) {
            free(detail);
            continue;
        }
        char hwid[1024] = {0};
        char location[512] = {0};
        get_dev_property_string(set, &data, SPDRP_HARDWAREID, hwid, sizeof(hwid));
        get_dev_property_string(set, &data, SPDRP_LOCATION_INFORMATION, location, sizeof(location));
        char *vp = strstr(hwid, "VID_");
        char *pp = strstr(hwid, "PID_");
        unsigned int vid = 0, pid = 0;
        uint32_t bus = 0, port = 0;
        bool valid = vp && pp && sscanf(vp + 4, "%4x", &vid) == 1 &&
                     sscanf(pp + 4, "%4x", &pid) == 1 &&
                     location_get_bus_port(location, &bus, &port) && bus > 0 && port > 0;
        if (valid) {
            char busid[SYSFS_BUS_ID_SIZE] = {0};
            snprintf(busid, sizeof(busid), "%lu-%lu", (unsigned long)bus, (unsigned long)port);
            bool duplicate = false;
            for (int j = 0; j < count; ++j) {
                if (strcmp(out[j].busid, busid) == 0) { duplicate = true; break; }
            }
            if (!duplicate) {
                strncpy(out[count].busid, busid, sizeof(out[count].busid) - 1);
                out[count].vid = (uint16_t)vid;
                out[count].pid = (uint16_t)pid;
                ++count;
            }
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(set);
#endif
    return count;
}



#include <vector>
#include <string>
#include <cstdio>
#include <cstring>

std::vector<std::string> usb_device_list_physical_devices(void) {
  std::vector<std::string> device_list;

  printf("====================================================================="
         "=\n");
  printf("  Connected USB Devices (VID/PID)\n");
  printf("====================================================================="
         "=\n");
#ifdef _WIN32
  HDEVINFO set =
      SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE_LOCAL, NULL, NULL,
                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) {
    print_winerr("SetupDiGetClassDevs");
    return device_list;
  }
  int count = 0;
  for (DWORD i = 0;; ++i) {
    SP_DEVICE_INTERFACE_DATA if_data;
    memset(&if_data, 0, sizeof(if_data));
    if_data.cbSize = sizeof(if_data);
    if (!SetupDiEnumDeviceInterfaces(
            set, NULL, &GUID_DEVINTERFACE_USB_DEVICE_LOCAL, i, &if_data)) {
      if (GetLastError() == ERROR_NO_MORE_ITEMS)
        break;
      continue;
    }
    DWORD needed = 0;
    SP_DEVINFO_DATA data;
    memset(&data, 0, sizeof(data));
    data.cbSize = sizeof(data);
    SetupDiGetDeviceInterfaceDetailW(set, &if_data, NULL, 0, &needed, &data);
    if (!needed)
      continue;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
        (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)calloc(1, needed);
    if (!detail)
      continue;
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &if_data, detail, needed, NULL,
                                          &data)) {
      free(detail);
      continue;
    }
    char hwid[1024] = {0}, location[512] = {0}, instance[512] = {0},
         service[128] = {0};
    get_dev_property_string(set, &data, SPDRP_HARDWAREID, hwid, sizeof(hwid));
    get_dev_property_string(set, &data, SPDRP_LOCATION_INFORMATION, location,
                            sizeof(location));
    get_dev_property_string(set, &data, SPDRP_SERVICE, service,
                            sizeof(service));
    get_instance_id(set, &data, instance, sizeof(instance));
    unsigned int v = 0, p = 0;
    char *vp = strstr(hwid, "VID_");
    char *pp = strstr(hwid, "PID_");
    if (vp && pp && sscanf(vp + 4, "%4x", &v) == 1 &&
        sscanf(pp + 4, "%4x", &p) == 1) {
      ++count;
      uint32_t bus = 0, port = 0;
      bool have_busid = location_get_bus_port(location, &bus, &port);

      // All original console printf statements kept completely intact
      printf("  %02d  %04X:%04X  service=%s  location=%s\n", count, v, p,
             service[0] ? service : "(none)",
             location[0] ? location : "(unknown)");
      if (have_busid)
        printf("      BUSID: %lu-%lu\n", (unsigned long)bus,
               (unsigned long)port);
      else
        printf("      BUSID: (not derivable from LocationInfo)\n");
      printf("      instance: %s\n", instance);

      // Populate std::vector<std::string> with VID, PID, and Service / Device
      // Type
      char buf[512];
      const char *service_str = service[0] ? service : "Unknown/Generic";

      if (have_busid) {
        snprintf(buf, sizeof(buf),
                 "[%02d] VID:%04X PID:%04X - Type: %s (BUSID: %lu-%lu)", count,
                 v, p, service_str, (unsigned long)bus, (unsigned long)port);
      } else {
        snprintf(buf, sizeof(buf), "[%02d] VID:%04X PID:%04X - Type: %s", count,
                 v, p, service_str);
      }

      device_list.push_back(std::string(buf));
    }
    free(detail);
  }
  SetupDiDestroyDeviceInfoList(set);
  if (!count) {
    printf("  No USB devices found.\n");
    device_list.push_back("No USB devices found.");
  }
#endif
  printf("====================================================================="
         "=\n");
  return device_list;
}
