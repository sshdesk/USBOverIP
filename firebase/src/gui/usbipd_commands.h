#pragma once

#include <atomic>
#include <string>
#include <memory>
#include <map>
#include <mutex>
#include <vector>
#include <cstdint>
#include "usbip_server.h"

// Forward declaration
class WebRtcManager_usbipd;

// Shared global state defined in main.cpp
extern std::atomic<bool> keep_running;
extern std::atomic<bool> is_datachannel_connected;
extern bool host_server;
extern std::mutex clients_mutex;
extern std::map<std::string, std::unique_ptr<WebRtcManager_usbipd>> connected_clients;
extern std::unique_ptr<WebRtcManager_usbipd> core_server_connection;

// Server context handle
extern usbip_server_t* g_server;

// USB Passthrough Globals
extern bool g_have_passthrough;
extern std::string g_passthrough_busid;
extern uint16_t g_passthrough_vid;
extern uint16_t g_passthrough_pid;
extern bool g_dynamic_mode;

// Physical USB device enumeration
std::vector<std::string> usb_device_list_physical_devices(void);

// USB/IP Server Session Lifecycle
void InitializeUsbIpServerSession();
void ShutdownUsbIpServerSession();

// WebRTC DataChannel Hook
void AttachDataChannelCallback(WebRtcManager_usbipd* manager);

// Host room verification, signaling listener setup, and WebRTC offer initialization
bool StartUsbIpHostSession(const std::string& active_room_id, const std::string& global_client_id);