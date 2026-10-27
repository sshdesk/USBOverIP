#include "usbipd_commands.h"
#include "../webrtc_usbipd.h"
#include "signaling.h"
#include "../vbox_service_manager.h"
#include "firebase/app.h"
#include "firebase/firestore.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include "logsync.h"

// Server context definition
usbip_server_t* g_server = nullptr;

// USB Passthrough Global Definitions
bool g_have_passthrough = false;
std::string g_passthrough_busid = "";
uint16_t g_passthrough_vid = 0;
uint16_t g_passthrough_pid = 0;
bool g_dynamic_mode = true;

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

void ShutdownUsbIpServerSession() {
    if (g_server) {
        usbip_server_destroy(g_server);
        g_server = nullptr;
        SPDLOG_INFO("USB/IP Server context destroyed.");
    }
}

void AttachDataChannelCallback(WebRtcManager_usbipd* manager) {
    if (!manager) return;
    manager->SetDataChannelStateCallback([](bool is_open) {
        is_datachannel_connected = is_open;

        // Thread-safe access to GUI
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

bool StartUsbIpHostSession(const std::string& active_room_id, const std::string& global_client_id) {
    SPDLOG_INFO("Evaluating capacity constraints for room matrix [{}]...", active_room_id);
    if (!VerifyAndReserveRoomCapacity(active_room_id, global_client_id)) {
        std::cout << "\n[CRITICAL REJECT] Access Denied: Room capacity limit reached.\n" << std::endl;
        ShutdownFirebaseFramework(nullptr);
        return false;
    }

  
    static std::unique_ptr<ScopedVBoxServiceManager> vbox_guard = nullptr;
    vbox_guard = std::make_unique<ScopedVBoxServiceManager>();


    InitializeUsbIpServerSession();

    SPDLOG_INFO("[Client Init] Instantiating WebRtcManager caller node to generate connection offer...");
    core_server_connection = std::make_unique<WebRtcManager_usbipd>(active_room_id, global_client_id, true);
    AttachDataChannelCallback(core_server_connection.get());
    core_server_connection->SetRemotePeerId("ALL");
    if (core_server_connection->Initialize()) {
        core_server_connection->StartConnectionSequence();
    }

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

    RegisterOnSdpReceivedEvent([&, active_room_id, global_client_id](const std::string& sender_id, const std::string& type, const std::string& sdp) {
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

    RegisterOnIceReceivedEvent([&, global_client_id](const std::string& sender_id, const std::string& mid, int mline_index, const std::string & sdp) {
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

    //SetUserPresence(active_room_id, global_client_id, true);
    ListenForTargetedMessages(active_room_id, global_client_id);
    return true;
}