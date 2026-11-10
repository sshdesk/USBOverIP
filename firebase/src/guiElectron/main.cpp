#include "gui.h"
#include "logsync.h"
#include "usbip_commands.h"
#include "usbipd_commands.h"
#include "signaling.h"
#include "webrtc_usbipd.h"
#include "tcptunnel.h"

#include <iostream>
#include <string>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include "json.hpp"
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

using json = nlohmann::json;

std::atomic<bool> keep_running{true};
std::atomic<bool> is_datachannel_connected{false};
std::atomic<bool> is_usbip_busy{false}; 
std::string active_room_id = ""; 
std::string global_client_id = "";

bool local_usb_triggered = false;
bool remote_usb_triggered = false;
static bool is_session_starting = false;

static UsbIpCommands *g_usbip = nullptr;
static UsbIpdCommands *g_usbipd = nullptr;

std::deque<std::string> g_log_buffer;
std::mutex g_log_mutex;

// Headless GUI shim implementation
GUI g_headless_gui;
GUI* g_active_gui = &g_headless_gui;


static httplib::ws::WebSocket* g_active_ws = nullptr;
static std::mutex g_ws_mutex;

void EmitEvent(const std::string& type, const std::string& payload) {
    json j;
    j["type"] = type;
    j["payload"] = payload;

    std::lock_guard<std::mutex> lock(g_ws_mutex);
    if (g_active_ws) {
        g_active_ws->send(j.dump());
    }
    
    // Fallback/mirror log to console
    SPDLOG_INFO("WSS_EVENT: [{}] {}", type, payload);
}

void GUI::setLabelText(const std::string& text) {
    EmitEvent("STATE", text);
}

void GUI::setInputText(const std::string& text) { 
    EmitEvent("INPUT_TEXT", text); 
}

std::string GUI::getInputText() const 
{ 
    return ""; 
}

void FetchAndDisplayRemoteDevices() {
    std::string temp_file = "usbip_devices_temp.txt";

    // Redirect stdout to temp file to capture usbip list output
    FILE* stream = nullptr;
#if defined(_WIN32)
    freopen_s(&stream, temp_file.c_str(), "w", stdout);
#else
    stream = freopen(temp_file.c_str(), "w", stdout);
#endif

    if (g_usbip) {
        g_usbip->executeCommand({"list", "-r", "127.0.0.1"});
    }
    fflush(stdout);

    // Restore stdout back to standard output
#if defined(_WIN32)
    freopen_s(&stream, "CONOUT$", "w", stdout);
#else
    freopen("/dev/tty", "w", stdout);
#endif

    std::ifstream infile(temp_file);
    if (!infile.is_open()) {
        SPDLOG_ERROR("Failed to open temp file for reading device list.");
        return;
    }

    EmitEvent("CLEAR_REMOTE_DEVICES", "");

    std::string line;
    while (std::getline(infile, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t first_char_idx = line.find_first_not_of(" \t");
        if (first_char_idx == std::string::npos) continue;

        char c = line[first_char_idx];
        if (c == ':' || c == '-' || c == '=') continue;

        size_t first_colon = line.find(':', first_char_idx);
        if (first_colon == std::string::npos) continue;

        std::string busid = line.substr(first_char_idx, first_colon - first_char_idx);
        size_t busid_end = busid.find_last_not_of(" \t");
        if (busid_end != std::string::npos) {
            busid = busid.substr(0, busid_end + 1);
        }

        if (busid.empty() || busid.find(' ') != std::string::npos) continue;

        std::string desc = line.substr(first_colon + 1);
        size_t desc_start = desc.find_first_not_of(" \t");
        if (desc_start != std::string::npos) {
            desc = desc.substr(desc_start);
        } else {
            desc = "Unknown USB Device";
        }

        size_t desc_end = desc.find_last_not_of(" \t\r\n");
        if (desc_end != std::string::npos) {
            desc = desc.substr(0, desc_end + 1);
        }

        EmitEvent("REMOTE_DEVICE", busid + "|" + desc);
        SPDLOG_INFO("[Parsed Device] [{}] {}", busid, desc);
    }
    infile.close();
}

void InitializeTunnelCallbacks() {
    if (!g_usbip || !g_usbip->getTunnelManager()) {
        SPDLOG_ERROR("Cannot initialize callbacks: g_usbip or tunnel_manager is null.");
        return;
    }

    g_usbip->getTunnelManager()->RegisterOnTcpServerStartedCallback([]() {
        SPDLOG_INFO("[Callback] TCP Tunnel Server successfully started and listening.");
        EmitEvent("TCP_SERVER", "listening");
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (is_datachannel_connected) {
                EmitEvent("DATACHANNEL", "open");
                EmitEvent("STATE", "Data channel connected. Querying devices...");
                FetchAndDisplayRemoteDevices();
            }
        }).detach();
    });

    g_usbip->getTunnelManager()->RegisterOnTcpServerStoppedCallback([]() {
        SPDLOG_WARN("[Callback] TCP Tunnel Server has stopped/disconnected.");
        EmitEvent("TCP_SERVER", "stopped");
    });

    g_usbip->getTunnelManager()->RegisterOnTcpClientConnectedCallback([]() {
        SPDLOG_INFO("[Callback] TCP client successfully connected to the tunnel.");
        EmitEvent("TCP_CLIENT", "connected");
    });

    g_usbip->getTunnelManager()->RegisterOnTcpClientDisconnectedCallback([]() {
        SPDLOG_WARN("[Callback] TCP client disconnected from the tunnel.");
        EmitEvent("TCP_CLIENT", "disconnected");
    });

    g_usbip->getTunnelManager()->RegisterOnDataChannelTotalReceivedCallback([](uint64_t total_bytes) {
        SPDLOG_INFO("[Callback] Cumulative data received from WebRTC data channel: {} bytes", total_bytes);
        EmitEvent("DATA_RECEIVED", std::to_string(total_bytes));
    });
}

void HandleCommand(const std::string& cmd) {
    if (cmd.rfind("SET_ROOM:", 0) == 0) {
        active_room_id = cmd.substr(9);
        EmitEvent("TOKEN_ID", active_room_id);
    }
    else if (cmd == "HOST_LOCAL") {
        local_usb_triggered = true;
        if (!g_usbipd) {
            g_usbipd = new UsbIpdCommands();
        }

        EmitEvent("CLEAR_LOCAL_DEVICES", "");
        for (const auto& dev : g_usbipd->listPhysicalDevices()) {
            EmitEvent("LOCAL_DEVICE", dev);
        }

        if (!is_datachannel_connected && !is_session_starting) {
            is_session_starting = true;
            if (!g_usbipd->startHostSession(active_room_id, global_client_id)) {
                is_session_starting = false;
                EmitEvent("ERROR", "Failed to initialize host sharing session");
            } else {
                EmitEvent("STATE", "Hosting local USB devices on room: " + active_room_id);
                EmitEvent("TOKEN_ID", active_room_id);
                EmitEvent("CLIENT_ID", global_client_id);
            }
        }
    }
    else if (cmd == "CONNECT_REMOTE") {
        if (remote_usb_triggered) {
            EmitEvent("ERROR", "Tunnel already active. Please detach first.");
            return;
        }

        remote_usb_triggered = true;
        g_usbip = new UsbIpCommands();

        InitializeTunnelCallbacks();

        EmitEvent("STATE", "Initializing WebRTC tunnel to remote host...");
        int ret = g_usbip->initializeTunnelSession();
        if (ret < 0) {
            EmitEvent("ERROR", "Failed to establish client tunnel");
            remote_usb_triggered = false;
            delete g_usbip;
            g_usbip = nullptr;
        } else {
            EmitEvent("TOKEN_ID", active_room_id);
            EmitEvent("CLIENT_ID", global_client_id);
        }
    }
    else if (cmd == "REFRESH_DEVICES") {
        if (is_datachannel_connected && g_usbip) {
            std::thread([]() {
                FetchAndDisplayRemoteDevices();
            }).detach();
        } else {
            EmitEvent("ERROR", "DataChannel not connected yet.");
        }
    }
    else if (cmd.rfind("ATTACH:", 0) == 0) {
        std::string busid = cmd.substr(7);
        if (is_usbip_busy) {
            EmitEvent("ERROR", "An attach/detach operation is already running.");
            return;
        }

        is_usbip_busy = true;
        EmitEvent("BUSY", "true");
        EmitEvent("STATE", "Attaching device " + busid + "...");

        std::thread([busid]() {
            if (g_usbip) {
                g_usbip->executeCommand({"attach", "-r", "127.0.0.1", "-b", busid});
            }
            is_usbip_busy = false;
            EmitEvent("BUSY", "false");
            EmitEvent("STATE", "Device " + busid + " attached successfully.");
        }).detach();
    }
    else if (cmd == "DETACH") {
        if (is_usbip_busy) {
            EmitEvent("ERROR", "An operation is already in progress.");
            return;
        }

        is_usbip_busy = true;
        EmitEvent("BUSY", "true");
        EmitEvent("STATE", "Detaching all remote devices and closing tunnel...");

        if (remote_usb_triggered && g_usbip) {
            g_usbip->shutdown();
            delete g_usbip;
            g_usbip = nullptr;
            remote_usb_triggered = false;
        }
        is_datachannel_connected = false;
        EmitEvent("DATACHANNEL", "closed");
        EmitEvent("CLEAR_REMOTE_DEVICES", "");
        is_usbip_busy = false;
        EmitEvent("BUSY", "false");
        EmitEvent("STATE", "Detached successfully.");
    }
}

int main(int argc, char* argv[]) {
    rtc::LogMessage::LogToDebug(rtc::LoggingSeverity::LS_ERROR);

    // Stream logs to console stdout
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::default_logger()->sinks().push_back(console_sink);
    spdlog::set_level(spdlog::level::info);

    // Determine port from command line argument if supplied (default to 8080)
    int port = 8080;
    if (argc > 1) {
        try {
            port = std::stoi(argv[1]);
        } catch (...) {
            port = 8080;
        }
    }

    // SSL Certificate paths (ensure cert.pem and key.pem are present)
    const char* cert_path = "cert.pem";
    const char* key_path = "key.pem";

    httplib::SSLServer svr(cert_path, key_path);
    if (!svr.is_valid()) {
        SPDLOG_ERROR("Error: SSL server initialization failed. Check certificate and key paths.");
        return 1;
    }

    svr.WebSocket("/ws", [](const httplib::Request& req, httplib::ws::WebSocket& ws) {
        SPDLOG_INFO("Electron client connected via Secure WebSocket (WSS).");
        {
            std::lock_guard<std::mutex> lock(g_ws_mutex);
            g_active_ws = &ws;
        }

        // Notify ready state
        json ready_event;
        ready_event["type"] = "READY";
        ready_event["payload"] = "Backend operational";
        ws.send(ready_event.dump());

        std::string msg;
        while (ws.read(msg)) {
            if (msg == "EXIT") {
                ws.send("Closing connection.");
                ws.close();
                keep_running = false;
                break;
            }

            SPDLOG_INFO("Received command from Electron: {}", msg);
            HandleCommand(msg);
        }

        {
            std::lock_guard<std::mutex> lock(g_ws_mutex);
            if (g_active_ws == &ws) {
                g_active_ws = nullptr;
            }
        }
        SPDLOG_INFO("Electron client disconnected.");
    });

    std::thread server_thread([&svr, port]() {
        SPDLOG_INFO("Starting C++ WSS server on port {}...", port);
        svr.listen("0.0.0.0", port);
    });

    // Main execution loop waiting for shutdown signal
    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    svr.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    if (remote_usb_triggered && g_usbip) {
        g_usbip->shutdown();
        delete g_usbip;
        g_usbip = nullptr;
    }
    if (local_usb_triggered && g_usbipd) {
        g_usbipd->shutdown();
        delete g_usbipd;
        g_usbipd = nullptr;
    }

    return 0;
}