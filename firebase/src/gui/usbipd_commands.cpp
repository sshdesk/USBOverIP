#include "usbipd_commands.h"
#include "../webrtc_usbipd.h"
#include "../signaling.h"
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
            SPDLOG_INFO("[Heartbeat Manager] DataChannel opened. Active streaming tunnel linked.");
        } else {
            SPDLOG_WARN("[Heartbeat Manager] DataChannel closed.");
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
        if (g_active_gui) {
            g_active_gui->setLabelText("Room created for host server. Share this ID with your peer: " + active_room_id);
        }
    } else {
        setHostServer(true);
        if (g_active_gui) {
            g_active_gui->setLabelText("Joining room as Host Server: " + active_room_id);
        }
    }

    signaling_client->SaveConfigToDisk(active_room_id, "info");
    SetupRotatingLogging(isHostServer(), "info");
    
    if (!signaling_client) {
        SPDLOG_ERROR("[Host Session Init] Error: signaling_client instance is null.");
        return false;
    }

    vbox_guard_ = std::make_unique<ScopedVBoxServiceManager>();
    initializeServerSession();

    signaling_client->RegisterOnPeerLeftEvent([this](const std::string& peer_client_id) {
        SPDLOG_ERROR("=================================================================");
        SPDLOG_ERROR("[EVENT NOTICE] Peer [{}] has disconnected or left the room.", peer_client_id);
        SPDLOG_ERROR("=================================================================");
        
        if (core_server_connection_) {
            core_server_connection_->ShutdownConnection();
            core_server_connection_.reset();
        }
        is_datachannel_connected = false;
        SPDLOG_INFO("[WebRTC State Reset] Standing by as Host. Waiting for new peer connection...");
    });

    signaling_client->RegisterOnSdpReceivedEvent([this](const std::string& sender_id, const std::string& type, const std::string& sdp) {
        SPDLOG_INFO("[Signaling Event] Received remote SDP {} from peer: {}", type, sender_id);
        if (sender_id == global_client_id) return;

        if (type == "offer") {
            if (!core_server_connection_) {
                SPDLOG_INFO("[Callee Setup] Ingesting remote offer. Initializing responder pipeline...");
                core_server_connection_ = std::make_unique<WebRtcManager_usbipd>(active_room_id, global_client_id, false);
                attachDataChannelCallback(core_server_connection_.get());
                core_server_connection_->SetRemotePeerId(sender_id);
                core_server_connection_->Initialize();
            }
            core_server_connection_->SetRemotePeerId(sender_id);
            core_server_connection_->HandleRemoteSdp(type, sdp);
        } else {
            if (core_server_connection_) {
                core_server_connection_->SetRemotePeerId(sender_id);
                core_server_connection_->HandleRemoteSdp(type, sdp);
            }
        }
    });

    signaling_client->RegisterOnIceReceivedEvent([this](const std::string& sender_id, const std::string& mid, int mline_index, const std::string & sdp) {
        SPDLOG_INFO("[Signaling Event] Received remote ICE candidate (sdpMid: {}, sdpMLineIndex: {}) from peer: {}", mid, mline_index, sender_id);
        if (sender_id == global_client_id) return;

        if (core_server_connection_) {
            core_server_connection_->SetRemotePeerId(sender_id);
            core_server_connection_->HandleRemoteIceCandidate(mid, mline_index, sdp);
        }
    });

    signaling_client->ListenForTargetedMessages(active_room_id, global_client_id);

    // Join room without blocking: first to join waits as CALLEE; second creates offer as CALLER
    signaling_client->JoinRoom(active_room_id, global_client_id, [this](bool is_caller, const std::string& remote_peer_id) {
        if (is_caller) {
            SPDLOG_INFO("[WebRTC Role] Assigned CALLER. Initiating connection offer to peer [{}]...", remote_peer_id);
            core_server_connection_ = std::make_unique<WebRtcManager_usbipd>(active_room_id, global_client_id, true);
            attachDataChannelCallback(core_server_connection_.get());
            core_server_connection_->SetRemotePeerId(remote_peer_id);
            if (core_server_connection_->Initialize()) {
                core_server_connection_->StartConnectionSequence();
            }
        } else {
            SPDLOG_INFO("[WebRTC Role] Assigned CALLEE. Waiting for remote peer connection offer...");
            core_server_connection_ = std::make_unique<WebRtcManager_usbipd>(active_room_id, global_client_id, false);
            attachDataChannelCallback(core_server_connection_.get());
            core_server_connection_->Initialize();
        }
    });

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