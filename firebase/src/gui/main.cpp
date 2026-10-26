#include "gui.h"
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

#include "signaling.h"
#include "../webrtc_usbipd.h"
#include "firebase/app.h"
#include "firebase/firestore.h"

// USB/IP dependencies
#include "usbip_server.h"
#include "usb_device_passthrough.h"
#include "../vbox_service_manager.h"

// --- GLOBALS ---
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

// USBIP Globals
usbip_server_t* g_server = nullptr;
bool g_have_passthrough = false;
std::string g_passthrough_busid = "";
uint16_t g_passthrough_vid = 0;
uint16_t g_passthrough_pid = 0;
bool g_dynamic_mode = true;

// --- FORWARD DECLARATIONS ---
std::vector<std::string> usb_device_list_physical_devices(void);

void InitializeUsbIpServerSession() {
    if (g_server) return;

    g_server = usbip_server_create();
    if (!g_server) {
        SPDLOG_ERROR("Failed to create USB/IP WebRTC server context.");
        return;
    }

    usbip_server_enable_dynamic_devices(g_server, true);
    SPDLOG_INFO("USB/IP Server initialized in dynamic device mode.");
}

void AttachDataChannelCallback(WebRtcManager_usbipd* manager) {
    if (!manager) return;
    manager->SetDataChannelStateCallback([](bool is_open) {
        is_datachannel_connected = is_open;
        if (is_open) {
            SPDLOG_INFO("[Heartbeat Manager] DataChannel opened. Pausing Firebase polling & switching to 500ms loop.");
        } else {
            SPDLOG_WARN("[Heartbeat Manager] DataChannel closed. Resuming Firebase polling & switching to 30ms loop.");
        }
    });
}

// --- CONNECT BUTTON ACTION HOOK ---
void OnConnectButtonClicked(GUI& gui) {
    std::cout << "\n[GUI Event] Connect button clicked. Initializing session sequence...\n" << std::endl;

    // 1. Enumerate physical USB devices and populate GUI list items
    std::vector<std::string> usb_devices = usb_device_list_physical_devices();
    for (const auto& dev_str : usb_devices) {
        gui.addListItem(dev_str);
    }

    // 2. Room ID setup & Capacity validation
    if (active_room_id.empty()) {
        active_room_id = GenerateRandomRoomId(20);
        host_server = true; 
        SPDLOG_INFO("[Room Creation] Generated fresh room key and set role to Host Server: {}", active_room_id);
    } else {
        host_server = IsRoomEmpty(active_room_id); 
        SPDLOG_INFO("[Room Join] Target room ID: {}. Assigned Role -> {}", active_room_id, host_server ? "Host Server" : "Client");
    }

    SaveConfigToDisk(active_room_id, "info");

    SPDLOG_INFO("Evaluating capacity constraints for room matrix [{}]...", active_room_id);
    if (!VerifyAndReserveRoomCapacity(active_room_id)) {
        std::cout << "\n[CRITICAL REJECT] Access Denied: Room capacity limit reached.\n" << std::endl;
        ShutdownFirebaseFramework(nullptr);
        return;
    }

    // 3. Setup USB/IP Server & VirtualBox Manager
    InitializeUsbIpServerSession();

    static std::unique_ptr<ScopedVBoxServiceManager> vbox_guard = nullptr;
    if (host_server) {
        vbox_guard = std::make_unique<ScopedVBoxServiceManager>();
    }

    if (!host_server) {
        SPDLOG_INFO("[Client Init] Instantiating WebRtcManager caller node to generate connection offer...");
        core_server_connection = std::make_unique<WebRtcManager_usbipd>(active_room_id, global_client_id, true);
        AttachDataChannelCallback(core_server_connection.get());
        core_server_connection->SetRemotePeerId("ALL");
        if (core_server_connection->Initialize()) {
            core_server_connection->StartConnectionSequence();
        }
    }

    // 4. Register Event Callbacks
    RegisterOnRoomReadyEvent([&](const std::string& ready_room_id) {
        SPDLOG_WARN("=================================================================");
        SPDLOG_WARN("[EVENT TRIGGERED] Room [{}] is full and ready for meeting!", ready_room_id);
        SPDLOG_WARN("=================================================================");
    });

    RegisterOnPeerLeftEvent([&](const std::string& peer_client_id) {
        SPDLOG_ERROR("=================================================================");
        SPDLOG_ERROR("[EVENT NOTICE] Peer [{}] has disconnected or left the room.", peer_client_id);
        SPDLOG_ERROR("=================================================================");
        
        if (host_server) {
            std::lock_guard<std::mutex> lock(clients_mutex);
            auto it = connected_clients.find(peer_client_id);
            if (it != connected_clients.end()) {
                it->second->ShutdownConnection();
                connected_clients.erase(it);
            }
            is_datachannel_connected = false;
            SPDLOG_INFO("[WebRTC State Reset] Standing by as Host. Waiting for new peer connection...");
        } else {
            if (core_server_connection) {
                core_server_connection->ShutdownConnection();
                core_server_connection = nullptr;
            }
            SPDLOG_WARN("Host disconnected. Exiting signaling session room.");
            keep_running = false;
        }
    });

    RegisterOnSdpReceivedEvent([&](const std::string& sender_id, const std::string& type, const std::string& sdp) {
        SPDLOG_INFO("[Signaling Event] Received remote SDP {} from peer: {}", type, sender_id);
        if (sender_id == global_client_id) return;

        if (host_server) {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (connected_clients.find(sender_id) == connected_clients.end()) {
                SPDLOG_INFO("[Server Hub] Spawning new WebRtcManager instance for incoming client: {}", sender_id);
                auto client_rtc = std::make_unique<WebRtcManager_usbipd>(active_room_id, global_client_id, false);
                AttachDataChannelCallback(client_rtc.get());
                client_rtc->SetRemotePeerId(sender_id);
                if (client_rtc->Initialize()) {
                    connected_clients[sender_id] = std::move(client_rtc);
                }
            }
            connected_clients[sender_id]->SetRemotePeerId(sender_id);
            connected_clients[sender_id]->HandleRemoteSdp(type, sdp);
        } else {
            if (core_server_connection) {
                core_server_connection->SetRemotePeerId(sender_id);
                core_server_connection->HandleRemoteSdp(type, sdp);
            }
        }
    });

    RegisterOnIceReceivedEvent([&](const std::string& sender_id, const std::string& mid, int mline_index, const std::string & sdp) {
        SPDLOG_INFO("[Signaling Event] Received remote ICE candidate (sdpMid: {}, sdpMLineIndex: {}) from peer: {}", mid, mline_index, sender_id);
        if (sender_id == global_client_id) return;

        if (host_server) {
            std::lock_guard<std::mutex> lock(clients_mutex);
            auto it = connected_clients.find(sender_id);
            if (it != connected_clients.end() && it->second) {
                it->second->SetRemotePeerId(sender_id);
                it->second->HandleRemoteIceCandidate(mid, mline_index, sdp);
            }
        } else {
            if (core_server_connection) {
                core_server_connection->SetRemotePeerId(sender_id);
                core_server_connection->HandleRemoteIceCandidate(mid, mline_index, sdp);
            }
        }
    });

    SetUserPresence(active_room_id, global_client_id, true);
    ListenForTargetedMessages(active_room_id, global_client_id);
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list" || arg == "-l") {
            usb_device_list_physical_devices();
            return 0;
        }
    }

    rtc::LogMessage::LogToDebug(rtc::LoggingSeverity::LS_ERROR);
    SetupLogging("info");
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    global_client_id = "cpp_peer_" + std::to_string(std::rand() % 90000 + 1000);

    if (!InitializeFirebaseFramework()) {
        std::cerr << "Firebase setup crash exception encountered." << std::endl;
        return -1;
    }
    SPDLOG_INFO("Successfully authenticated with Cloud Firestore. Client ID: [{}]", global_client_id);

    LoadConfigFromDisk(active_room_id, global_client_id);

    // Initialize SDL GUI
    GUI gui;
    if (!gui.init()) {
        ShutdownFirebaseFramework(nullptr);
        return -1;
    }

    // Bind Connect Button Event Handler
    gui.setConnectHandler([](GUI& target_gui) {
        OnConnectButtonClicked(target_gui);
    });

    // Background thread for handling heartbeat polling/signaling loops while GUI runs smoothly
    std::thread background_worker([]() {
        auto last_heartbeat_time = std::chrono::steady_clock::now();
        while (keep_running) {
            int sleep_ms = is_datachannel_connected ? 500 : 30;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));

            if (!active_room_id.empty()) {
                if (is_datachannel_connected) {
                    last_heartbeat_time = std::chrono::steady_clock::now();
                } else {
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat_time).count() >= 5) {
                        SetUserPresence(active_room_id, global_client_id, true);
                        last_heartbeat_time = now;
                    }
                }
            }
        }
    });

    // Run the main SDL event and render loop on the main thread
    gui.run();

    // Clean exit teardown
    keep_running = false;
    if (!active_room_id.empty()) {
        HandleGracefulExit(active_room_id, global_client_id);
    }

    if (core_server_connection) {
        core_server_connection->ShutdownConnection();
        core_server_connection.reset();
    }
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        connected_clients.clear();
    }

    if (background_worker.joinable()) {
        background_worker.join();
    }

    if (g_server) {
        usbip_server_destroy(g_server);
        g_server = nullptr;
    }

    ShutdownFirebaseFramework(nullptr);
    return 0;
}