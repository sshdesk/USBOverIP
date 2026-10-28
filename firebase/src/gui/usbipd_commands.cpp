#include "usbipd_commands.h"
#include "../webrtc_usbipd.h"
#include "signaling.h"
#include "../vbox_service_manager.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include "logsync.h"
#include "gui.h"

// Define the global server context expected by webrtc_usbipd.obj
usbip_server_t* g_server = nullptr;

extern std::atomic<bool> keep_running;
extern std::atomic<bool> is_datachannel_connected;
extern std::string active_room_id;
extern std::string global_client_id;

// Device enumeration function linkage
std::vector<std::string> usb_device_list_physical_devices(void);

UsbIpdCommands::UsbIpdCommands() = default;

UsbIpdCommands::~UsbIpdCommands() {
    shutdown();
}

std::vector<std::string> UsbIpdCommands::listPhysicalDevices() {
    return usb_device_list_physical_devices();
}

void UsbIpdCommands::initializeServerSession() {
    if (g_server) return;

    g_server = usbip_server_create();
    if (!g_server) {
        SPDLOG_ERROR("Failed to create USB/IP WebRTC server context.");
        return;
    }

    server_ = g_server;
    usbip_server_enable_dynamic_devices(g_server, true);
    SPDLOG_INFO("USB/IP Server initialized in dynamic device mode.");
}

void UsbIpdCommands::shutdownServerSession() {
    if (g_server) {
        usbip_server_destroy(g_server);
        g_server = nullptr;
        server_ = nullptr;
        SPDLOG_INFO("USB/IP Server context destroyed.");
    }
}

void UsbIpdCommands::attachDataChannelCallback(WebRtcManager_usbipd* manager) {
    if (!manager) return;
    manager->SetDataChannelStateCallback([](bool is_open) {
        is_datachannel_connected = is_open;

        {
            std::lock_guard<std::mutex> lock(g_log_mutex);
            if (g_active_gui) {
                if (is_open) {
                    g_active_gui->setLabelText("Data channel pipe opened");
                } else {
                    g_active_gui->setLabelText("Data channel pipe closed");
                }
            }
        }

        if (is_open) {
            SPDLOG_INFO("[Heartbeat Manager] DataChannel opened. Pausing Firebase polling & switching to 500ms loop.");
        } else {
            SPDLOG_WARN("[Heartbeat Manager] DataChannel closed. Resuming Firebase polling & switching to 30ms loop.");
        }
    });
}

bool UsbIpdCommands::startHostSession(const std::string& room_id, const std::string& client_id) {
    signaling_client = std::make_unique<SignalingClient>();
    if (!signaling_client->InitializeFramework()) {
        std::cerr << "Firebase setup crash exception encountered." << std::endl;
        return false;
    }

    std::string saved_log_level = "info";
    signaling_client->LoadConfigFromDisk(active_room_id, saved_log_level);

    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    global_client_id = "cpp_peer_" + std::to_string(std::rand() % 90000 + 1000);

    if (active_room_id.empty()) {
        active_room_id = signaling_client->GenerateRandomRoomId(20);
        setHostServer(true);
        g_active_gui->setLabelText("Room created for host server. Share this ID with your peer: " + active_room_id);
    } else {
        setHostServer(signaling_client->IsRoomEmpty(active_room_id, global_client_id));
        g_active_gui->setLabelText("Joining room. Assigned Role -> " + std::string(isHostServer() ? "Host Server" : "Client"));
    }

    signaling_client->SaveConfigToDisk(active_room_id, "info");
    SetupRotatingLogging(isHostServer(), "info");
    
    if (!signaling_client) {
        SPDLOG_ERROR("[Host Session Init] Error: signaling_client instance is null.");
        return false;
    }

    SPDLOG_INFO("Evaluating capacity constraints for room matrix [{}]...", room_id);
    if (!signaling_client->VerifyAndReserveRoomCapacity(room_id, client_id)) {
        std::cout << "\n[CRITICAL REJECT] Access Denied: Room capacity limit reached.\n" << std::endl;
        signaling_client->ShutdownFramework();
        return false;
    }

    vbox_guard_ = std::make_unique<ScopedVBoxServiceManager>();
    initializeServerSession();

    if (!host_server_) {
        SPDLOG_INFO("[Client Init] Instantiating WebRtcManager caller node to generate connection offer...");
        core_server_connection_ = std::make_unique<WebRtcManager_usbipd>(room_id, client_id, true);
        attachDataChannelCallback(core_server_connection_.get());
        core_server_connection_->SetRemotePeerId("ALL");
        if (core_server_connection_->Initialize()) {
            core_server_connection_->StartConnectionSequence();
        }
    } else {
        SPDLOG_INFO("[Host server Init] Instantiating WebRtcManager caller waiting for offer...");
    }

    signaling_client->RegisterOnRoomReadyEvent([](const std::string& ready_room_id) {
        SPDLOG_WARN("=================================================================");
        SPDLOG_WARN("[EVENT TRIGGERED] Room [{}] is full and ready for meeting!", ready_room_id);
        SPDLOG_WARN("=================================================================");
    });

    signaling_client->RegisterOnPeerLeftEvent([this](const std::string& peer_client_id) {
        SPDLOG_ERROR("=================================================================");
        SPDLOG_ERROR("[EVENT NOTICE] Peer [{}] has disconnected or left the room.", peer_client_id);
        SPDLOG_ERROR("=================================================================");
        
        if (host_server_) {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto it = connected_clients_.find(peer_client_id);
            if (it != connected_clients_.end()) {
                if (it->second) {
                    it->second->ShutdownConnection();
                }
                connected_clients_.erase(it);
            }
            is_datachannel_connected = false;
            SPDLOG_INFO("[WebRTC State Reset] Standing by as Host. Waiting for new peer connection...");
        } else {
            if (core_server_connection_) {
                core_server_connection_->ShutdownConnection();
                core_server_connection_.reset();
            }
            SPDLOG_WARN("Host disconnected. Exiting signaling session room.");
            keep_running = false;
        }
    });

    signaling_client->RegisterOnSdpReceivedEvent([this, room_id, client_id](const std::string& sender_id, const std::string& type, const std::string& sdp) {
        SPDLOG_INFO("[Signaling Event] Received remote SDP {} from peer: {}", type, sender_id);
        if (sender_id == client_id) return;

        if (host_server_) {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            if (connected_clients_.find(sender_id) == connected_clients_.end()) {
                SPDLOG_INFO("[Server Hub] Spawning new WebRtcManager instance for incoming client: {}", sender_id);
                auto client_rtc = std::make_unique<WebRtcManager_usbipd>(room_id, client_id, false);
                attachDataChannelCallback(client_rtc.get());
                client_rtc->SetRemotePeerId(sender_id);
                if (client_rtc->Initialize()) {
                    connected_clients_[sender_id] = std::move(client_rtc);
                }
            }
            auto it = connected_clients_.find(sender_id);
            if (it != connected_clients_.end() && it->second) {
                it->second->SetRemotePeerId(sender_id);
                it->second->HandleRemoteSdp(type, sdp);
            }
        } else {
            if (core_server_connection_) {
                core_server_connection_->SetRemotePeerId(sender_id);
                core_server_connection_->HandleRemoteSdp(type, sdp);
            }
        }
    });

    signaling_client->RegisterOnIceReceivedEvent([this, client_id](const std::string& sender_id, const std::string& mid, int mline_index, const std::string & sdp) {
        SPDLOG_INFO("[Signaling Event] Received remote ICE candidate (sdpMid: {}, sdpMLineIndex: {}) from peer: {}", mid, mline_index, sender_id);
        if (sender_id == client_id) return;

        if (host_server_) {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto it = connected_clients_.find(sender_id);
            if (it != connected_clients_.end() && it->second) {
                it->second->SetRemotePeerId(sender_id);
                it->second->HandleRemoteIceCandidate(mid, mline_index, sdp);
            }
        } else {
            if (core_server_connection_) {
                core_server_connection_->SetRemotePeerId(sender_id);
                core_server_connection_->HandleRemoteIceCandidate(mid, mline_index, sdp);
            }
        }
    });

    signaling_client->ListenForTargetedMessages(room_id, client_id);
    return true;
}

void UsbIpdCommands::shutdown() {
    if (signaling_client && !active_room_id.empty()) {
        signaling_client->HandleGracefulExit(active_room_id, global_client_id);
    }

    if (core_server_connection_) {
        core_server_connection_->ShutdownConnection();
        core_server_connection_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& pair : connected_clients_) {
            if (pair.second) {
                pair.second->ShutdownConnection();
            }
        }
        connected_clients_.clear();
    }
    shutdownServerSession();
    vbox_guard_.reset();

    if (signaling_client) {
        signaling_client->ShutdownFramework();
        signaling_client.reset();
    }
}