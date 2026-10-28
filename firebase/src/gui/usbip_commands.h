#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <map>

class WebRtcManager_usbip;
class WebRtcTcpTunnelManager;

class UsbIpCommands {
public:
    using OnDataChannelOpenCallback = std::function<void()>;
    using OnTcpServerStartedCallback = std::function<void(int port)>;

    UsbIpCommands();
    ~UsbIpCommands();

    // Disable copy and move semantics (std::mutex and std::atomic cannot be copied or moved)
    UsbIpCommands(const UsbIpCommands&) = delete;
    UsbIpCommands& operator=(const UsbIpCommands&) = delete;
    UsbIpCommands(UsbIpCommands&&) = delete;
    UsbIpCommands& operator=(UsbIpCommands&&) = delete;

    // CLI and Command Execution
    int executeCLI(int argc, wchar_t *argv[]);
    void executeCommand(const std::vector<std::string>& args_utf8);

    // Client Tunnel Lifecycle
    int initializeTunnelSession();
    void shutdownTunnel();
    void shutdown();

    WebRtcTcpTunnelManager* getTunnelManager() const { return tunnel_manager_.get(); }
    bool isHostServer() const { return host_server_; }
    void setHostServer(bool host) { host_server_ = host; }

private:
    void attachDataChannelCallback(WebRtcManager_usbip* manager);

    std::unique_ptr<WebRtcTcpTunnelManager> tunnel_manager_;
    std::atomic<bool> client_tunnel_active_{false};

    std::mutex clients_mutex_;
    std::map<std::string, std::unique_ptr<WebRtcManager_usbip>> connected_clients_;
    std::unique_ptr<WebRtcManager_usbip> core_server_connection_{nullptr};

    bool host_server_{false};
};