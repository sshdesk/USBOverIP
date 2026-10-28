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
#include <spdlog/spdlog.h>

#include "signaling.h"
#include "../webrtc_usbipd.h"
#include "../tcptunnel.h"

std::atomic<bool> keep_running{true};
std::atomic<bool> is_datachannel_connected{false};
std::string active_room_id = ""; 
std::string global_client_id = "";

bool local_usb_triggered = false;
bool remote_usb_triggered = false;
bool attach_triggered = false;
static bool is_session_starting = false;

// Subsystem Controller Instances
static UsbIpCommands *g_usbip = nullptr;
static UsbIpdCommands *g_usbipd = nullptr;

// Reads remote devices via usbip and populates the GUI list
void FetchAndDisplayRemoteDevices(GUI &gui) {
    std::string temp_file = "usbip_devices_temp.txt";
    FILE *fp = nullptr;
    if (freopen_s(&fp, temp_file.c_str(), "w", stdout) != 0) {
        SPDLOG_ERROR("Failed to redirect stdout for device listing.");
        return;
    }

    if (g_usbip) {
        g_usbip->executeCommand({"usbip", "list", "-r", "127.0.0.1"});
    }

    fflush(stdout);
    if (fp) {
        fclose(fp);
    }

    // Reopen stdout back to console output on Windows to prevent invalidating stdout
    freopen_s(&fp, "CONOUT$", "w", stdout);

    std::ifstream infile(temp_file);
    std::string line;
    gui.clearListItems();
    gui.addListItem("--- Select a Device to Attach ---");

    std::string current_busid = "";
    std::string current_desc = "";

    while (std::getline(infile, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t colon_pos = line.find(" : ");
        if (colon_pos != std::string::npos &&
            line.find("/sys/") == std::string::npos &&
            line.find("Type:") == std::string::npos) {

            if (!current_busid.empty()) {
                gui.addListItem("[" + current_busid + "] " + current_desc);
            }
            std::stringstream ss(line);
            ss >> current_busid;
            current_desc = line.substr(colon_pos + 3);
        }
    }

    if (!current_busid.empty()) {
        gui.addListItem("[" + current_busid + "] " + current_desc);
    }
    infile.close();
}

// Register tunnel callbacks once globally to prevent callback stacking
void InitializeTunnelCallbacks() {
    if (!g_usbip || !g_usbip->getTunnelManager()) {
        SPDLOG_ERROR("Cannot initialize callbacks: g_usbip or tunnel_manager is null.");
        return;
    }

    g_usbip->getTunnelManager()->RegisterOnTcpServerStartedCallback([]() {
        SPDLOG_INFO("[Callback] TCP Tunnel Server successfully started and listening.");
        if (g_active_gui) {
            FetchAndDisplayRemoteDevices(*g_active_gui);
        }
    });

    g_usbip->getTunnelManager()->RegisterOnTcpServerStoppedCallback([]() {
        SPDLOG_WARN("[Callback] TCP Tunnel Server has stopped/disconnected.");
    });

    g_usbip->getTunnelManager()->RegisterOnTcpClientConnectedCallback([]() {
        SPDLOG_INFO("[Callback] TCP client successfully connected to the tunnel.");
    });

    g_usbip->getTunnelManager()->RegisterOnTcpClientDisconnectedCallback([]() {
        SPDLOG_WARN("[Callback] TCP client disconnected from the tunnel.");
    });
}

int main(int argc, char* argv[]) {
    rtc::LogMessage::LogToDebug(rtc::LoggingSeverity::LS_ERROR);

    GUI gui;
    if (!gui.init()) {
        if (signaling_client) {
            signaling_client->ShutdownFramework();
            signaling_client.reset();
        }
        return -1;
    }

    g_active_gui = &gui;
    auto gui_sink = std::make_shared<GuiLogSink<std::mutex>>();
    spdlog::default_logger()->sinks().push_back(gui_sink);

    // 1. Local USB
    gui.setLocalUSBHandler([](GUI& target_gui) {
        local_usb_triggered = true;
        if (!g_usbipd) {
            g_usbipd = new UsbIpdCommands();
        }
        target_gui.clearListItems();
        for (const auto& dev : g_usbipd->listPhysicalDevices()) {
            target_gui.addListItem(dev);
        }

        if (is_datachannel_connected || is_session_starting) return;
        is_session_starting = true;

        if (!g_usbipd->startHostSession(active_room_id, global_client_id)) {
            is_session_starting = false;
            target_gui.setLabelText("Failed to initialize host sharing session");
            return;
        }
        target_gui.setLabelText("Hosting local USB devices on room: " + active_room_id);
    });

    // 2. Remote USB
    gui.setRemoteUSBHandler([](GUI& target_gui) {
        remote_usb_triggered = true;

        // Initialize tunnel callbacks once to prevent callback stacking


        if (!g_usbip) {
            g_usbip = new UsbIpCommands();
        }

        // If the data channel is already active, refresh and list remote devices directly
      /*  if (is_datachannel_connected) {
            FetchAndDisplayRemoteDevices(target_gui);
            return;
        }*/

        target_gui.clearListItems();
        target_gui.addListItem("Initializing WebRTC tunnel to remote host...");

        int ret = g_usbip->initializeTunnelSession();
        if (ret < 0) {
            target_gui.setLabelText("Failed to establish client tunnel");
            return;
        }

        static std::once_flag callbacks_flag;
        std::call_once(callbacks_flag, []() { InitializeTunnelCallbacks(); });

    });

    // 3. Attach
    gui.setRemoteAttachHandler([](GUI& target_gui) {
        std::string selected = target_gui.getSelectedItem();
        if (selected.empty() || selected.find("---") != std::string::npos) return;

        size_t start = selected.find('[');
        size_t end = selected.find(']');
        if (start != std::string::npos && end != std::string::npos && end > start + 1) {
            std::string busid = selected.substr(start + 1, end - start - 1);
            if (g_usbip) {
                g_usbip->executeCommand({"usbip", "attach", "-r", "127.0.0.1", "-b", busid});
                attach_triggered = true;
                target_gui.setLabelText("Attached device " + busid + " successfully");
            }
        }
    });

    // 4. Detach
    gui.setRemoteDetachHandler([](GUI& target_gui) {
        if (g_usbip) {
            g_usbip->executeCommand({"usbip", "detach", "-a"});
            target_gui.setLabelText("All remote USB devices detached");
        }
    });

    // 5. Logs
    gui.setLogHandler([](GUI& target_gui) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        target_gui.clearListItems();
        target_gui.addListItem("--- Recent Log Snapshot ---");
        for (const auto& log : g_log_buffer) {
            target_gui.addListItem(log);
        }
    });

    gui.run();

    // Lifecycle Shutdown
    keep_running = false;
    
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
