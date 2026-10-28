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

#include "signaling.h"
#include "webrtc.h"
#include "tcptunnel.h"
#include "firebase/app.h"
#include "firebase/firestore.h"

std::atomic<bool> keep_running{true};
std::atomic<bool> is_datachannel_connected{false};
std::string shared_input_buffer = "";
std::mutex input_mutex;
std::mutex clients_mutex;

std::string active_room_id = ""; 
std::string global_client_id = "";

std::map<std::string, std::unique_ptr<WebRtcManager>> connected_clients;
std::unique_ptr<WebRtcManager> core_server_connection = nullptr;

bool host_server = false;

extern std::unique_ptr<SignalingClient> signaling_client;

WebRtcTcpTunnelManager tunnel_manager(3240);

void SignalExitHandler(int signum) {
    SPDLOG_WARN("\nProcess termination signal [{}] intercepted!", signum);
    keep_running = false;
}

// Register tunnel callbacks once globally to prevent callback stacking
void InitializeTunnelCallbacks() {
    tunnel_manager.RegisterOnTcpServerStartedCallback([]() {
        SPDLOG_INFO("[Callback] TCP Tunnel Server successfully started and listening.");
    });

    tunnel_manager.RegisterOnTcpServerStoppedCallback([]() {
        SPDLOG_WARN("[Callback] TCP Tunnel Server has stopped/disconnected.");
    });

    tunnel_manager.RegisterOnTcpClientConnectedCallback([]() {
        SPDLOG_INFO("[Callback] TCP client successfully connected to the tunnel.");
    });

    tunnel_manager.RegisterOnTcpClientDisconnectedCallback([]() {
        SPDLOG_WARN("[Callback] TCP client disconnected from the tunnel.");
    });
}

void AttachDataChannelCallback(WebRtcManager* manager) {
    if (!manager) return;
    manager->SetDataChannelStateCallback([](bool is_open) {
        is_datachannel_connected = is_open;
        if (is_open) {
            SPDLOG_INFO("[Heartbeat Manager] DataChannel opened. Pausing Firebase polling & switching to 500ms loop.");
            if (signaling_client) {
                signaling_client->StopHeartbeat();
            }
            if (core_server_connection && core_server_connection->GetDataChannel()) {
                if (!host_server) {
                    // Run tunnel start asynchronously to prevent blocking the initialization/Firestore loop
                    std::thread([]() {
                        if (!tunnel_manager.Start()) {
                            SPDLOG_ERROR("Failed to start tunnel manager because TCP bind failed.");
                        } else {
                            SPDLOG_INFO("Caller DataChannel linked. Linking to TCP tunnel manager...");
                            tunnel_manager.HandleDataChannel(core_server_connection->GetDataChannel(), core_server_connection->GetSignalingThread());
                        }
                    }).detach();
                }
            }
        } else {
            SPDLOG_WARN("[Heartbeat Manager] DataChannel closed. Resuming Firebase polling & switching to 30ms loop.");
            if (signaling_client) {
                signaling_client->StartHeartbeat(active_room_id, global_client_id);
            }
            
            // Run tunnel stop asynchronously to prevent deadlocks
//            std::thread([]() {
//                tunnel_manager.Stop();
//            }).detach();
        }
    });
}

int main() {
    signaling_client = std::make_unique<SignalingClient>();

    signaling_client->SetupLogging("info");
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    std::signal(SIGINT, SignalExitHandler);
    std::signal(SIGTERM, SignalExitHandler);

    global_client_id = "cpp_peer_" + std::to_string(std::rand() % 90000 + 1000);

    // Initialize global tunnel event hooks once
    InitializeTunnelCallbacks();

    if (!signaling_client->InitializeFramework()) {
        SPDLOG_ERROR("Firebase setup crash exception encountered.");
        return -1;
    }
    SPDLOG_INFO("Successfully authenticated with Cloud Firestore.");
    SPDLOG_INFO("Your Client Identity Token: [{}]", global_client_id);

    std::string saved_room_id = "";
    std::string saved_log_level = "info";
    signaling_client->LoadConfigFromDisk(saved_room_id, saved_log_level);

    std::string input_room;
    if (!saved_room_id.empty()) {
        char use_saved = 'y';
        std::cout << "Found cached room identifier token: [" << saved_room_id << "]\n";
        std::cout << "Use saved token? (y/n): ";
        std::cin >> use_saved;
        if (use_saved == 'y' || use_saved == 'Y') {
            input_room = saved_room_id;
        }
        std::cin.ignore();
    }

    if (input_room.empty()) {
        std::cout << "Enter target room identifier string token (or leave blank to create a new room): ";
        std::getline(std::cin, input_room);
    }

    if (input_room.empty()) {
        active_room_id = signaling_client->GenerateRandomRoomId(20);
        host_server = true; 
        SPDLOG_INFO("[Room Creation] Generated fresh room key and set role to Host Server: {}", active_room_id);
    } else {
        active_room_id = input_room;
        host_server = signaling_client->IsRoomEmpty(active_room_id, global_client_id); 
        SPDLOG_INFO("[Room Join] Target room ID: {}. Assigned Role -> {}", active_room_id, host_server ? "Host Server" : "Client");
    }

    signaling_client->SaveConfigToDisk(active_room_id, "info");

    SPDLOG_INFO("Evaluating capacity constraints for room matrix [{}]...", active_room_id);
    if (!signaling_client->VerifyAndReserveRoomCapacity(active_room_id, global_client_id)) {
        std::cout << "\n[CRITICAL REJECT] Access Denied: Room capacity limit reached.\n" << std::endl;
        signaling_client->ShutdownFramework();
        return -1;
    }

    if (!host_server) {
        SPDLOG_INFO("[Client Init] Instantiating WebRtcManager caller node to generate connection offer...");
        core_server_connection = std::make_unique<WebRtcManager>(active_room_id, global_client_id, true);
        AttachDataChannelCallback(core_server_connection.get());
        core_server_connection->SetRemotePeerId("ALL");
        if (core_server_connection->Initialize()) {
            core_server_connection->StartConnectionSequence();
        }
    }

    signaling_client->RegisterOnRoomReadyEvent([&](const std::string& ready_room_id) {
        SPDLOG_WARN("=================================================================");
        SPDLOG_WARN("[EVENT TRIGGERED] Room [{}] is full and ready for meeting!", ready_room_id);
        SPDLOG_WARN("=================================================================");
    });

    signaling_client->RegisterOnPeerLeftEvent([&](const std::string& peer_client_id) {
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
            
            tunnel_manager.Stop();
            if (core_server_connection) {
                core_server_connection->ShutdownConnection();
                core_server_connection = nullptr;
            }
            SPDLOG_WARN("Host disconnected. Exiting signaling session room.");
            keep_running = false;
        }
    });

    signaling_client->RegisterOnSdpReceivedEvent([&](const std::string& sender_id, const std::string& type, const std::string& sdp) {
        SPDLOG_INFO("[Signaling Event] Received remote SDP {} from peer: {}", type, sender_id);
        if (sender_id == global_client_id) return;

        if (host_server) {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (connected_clients.find(sender_id) == connected_clients.end()) {
                SPDLOG_INFO("[Server Hub] Spawning new WebRtcManager instance for incoming client: {}", sender_id);
                auto client_rtc = std::make_unique<WebRtcManager>(active_room_id, global_client_id, false);
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

    signaling_client->RegisterOnIceReceivedEvent([&](const std::string& sender_id, const std::string& mid, int mline_index, const std::string& sdp) {
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

    signaling_client->ListenForTargetedMessages(active_room_id, global_client_id);

    std::cout << "\n=======================================================" << std::endl;
    std::cout << "MESSAGE FORMAT RULES:" << std::endl;
    std::cout << " -> To Broadcast to everyone use  -> ALL:<your message>" << std::endl;
    std::cout << " -> To Message a specific peer use -> peer_id:<your message>" << std::endl;
    std::cout << " -> Type '/exit' or use Ctrl+C to terminate app." << std::endl;
    std::cout << "=======================================================\n" << std::endl;

    std::thread input_thread([&]() {
        while (keep_running) {
            std::string raw_input;
            if (std::getline(std::cin, raw_input)) {
                if (raw_input == "/exit") {
                    keep_running = false;
                    break;
                }
                std::lock_guard<std::mutex> lock(input_mutex);
                shared_input_buffer = raw_input;
            }
        }
    });

    while (keep_running) {
        int sleep_ms = 30;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));

        if (!keep_running) break;

        std::string active_msg = "";
        {
            std::lock_guard<std::mutex> lock(input_mutex);
            if (!shared_input_buffer.empty()) {
                active_msg = shared_input_buffer;
                shared_input_buffer.clear();
            }
        }

        if (!active_msg.empty()) {
            size_t delimiter_pos = active_msg.find(':');
            if (delimiter_pos != std::string::npos) {
                std::string target_peer = active_msg.substr(0, delimiter_pos);
                std::string text_payload = active_msg.substr(delimiter_pos + 1);
                signaling_client->SendTargetedMessage(active_room_id, global_client_id, target_peer, text_payload);
            } else {
                SPDLOG_WARN("Invalid format pattern! Remember to use 'target_id:message'.");
            }
        }
    }

    SPDLOG_INFO("Shutting down application cleanly...");

    signaling_client->HandleGracefulExit(active_room_id, global_client_id);

   tunnel_manager.Stop();

    if (core_server_connection) {
        core_server_connection->ShutdownConnection();
        core_server_connection.reset();
    }
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (auto& pair : connected_clients) {
            if (pair.second) {
                pair.second->ShutdownConnection();
            }
        }
        connected_clients.clear();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    if (input_thread.joinable()) {
        input_thread.join(); 
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

   // 
    signaling_client->ShutdownFramework();
    signaling_client.reset();
    return 0;
}