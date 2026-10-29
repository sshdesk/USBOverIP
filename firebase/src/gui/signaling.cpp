#include "signaling.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <ctime>

#include "firebase/app.h"
#include "firebase/firestore.h"
#include "firebase/util.h"

using namespace firebase::firestore;

// Define global signaling client pointer for external helper linkage
std::unique_ptr<SignalingClient> signaling_client = nullptr;

void SendTargetedMessage(const std::string& room_id, const std::string& my_client_id, 
                          const std::string& target_client_id, const std::string& text_payload) {
    if (signaling_client) {
        signaling_client->SendTargetedMessage(room_id, my_client_id, target_client_id, text_payload);
    }
}

SignalingClient::SignalingClient() = default;

SignalingClient::~SignalingClient() {
    ShutdownFramework();
}

uint64_t SignalingClient::GetCurrentEpochMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t SignalingClient::ExtractEpochMs(const FieldValue& val) const {
    if (val.is_timestamp()) {
        firebase::Timestamp ts = val.timestamp_value();
        return (ts.seconds() * 1000) + (ts.nanoseconds() / 1000000);
    } else if (val.is_integer()) {
        return val.integer_value();
    }
    return 0;
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
#if defined(WEBRTC_WIN)
    app_ = firebase::App::Create(firebase::AppOptions());
#else
    app_ = firebase::App::Create();
#endif
    if (!app_) return false;
    
    db_ = firebase::firestore::Firestore::GetInstance(app_);
    if (!db_) return false;

    firebase::firestore::Settings settings;
    settings.set_persistence_enabled(false);
    firebase::SetLogLevel(firebase::kLogLevelError);
    db_->set_settings(settings);
    
    SPDLOG_DEBUG("[Firestore Engine] High-performance real-time configuration applied successfully.");
    return true;
}

void SignalingClient::ShutdownFramework() {
    StopHeartbeat();
    if (messages_listener_reg_.is_valid()) {
        messages_listener_reg_.Remove();
    }
    db_ = nullptr;
    if (app_) {
        delete app_;
        app_ = nullptr;
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

bool SignalingClient::SaveConfigToDisk(const std::string& room_id, const std::string& log_level, const std::string& filename) {
    std::ofstream configFile(filename);
    if (!configFile.is_open()) {
        SPDLOG_WARN("Could not open {} for writing.", filename);
        return false;
    }
    configFile << "ROOM_ID=" << room_id << "\n";
    configFile << "LOG_LEVEL=" << log_level << "\n";
    return true;
}

void SignalingClient::LoadConfigFromDisk(std::string& out_room_id, std::string& out_log_level, const std::string& filename) {
    std::ifstream configFile(filename);
    out_room_id.clear();
    out_log_level = "info";

    if (!configFile.is_open()) return;

    std::string line;
    while (std::getline(configFile, line)) {
        std::istringstream is_line(line);
        std::string key;
        if (std::getline(is_line, key, '=')) {
            std::string value;
            if (std::getline(is_line, value)) {
                if (key == "ROOM_ID") out_room_id = value;
                else if (key == "LOG_LEVEL") out_log_level = value;
            }
        }
    }
}

void SignalingClient::DeleteCollectionRecursively(const CollectionReference& collection_ref) {
    collection_ref.Get().OnCompletion([collection_ref](const firebase::Future<QuerySnapshot>& future) {
        if (future.status() == firebase::kFutureStatusComplete && !future.error()) {
            auto snapshot = future.result();
            for (const auto& doc_snap : snapshot->documents()) {
                doc_snap.reference().Delete();
            }
        }
    });
}

void SignalingClient::PurgeOldMessages(const std::string& room_id) {
    if (!db_ || room_id.empty()) return;
    CollectionReference messages_ref = db_->Collection("rooms").Document(room_id).Collection("messages");
    DeleteCollectionRecursively(messages_ref);
}

void SignalingClient::StopHeartbeat() {
    heartbeat_active_ = false;
    std::unique_ptr<std::thread> old_thread;
    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        old_thread = std::move(heartbeat_thread_);
    }
    if (old_thread && old_thread->joinable()) {
        try {
            old_thread->join();
        } catch (...) {}
    }
}

void SignalingClient::StartHeartbeat(const std::string& room_id, const std::string& my_client_id) {
    StopHeartbeat();
    std::lock_guard<std::mutex> lock(heartbeat_mutex_);
    heartbeat_active_ = true;
    heartbeat_thread_ = std::make_unique<std::thread>([room_id, my_client_id, this]() {
        while (heartbeat_active_) {
            if (db_ && !room_id.empty() && !my_client_id.empty()) {
                db_->Collection("rooms").Document(room_id).Collection("peers").Document(my_client_id).Set({
                    {"lastSeen", FieldValue::ServerTimestamp()}
                });
            }
            for (int i = 0; i < 30 && heartbeat_active_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Sleep 3 seconds total
            }
        }
    });
}

void SignalingClient::JoinRoom(const std::string& room_id, const std::string& my_client_id, 
                               RoleAssignedCallback on_role_assigned) {
    if (!db_ || room_id.empty() || my_client_id.empty()) return;

    CollectionReference peers_ref = db_->Collection("rooms").Document(room_id).Collection("peers");

    // Start background presence loop
    StartHeartbeat(room_id, my_client_id);

    // Initial presence write
    peers_ref.Document(my_client_id).Set({
        {"lastSeen", FieldValue::ServerTimestamp()}
    }).OnCompletion([this, room_id, my_client_id, peers_ref, on_role_assigned](const firebase::Future<void>& set_future) {
        // Query peers collection to see who else is in the room
        peers_ref.Get().OnCompletion([this, room_id, my_client_id, on_role_assigned](const firebase::Future<QuerySnapshot>& future) {
            if (future.status() != firebase::kFutureStatusComplete || future.error()) {
                if (on_role_assigned) on_role_assigned(false, "");
                return;
            }

            auto snapshot = future.result();
            const auto& docs = snapshot->documents();
            int64_t ref_time_ms = static_cast<int64_t>(GetCurrentEpochMs());

            std::string existing_peer_id = "";
            size_t active_other_peers = 0;

            for (const auto& doc : docs) {
                std::string doc_id = doc.id();
                if (doc_id == my_client_id) continue;

                MapFieldValue data = doc.GetData();
                bool is_stale = true;

                if (data.find("lastSeen") != data.end()) {
                    const FieldValue& val = data["lastSeen"];
                    if (!val.is_null()) {
                        int64_t last_seen = ExtractEpochMs(val);
                        int64_t diff = ref_time_ms - last_seen;
                        // Since heartbeat runs every 3s, any peer updated within 20s is alive
                        if (last_seen > 0 && diff < 20000) {
                            is_stale = false;
                        }
                    }
                }

                if (is_stale) {
                    SPDLOG_WARN("[JoinRoom] Purging stale peer document: {}", doc_id);
                    doc.reference().Delete();
                } else {
                    existing_peer_id = doc_id;
                    active_other_peers++;
                }
            }

            if (active_other_peers == 0) {
                // First peer to join room: purge previous stale room messages and stand by as Callee
                PurgeOldMessages(room_id);
                SPDLOG_INFO("[JoinRoom] First peer in room [{}]. Role -> CALLEE (waiting for offer).", room_id);
                if (on_role_assigned) on_role_assigned(false, "");
            } else {
                // Second peer to join: act as Caller and send offer to existing_peer_id
                SPDLOG_INFO("[JoinRoom] Found active peer [{}] in room [{}]. Role -> CALLER.", existing_peer_id, room_id);
                if (on_role_assigned) on_role_assigned(true, existing_peer_id);
            }
        });
    });
}

void SignalingClient::RegisterOnPeerLeftEvent(PeerLeftCallback callback) { peer_left_callback_ = callback; }
void SignalingClient::RegisterOnSdpReceivedEvent(SdpReceivedCallback callback) { sdp_received_callback_ = callback; }
void SignalingClient::RegisterOnIceReceivedEvent(IceReceivedCallback callback) { ice_received_callback_ = callback; }

void SignalingClient::HandleGracefulExit(const std::string& room_id, const std::string& my_client_id) {
    if (!db_ || room_id.empty()) return;
    StopHeartbeat();
    SendTargetedMessage(room_id, my_client_id, "ALL", "SYSTEM_EVENT: Goodbye! I have closed my connection.");
    if (!my_client_id.empty()) {
        db_->Collection("rooms").Document(room_id).Collection("peers").Document(my_client_id).Delete();
    }
}

void SignalingClient::ListenForTargetedMessages(const std::string& room_id, const std::string& my_client_id) {
    if (!db_ || room_id.empty()) return;

    int64_t session_start_ms = static_cast<int64_t>(GetCurrentEpochMs()) - 1000;

    CollectionReference messages_ref = db_->Collection("rooms").Document(room_id).Collection("messages");
    std::vector<FieldValue> allowed_targets = { FieldValue::String("ALL"), FieldValue::String(my_client_id) };
    Query targeted_query = messages_ref.WhereIn("targetId", allowed_targets);

    if (messages_listener_reg_.is_valid()) messages_listener_reg_.Remove();

    messages_listener_reg_ = targeted_query.AddSnapshotListener([room_id, my_client_id, session_start_ms, this](const QuerySnapshot& snapshot, Error error, const std::string& error_msg) {
        if (error != kErrorOk) {
            SPDLOG_ERROR("Snapshot listener error: {}", error_msg);
            return;
        }
        for (const auto& change : snapshot.DocumentChanges()) {
            if (change.type() == DocumentChange::Type::kAdded) {
                MapFieldValue data = change.document().GetData();
                std::string sender_id = data["senderId"].string_value();
                std::string target_id = data["targetId"].string_value();
                std::string text_body = data["text"].string_value();
                
                if (sender_id == my_client_id) continue;

                if (data.find("timestamp") != data.end()) {
                    int64_t msg_time = ExtractEpochMs(data["timestamp"]);
                    if (msg_time > 0 && msg_time < session_start_ms) {
                        continue;
                    }
                }

                SPDLOG_INFO("[Message Received] sender_id: [{}], target_id: [{}], my_client_id: [{}]", 
                            sender_id, target_id, my_client_id);

                if (text_body == "SYSTEM_EVENT: Goodbye! I have closed my connection.") {
                    if (peer_left_callback_) peer_left_callback_(sender_id);
                    continue;
                }
                if (text_body.rfind("SDP_OFFER:", 0) == 0) {
                    if (sdp_received_callback_) sdp_received_callback_(sender_id, "offer", text_body.substr(10));
                    continue;
                }
                if (text_body.rfind("SDP_ANSWER:", 0) == 0) {
                    if (sdp_received_callback_) sdp_received_callback_(sender_id, "answer", text_body.substr(11));
                    continue;
                }
                if (text_body.rfind("ICE:", 0) == 0) {
                    if (ice_received_callback_) {
                        std::stringstream ss(text_body.substr(4));
                        std::string mid, index_str, candidate_sdp;
                        std::getline(ss, mid, ':');
                        std::getline(ss, index_str, ':');
                        std::getline(ss, candidate_sdp, '\0');
                        try {
                            ice_received_callback_(sender_id, mid, std::stoi(index_str), candidate_sdp);
                        } catch (...) {}
                    }
                    continue;
                }
            }
        }
    });
}

void SignalingClient::SendTargetedMessage(const std::string& room_id, const std::string& my_client_id, 
                                          const std::string& target_client_id, const std::string& text_payload) {
    if (!db_ || room_id.empty() || my_client_id.empty()) return;
    CollectionReference messages_ref = db_->Collection("rooms").Document(room_id).Collection("messages");

    MapFieldValue msg_map = {
        {"senderId",  FieldValue::String(my_client_id)},
        {"targetId",  FieldValue::String(target_client_id)},
        {"text",      FieldValue::String(text_payload)},
        {"timestamp", FieldValue::ServerTimestamp()}
    };
    messages_ref.Add(msg_map);
}