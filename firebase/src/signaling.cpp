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

#include "firebase/app.h"
#include "firebase/firestore.h"
#include "firebase/util.h"

firebase::firestore::Firestore* db = nullptr;
std::string active_log_level = "info";

using namespace firebase::firestore;

static RoomReadyCallback global_room_ready_callback = nullptr;
static PeerLeftCallback global_peer_left_callback = nullptr;
static SdpReceivedCallback global_sdp_received_callback = nullptr;
static IceReceivedCallback global_ice_received_callback = nullptr;

uint64_t GetCurrentEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void SetupLogging(const std::string& level_str) {
    std::string level = level_str;
    std::transform(level.begin(), level.end(), level.begin(), ::tolower);
    if (level == "debug") spdlog::set_level(spdlog::level::debug);
    else if (level == "trace") spdlog::set_level(spdlog::level::trace);
    else spdlog::set_level(spdlog::level::info);
    active_log_level = level;
}

bool InitializeFirebaseFramework() {
#if defined(WEBRTC_WIN)
    firebase::App* app = firebase::App::Create(firebase::AppOptions());
#else
    firebase::App* app = firebase::App::Create();
#endif
    if (!app) return false;
    
    db = firebase::firestore::Firestore::GetInstance(app);
    if (!db) return false;

    firebase::firestore::Settings settings;
    settings.set_persistence_enabled(false);
    firebase::SetLogLevel(firebase::kLogLevelError);
    db->set_settings(settings);
    
    SPDLOG_INFO("[Firestore Engine] High-performance real-time configuration applied successfully.");
    return true;
}

void ShutdownFirebaseFramework(firebase::App* app) {
    if (db) db = nullptr;
    if (app) delete app;
}

std::string GenerateRandomRoomId(size_t length) {
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::string random_id;
    random_id.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        random_id += chars[std::rand() % chars.length()];
    }
    return random_id;
}

bool SaveConfigToDisk(const std::string& room_id, const std::string& log_level, const std::string& filename) {
    std::ofstream configFile(filename);
    if (!configFile.is_open()) {
        SPDLOG_WARN("Could not open {} for writing.", filename);
        return false;
    }
    configFile << "ROOM_ID=" << room_id << "\n";
    configFile << "LOG_LEVEL=" << log_level << "\n";
    return true;
}

void LoadConfigFromDisk(std::string& out_room_id, std::string& out_log_level, const std::string& filename) {
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

void SetUserPresence(const std::string& room_id, const std::string& my_client_id, bool is_online) {
    if (!db) return;
    DocumentReference user_doc = db->Collection("rooms").Document(room_id).Collection("users").Document(my_client_id);
    if (is_online) {
        user_doc.Set({{"status", FieldValue::String("online")}, {"lastSeen", FieldValue::Integer(GetCurrentEpochMs())}});
    } else {
        user_doc.Delete();
    }
}

void DeleteCollectionRecursively(const CollectionReference& collection_ref) {
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

bool VerifyAndReserveRoomCapacity(const std::string& room_id) {
    if (!db) return false;
    auto active_users_promise = std::make_shared<std::promise<size_t>>();
    std::future<size_t> active_users_future = active_users_promise->get_future();

    CollectionReference users_ref = db->Collection("rooms").Document(room_id).Collection("users");
    uint64_t current_time = GetCurrentEpochMs();

    users_ref.Get().OnCompletion([active_users_promise, current_time](const firebase::Future<QuerySnapshot>& future) {
        if (future.status() == firebase::kFutureStatusComplete && !future.error()) {
            size_t active_count = 0;
            for (const auto& doc : future.result()->documents()) {
                MapFieldValue data = doc.GetData();
                bool is_stale = true;
                
                if (data.find("lastSeen") != data.end()) {
                    uint64_t last_seen = data["lastSeen"].integer_value();
                    
                    int64_t diff = static_cast<int64_t>(current_time) - static_cast<int64_t>(last_seen);
                    if (std::abs(diff) < 30000) { // 30 seconds threshold
                        is_stale = false;
                    }
                }

                if (is_stale) {
                    SPDLOG_WARN("[Room Capacity] Purging stale client document: {}", doc.id());
                    doc.reference().Delete();
                } else {
                    active_count++;
                }
            }
            active_users_promise->set_value(active_count);
        } else {
            active_users_promise->set_value(0);
        }
    });

    active_users_future.wait();
    size_t current_occupancy = active_users_future.get();
    return current_occupancy < 2;
}

void RegisterOnRoomReadyEvent(RoomReadyCallback callback) { global_room_ready_callback = callback; }
void RegisterOnPeerLeftEvent(PeerLeftCallback callback) { global_peer_left_callback = callback; }
void RegisterOnSdpReceivedEvent(SdpReceivedCallback callback) { global_sdp_received_callback = callback; }
void RegisterOnIceReceivedEvent(IceReceivedCallback callback) { global_ice_received_callback = callback; }

void HandleGracefulExit(const std::string& room_id, const std::string& my_client_id) {
    if (!db) return;
    SPDLOG_INFO("Cleaning up Firestore presence nodes for client {}...", my_client_id);
    
    SendTargetedMessage(room_id, my_client_id, "ALL", "SYSTEM_EVENT: Goodbye! I have closed my connection.");
    SetUserPresence(room_id, my_client_id, false);
    
    DocumentReference room_doc = db->Collection("rooms").Document(room_id);
    CollectionReference users_ref = room_doc.Collection("users");
    CollectionReference messages_ref = room_doc.Collection("messages");

    users_ref.Get().OnCompletion([room_id, room_doc, users_ref, messages_ref](const firebase::Future<QuerySnapshot>& future) mutable {
        if (future.status() == firebase::kFutureStatusComplete && !future.error()) {
            auto snapshot = future.result();
            size_t remaining_users = snapshot->documents().size();
            
            SPDLOG_INFO("[Room Watchdog] Remaining active peers in room: {}", remaining_users);
            
            if (remaining_users == 0) {
                SPDLOG_WARN("[Room Watchdog] Last user left. Purging room database trace completely...");
                DeleteCollectionRecursively(messages_ref);
                DeleteCollectionRecursively(users_ref);
                room_doc.Delete();
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

void ListenForTargetedMessages(const std::string& room_id, const std::string& my_client_id) {
    if (!db) return;
    uint64_t app_boot_time = GetCurrentEpochMs();

    CollectionReference messages_ref = db->Collection("rooms").Document(room_id).Collection("messages");
    CollectionReference users_ref = db->Collection("rooms").Document(room_id).Collection("users");

    std::vector<FieldValue> allowed_targets = { FieldValue::String("ALL"), FieldValue::String(my_client_id) };

    Query targeted_query = messages_ref
        .WhereIn("targetId", allowed_targets)
        .WhereGreaterThan("timestamp", FieldValue::Integer(app_boot_time));

    users_ref.AddSnapshotListener([room_id, my_client_id](const QuerySnapshot& snapshot, Error error, const std::string& error_msg) {
        if (error != kErrorOk) return;
        for (const auto& change : snapshot.DocumentChanges()) {
            std::string user_id = change.document().id();
            if (user_id == my_client_id) continue;
            
            if (change.type() == DocumentChange::Type::kAdded) {
                SPDLOG_INFO("[ROOM PRESENCE] Peer entered room active pool: {}", user_id);
            } else if (change.type() == DocumentChange::Type::kRemoved) {
                SPDLOG_INFO("[ROOM PRESENCE] Peer left room active pool: {}", user_id);
                if (global_peer_left_callback) global_peer_left_callback(user_id);
            }
        }
    });

    targeted_query.AddSnapshotListener([room_id, my_client_id](const QuerySnapshot& snapshot, Error error, const std::string& error_msg) {
        if (error != kErrorOk) {
            SPDLOG_ERROR("Snapshot listener broken down: {}", error_msg);
            return;
        }
        for (const auto& change : snapshot.DocumentChanges()) {
            if (change.type() == DocumentChange::Type::kAdded) {
                MapFieldValue data = change.document().GetData();
                std::string sender_id = data["senderId"].string_value();
                std::string target_id = data["targetId"].string_value();
                std::string text_body = data["text"].string_value();
                
                SPDLOG_INFO("[Message Received] sender_id: [{}], target_id: [{}], my_client_id: [{}]", 
                            sender_id, target_id, my_client_id);

                if (sender_id == my_client_id) continue;

                if (text_body == "SYSTEM_EVENT: ROOM_FULL_READY_FOR_WEBRTC") {
                    if (global_room_ready_callback) global_room_ready_callback(room_id);
                    continue;
                }
                if (text_body == "SYSTEM_EVENT: Goodbye! I have closed my connection.") {
                    if (global_peer_left_callback) global_peer_left_callback(sender_id);
                    continue;
                }
                if (text_body.rfind("SDP_OFFER:", 0) == 0) {
                    if (global_sdp_received_callback) global_sdp_received_callback(sender_id, "offer", text_body.substr(10));
                    continue;
                }
                if (text_body.rfind("SDP_ANSWER:", 0) == 0) {
                    if (global_sdp_received_callback) global_sdp_received_callback(sender_id, "answer", text_body.substr(11));
                    continue;
                }
                if (text_body.rfind("ICE:", 0) == 0) {
                    if (global_ice_received_callback) {
                        std::stringstream ss(text_body.substr(4));
                        std::string mid, index_str, candidate_sdp;
                        std::getline(ss, mid, ':');
                        std::getline(ss, index_str, ':');
                        std::getline(ss, candidate_sdp);
                        global_ice_received_callback(sender_id, mid, std::stoi(index_str), candidate_sdp);
                    }
                    continue;
                }
            }
        }
    });

    users_ref.Get().OnCompletion([room_id, my_client_id](const firebase::Future<QuerySnapshot>& future) {
        if (future.status() == firebase::kFutureStatusComplete && !future.error()) {
            size_t occupancy = future.result()->documents().size();
            if (occupancy == 2) {
                SendTargetedMessage(room_id, my_client_id, "ALL", "SYSTEM_EVENT: ROOM_FULL_READY_FOR_WEBRTC");
                if (global_room_ready_callback) {
                    global_room_ready_callback(room_id);
                }
            }
        }
    });
}

void SendTargetedMessage(const std::string& room_id, const std::string& my_client_id, 
                          const std::string& target_client_id, const std::string& text_payload) {
    if (!db) return;
    CollectionReference messages_ref = db->Collection("rooms").Document(room_id).Collection("messages");

    MapFieldValue msg_map = {
        {"senderId",  FieldValue::String(my_client_id)},
        {"targetId",  FieldValue::String(target_client_id)},
        {"text",      FieldValue::String(text_payload)},
        {"timestamp", FieldValue::Integer(GetCurrentEpochMs())}
    };
    messages_ref.Add(msg_map);
}

int GetActiveClientCount(const std::string& room_id) {
    if (!db) return 0;
    
    auto active_users_promise = std::make_shared<std::promise<size_t>>();
    std::future<size_t> active_users_future = active_users_promise->get_future();

    CollectionReference users_ref = db->Collection("rooms").Document(room_id).Collection("users");
    uint64_t current_time = GetCurrentEpochMs();

    users_ref.Get().OnCompletion([active_users_promise, current_time](const firebase::Future<QuerySnapshot>& future) {
        if (future.status() == firebase::kFutureStatusComplete && !future.error()) {
            size_t active_count = 0;
            for (const auto& doc : future.result()->documents()) {
                MapFieldValue data = doc.GetData();
                bool is_stale = true;
                
                if (data.find("lastSeen") != data.end()) {
                    uint64_t last_seen = data["lastSeen"].integer_value();
                    int64_t diff = static_cast<int64_t>(current_time) - static_cast<int64_t>(last_seen);
                    if (std::abs(diff) < 30000) { // 30 seconds threshold
                        is_stale = false;
                    }
                }

                if (!is_stale) {
                    active_count++;
                }
            }
            active_users_promise->set_value(active_count);
        } else {
            active_users_promise->set_value(0);
        }
    });

    active_users_future.wait();
    return static_cast<int>(active_users_future.get());
}

bool IsRoomEmpty(const std::string& room_id) {
    int count = GetActiveClientCount(room_id);
    SPDLOG_INFO("[Room Check] Room [{}] current active client count: {}", room_id, count);
    return (count == 0);
}
