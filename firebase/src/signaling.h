#ifndef SIGNALING_H
#define SIGNALING_H

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <string>
#include <fstream>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <spdlog/spdlog.h>

#include "firebase/app.h"
#include "firebase/firestore.h"

class SignalingClient {
public:
    // Callback type definitions
    using PeerLeftCallback = std::function<void(const std::string& peer_client_id)>;
    using SdpReceivedCallback = std::function<void(const std::string& sender_id, const std::string& type, const std::string& sdp)>;
    using IceReceivedCallback = std::function<void(const std::string& sender_id, const std::string& mid, int mline_index, const std::string& sdp)>;
    using RoleAssignedCallback = std::function<void(bool is_caller, const std::string& remote_peer_id)>;

    SignalingClient();
    ~SignalingClient();

    // Prevent copy
    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    // Framework and Logging Setup
    bool InitializeFramework();
    void ShutdownFramework();
    void SetupLogging(const std::string& level_str);

    // Config and Identity
    std::string GenerateRandomRoomId(size_t length = 20);
    bool SaveConfigToDisk(const std::string& room_id, const std::string& log_level = "info", const std::string& filename = "config.txt");
    void LoadConfigFromDisk(std::string& out_room_id, std::string& out_log_level, const std::string& filename = "config.txt");

    // Room Lifecycle & Role Assignment
    void JoinRoom(const std::string& room_id, const std::string& my_client_id, RoleAssignedCallback on_role_assigned);
    void HandleGracefulExit(const std::string& room_id, const std::string& my_client_id);
    void PurgeOldMessages(const std::string& room_id);

    // Heartbeat Management (non-blocking background thread)
    void StartHeartbeat(const std::string& room_id, const std::string& my_client_id);
    void StopHeartbeat();

    // Event Registration
    void RegisterOnPeerLeftEvent(PeerLeftCallback callback);
    void RegisterOnSdpReceivedEvent(SdpReceivedCallback callback);
    void RegisterOnIceReceivedEvent(IceReceivedCallback callback);

    // Messaging
    void ListenForTargetedMessages(const std::string& room_id, const std::string& my_client_id);
    void SendTargetedMessage(const std::string& room_id, const std::string& my_client_id, 
                             const std::string& target_client_id, const std::string& text_payload);

    // Getters
    firebase::firestore::Firestore* GetDatabase() const { return db_; }
    std::string GetActiveLogLevel() const { return active_log_level_; }

private:
    uint64_t GetCurrentEpochMs() const;
    int64_t ExtractEpochMs(const firebase::firestore::FieldValue& val) const;
    static void DeleteCollectionRecursively(const firebase::firestore::CollectionReference& collection_ref);

    firebase::App* app_ = nullptr;
    firebase::firestore::Firestore* db_ = nullptr;
    std::string active_log_level_ = "info";

    PeerLeftCallback peer_left_callback_ = nullptr;
    SdpReceivedCallback sdp_received_callback_ = nullptr;
    IceReceivedCallback ice_received_callback_ = nullptr;

    firebase::firestore::ListenerRegistration messages_listener_reg_;

    std::atomic<bool> heartbeat_active_{false};
    std::unique_ptr<std::thread> heartbeat_thread_ = nullptr;
    std::mutex heartbeat_mutex_;
};

// Global backward-compatible bridge for webrtc.cpp
extern std::unique_ptr<SignalingClient> signaling_client;

void SendTargetedMessage(const std::string& room_id, const std::string& my_client_id, 
                          const std::string& target_client_id, const std::string& text_payload);

#endif // SIGNALING_H