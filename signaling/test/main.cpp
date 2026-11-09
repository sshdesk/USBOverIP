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

#include "signaling.h"
#include "webrtc.h"

#ifdef TUNNEL
#include "tcptunnel.h"
#endif

std::atomic<bool> keep_running{true};
std::atomic<bool> is_datachannel_connected{false};
std::atomic<bool> should_reset_rtc{false};
std::mutex input_mutex;
std::string shared_input_buffer = "";

std::string active_token_id = "";
std::string global_client_id = "";
bool remote_usb_mode = false;
bool is_caller_role = false;
std::string target_peer_id = "";

std::unique_ptr<WebRtcManager> rtc_connection = nullptr;

extern std::unique_ptr<SignalingClient> signaling_client;

#ifdef TUNNEL
WebRtcTcpTunnelManager tunnel_manager(3240);
#endif

void SignalExitHandler(int signum) {
    SPDLOG_INFO("Process termination signal [{}] intercepted!", signum);
    keep_running = false;
}

void InitializeTunnelCallbacks() {
#ifdef TUNNEL
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
        SPDLOG_INFO("[Callback] TCP client disconnected from the tunnel.");
    });
#endif
}

void AttachDataChannelCallback(WebRtcManager* manager) {
    if (!manager) return;
    manager->SetDataChannelStateCallback([](bool is_open) {
        is_datachannel_connected = is_open;
        if (is_open) {
#ifdef TUNNEL
            SPDLOG_INFO("[Heartbeat Manager] DataChannel opened. Starting TCP tunnel manager...");
            if (rtc_connection && rtc_connection->GetDataChannel()) {
                std::thread([]() {
                    if (!tunnel_manager.Start()) {
                        SPDLOG_ERROR("Failed to start tunnel manager because TCP bind failed.");
                    } else {
                        SPDLOG_INFO("DataChannel linked. Linking to TCP tunnel manager...");
                        tunnel_manager.HandleDataChannel(rtc_connection->GetDataChannel(), rtc_connection->GetSignalingThread());
                    }
                }).detach();
            }
#else
            SPDLOG_INFO("[DataChannel Manager] DataChannel opened successfully. Ready to send/receive data directly.");
#endif
        } else {
#ifdef TUNNEL
            SPDLOG_INFO("[Heartbeat Manager] DataChannel closed. Stopping TCP tunnel manager...");
            tunnel_manager.Stop();
#else
            SPDLOG_INFO("[DataChannel Manager] DataChannel closed.");
#endif
            should_reset_rtc = true;
        }
    });
}

void InitializeAndStartWebRtc(bool caller_role, const std::string& remote_peer) {
    if (rtc_connection) return;

    is_caller_role = caller_role;
    target_peer_id = remote_peer;

    SPDLOG_INFO("[WebRtc Setup] Initializing connection. Is Caller: {}, Target Peer ID: {}", is_caller_role ? "TRUE" : "FALSE", target_peer_id);

    rtc_connection = std::make_unique<WebRtcManager>(active_token_id, global_client_id, is_caller_role);
    AttachDataChannelCallback(rtc_connection.get());
    rtc_connection->SetRemotePeerId(target_peer_id);

    if (rtc_connection->Initialize()) {
        rtc_connection->StartConnectionSequence();
    }
}

int main() {
    signaling_client = std::make_unique<SignalingClient>();
    signaling_client->SetupLogging("info");

    // signaling_client->SetApiEndpoint("http://localhost:8080/prod");
    signaling_client->SetApiEndpoint("https://your-api-id.execute-api.us-east-1.amazonaws.com/prod");

    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    std::signal(SIGINT, SignalExitHandler);
    std::signal(SIGTERM, SignalExitHandler);

    global_client_id = "stim.io_" + std::to_string(std::rand() % 90000 + 1000);

    InitializeTunnelCallbacks();

    if (!signaling_client->InitializeFramework()) {
        SPDLOG_ERROR("Signaling setup initialization failed.");
        return -1;
    }
    SPDLOG_INFO("Successfully initialized HTTP signaling client.");
    SPDLOG_INFO("Your Client Identity Token: [{}]", global_client_id);

    std::string saved_token_id = "";
    bool saved_remote_usb = false;
    std::string saved_log_level = "info";
    signaling_client->LoadConfigFromDisk(saved_token_id, saved_remote_usb, saved_log_level, "config.txt");

    std::string input_token;
    if (!saved_token_id.empty() && saved_token_id != "Email_TO_GET_Tocken_ID") {
        char use_saved = 'y';
        std::cout << "Found cached token identifier: [" << saved_token_id << "]\n";
        std::cout << "Use saved token? (y/n): ";
        std::cin >> use_saved;
        if (use_saved == 'y' || use_saved == 'Y') {
            input_token = saved_token_id;
            remote_usb_mode = saved_remote_usb;
        }
        std::cin.ignore();
    }

    if (input_token.empty()) {
        std::cout << "Enter target token identifier string (or leave blank to generate a new one): ";
        std::getline(std::cin, input_token);
    }

    if (input_token.empty()) {
        active_token_id = signaling_client->GenerateRandomRoomId(20);
        SPDLOG_INFO("[Token Generation] Generated fresh token key: {}", active_token_id);
    } else {
        active_token_id = input_token;
    }

    signaling_client->SaveConfigToDisk(active_token_id, remote_usb_mode, "info", "config.txt");

    signaling_client->PurgeOldMessages(active_token_id);

    signaling_client->RegisterOnPeerLeftEvent([&](const std::string& peer_client_id) {
        SPDLOG_ERROR("=================================================================");
        SPDLOG_ERROR("[EVENT NOTICE] Peer [{}] has disconnected or left the session.", peer_client_id);
        SPDLOG_ERROR("=================================================================");
#ifdef TUNNEL
        tunnel_manager.Stop();
#endif
        should_reset_rtc = true;
    });

    signaling_client->RegisterOnSdpReceivedEvent([&](const std::string& sender_id, const std::string& type, const std::string& sdp) {
        SPDLOG_INFO("[Signaling Event] Received remote SDP {} from peer: {}", type, sender_id);
        if (sender_id == global_client_id) return;

        if (!rtc_connection) {
            InitializeAndStartWebRtc(false, sender_id);
        }

        if (rtc_connection) {
            rtc_connection->SetRemotePeerId(sender_id);
            rtc_connection->HandleRemoteSdp(type, sdp);
        }
    });

    signaling_client->RegisterOnIceReceivedEvent([&](const std::string& sender_id, const std::string& mid, int mline_index, const std::string& sdp) {
        SPDLOG_INFO("[Signaling Event] Received remote ICE candidate from peer: {}", sender_id);
        if (sender_id == global_client_id) return;

        if (!rtc_connection) {
            InitializeAndStartWebRtc(false, sender_id);
        }

        if (rtc_connection) {
            rtc_connection->SetRemotePeerId(sender_id);
            rtc_connection->HandleRemoteIceCandidate(mid, mline_index, sdp);
        }
    });

    signaling_client->ListenForTargetedMessages(active_token_id, global_client_id);

    std::atomic<bool> role_resolved{false};
    signaling_client->JoinRoom(active_token_id, global_client_id, [&](bool is_caller, const std::string& remote_peer) {
        is_caller_role = is_caller;
        target_peer_id = remote_peer;
        role_resolved = true;

        SPDLOG_INFO("[Role Assigned] Is Caller: {}, Target Peer ID: {}", is_caller ? "TRUE" : "FALSE", target_peer_id.empty() ? "(None / Waiting)" : target_peer_id);

        if (target_peer_id.empty()) {
            SPDLOG_INFO("[Waiting Room] You are first in the room. Waiting for a peer to join...");
            return;
        }

        InitializeAndStartWebRtc(is_caller_role, target_peer_id);
    });

    while (keep_running && !role_resolved) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "\n=======================================================" << std::endl;
    std::cout << "DATA CHANNEL MESSAGE RULES:" << std::endl;
    std::cout << " -> Type any text to send directly via WebRTC DataChannel." << std::endl;
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
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        if (should_reset_rtc.exchange(false)) {
            if (rtc_connection) {
                rtc_connection->ShutdownConnection();
                rtc_connection.reset();
                SPDLOG_INFO("[WebRtc Manager] Connection instance safely shutdown and reset.");
            }
        }

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
#ifndef TUNNEL
            if (rtc_connection && rtc_connection->IsDataChannelOpen()) {
                auto dc = rtc_connection->GetDataChannel();
                if (dc) {
                    webrtc::DataBuffer data_buffer(active_msg);
                    dc->Send(data_buffer);
                    SPDLOG_INFO("[DataChannel Sent]: {}", active_msg);
                }
            } else {
                SPDLOG_WARN("DataChannel is not open yet! Cannot send message.");
            }
#else
            size_t delimiter_pos = active_msg.find(':');
            if (delimiter_pos != std::string::npos) {
                std::string target_peer = active_msg.substr(0, delimiter_pos);
                std::string text_payload = active_msg.substr(delimiter_pos + 1);
                
                nlohmann::json j;
                j["type"] = "chat";
                j["content"] = text_payload;
                
                signaling_client->SendTargetedMessage(active_token_id, global_client_id, target_peer, j.dump());
            } else {
                SPDLOG_WARN("Invalid format pattern! Remember to use 'target_id:message'.");
            }
#endif
        }
    }

    SPDLOG_INFO("Shutting down application cleanly...");

    signaling_client->HandleGracefulExit(active_token_id, global_client_id);

#ifdef TUNNEL
    tunnel_manager.Stop();
#endif

    if (rtc_connection) {
        rtc_connection->ShutdownConnection();
        rtc_connection.reset();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (input_thread.joinable()) {
        input_thread.join();
    }

    signaling_client->ShutdownFramework();
    signaling_client.reset();
    return 0;
}
