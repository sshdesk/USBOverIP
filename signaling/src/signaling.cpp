#include "signaling.h"
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <regex>
#include "json.hpp"

using json = nlohmann::json;

std::unique_ptr<SignalingClient> signaling_client = nullptr;

void SendTargetedMessage(const std::string& token_id, const std::string& my_client_id, 
                          const std::string& target_client_id, const std::string& text_payload) {
    if (signaling_client) {
        signaling_client->SendTargetedMessage(token_id, my_client_id, target_client_id, text_payload);
    }
}

static void ParseEndpoint(const std::string& endpoint, std::string& out_host, std::string& out_path_prefix) {
    std::regex rx("(https?://[^/]+)(/.*)?");
    std::smatch match;
    if (std::regex_match(endpoint, match, rx)) {
        out_host = match[1];
        out_path_prefix = match[2].matched ? match[2].str() : "";
        if (!out_path_prefix.empty() && out_path_prefix.back() == '/') {
            out_path_prefix.pop_back();
        }
    } else {
        out_host = endpoint;
        out_path_prefix = "";
    }
}

SignalingClient::SignalingClient() = default;

SignalingClient::~SignalingClient() {
    ShutdownFramework();
}

void SignalingClient::SetupLogging(const std::string& level_str) {
    std::string level = level_str;
    std::transform(level.begin(), level.end(), level.begin(), ::tolower);
    if (level == "debug") spdlog::set_level(spdlog::level::debug);
    else if (level == "trace") spdlog::set_level(spdlog::level::trace);
    else spdlog::set_level(spdlog::level::info);
    active_log_level_ = level;
}

bool SignalingClient::InitializeFramework() {
    SPDLOG_INFO("[AWS Amplify Signaling] Initializing HTTP signaling client targeting: {}", api_endpoint_);
    return true;
}

void SignalingClient::ShutdownFramework() {
    StopHeartbeat();
    polling_active_ = false;
    if (polling_thread_ && polling_thread_->joinable()) {
        try { polling_thread_->join(); } catch (...) {}
    }
}

std::string SignalingClient::GenerateRandomRoomId(size_t length) {
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::string random_id;
    random_id.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        random_id += chars[std::rand() % chars.length()];
    }
    return random_id;
}


bool SignalingClient::SaveConfigToDisk(const std::string& token_id, bool remote_usb, const std::string& log_level, const std::string& filename) {
    nlohmann::json j;
    j["TOKEN_ID"] = token_id;
    j["REMOTE_USB"] = remote_usb;
    j["LOG_LEVEL"] = log_level;

    std::ofstream configFile(filename);
    if (!configFile.is_open()) return false;
    configFile << j.dump(4);
    return true;
}

void SignalingClient::LoadConfigFromDisk(std::string& out_room_id, bool& out_remote_usb, std::string& out_log_level, const std::string& filename) {
    out_room_id = "";
    out_remote_usb = false;
    out_log_level = "info";

    std::ifstream configFile(filename);
    if (!configFile.is_open()) return;

    try {
        nlohmann::json j;
        configFile >> j;
        
        if (j.contains("TOKEN_ID") && j["TOKEN_ID"].is_string()) {
            out_room_id = j["TOKEN_ID"].get<std::string>();
        }
        if (j.contains("REMOTE_USB")) {
            if (j["REMOTE_USB"].is_boolean()) {
                out_remote_usb = j["REMOTE_USB"].get<bool>();
            } else if (j["REMOTE_USB"].is_string()) {
                std::string val = j["REMOTE_USB"].get<std::string>();
                out_remote_usb = (val == "true" || val == "1");
            }
        }
        if (j.contains("LOG_LEVEL") && j["LOG_LEVEL"].is_string()) {
            out_log_level = j["LOG_LEVEL"].get<std::string>();
        }
    } catch (const std::exception& e) {
        spdlog::error("Failed to parse config JSON from {}: {}", filename, e.what());
    }
}

void SignalingClient::PurgeOldMessages(const std::string& token_id) {
    std::string host, path_prefix;
    ParseEndpoint(api_endpoint_, host, path_prefix);
    httplib::Client cli(host);

    nlohmann::json req_json;
    req_json["room_id"] = token_id;

    // Try DELETE request first to purge stale messages/clients, falling back to POST
    auto res = cli.Delete((path_prefix + "/purgeStale").c_str(), req_json.dump(), "application/json");
    if (!res || res->status != 200) {
        cli.Post((path_prefix + "/purgeStale").c_str(), req_json.dump(), "application/json");
    }
}

void SignalingClient::StopHeartbeat() {
    heartbeat_active_ = false;
    std::unique_ptr<std::thread> old_thread;
    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        old_thread = std::move(heartbeat_thread_);
    }
    if (old_thread && old_thread->joinable()) {
        try { old_thread->join(); } catch (...) {}
    }

    SPDLOG_INFO("Stopped Heartbeat");
}

void SignalingClient::StartHeartbeat(const std::string& token_id, const std::string& my_client_id) {
    StopHeartbeat();
    std::lock_guard<std::mutex> lock(heartbeat_mutex_);
    heartbeat_active_ = true;
    heartbeat_thread_ = std::make_unique<std::thread>([token_id, my_client_id, this]() {
        while (heartbeat_active_) {
            for (int i = 0; i < 30 && heartbeat_active_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}

void SignalingClient::JoinRoom(const std::string& token_id, const std::string& my_client_id, 
                               RoleAssignedCallback on_role_assigned) {
    StartHeartbeat(token_id, my_client_id);

    std::string host, path_prefix;
    ParseEndpoint(api_endpoint_, host, path_prefix);

    httplib::Client cli(host);
    std::string payload = "{\"room_id\":\"" + token_id + "\",\"client_id\":\"" + my_client_id + "\"}";
    
    if (auto res = cli.Post((path_prefix + "/join").c_str(), payload, "application/json")) {
        if (res->status == 200) {
            auto j = json::parse(res->body);
            bool is_caller = j.value("is_caller", false);
            std::string remote_peer = j.value("remote_peer_id", "");
            if (on_role_assigned) on_role_assigned(is_caller, remote_peer);
            return;
        }
    }

    if (on_role_assigned) on_role_assigned(false, "");
}

void SignalingClient::RegisterOnPeerLeftEvent(PeerLeftCallback callback) { peer_left_callback_ = callback; }
void SignalingClient::RegisterOnSdpReceivedEvent(SdpReceivedCallback callback) { sdp_received_callback_ = callback; }
void SignalingClient::RegisterOnIceReceivedEvent(IceReceivedCallback callback) { ice_received_callback_ = callback; }



void SignalingClient::HandleGracefulExit(const std::string& token_id, const std::string& my_client_id) {
    StopHeartbeat();

    std::string host, path_prefix;
    ParseEndpoint(api_endpoint_, host, path_prefix);
    httplib::Client cli(host);

    nlohmann::json req_json;
    req_json["room_id"] = token_id;
    req_json["client_id"] = my_client_id;

    bool deletion_confirmed = false;

    // 1. Try DELETE request and verify response status
    auto res = cli.Delete((path_prefix + "/leave").c_str(), req_json.dump(), "application/json");
    if (res && (res->status == 200 || res->status == 204)) {
        SPDLOG_INFO("Successfully confirmed deletion of client [{}] from database via DELETE.", my_client_id);
        deletion_confirmed = true;
    } else {
        SPDLOG_WARN("DELETE /leave failed or returned status {}. Trying POST fallback...", res ? res->status : -1);
        
        // 2. Fallback to POST request and verify response status
        auto post_res = cli.Post((path_prefix + "/leave").c_str(), req_json.dump(), "application/json");
        if (post_res && (post_res->status == 200 || post_res->status == 204)) {
            SPDLOG_INFO("Successfully confirmed deletion of client [{}] from database via POST fallback.", my_client_id);
            deletion_confirmed = true;
        } else {
            SPDLOG_ERROR("CRITICAL: Failed to confirm deletion of client [{}] from database. Server status: {}", 
                         my_client_id, post_res ? post_res->status : -1);
        }
    }

    // Broadcast graceful exit notification to active peers
    nlohmann::json j;
    j["type"] = "system";
    j["message"] = "Goodbye! I have closed my connection.";
    SendTargetedMessage(token_id, my_client_id, "ALL", j.dump());

    SPDLOG_INFO("HandleGracefulExit");
}



void SignalingClient::ListenForTargetedMessages(const std::string& token_id, const std::string& my_client_id) {
    polling_active_ = true;
    polling_thread_ = std::make_unique<std::thread>([token_id, my_client_id, this]() {
        std::string host, path_prefix;
        ParseEndpoint(api_endpoint_, host, path_prefix);
        httplib::Client cli(host);

        while (polling_active_) {
            std::string path = path_prefix + "/messages?room_id=" + token_id + "&client_id=" + my_client_id;
            if (auto res = cli.Get(path.c_str())) {
                if (res->status == 200 && !res->body.empty()) {
                    auto messages = json::parse(res->body);
                    for (const auto& msg : messages) {
                        std::string sender = msg.value("sender_id", "");
                        
                        try {
                            json payload_json;
                            if (msg.contains("text") && msg["text"].is_object()) {
                                payload_json = msg["text"];
                            } else if (msg.contains("text") && msg["text"].is_string()) {
                                std::string raw_text = msg["text"].get<std::string>();
                                // Check if it's a valid JSON string before parsing
                                if (!raw_text.empty() && raw_text.front() == '{') {
                                    payload_json = json::parse(raw_text);
                                } else {
                                    SPDLOG_INFO("Skipping non-JSON system message: {}", raw_text);
                                    continue;
                                }
                            }

                            std::string msg_type = payload_json.value("type", "");

                            if ((msg_type == "offer" || msg_type == "answer") && sdp_received_callback_) {
                                std::string sdp_content = payload_json.value("sdp", "");
                                sdp_received_callback_(sender, msg_type, sdp_content);
                            } 
                            else if (msg_type == "ice" && ice_received_callback_) {
                                std::string mid = payload_json.value("mid", "");
                                int mline_index = payload_json.value("mline_index", 0);
                                std::string candidate_str = payload_json.value("candidate", "");
                                
                                ice_received_callback_(sender, mid, mline_index, candidate_str);
                            }
                        } catch (const std::exception& e) {
                            SPDLOG_ERROR("Failed to parse incoming message payload as JSON: {}", e.what());
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
}

void SignalingClient::SendTargetedMessage(const std::string& token_id, const std::string& my_client_id, 
                                          const std::string& target_client_id, const std::string& text_payload) {
    std::string host, path_prefix;
    ParseEndpoint(api_endpoint_, host, path_prefix);
    httplib::Client cli(host);

    json req_json;
    req_json["room_id"] = token_id;
    req_json["sender_id"] = my_client_id;
    req_json["target_id"] = target_client_id;

    // Ensure text_payload is nested as a JSON object if it's a valid JSON string
    try {
        req_json["text"] = json::parse(text_payload);
    } catch (...) {
        req_json["text"] = text_payload;
    }

    cli.Post((path_prefix + "/messages").c_str(), req_json.dump(), "application/json");
}