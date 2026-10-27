#include "gui.h"
#include "logsync.h"
#include "usbip_commands.h"
#include "usbipd_commands.h"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <csignal>
#include <memory>
#include <map>
#include <vector>
#include <fstream>
#include <sstream>

#include "signaling.h"
#include "../webrtc_usbipd.h"
#include "firebase/app.h"
#include "firebase/firestore.h"

#include "usbip_server.h"
#include "usb_device_passthrough.h"
#include "../vbox_service_manager.h"


std::atomic<bool> keep_running{true};
std::atomic<bool> is_datachannel_connected{false};
std::string shared_input_buffer = "";
std::mutex input_mutex;
std::mutex clients_mutex;

std::string active_room_id = ""; 
std::string global_client_id = "";

std::map<std::string, std::unique_ptr<WebRtcManager_usbipd>> connected_clients;
std::unique_ptr<WebRtcManager_usbipd> core_server_connection = nullptr;

bool host_server = false;
bool share_usb_triggered = false;
bool attach_triggered = false;

static bool is_session_starting = false;

void OnShareUSBButtonClicked(GUI& gui) {
    std::cout << "\n[GUI Event] ShareUSB button clicked. Initializing session sequence...\n" << std::endl;
    share_usb_triggered = true;

    std::vector<std::string> usb_devices = usb_device_list_physical_devices();
    for (const auto& dev_str : usb_devices) {
        gui.addListItem(dev_str);
    }
    
    if (is_datachannel_connected || is_session_starting) {
        SPDLOG_INFO("[GUI Event] Session is already active or initializing. Ignoring re-init.");
        return;
    }

    is_session_starting = true; // Lock immediately to block concurrent clicks

    // Delegate capacity checking, server setup, signaling hooks, and offer generation
    if (!StartUsbIpHostSession(active_room_id, global_client_id)) {
        is_session_starting = false;
        return;
    }
}

void FetchAndDisplayRemoteDevices(GUI &gui) {
    std::string temp_file = "usbip_devices_temp.txt";

    FILE *fp = nullptr;
    if (freopen_s(&fp, temp_file.c_str(), "w", stdout) != 0) {
        SPDLOG_ERROR("Failed to redirect stdout for device listing.");
        return;
    }

    execute_usbip_command({"usbip", "list", "-r", "127.0.0.1"});

    fflush(stdout);
    if (fp) {
        fclose(fp);
    }

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

        // Check if line starts with a bus ID pattern (e.g., "   1-10    : ")
        size_t colon_pos = line.find(" : ");
        if (colon_pos != std::string::npos &&
            line.find("/sys/") == std::string::npos &&
            line.find("Type:") == std::string::npos) {
            
            if (!current_busid.empty()) {
                std::string formatted_item = "[" + current_busid + "] " + current_desc;
                gui.addListItem(formatted_item);
                SPDLOG_INFO("[Parsed Device] {}", formatted_item);
            }

            std::stringstream ss(line);
            ss >> current_busid;
            current_desc = line.substr(colon_pos + 3);
        }
    }

    if (!current_busid.empty()) {
        std::string formatted_item = "[" + current_busid + "] " + current_desc;
        gui.addListItem(formatted_item);
        SPDLOG_INFO("[Parsed Device] {}", formatted_item);
    }

    infile.close();
}





// --- ATTACH BUTTON ACTION HOOK (USB/IP Client Mode) ---
void OnAttachButtonClicked(GUI &gui) {
    std::cout << "\n[GUI Event] Attach button clicked. Initializing tunnel...\n" << std::endl;
    attach_triggered = true;
    gui.addListItem("Initializing WebRTC tunnel...");

    int ret = InitializeUsbIpClientTunnelSession();
    if (ret < 0) {
        if (ret == -1) gui.setLabelText("Room is fulled");
        if (ret == -2) gui.setLabelText("Enter your email id");
        return;
    }

    //std::thread([](GUI& gui) {
    //    // Wait briefly for the TCP tunnel bridge to bind and connect
    //    while (!is_datachannel_connected) {
    //        gui.setLabelText("Waiting for WebRTC connection to establish...");
    //        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    //    }
    //    
    //    if (is_datachannel_connected) {
    //        gui.setLabelText("Data channel connected");
    //        SPDLOG_INFO("Querying remote USB/IP server device list...");
    //        FetchAndDisplayRemoteDevices(gui);
    //    }
    //}, std::ref(gui)).detach();
}

int main(int argc, char* argv[]) {

    rtc::LogMessage::LogToDebug(rtc::LoggingSeverity::LS_ERROR);
   // SetupLogging("info");
   

    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    global_client_id = "cpp_peer_" + std::to_string(std::rand() % 90000 + 1000);

    if (!InitializeFirebaseFramework()) {
        std::cerr << "Firebase setup crash exception encountered." << std::endl;
        return -1;
    }


    std::string saved_log_level = "info";
    LoadConfigFromDisk(active_room_id, saved_log_level);


    // Initialize SDL GUI
    GUI gui;
    if (!gui.init()) {
      ShutdownFirebaseFramework(nullptr);
      return -1;
    }

    if (active_room_id.empty()) {
        active_room_id = GenerateRandomRoomId(20);
        host_server = true;
        gui.setLabelText("Room created for host sever. Share this ID with your peer: " +     active_room_id);
        SPDLOG_INFO("[Room Creation] Generated fresh room key and set role to Host Server: {}", active_room_id);
    } else {
        host_server = IsRoomEmpty(active_room_id, global_client_id);
        gui.setLabelText("Joining room. Assigned Role -> " + std::string(host_server ? "Host Server" : "Client"));
        SPDLOG_INFO("[Room Join] Target room ID: {}. Assigned Role -> {}", active_room_id, host_server ? "Host Server" : "Client");
    }

     SetupRotatingLogging(host_server , "info");

     SaveConfigToDisk(active_room_id, "info");



    // Assign global GUI pointer so log sink can update labelText live
    g_active_gui = &gui;

    // Attach custom spdlog sink
    auto gui_sink = std::make_shared<GuiLogSink<std::mutex>>();
    spdlog::default_logger()->sinks().push_back(gui_sink);

    SPDLOG_INFO("Successfully authenticated with Cloud Firestore. Client ID: [{}]", global_client_id);

    // Bind ShareUSB Button Event Handler
    gui.setShareUSBHandler([](GUI& target_gui) {
        OnShareUSBButtonClicked(target_gui);
    });

    // Bind Attach Button Event Handler
    gui.setAttachHandler([](GUI& target_gui) {
        OnAttachButtonClicked(target_gui);
    });

    // Bind Log Button Event Handler to load history snapshots into list view
    gui.setLogHandler([](GUI& target_gui) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        target_gui.clearListItems();
        target_gui.addListItem("--- Recent Log Snapshot ---");
        for (const auto& log : g_log_buffer) {
            target_gui.addListItem(log);
        }
    });

   /* std::thread background_worker([]() {
        auto last_heartbeat_time = std::chrono::steady_clock::now();
        while (keep_running) {
            int sleep_ms = is_datachannel_connected ? 500 : 30;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));

            if (!active_room_id.empty()) {
                if (is_datachannel_connected) {
                    last_heartbeat_time = std::chrono::steady_clock::now();
                } else {
                    auto now = std::chrono::steady_clock::now();
                    if (sleep_ms == 30 && std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat_time).count() >= 5) {
                        SetUserPresence(active_room_id, global_client_id, true);
                        last_heartbeat_time = now;
                    }
                }
            }
        }
    });*/

    // Run the main SDL event and render loop on the main thread
    gui.run();

    // Clean exit teardown
    keep_running = false;
    if (!active_room_id.empty()) {
        HandleGracefulExit(active_room_id, global_client_id);
    }

    // Clean up usbip client/tunnel code if Attach button was clicked
    if (attach_triggered) {
        ShutdownUsbIpClientTunnel();
        //execute_usbip_command({"usbip", "detach", "-a"});
    }

    // Clean up usbipd host/server code if ShareUSB button was clicked
    if (share_usb_triggered) {
        if (core_server_connection) {
            core_server_connection->ShutdownConnection();
            core_server_connection.reset();
        }
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            connected_clients.clear();
        }

        ShutdownUsbIpServerSession();
    }

  /*  if (background_worker.joinable()) {
        background_worker.join();
    }*/

    ShutdownFirebaseFramework(nullptr);
    return 0;
}
