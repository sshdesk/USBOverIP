import socket
import threading
import sys
import time

class TCPClient:
    def __init__(self, host: str, port: int, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.is_running = False

    def connect(self):
        """Establishes connection with a timeout, sends initial data, and listens."""
        try:
            print(f"[CONNECTING] Attempting to reach {self.host}:{self.port}...", flush=True)
            
            # Set a timeout so the script doesn't hang forever if the server is down
            self.client_socket.settimeout(self.timeout)
            self.client_socket.connect((self.host, self.port))
            
            # Remove timeout for standard blocking operations after a successful connection
            self.client_socket.settimeout(None)
            
            self.is_running = True
            print(f"[CONNECTED] Successfully connected to {self.host}:{self.port}", flush=True)
            
            # --- 1. SEND DATA IMMEDIATELY ON CONNECT ---
            initial_payload = b'\x00\x01\x02\x03'
            print("[CONNECT EVENT] Sending initial greeting payload...", flush=True)
            self.send_binary(initial_payload)
            
            # Start background listener for subsequent data
            receive_thread = threading.Thread(target=self._listen_for_data, daemon=True)
            receive_thread.start()
            
        except socket.timeout:
            print(f"[ERROR] Connection timed out after {self.timeout} seconds. Is the server running?", flush=True)
            sys.exit(1)
        except Exception as e:
            print(f"[ERROR] Connection failed: {e}", flush=True)
            sys.exit(1)

    def _listen_for_data(self):
        """Internal loop that waits for server data."""
        while self.is_running:
            try:
                data = self.client_socket.recv(4096)
                if not data:
                    print("[DISCONNECTED] Server closed the connection cleanly.", flush=True)
                    self.disconnect()
                    break
                
                self.on_receive(data)
                
            except Exception as e:
                if self.is_running:
                    print(f"[ERROR] Error receiving data: {e}", flush=True)
                break

    def on_receive(self, data: bytes):
        """Callback executed on incoming data."""
        print(f"\n[RECEIVED] Received {len(data)} bytes from server.", flush=True)
        print(f"[RECEIVED] Hex: {data.hex()}", flush=True)
        
        # --- 2. SEND DATA ON RECEIVE ---
        response_payload = b'\xAA\xBB\xCC\xDD'
        print("[RECEIVE EVENT] Triggering automatic binary reply...", flush=True)
        self.send_binary(response_payload)

    def send_binary(self, binary_data: bytes):
        """Sends raw binary data to the server."""
        if not self.is_running:
            return
        try:
            self.client_socket.sendall(binary_data)
            print(f"[SENT] Sent {len(binary_data)} bytes to server.", flush=True)
        except Exception as e:
            print(f"[ERROR] Failed to send data: {e}", flush=True)
            self.disconnect()

    def disconnect(self):
        """Closes connection cleanly."""
        self.is_running = False
        try:
            self.client_socket.close()
            print("[CLOSED] Connection closed safely.", flush=True)
        except Exception:
            pass

if __name__ == "__main__":
    # Target Configuration
    SERVER_HOST = "127.0.0.1" 
    SERVER_PORT = 3240

    client = TCPClient(SERVER_HOST, SERVER_PORT, timeout=5.0)
    client.connect()

    # CPU-Friendly main thread loop
    try:
        while client.is_running:
            time.sleep(0.1)  # Free up CPU cycles while waiting
    except KeyboardInterrupt:
        print("\n[EXITING] User interrupted.", flush=True)
        client.disconnect()
