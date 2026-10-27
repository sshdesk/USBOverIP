#ifndef SIGNALING_H
#define SIGNALING_H

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <string>
#include <fstream>
#include <cstdio>
#include <functional>
#include <spdlog/spdlog.h>

namespace firebase {
    class App;
    namespace firestore {
        class Firestore;
    }
}

// Global shared configuration tokens
extern firebase::firestore::Firestore* db;
extern std::string active_room_id;
extern std::string active_log_level;

// Callback type definitions for decoupled abstraction boundaries
typedef std::function<void(const std::string& room_id)> RoomReadyCallback;
typedef std::function<void(const std::string& peer_client_id)> PeerLeftCallback;
typedef std::function<void(const std::string& sender_id, const std::string& type, const std::string& sdp)> SdpReceivedCallback;
typedef std::function<void(const std::string& sender_id, const std::string& mid, int mline_index, const std::string& sdp)> IceReceivedCallback;

// Baseline Framework Setup Engine
bool InitializeFirebaseFramework();
void ShutdownFirebaseFramework(firebase::App* app);
void SetupLogging(const std::string& level_str);

// Config and Identity helpers
std::string GenerateRandomRoomId(size_t length = 20);
bool SaveConfigToDisk(const std::string& room_id, const std::string& log_level = "info", const std::string& filename = "config.txt");
void LoadConfigFromDisk(std::string& out_room_id, std::string& out_log_level, const std::string& filename = "config.txt");

// Presence & Automated Purge Cleanups
void SetUserPresence(const std::string& room_id, const std::string& my_client_id, bool is_online);
void HandleGracefulExit(const std::string& room_id, const std::string& my_client_id);
bool VerifyAndReserveRoomCapacity(const std::string& room_id, const std::string& my_client_id);

// Event Registration Interface
void RegisterOnRoomReadyEvent(RoomReadyCallback callback);
void RegisterOnPeerLeftEvent(PeerLeftCallback callback);
void RegisterOnSdpReceivedEvent(SdpReceivedCallback callback);
void RegisterOnIceReceivedEvent(IceReceivedCallback callback);

// Messaging routing pipeline
void ListenForTargetedMessages(const std::string& room_id, const std::string& my_client_id);
void SendTargetedMessage(const std::string& room_id, const std::string& my_client_id, 
                          const std::string& target_client_id, const std::string& text_payload);

int GetActiveClientCount(const std::string& room_id, const std::string& my_client_id);
bool IsRoomEmpty(const std::string& room_id, const std::string& my_client_id);

void StopHeartbeat();

void StartHeartbeat(const std::string &room_id, const std::string &my_client_id);

#endif // SIGNALING_H
