#include "gui.h"
#include "logsync.h"
#include "usbip_commands.h"
#include "usbipd_commands.h"
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

#if defined(_WIN32)
#include <io.h>
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#else
#include <unistd.h>
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#endif

#include "signaling.h"
#include "webrtc_usbipd.h"
#include "tcptunnel.h"

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

void EmitEvent(const std::string& type, const std::string& payload) {
    std::cout << "IPC_EVENT:[" << type << "] " << payload << std::endl;
    std::cout.flush();
}

void GUI::setLabelText(const std::string& text) {
    EmitEvent("STATE", text);
}



void GUI::setInputText(const std::string& text) { 
    EmitEvent("STATE", text); 
}

std::string GUI::getInputText() const 
{ 
    return ""; 
}


void FetchAndDisplayRemoteDevices() {
    std::string temp_file = "usbip_devices_temp.txt";

    // Safely redirect stdout using descriptor duplication
    int original_stdout = DUP(_fileno(stdout));
    FILE *fp = nullptr;
#if defined(_WIN32)
    freopen_s(&fp, temp_file.c_str(), "w", stdout);
#else
    fp = freopen(temp_file.c_str(), "w", stdout);
#endif

    if (g_usbip) {
        g_usbip->executeCommand({"list", "-r", "127.0.0.1"});
    }
    fflush(stdout);

    // Restore stdout back to the parent Node.js pipe
    DUP2(original_stdout, _fileno(stdout));
    CLOSE(original_stdout);

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
}

int main(int argc, char* argv[]) {
    rtc::LogMessage::LogToDebug(rtc::LoggingSeverity::LS_ERROR);

    // Stream logs to console stdout so Electron captures them
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::default_logger()->sinks().push_back(console_sink);
    spdlog::set_level(spdlog::level::info);

    EmitEvent("READY", "Backend operational");

    std::string line;
    while (keep_running && std::getline(std::cin, line)) {
        if (line.empty()) continue;

        if (line.rfind("SET_ROOM:", 0) == 0) {
            active_room_id = line.substr(9);
            EmitEvent("TOKEN_ID", active_room_id);
        }
        else if (line == "HOST_LOCAL") {
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
        else if (line == "CONNECT_REMOTE") {
            if (remote_usb_triggered) {
                EmitEvent("ERROR", "Tunnel already active. Please detach first.");
                continue;
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
        else if (line == "REFRESH_DEVICES") {
            if (is_datachannel_connected && g_usbip) {
                std::thread([]() {
                    FetchAndDisplayRemoteDevices();
                }).detach();
            } else {
                EmitEvent("ERROR", "DataChannel not connected yet.");
            }
        }
        else if (line.rfind("ATTACH:", 0) == 0) {
            std::string busid = line.substr(7);
            if (is_usbip_busy) {
                EmitEvent("ERROR", "An attach/detach operation is already running.");
                continue;
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
        else if (line == "DETACH") {
            if (is_usbip_busy) {
                EmitEvent("ERROR", "An operation is already in progress.");
                continue;
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
        else if (line == "EXIT") {
            keep_running = false;
            break;
        }
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
