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

class SignalingClient {
public:
    using PeerLeftCallback = std::function<void(const std::string& peer_client_id)>;
    using SdpReceivedCallback = std::function<void(const std::string& sender_id, const std::string& type, const std::string& sdp)>;
    using IceReceivedCallback = std::function<void(const std::string& sender_id, const std::string& mid, int mline_index, const std::string& sdp)>;
    using RoleAssignedCallback = std::function<void(bool is_caller, const std::string& remote_peer_id)>;

    SignalingClient();
    ~SignalingClient();

    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    bool InitializeFramework();
    void ShutdownFramework();
    void SetupLogging(const std::string& level_str);

    std::string GenerateRandomRoomId(size_t length = 20);
    bool SaveConfigToDisk(const std::string& token_id, bool remote_usb, const std::string& log_level = "info", const std::string& filename = "config.txt");
    void LoadConfigFromDisk(std::string& out_room_id, bool& out_remote_usb, std::string& out_log_level, const std::string& filename = "config.txt");

    void JoinRoom(const std::string& token_id, const std::string& my_client_id, RoleAssignedCallback on_role_assigned);
    void HandleGracefulExit(const std::string& token_id, const std::string& my_client_id);
    void PurgeOldMessages(const std::string& token_id);

    void StartHeartbeat(const std::string& token_id, const std::string& my_client_id);
    void StopHeartbeat();

    void RegisterOnPeerLeftEvent(PeerLeftCallback callback);
    void RegisterOnSdpReceivedEvent(SdpReceivedCallback callback);
    void RegisterOnIceReceivedEvent(IceReceivedCallback callback);

    void ListenForTargetedMessages(const std::string& token_id, const std::string& my_client_id);
    void SendTargetedMessage(const std::string& token_id, const std::string& my_client_id, 
                             const std::string& target_client_id, const std::string& text_payload);

    std::string GetActiveLogLevel() const { return active_log_level_; }
    void SetApiEndpoint(const std::string& endpoint) { api_endpoint_ = endpoint; }

private:
    std::string active_log_level_ = "info";
    // Replace with your actual AWS Amplify / API Gateway REST endpoint URL
    std::string api_endpoint_ = "https://your-api-id.execute-api.us-east-1.amazonaws.com/prod";

    PeerLeftCallback peer_left_callback_ = nullptr;
    SdpReceivedCallback sdp_received_callback_ = nullptr;
    IceReceivedCallback ice_received_callback_ = nullptr;

    std::atomic<bool> polling_active_{false};
    std::unique_ptr<std::thread> polling_thread_ = nullptr;

    std::atomic<bool> heartbeat_active_{false};
    std::unique_ptr<std::thread> heartbeat_thread_ = nullptr;
    std::mutex heartbeat_mutex_;
};

extern std::unique_ptr<SignalingClient> signaling_client;

void SendTargetedMessage(const std::string& token_id, const std::string& my_client_id, 
                          const std::string& target_client_id, const std::string& text_payload);

#endif // SIGNALING_H