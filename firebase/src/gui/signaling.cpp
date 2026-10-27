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

firebase::firestore::Firestore* db = nullptr;
std::string active_log_level = "info";

using namespace firebase::firestore;

static RoomReadyCallback global_room_ready_callback = nullptr;
static PeerLeftCallback global_peer_left_callback = nullptr;
static SdpReceivedCallback global_sdp_received_callback = nullptr;
static IceReceivedCallback global_ice_received_callback = nullptr;

// Heartbeat management thread controls
static std::atomic<bool> heartbeat_active{false};
static std::unique_ptr<std::thread> heartbeat_thread = nullptr;
static std::atomic<bool> room_ready_fired{false};

uint64_t GetCurrentEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Helper to reliably extract UTC milliseconds whether stored as Timestamp or legacy Integer
static int64_t ExtractEpochMs(const FieldValue& val) {
    if (val.is_timestamp()) {
        firebase::Timestamp ts = val.timestamp_value();
        return (ts.seconds() * 1000) + (ts.nanoseconds() / 1000000);
    } else if (val.is_integer()) {
        return val.integer_value();
    }
    return 0;
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
    
    SPDLOG_DEBUG("[Firestore Engine] High-performance real-time configuration applied successfully.");
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

// Writes authoritative server timestamp to Firestore
void SetUserPresence(const std::string& room_id, const std::string& my_client_id, bool is_online) {
    if (!db || room_id.empty() || my_client_id.empty()) return;
    DocumentReference user_doc = db->Collection("rooms").Document(room_id).Collection("users").Document(my_client_id);
    if (is_online) {
        user_doc.Set({
            {"status", FieldValue::String("online")}, 
            {"lastSeen", FieldValue::ServerTimestamp()}
        });
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

// Verified and calibrated using server timestamps to prevent client clock skew
bool VerifyAndReserveRoomCapacity(const std::string& room_id, const std::string& my_client_id) {
    if (!db || room_id.empty()) return false;
    auto active_users_promise = std::make_shared<std::promise<size_t>>();
    std::future<size_t> active_users_future = active_users_promise->get_future();

    CollectionReference users_ref = db->Collection("rooms").Document(room_id).Collection("users");

    // Write presence with ServerTimestamp first so our client is registered with server time
    if (!my_client_id.empty()) {
        SetUserPresence(room_id, my_client_id, true);
    }

    users_ref.Get().OnCompletion([active_users_promise, room_id, my_client_id](const firebase::Future<QuerySnapshot>& future) {
        if (future.status() == firebase::kFutureStatusComplete && !future.error()) {
            auto snapshot = future.result();
            const auto& docs = snapshot->documents();

            // Establish server reference time from our own freshly set doc or newest server timestamp in collection
            int64_t ref_time_ms = 0;
            if (!my_client_id.empty()) {
                for (const auto& doc : docs) {
                    if (doc.id() == my_client_id) {
                        MapFieldValue data = doc.GetData();
                        if (data.find("lastSeen") != data.end()) {
                            ref_time_ms = ExtractEpochMs(data["lastSeen"]);
                        }
                        break;
                    }
                }
            }
            if (ref_time_ms == 0) {
                for (const auto& doc : docs) {
                    MapFieldValue data = doc.GetData();
                    if (data.find("lastSeen") != data.end()) {
                        int64_t t = ExtractEpochMs(data["lastSeen"]);
                        if (t > ref_time_ms) {
                            ref_time_ms = t;
                        }
                    }
                }
            }
            if (ref_time_ms == 0) {
                ref_time_ms = static_cast<int64_t>(GetCurrentEpochMs());
            }

            size_t active_other_count = 0;
            for (const auto& doc : docs) {
                std::string doc_id = doc.id();
                
                // Do not count self as an "other" occupant
                if (!my_client_id.empty() && doc_id == my_client_id) {
                    continue; 
                }

                MapFieldValue data = doc.GetData();
                bool is_stale = true;

                if (data.find("lastSeen") != data.end()) {
                    const FieldValue& val = data["lastSeen"];
                    if (val.is_null()) {
                        // Pending server timestamp write means client just joined
                        is_stale = false;
                    } else {
                        int64_t last_seen = ExtractEpochMs(val);
                        int64_t diff = ref_time_ms - last_seen;
                        if (std::abs(diff) < 30000) { // 30 seconds threshold
                            is_stale = false;
                        }
                    }
                }

                if (is_stale) {
                    SPDLOG_WARN("[Room Capacity] Purging stale client document: {}", doc_id);
                    doc.reference().Delete();
                } else {
                    active_other_count++;
                }
            }
            active_users_promise->set_value(active_other_count);
        } else {
            active_users_promise->set_value(0);
        }
    });

    if (active_users_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        size_t other_occupants = active_users_future.get();
        // A 2-peer room allows at most 1 other remote occupant (total occupancy <= 2)
        bool allowed = (other_occupants < 2);
        if (allowed) {
            StartHeartbeat(room_id, my_client_id);
        } else {
            // Room is full: revert our presence reservation
            if (!my_client_id.empty()) {
                SetUserPresence(room_id, my_client_id, false);
            }
        }
        return allowed;
    }
    return false;
}

void RegisterOnRoomReadyEvent(RoomReadyCallback callback) { global_room_ready_callback = callback; }
void RegisterOnPeerLeftEvent(PeerLeftCallback callback) { global_peer_left_callback = callback; }
void RegisterOnSdpReceivedEvent(SdpReceivedCallback callback) { global_sdp_received_callback = callback; }
void RegisterOnIceReceivedEvent(IceReceivedCallback callback) { global_ice_received_callback = callback; }


void HandleGracefulExit(const std::string& room_id, const std::string& my_client_id) {
    if (!db || room_id.empty()) return;
    
    StopHeartbeat();
    
    SendTargetedMessage(room_id, my_client_id, "ALL", "SYSTEM_EVENT: Goodbye! I have closed my connection.");
    SetUserPresence(room_id, my_client_id, false);
    
    DocumentReference room_doc = db->Collection("rooms").Document(room_id);
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

void ListenForTargetedMessages(const std::string& room_id, const std::string& my_client_id) {
    if (!db || room_id.empty()) return;
    room_ready_fired = false;

    // Record session start time with 30s grace window to filter stale messages in-memory
    int64_t session_start_ms = static_cast<int64_t>(GetCurrentEpochMs()) - 30000;

    CollectionReference messages_ref = db->Collection("rooms").Document(room_id).Collection("messages");
    CollectionReference users_ref = db->Collection("rooms").Document(room_id).Collection("users");

    std::vector<FieldValue> allowed_targets = { FieldValue::String("ALL"), FieldValue::String(my_client_id) };

    // Query on targetId only (requires no composite indexes on Firestore)
    Query targeted_query = messages_ref.WhereIn("targetId", allowed_targets);

    users_ref.AddSnapshotListener([room_id, my_client_id](const QuerySnapshot& snapshot, Error error, const std::string& error_msg) {
        if (error != kErrorOk) return;
        for (const auto& change : snapshot.DocumentChanges()) {
            std::string user_id = change.document().id();
            if (user_id == my_client_id) continue;
            
            if (change.type() == DocumentChange::Type::kAdded) {
                SPDLOG_INFO("[ROOM PRESENCE] Peer entered room active pool: {}", user_id);
                if (snapshot.documents().size() >= 2 && !room_ready_fired.exchange(true)) {
                    SendTargetedMessage(room_id, my_client_id, "ALL", "SYSTEM_EVENT: ROOM_FULL_READY_FOR_WEBRTC");
                    if (global_room_ready_callback) global_room_ready_callback(room_id);
                }
            } else if (change.type() == DocumentChange::Type::kRemoved) {
                SPDLOG_INFO("[ROOM PRESENCE] Peer left room active pool: {}", user_id);
                room_ready_fired = false;
                if (global_peer_left_callback) global_peer_left_callback(user_id);
            }
        }
    });

    targeted_query.AddSnapshotListener([room_id, my_client_id, session_start_ms](const QuerySnapshot& snapshot, Error error, const std::string& error_msg) {
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

                // Filter out stale messages from prior sessions using server timestamp or legacy integer
                if (data.find("timestamp") != data.end()) {
                    int64_t msg_time = ExtractEpochMs(data["timestamp"]);
                    if (msg_time > 0 && msg_time < session_start_ms) {
                        continue;
                    }
                }

                SPDLOG_INFO("[Message Received] sender_id: [{}], target_id: [{}], my_client_id: [{}]", 
                            sender_id, target_id, my_client_id);

                if (text_body == "SYSTEM_EVENT: ROOM_FULL_READY_FOR_WEBRTC") {
                    if (!room_ready_fired.exchange(true)) {
                        if (global_room_ready_callback) global_room_ready_callback(room_id);
                    }
                    continue;
                }
                if (text_body == "SYSTEM_EVENT: Goodbye! I have closed my connection.") {
                    room_ready_fired = false;
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
                if (!room_ready_fired.exchange(true)) {
                    SendTargetedMessage(room_id, my_client_id, "ALL", "SYSTEM_EVENT: ROOM_FULL_READY_FOR_WEBRTC");
                    if (global_room_ready_callback) {
                        global_room_ready_callback(room_id);
                    }
                }
            }
        }
    });
}

void SendTargetedMessage(const std::string& room_id, const std::string& my_client_id, 
                          const std::string& target_client_id, const std::string& text_payload) {
    if (!db || room_id.empty() || my_client_id.empty()) return;
    CollectionReference messages_ref = db->Collection("rooms").Document(room_id).Collection("messages");

    MapFieldValue msg_map = {
        {"senderId",  FieldValue::String(my_client_id)},
        {"targetId",  FieldValue::String(target_client_id)},
        {"text",      FieldValue::String(text_payload)},
        {"timestamp", FieldValue::ServerTimestamp()}
    };
    messages_ref.Add(msg_map);
}


int GetActiveClientCount(const std::string& room_id, const std::string& my_client_id) {
    if (!db || room_id.empty()) return 0;
    
    auto active_users_promise = std::make_shared<std::promise<size_t>>();
    std::future<size_t> active_users_future = active_users_promise->get_future();

    CollectionReference users_ref = db->Collection("rooms").Document(room_id).Collection("users");

    users_ref.Get().OnCompletion([active_users_promise, my_client_id](const firebase::Future<QuerySnapshot>& future) {
        if (future.status() == firebase::kFutureStatusComplete && !future.error()) {
            auto snapshot = future.result();
            const auto& docs = snapshot->documents();

            int64_t ref_time_ms = 0;
            for (const auto& doc : docs) {
                MapFieldValue data = doc.GetData();
                if (data.find("lastSeen") != data.end()) {
                    int64_t t = ExtractEpochMs(data["lastSeen"]);
                    if (t > ref_time_ms) {
                        ref_time_ms = t;
                    }
                }
            }
            if (ref_time_ms == 0) {
                ref_time_ms = static_cast<int64_t>(GetCurrentEpochMs());
            }

            size_t active_count = 0;
            for (const auto& doc : docs) {
                if (!my_client_id.empty() && doc.id() == my_client_id) {
                    continue;
                }
                MapFieldValue data = doc.GetData();
                bool is_stale = true;
                
                if (data.find("lastSeen") != data.end()) {
                    const FieldValue& val = data["lastSeen"];
                    if (val.is_null()) {
                        is_stale = false;
                    } else {
                        int64_t last_seen = ExtractEpochMs(val);
                        int64_t diff = ref_time_ms - last_seen;
                        if (std::abs(diff) < 30000) {
                            is_stale = false;
                        }
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

    if (active_users_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        return static_cast<int>(active_users_future.get());
    }
    return 0;
}

bool IsRoomEmpty(const std::string& room_id, const std::string& my_client_id) {
    int count = GetActiveClientCount(room_id, my_client_id);
    SPDLOG_INFO("[Room Check] Room [{}] other active client count: {}", room_id, count);
    return (count == 0);
}


void StopHeartbeat() {
    heartbeat_active = false;
    if (heartbeat_thread && heartbeat_thread->joinable()) {
        heartbeat_thread->join();
    }
    heartbeat_thread.reset();
}

void StartHeartbeat(const std::string& room_id, const std::string& my_client_id) {
    StopHeartbeat();
    heartbeat_active = true;
    heartbeat_thread = std::make_unique<std::thread>([room_id, my_client_id]() {
        while (heartbeat_active) {
            SetUserPresence(room_id, my_client_id, true);
            // Sleep in 100ms chunks up to 10 seconds to allow fast shutdown
            for (int i = 0; i < 100 && heartbeat_active; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}

