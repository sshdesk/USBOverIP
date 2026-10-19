# Using usbip-win2 with the custom USB/IP server

The custom `usbipd_cxx_cmake_v27_dynamic_select` server uses the standard USB/IP
management protocol for device discovery and import (`OP_REQ_DEVLIST` followed
by `OP_REQ_IMPORT` with the selected BUSID). This client build adds an optional
interactive selection flow on top of that standard protocol.

## Usage

List devices exposed by the custom server:

```powershell
usbip.exe -t 3240 list -r SERVER_IP
```

Interactively select a device and attach it:

```powershell
usbip.exe -t 3240 attach -r SERVER_IP --select
```

Or attach a known BUSID as before:

```powershell
usbip.exe -t 3240 attach -r SERVER_IP -b 3-2
```

`--select` opens a short-lived connection to retrieve the device list, prompts
for a device number, then passes the selected BUSID to the existing VHCI attach
path. The VHCI attach path opens a new connection and sends the standard
`OP_REQ_IMPORT` request. The custom server captures the physical device only
when that import request arrives.

The dynamic server's DEVLIST response currently reports no interface records;
this client therefore displays BUSID, VID:PID, and device path. The actual device
descriptors are received during IMPORT, as implemented by the server.

Build this source with the project's normal Windows x64 CMake/MSVC build
instructions. This patch has not been validated by a Windows/MSVC build or a
real hardware passthrough test in this environment.
