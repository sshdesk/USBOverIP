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
#include <spdlog/spdlog.h>

#include "firebase/app.h"
#include "firebase/firestore.h"

class SignalingClient {
public:
    // Callback type definitions
    using RoomReadyCallback = std::function<void(const std::string& room_id)>;
    using PeerLeftCallback = std::function<void(const std::string& peer_client_id)>;
    using SdpReceivedCallback = std::function<void(const std::string& sender_id, const std::string& type, const std::string& sdp)>;
    using IceReceivedCallback = std::function<void(const std::string& sender_id, const std::string& mid, int mline_index, const std::string& sdp)>;

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

    // Room and Presence Management
    void SetUserPresence(const std::string& room_id, const std::string& my_client_id, bool is_online);
    bool VerifyAndReserveRoomCapacity(const std::string& room_id, const std::string& my_client_id);
    void HandleGracefulExit(const std::string& room_id, const std::string& my_client_id);

    // Event Registration
    void RegisterOnRoomReadyEvent(RoomReadyCallback callback);
    void RegisterOnPeerLeftEvent(PeerLeftCallback callback);
    void RegisterOnSdpReceivedEvent(SdpReceivedCallback callback);
    void RegisterOnIceReceivedEvent(IceReceivedCallback callback);

    // Messaging & State Check
    void ListenForTargetedMessages(const std::string& room_id, const std::string& my_client_id);
    void SendTargetedMessage(const std::string& room_id, const std::string& my_client_id, 
                             const std::string& target_client_id, const std::string& text_payload);
    int GetActiveClientCount(const std::string& room_id, const std::string& my_client_id);
    bool IsRoomEmpty(const std::string& room_id, const std::string& my_client_id);

    // Heartbeat Control
    void StartHeartbeat(const std::string& room_id, const std::string& my_client_id);
    void StopHeartbeat();

    // Getters
    firebase::firestore::Firestore* GetDatabase() const { return db_; }
    std::string GetActiveLogLevel() const { return active_log_level_; }

private:
    uint64_t GetCurrentEpochMs() const;
    int64_t ExtractEpochMs(const firebase::firestore::FieldValue& val) const;
    void DeleteCollectionRecursively(const firebase::firestore::CollectionReference& collection_ref);
    void PurgeOldMessages(const std::string& room_id);

    firebase::App* app_ = nullptr;
    firebase::firestore::Firestore* db_ = nullptr;
    std::string active_log_level_ = "info";

    RoomReadyCallback room_ready_callback_ = nullptr;
    PeerLeftCallback peer_left_callback_ = nullptr;
    SdpReceivedCallback sdp_received_callback_ = nullptr;
    IceReceivedCallback ice_received_callback_ = nullptr;

    std::atomic<bool> heartbeat_active_{false};
    std::unique_ptr<std::thread> heartbeat_thread_ = nullptr;
    std::atomic<bool> room_ready_fired_{false};
    std::mutex heartbeat_mutex_;
};

// Global backward-compatible bridge for webrtc.cpp
extern std::unique_ptr<SignalingClient> signaling_client;

void SendTargetedMessage(const std::string& room_id, const std::string& my_client_id, 
                          const std::string& target_client_id, const std::string& text_payload);

#endif // SIGNALING_H