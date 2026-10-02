# shared-usb

shared-usb/

├── firebase/          # websocket with libwebrtc

├── usbip/             # Core Windows USB/IP utilities, scripts, and client wrappers

├── usbipd/            # Windows USB/IP daemon components and host tools

└── usbipdTest/        # Testing suite, automation scripts, and validation sandboxes






 4-Week Plan: WebRTC-Based USBIP Forwarding

This document outlines a 4-week task list to bridge a local USB device at a laptop (Client) to an AWS Cloud EC2 instance (Server) via `usbipd-win2`, a custom client wrapper (`usbip.exe`), and `libwebrtc` Data Channels. The project utilizes `conan` for dependency management and `spdlog` for structured logging.

---

## Architecture Blueprint

```
+-------------------------------------------------------------+               +-------------------------------------------------------+
|                       LOCAL LAPTOP                          |               |                       AWS CLOUD                       |
|                                                             |               |                                                       |
|  +------------------+     +------------+     +-----------+  |  WebRTC Data  |  +-----------+     +------------+     +------------+  |
|  | Physical USB     | --> | usbipd-win | --> |usbipd.exe |  |  Channel      |  | usbipd    | <-- | Custom     | <-- | Client     |  |
|  | Plug & Play Device|    | backend    |     | (Client)  |  =============== |  | Server    |     | AWS Daemon |     | App / App  |  |
|  +------------------+     +------------+     +-----------+  | (STUN/TURN)   |  +-----------+     +------------+     +------------+  |
+-------------------------------------------------------------+               +-------------------------------------------------------+
```

**Salient Feature**

* This will be end to end solution, just plugin usb at laptop and access at aws clound. 
* Customer can check USB status at static webpage and in worst case can configure USB from satic webpage itself.

## Week 1: deliver Conan build Environment

### Goal
Establish the cross-platform Conan build pipelines, integrate structured logging, and try open source code `usbipd-win2` and `usbip.exe`

*   **Task 1.1: Conan Recipe Configuration**
    *   Create a unified `conanfile.py`  and conan text file for building usbipd-win, usbip.exe and libwebrtc
    *   Target packaging: `libwebrtc and spdlog
    *   Configure dual profiles:  At Laptop side both Linux and widows. At AWS Coud side only windos for now
*   **Task 1.2: Compile exist open soure with Conan**
    *   Create fodler stucture like (`/src/client`, `/src/server`, `/src/common`).
  
*   **Task 1.3: Local Loopback USBIP Validation**
    *   Install `usbip-win` on the Windows laptop.
    *   Local build test with open source code. Manually attach, and share a physical USB device over local host interfaces (`127.0.0.1`) to ensure hardware compatibility and driver baseline stability.

---

## Week 2: WebRTC Integration & Signaling Plane Establishment

### Goal
Establish a resilient WebRTC PeerConnection peer-to-peer  between the laptop and the AWS instance capable of traversing symmetric NATs.

*   **Task 2.1: Signaling Channel Protocol Design**
    *   Firebase signalling to exchange Session Description Protocol (SDP) offers, answers, and ICE candidates.
*   **Task 2.2: PeerConnection & Data Channel**
    *   Implement `libwebrtc` connection states (`IceConnectionState`, `PeerConnectionState`) within both `usbip.exe` (client) and    `usbipd.exe`(sever).
*   **Task 2.3: STUN/TURN Network Traversal Integration**
    *  open and free stun severs only

---

## Week 3: USBIP Protocol Multiplexing over Data Channel

### Goal
Intercept raw USBIP network traffic from local abstraction layers and encapsulate it into WebRTC Data Channel frames for remote consumption.

*   **Task 3.1: `usbip.exe` USBIP Traffic Hijacking**
    *   Program `usbip.exe` to attach locally to `usbipd-win` via local scptp datachanel
    *   Read incoming USBIP packets (OP_REQ_IMPORT, USBIP_CMD_SUBMIT, etc.) out of the socket and stream them sequentially as binary payloads directly into the WebRTC Data Channel.
*   **Task 3.2:Device Driver Installation**
    *   Driver installtion and feed stream into a local vhci-hcd` 
*   **Task 3.3: Async Frame**
    *   Optimize datachannel transmission with non-blocking I/O routines 

---

## Week 4: Automated Plug & Play (P&P) Workflows, Hardening & E2E Validation

### Goal
Automate operational lifecycles to seamlessly catch hardware events, gracefully recover from network disconnects, and ensure continuous operation.

*   **Task 4.1: Windows Hardware Event Detection**
    *   Implement a Windows Management Instrumentation (WMI) or native `WM_DEVICECHANGE` message loop observer
    *   Trigger automated background execution commands (`usbipd bind` and `usbipd attach`) the moment a targeted Vendor ID/Product ID (VID/PID) is inserted.
*   **Task 4.2: Connection Heartbeats & Auto-Reconnection**
    *  Automatic Disconnecting/reconnecting physical USB device updates the AWS system automatically without service restarts. | Unplug hardware, replug, and confirm live data stream recovery

---

## Demo every week with Milestone completion

