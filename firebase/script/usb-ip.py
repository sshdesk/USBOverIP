import socket
import struct
import sys
import ctypes

# Protocol Constants
USBIP_VERSION = 0x0111
OP_REQ_DEVLIST = 0x8005
OP_REP_DEVLIST = 0x0005
OP_REQ_IMPORT  = 0x8003
OP_REP_IMPORT  = 0x0003

ST_OK          = 0x00

USBIP_CMD_SUBMIT = 0x0001
USBIP_RET_SUBMIT = 0x0003
USBIP_CMD_UNLINK = 0x0002
USBIP_RET_UNLINK = 0x0004

USBIP_DIR_OUT = 0
USBIP_DIR_IN  = 1

# Binary Layout Formats
USBIP_OP_COMMON_FMT  = ">HH"
USBIP_OP_REP_HDR_FMT = ">HHII"
USBIP_OP_DEVINFO_FMT = "256s32sIIIHBBBBBB" 
USBIP_OP_INTF_FMT    = ">BBBBB"
USBIP_RET_SUBMIT_FMT = ">IIIIIIiiii"

# Device Target Specifics
TARGET_BUSID = "1-10"
VID = 0x0781
PID = 0x558a

class SetupPacket(ctypes.BigEndianStructure):
    _fields_ = [
        ("bmRequestType", ctypes.c_uint8),
        ("bRequest",      ctypes.c_uint8),
        ("wValue",        ctypes.c_uint16),
        ("wIndex",        ctypes.c_uint16),
        ("wLength",       ctypes.c_uint16),
    ]

# --- Static Binary USB Descriptors for Mass Storage Simulation ---
# Device Descriptor
USB_DEV_DESC = struct.pack(
    "BBHBBBBHHHBBBB",
    18,         # bLength
    0x01,       # bDescriptorType (Device)
    0x0200,     # bcdUSB (USB 2.0)
    0x00,       # bDeviceClass (Defined at Interface level)
    0x00,       # bDeviceSubClass
    0x00,       # bDeviceProtocol
    64,         # bMaxPacketSize0
    VID,        # idVendor
    PID,        # idProduct
    0x0100,     # bcdDevice
    1,          # iManufacturer (String 1)
    2,          # iProduct (String 2)
    3,          # iSerialNumber (String 3)
    1           # bNumConfigurations
)

# Configuration, Interface, and Bulk Endpoints (IN/OUT)
USB_CONFIG_DESC = struct.pack(
    "BBHBBBBB" + "BBBBBBBBB" + "BBHBBBB" + "BBHBBBB",
    # Config Descriptor (9 bytes)
    9, 0x02, 32, 1, 1, 0, 0x80, 250,
    # Interface Descriptor (9 bytes) - Mass Storage (0x08), SCSI (0x06), Bulk-Only (0x50)
    9, 0x04, 0, 0, 2, 0x08, 0x06, 0x50, 0,
    # Bulk Endpoint IN (7 bytes) -> EP 1 IN (0x81)
    7, 0x05, 0x81, 0x02, 512, 0, 0,
    # Bulk Endpoint OUT (7 bytes) -> EP 1 OUT (0x01)
    7, 0x05, 0x01, 0x02, 512, 0, 0
)

# String Descriptors (Index 0: LangID, Index 1: Manuf, Index 2: Product)
USB_STR_LANG = b"\x04\x03\x09\x04" # English US
USB_STR_MANU = b"\x10\x03S\x00a\x00n\x00D\x00i\x00s\x00k\x00" 
USB_STR_PROD = b"\x34\x03U\x00S\x00B\x00 \x00M\x00a\x00s\x00s\x00 \x00S\x00t\x00o\x00r\x00a\x00g\x00e\x00 \x00D\x00e\x00v\x00i\x00c\x00e\x00"

def send_exact(sock, data):
    sock.sendall(data)

def recv_exact(sock, size):
    buf = b""
    while len(buf) < size:
        chunk = sock.recv(size - len(buf))
        if not chunk:
            raise ConnectionError("Socket closed closed unexpectedly.")
        buf += chunk
    return buf

def query_device_list(host, port):
    """Executes OP_REQ_DEVLIST validation handshake."""
    print(f"[*] Querying device list from {host}:{port}...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((host, port))
    
    req = struct.pack(USBIP_OP_COMMON_FMT, USBIP_VERSION, OP_REQ_DEVLIST) + struct.pack(">I", 0)
    send_exact(s, req)
    
    hdr_data = recv_exact(s, struct.calcsize(USBIP_OP_REP_HDR_FMT))
    version, command, status, ndev = struct.unpack(USBIP_OP_REP_HDR_FMT, hdr_data)
    
    if version != USBIP_VERSION or command != OP_REP_DEVLIST or status != ST_OK:
        print(f"[-] Invalid server handshake. Ver: {version:04X}, Cmd: {command:04X}")
        s.close()
        return

    print(f"[+] Server reports {ndev} available device(s).")
    for i in range(ndev):
        dev_data = recv_exact(s, struct.calcsize(USBIP_OP_DEVINFO_FMT))
        unpacked = struct.unpack(USBIP_OP_DEVINFO_FMT, dev_data)
        busid = unpacked[1].split(b'\x00')[0].decode('utf-8')
        vid = unpacked[5]
        pid = unpacked[6]
        num_interfaces = unpacked[13]
        
        print(f"  -> Device [{i}]: BusID='{busid}' (VID:PID = {vid:04x}:{pid:04x})")
        for _ in range(num_interfaces):
            recv_exact(s, struct.calcsize(USBIP_OP_INTF_FMT))
    s.close()

def handle_control_transfer(setup, direction, buf_len):
    """Answers Standard USB setup tokens sent over Endpoint 0."""
    ret_payload = b""
    
    # Standard GET_DESCRIPTOR request (bmRequestType == 0x80, bRequest == 0x06)
    if setup.bmRequestType == 0x80 and setup.bRequest == 0x06:
        desc_type = setup.wValue >> 8
        desc_index = setup.wValue & 0xFF
        
        if desc_type == 0x01: # Device Descriptor
            print("  [EP0] Host requested: DEVICE Descriptor")
            ret_payload = USB_DEV_DESC
        elif desc_type == 0x02: # Configuration Descriptor
            print("  [EP0] Host requested: CONFIGURATION Descriptor")
            ret_payload = USB_CONFIG_DESC
        elif desc_type == 0x03: # String Descriptor
            print(f"  [EP0] Host requested: STRING Descriptor Index {desc_index}")
            if desc_index == 0: ret_payload = USB_STR_LANG
            elif desc_index == 1: ret_payload = USB_STR_MANU
            elif desc_index == 2: ret_payload = USB_STR_PROD
            else: ret_payload = b"\x04\x03\x00\x00"

    # Standard SET_CONFIGURATION request (bmRequestType == 0x00, bRequest == 0x09)
    elif setup.bmRequestType == 0x00 and setup.bRequest == 0x09:
        print(f"  [EP0] Host requested: SET_CONFIGURATION to #{setup.wValue}")
        ret_payload = b"" # Status stage only

    # Mass Storage Specific Request: GET_MAX_LUN (bmRequestType == 0xA1, bRequest == 0xFE)
    elif setup.bmRequestType == 0xA1 and setup.bRequest == 0xFE:
        print("  [EP0 Class] Mass Storage Request: GET_MAX_LUN")
        ret_payload = b"\x00" # Reports 1 Logical Unit (LUN 0)

    # Truncate response to the maximum size expected by the host buffer
    if len(ret_payload) > buf_len:
        ret_payload = ret_payload[:buf_len]
        
    return ret_payload

def attach_and_loop(host, port, target_busid):
    """Main data handling thread mirroring the handle_urb_transfer_loop structure."""
    print(f"[*] Attaching to device {target_busid} on {host}:{port}...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.connect((host, port))
    
    # Step 1: Send Import Frame Header
    req_hdr = struct.pack(USBIP_OP_COMMON_FMT, USBIP_VERSION, OP_REQ_IMPORT) + struct.pack(">I", 0)
    busid_bytes = target_busid.encode('utf-8').ljust(32, b'\x00')
    send_exact(s, req_hdr + busid_bytes)
    
    # Step 2: Parse Import Acknowledgement
    rep_hdr_data = recv_exact(s, struct.calcsize(">HH"))
    version, command = struct.unpack(">HH", rep_hdr_data)
    status = struct.unpack(">I", recv_exact(s, 4))[0]
    
    if status != ST_OK:
        print(f"[-] Server refused connection on BusID '{target_busid}'. Error Status: {status}")
        s.close()
        return

    recv_exact(s, struct.calcsize(USBIP_OP_DEVINFO_FMT))
    print(f"[+] Attached to {target_busid} completely. Processing engine running...")

    try:
        while True:
            cmd_code_data = recv_exact(s, 4)
            cmd_code = struct.unpack(">I", cmd_code_data)[0]
            
            if cmd_code == USBIP_CMD_SUBMIT:
                rem_submit_fmt = ">IIIIII" 
                submit_body = recv_exact(s, struct.calcsize(rem_submit_fmt))
                seqnum, devid, direction, ep, flags, buf_len = struct.unpack(rem_submit_fmt, submit_body)
                
                setup_bytes = recv_exact(s, 8)
                setup_pkt = SetupPacket.from_buffer_copy(setup_bytes)
                
                if direction == USBIP_DIR_OUT and buf_len > 0:
                    recv_exact(s, buf_len) # Absorb raw incoming output payloads
                
                ret_status = 0
                ret_payload = b""
                
                if ep == 0:
                    ret_payload = handle_control_transfer(setup_pkt, direction, buf_len)
                else:
                    # Bulk Endpoint Data Transfers (EP 1 IN / OUT)
                    if direction == USBIP_DIR_IN and buf_len > 0:
                        # Provide clean empty tracking states back to bulk queries if idle
                        ret_payload = b"\x00" * buf_len 
                
                actual_len = len(ret_payload)
                
                # Write back matching USBIP_RET_SUBMIT structure block to server socket stream
                ret_header = struct.pack(
                    USBIP_RET_SUBMIT_FMT,
                    USBIP_RET_SUBMIT, seqnum, devid, direction, ep,
                    ret_status, actual_len, 0, 0, 0
                )
                send_exact(s, ret_header)
                if direction == USBIP_DIR_IN and actual_len > 0:
                    send_exact(s, ret_payload)
                    
            elif cmd_code == USBIP_CMD_UNLINK:
                unlink_body = recv_exact(s, struct.calcsize(">IIII"))
                seqnum, devid, direction, ep = struct.unpack(">IIII", unlink_body)
                unlink_seqnum = struct.unpack(">I", recv_exact(s, 4))[0]
                
                print(f"[-] Unlink requested for transaction #{unlink_seqnum}")
                ret_unlink = struct.pack(">IIIIII", USBIP_RET_UNLINK, seqnum, devid, direction, ep, 0)
                send_exact(s, ret_unlink)
            else:
                print(f"[-] Unknown Command Code encountered: 0x{cmd_code:08X}")
                break
                
    except (ConnectionError, KeyboardInterrupt) as e:
        print(f"\n[*] Client session pipeline teardown completed. Reason: {e}")
    finally:
        s.close()

if __name__ == "__main__":
    # Point this to the machine where your compiled usbip_server daemon is running
    SERVER_IP = "127.0.0.1" 
    SERVER_PORT = 3240
    
    query_device_list(SERVER_IP, SERVER_PORT)
    print("\n" + "="*60)
    attach_and_loop(SERVER_IP, SERVER_PORT, TARGET_BUSID)

