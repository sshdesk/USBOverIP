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
#include <future>
#include <thread>
#include <atomic>

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
    if (users_listener_reg_.is_valid()) {
        users_listener_reg_.Remove();
    }
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

void SignalingClient::SetUserPresence(const std::string& room_id, const std::string& my_client_id, bool is_online) {
    if (!db_ || room_id.empty() || my_client_id.empty()) return;
    DocumentReference user_doc = db_->Collection("rooms").Document(room_id).Collection("users").Document(my_client_id);
    if (is_online) {
        user_doc.Set({
            {"status", FieldValue::String("online")}, 
            {"lastSeen", FieldValue::ServerTimestamp()}
        });
    } else {
        user_doc.Delete();
    }
}

void SignalingClient::DeleteCollectionRecursively(const CollectionReference& collection_ref) {
    collection_ref.Get().OnCompletion([collection_ref](const firebase::Future<QuerySnapshot>& future) {
        if (future.status() == firebase::kFutureStatusComplete && !future.error()) {
            auto snapshot = future.result();
            for (const auto& doc_snap : snapshot->documents()) {
                DocumentReference doc_ref = doc_snap.reference();
                doc_ref.Delete();
            }
        }
    });
}

void SignalingClient::PurgeOldMessages(const std::string& room_id) {
    if (!db_ || room_id.empty()) return;
    CollectionReference messages_ref = db_->Collection("rooms").Document(room_id).Collection("messages");
    DeleteCollectionRecursively(messages_ref);
}

bool SignalingClient::VerifyAndReserveRoomCapacity(const std::string& room_id, const std::string& my_client_id) {
    if (!db_ || room_id.empty()) return false;
    
    PurgeOldMessages(room_id);

    auto capacity_promise = std::make_shared<std::promise<bool>>();
    std::future<bool> capacity_future = capacity_promise->get_future();

    CollectionReference users_ref = db_->Collection("rooms").Document(room_id).Collection("users");

    users_ref.Get().OnCompletion([capacity_promise, room_id, my_client_id, users_ref, this](const firebase::Future<QuerySnapshot>& future) {
        if (future.status() != firebase::kFutureStatusComplete || future.error()) {
            capacity_promise->set_value(false);
            return;
        }

        auto snapshot = future.result();
        const auto& docs = snapshot->documents();

        int64_t ref_time_ms = static_cast<int64_t>(GetCurrentEpochMs());
        std::vector<DocumentReference> docs_to_delete;
        size_t active_other_count = 0;

        for (const auto& doc : docs) {
            std::string doc_id = doc.id();
            if (!my_client_id.empty() && doc_id == my_client_id) {
                continue; 
            }

            MapFieldValue data = doc.GetData();
            bool is_stale = true;

            if (data.find("lastSeen") != data.end()) {
                const FieldValue& val = data["lastSeen"];
                if (!val.is_null()) {
                    int64_t last_seen = ExtractEpochMs(val);
                    int64_t diff = ref_time_ms - last_seen;
                    if (std::abs(diff) < 30000) {
                        is_stale = false; 
                    }
                }
            }

            if (is_stale) {
                SPDLOG_WARN("[Room Capacity] Purging stale client document: {}", doc_id);
                docs_to_delete.push_back(doc.reference());
            } else {
                active_other_count++;
            }
        }

        auto evaluate_capacity = [capacity_promise, active_other_count, room_id, my_client_id, this]() {
            bool allowed = (active_other_count < 2);
            if (allowed) {
                SetUserPresence(room_id, my_client_id, true);
                StartHeartbeat(room_id, my_client_id);
            } else {
                SPDLOG_ERROR("[Room Capacity] Access Denied: Room is full (Active peers: {})", active_other_count);
            }
            capacity_promise->set_value(allowed);
        };

        if (!docs_to_delete.empty()) {
            auto counter = std::make_shared<std::atomic<int>>(static_cast<int>(docs_to_delete.size()));
            for (auto doc_ref : docs_to_delete) {
                doc_ref.Delete().OnCompletion([counter, evaluate_capacity](const firebase::Future<void>& del_future) {
                    if (--(*counter) == 0) {
                        evaluate_capacity();
                    }
                });
            }
        } else {
            evaluate_capacity();
        }
    });

    if (capacity_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        return capacity_future.get();
    }
    
    SPDLOG_ERROR("[Room Capacity] Timeout waiting for Firestore query response.");
    return false;
}

void SignalingClient::RegisterOnRoomReadyEvent(RoomReadyCallback callback) { room_ready_callback_ = callback; }
void SignalingClient::RegisterOnPeerLeftEvent(PeerLeftCallback callback) { peer_left_callback_ = callback; }
void SignalingClient::RegisterOnSdpReceivedEvent(SdpReceivedCallback callback) { sdp_received_callback_ = callback; }
void SignalingClient::RegisterOnIceReceivedEvent(IceReceivedCallback callback) { ice_received_callback_ = callback; }

void SignalingClient::HandleGracefulExit(const std::string& room_id, const std::string& my_client_id) {
    if (!db_ || room_id.empty()) return;
    
    StopHeartbeat();
    
    SendTargetedMessage(room_id, my_client_id, "ALL", "SYSTEM_EVENT: Goodbye! I have closed my connection.");
    SetUserPresence(room_id, my_client_id, false);
    
    DocumentReference room_doc = db_->Collection("rooms").Document(room_id);
    CollectionReference users_ref = room_doc.Collection("users");
    CollectionReference messages_ref = room_doc.Collection("messages");

    users_ref.Get().OnCompletion([room_id, room_doc, users_ref, messages_ref](const firebase::Future<QuerySnapshot>& future) mutable {
        if (future.status() == firebase::kFutureStatusComplete && !future.error()) {
            auto snapshot = future.result();
            size_t remaining_users = snapshot->documents().size();
                        
            if (remaining_users == 0) {
                SPDLOG_WARN("[Room Watchdog] Last user left. Purging room database trace completely...");
                DeleteCollectionRecursively(messages_ref);
                DeleteCollectionRecursively(users_ref);
                room_doc.Delete();
            }
        }
    });
}

void SignalingClient::ListenForTargetedMessages(const std::string& room_id, const std::string& my_client_id) {
    if (!db_ || room_id.empty()) return;
    room_ready_fired_ = false;

    int64_t session_start_ms = static_cast<int64_t>(GetCurrentEpochMs()) - 30000;

    CollectionReference messages_ref = db_->Collection("rooms").Document(room_id).Collection("messages");
    CollectionReference users_ref = db_->Collection("rooms").Document(room_id).Collection("users");

    std::vector<FieldValue> allowed_targets = { FieldValue::String("ALL"), FieldValue::String(my_client_id) };
    Query targeted_query = messages_ref.WhereIn("targetId", allowed_targets);

    if (users_listener_reg_.is_valid()) users_listener_reg_.Remove();
    if (messages_listener_reg_.is_valid()) messages_listener_reg_.Remove();

    users_listener_reg_ = users_ref.AddSnapshotListener([room_id, my_client_id, this](const QuerySnapshot& snapshot, Error error, const std::string& error_msg) {
        if (error != kErrorOk) return;
        for (const auto& change : snapshot.DocumentChanges()) {
            std::string user_id = change.document().id();
            if (user_id == my_client_id) continue;
            
            if (change.type() == DocumentChange::Type::kAdded) {
                SPDLOG_INFO("[ROOM PRESENCE] Peer entered room active pool: {}", user_id);
                if (snapshot.documents().size() >= 2 && !room_ready_fired_.exchange(true)) {
                    SendTargetedMessage(room_id, my_client_id, "ALL", "SYSTEM_EVENT: ROOM_FULL_READY_FOR_WEBRTC");
                    if (room_ready_callback_) room_ready_callback_(room_id);
                }
            } else if (change.type() == DocumentChange::Type::kRemoved) {
                SPDLOG_INFO("[ROOM PRESENCE] Peer left room active pool: {}", user_id);
                room_ready_fired_ = false;
                if (peer_left_callback_) peer_left_callback_(user_id);
            }
        }
    });

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

                if (text_body == "SYSTEM_EVENT: ROOM_FULL_READY_FOR_WEBRTC") {
                    if (!room_ready_fired_.exchange(true)) {
                        if (room_ready_callback_) room_ready_callback_(room_id);
                    }
                    continue;
                }
                if (text_body == "SYSTEM_EVENT: Goodbye! I have closed my connection.") {
                    room_ready_fired_ = false;
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

    users_ref.Get().OnCompletion([room_id, my_client_id, this](const firebase::Future<QuerySnapshot>& future) {
        if (future.status() == firebase::kFutureStatusComplete && !future.error()) {
            auto snapshot = future.result();
            size_t occupancy = snapshot->documents().size();

            bool has_remote_peer = false;
            for (const auto& doc_snap : snapshot->documents()) {
                if (doc_snap.id() != my_client_id) {
                    has_remote_peer = true;
                    break;
                }
            }

            if (occupancy >= 2 && has_remote_peer) {
                if (!room_ready_fired_.exchange(true)) {
                    SendTargetedMessage(room_id, my_client_id, "ALL", "SYSTEM_EVENT: ROOM_FULL_READY_FOR_WEBRTC");
                    if (room_ready_callback_) {
                        room_ready_callback_(room_id);
                    }
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

int SignalingClient::GetActiveClientCount(const std::string& room_id, const std::string& my_client_id) {
    if (!db_ || room_id.empty()) return 0;
    
    auto active_users_promise = std::make_shared<std::promise<size_t>>();
    std::future<size_t> active_users_future = active_users_promise->get_future();

    CollectionReference users_ref = db_->Collection("rooms").Document(room_id).Collection("users");

    users_ref.Get().OnCompletion([active_users_promise, my_client_id, this](const firebase::Future<QuerySnapshot>& future) {
        if (future.status() != firebase::kFutureStatusComplete || future.error()) {
            active_users_promise->set_value(0);
            return;
        }

        auto snapshot = future.result();
        const auto& docs = snapshot->documents();

        int64_t ref_time_ms = static_cast<int64_t>(GetCurrentEpochMs());
        std::vector<DocumentReference> docs_to_delete;
        size_t active_count = 0;

        for (const auto& doc : docs) {
            std::string doc_id = doc.id();
            if (!my_client_id.empty() && doc_id == my_client_id) {
                continue;
            }

            MapFieldValue data = doc.GetData();
            bool is_stale = true;

            if (data.find("lastSeen") != data.end()) {
                const FieldValue& val = data["lastSeen"];
                if (!val.is_null()) {
                    int64_t last_seen = ExtractEpochMs(val);
                    int64_t diff = ref_time_ms - last_seen;
                    if (std::abs(diff) < 30000) {
                        is_stale = false;
                    }
                }
            }

            if (is_stale) {
                SPDLOG_WARN("[Active Count] Purging stale client document: {}", doc_id);
                docs_to_delete.push_back(doc.reference());
            } else {
                active_count++;
            }
        }

        auto finalize_count = [active_users_promise, active_count]() {
            active_users_promise->set_value(active_count);
        };

        if (!docs_to_delete.empty()) {
            auto counter = std::make_shared<std::atomic<int>>(static_cast<int>(docs_to_delete.size()));
            for (auto doc_ref : docs_to_delete) {
                doc_ref.Delete().OnCompletion([counter, finalize_count](const firebase::Future<void>& del_future) {
                    if (--(*counter) == 0) {
                        finalize_count();
                    }
                });
            }
        } else {
            finalize_count();
        }
    });

    if (active_users_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        return static_cast<int>(active_users_future.get());
    }
    return 0;
}

bool SignalingClient::IsRoomEmpty(const std::string& room_id, const std::string& my_client_id) {
    int count = GetActiveClientCount(room_id, my_client_id);
    SPDLOG_INFO("[Room Check] Room [{}] other active client count: {}", room_id, count);
    return (count == 0);
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
            SetUserPresence(room_id, my_client_id, true);
            for (int i = 0; i < 100 && heartbeat_active_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}
