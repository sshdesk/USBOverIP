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

#include "../signaling.h"
#include "../webrtc_usbipd.h"
#include "../tcptunnel.h"

std::atomic<bool> keep_running{true};
std::atomic<bool> is_datachannel_connected{false};
std::atomic<bool> is_usbip_busy{false}; 
std::string active_room_id = ""; 
std::string global_client_id = "";

bool local_usb_triggered = false;
bool remote_usb_triggered = false;
bool attach_triggered = false;
static bool is_session_starting = false;

// Subsystem Controller Instances
static UsbIpCommands *g_usbip = nullptr;
static UsbIpdCommands *g_usbipd = nullptr;

void FetchAndDisplayRemoteDevices(GUI &gui) {
    std::string temp_file = "usbip_devices_temp.txt";

    FILE *fp = nullptr;
    if (freopen_s(&fp, temp_file.c_str(), "w", stdout) != 0 || !fp) {
        SPDLOG_ERROR("Failed to redirect stdout for device listing.");
        return;
    }

    if (g_usbip) {
        g_usbip->executeCommand({"list", "-r", "127.0.0.1"});
    }

    fflush(stdout);

    // Restore stdout back to Windows console buffer instead of closing it
    freopen_s(&fp, "CONOUT$", "w", stdout);

    std::ifstream infile(temp_file);
    if (!infile.is_open()) {
        SPDLOG_ERROR("Failed to open temp file for reading.");
        return;
    }

    gui.clearListItems();
    gui.addListItem("--- Select a Device to Attach ---");

    std::string line;
    while (std::getline(infile, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t first_char_idx = line.find_first_not_of(" \t");
        if (first_char_idx == std::string::npos) {
            continue;
        }

        char c = line[first_char_idx];
        if (c == ':' || c == '-' || c == '=') {
            continue;
        }

        size_t first_colon = line.find(':', first_char_idx);
        if (first_colon == std::string::npos) {
            continue; 
        }

        std::string busid = line.substr(first_char_idx, first_colon - first_char_idx);
        size_t busid_end = busid.find_last_not_of(" \t");
        if (busid_end != std::string::npos) {
            busid = busid.substr(0, busid_end + 1);
        }

        if (busid.empty() || busid.find(' ') != std::string::npos) {
            continue;
        }

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

        std::string formatted_item = "[" + busid + "] " + desc;
        gui.addListItem(formatted_item);
        SPDLOG_INFO("[Parsed Device] {}", formatted_item);
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
        if (g_active_gui) {
            std::thread([](GUI& gui) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                if (is_datachannel_connected) {
                    gui.setLabelText("Data channel connected");
                    SPDLOG_INFO("Querying remote USB/IP server device list...");
                    FetchAndDisplayRemoteDevices(gui);
                }
            }, std::ref(*g_active_gui)).detach();
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

        if (!g_usbip) {
            g_usbip = new UsbIpCommands();
        }
        else
        {
          target_gui.clearListItems();
          target_gui.addListItem("Press detach button first");
          return;
        }

        target_gui.clearListItems();
        target_gui.addListItem("Initializing WebRTC tunnel to remote host...");

        //static std::once_flag callbacks_flag;
        //std::call_once(callbacks_flag, []() {  });

        InitializeTunnelCallbacks();

        int ret = g_usbip->initializeTunnelSession();
        if (ret < 0) {
            target_gui.setLabelText("Failed to establish client tunnel");
            return;
        }
    });

    // 3. Attach (Non-blocking background thread with UI locking)
    gui.setRemoteAttachHandler([](GUI& target_gui) {
        if (is_usbip_busy) {
            target_gui.setLabelText("An operation is already in progress...");
            return;
        }

        std::string selected = target_gui.getSelectedItem();
        if (selected.empty() || selected.find("---") != std::string::npos) return;

        size_t start = selected.find('[');
        size_t end = selected.find(']');
        if (start != std::string::npos && end != std::string::npos && end > start + 1) {
            std::string busid = selected.substr(start + 1, end - start - 1);
            
            is_usbip_busy = true;
            target_gui.setButtonsEnabled(false); // Dim and disable buttons
            target_gui.setLabelText("Attaching device " + busid + "...");

            std::thread([busid, &target_gui]() {
                if (g_usbip) {
                    g_usbip->executeCommand({"attach", "-r", "127.0.0.1", "-b", busid});
                    attach_triggered = true;
                }
                is_usbip_busy = false;
                target_gui.setButtonsEnabled(true); // Re-enable buttons and restore colors
            }).detach();
        }
    });

    // 4. Detach (Non-blocking background thread with UI locking)
    gui.setRemoteDetachHandler([](GUI& target_gui) {
        if (is_usbip_busy) {
            target_gui.setLabelText("An operation is already in progress...");
            return;
        }

        is_usbip_busy = true;
        target_gui.setButtonsEnabled(false); // Dim and disable buttons
        target_gui.setLabelText("Detaching all remote USB devices...");

        if (remote_usb_triggered && g_usbip) {
          g_usbip->shutdown();
          delete g_usbip;
          g_usbip = nullptr;
        }
        is_usbip_busy = false;
        target_gui.setButtonsEnabled(true);
        
        //std::thread([&target_gui]() {
        //    if (g_usbip) {
        //        g_usbip->executeCommand({"detach", "-a"});
        //    }
        //    is_usbip_busy = false;
        //    target_gui.setButtonsEnabled(true); // Re-enable buttons and restore colors
        //}).detach();
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
