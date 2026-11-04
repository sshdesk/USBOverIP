#pragma once

#include <atomic>
#include <string>
#include <memory>
#include <map>
#include <mutex>
#include <vector>
#include <cstdint>
#include "usbip_server.h"

class WebRtcManager_usbipd;
class ScopedVBoxServiceManager;

extern usbip_server_t *g_server;

class UsbIpdCommands {
public:
    UsbIpdCommands();
    ~UsbIpdCommands();

    // Disable copy and move semantics (std::mutex cannot be copied or moved)
    UsbIpdCommands(const UsbIpdCommands&) = delete;
    UsbIpdCommands& operator=(const UsbIpdCommands&) = delete;
    UsbIpdCommands(UsbIpdCommands&&) = delete;
    UsbIpdCommands& operator=(UsbIpdCommands&&) = delete;

    // Device Listing
    std::vector<std::string> listPhysicalDevices();

    // Session Management
    void initializeServerSession();
    void shutdownServerSession();
    bool startHostSession(const std::string& token_id, const std::string& client_id);
    void shutdown();

    // Host Server Flag
    bool isHostServer() const { return host_server_; }
    void setHostServer(bool host) { host_server_ = host; }

private:
    void attachDataChannelCallback(WebRtcManager_usbipd* manager);

    usbip_server_t* server_{nullptr};
    std::mutex clients_mutex_;
    std::map<std::string, std::unique_ptr<WebRtcManager_usbipd>> connected_clients_;
    std::unique_ptr<WebRtcManager_usbipd> core_server_connection_{nullptr};
    std::unique_ptr<ScopedVBoxServiceManager> vbox_guard_{nullptr};
    bool host_server_{false};
};