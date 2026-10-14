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
        self.send_counter = 0  # Incremental counter for sending

    def connect(self):
        try:
            print(f"[CONNECTING] Attempting to reach {self.host}:{self.port}...", flush=True)
            self.client_socket.settimeout(self.timeout)
            self.client_socket.connect((self.host, self.port))
            self.client_socket.settimeout(None)
            
            self.is_running = True
            print(f"[CONNECTED] Successfully connected to {self.host}:{self.port}", flush=True)
            
            # --- Send initial incremental number ---
            self.send_next_counter()
            
            receive_thread = threading.Thread(target=self._listen_for_data, daemon=True)
            receive_thread.start()
            
        except socket.timeout:
            print(f"[ERROR] Connection timed out after {self.timeout} seconds.", flush=True)
            sys.exit(1)
        except Exception as e:
            print(f"[ERROR] Connection failed: {e}", flush=True)
            sys.exit(1)

    def _listen_for_data(self):
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
        """Callback executed on incoming data from server."""
        # Assuming the server replies with an 8-byte little/big-endian integer counter
        if len(data) >= 8:
            received_val = int.from_bytes(data[:8], byteorder='little')
            print(f"\n[RECEIVED] Server Counter Value: #{received_val} (Raw Hex: {data.hex()})", flush=True)
        else:
            print(f"\n[RECEIVED] Received {len(data)} bytes: {data.hex()}", flush=True)
        
        # Automatically send the next incremental number back after a short delay
        time.sleep(1.0)
        self.send_next_counter()

    def send_next_counter(self):
        """Increments and sends the counter as an 8-byte binary payload."""
        if not self.is_running:
            return
        try:
            self.send_counter += 1
            # Pack as an 8-byte unsigned integer (little-endian)
            payload = self.send_counter.to_bytes(8, byteorder='little')
            self.client_socket.sendall(payload)
            print(f"[SENT] Client Counter Sent: #{self.send_counter}", flush=True)
        except Exception as e:
            print(f"[ERROR] Failed to send data: {e}", flush=True)
            self.disconnect()

    def disconnect(self):
        self.is_running = False
        try:
            self.client_socket.close()
            print("[CLOSED] Connection closed safely.", flush=True)
        except Exception:
            pass

if __name__ == "__main__":
    SERVER_HOST = "127.0.0.1" 
    SERVER_PORT = 3240

    client = TCPClient(SERVER_HOST, SERVER_PORT, timeout=5.0)
    client.connect()

    try:
        while client.is_running:
            time.sleep(0.1)
    except KeyboardInterrupt:
        print("\n[EXITING] User interrupted.", flush=True)
        client.disconnect()