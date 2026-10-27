#include "usbip_commands.h"
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <csignal>
#include <sstream>
#include <map>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <functional>

#include "signaling.h"
#include "../webrtc_usbip.h"
#include "../webrtc_stats.h"
#include "firebase/app.h"
#include "firebase/firestore.h"
#include "../tcptunnel.h"

#include "usbip.h"
#include "resource.h"
#include "log.h"
#include "strings.h"
#include <libusbip/vhci.h>
#include <libusbip/output.h>
#include <libusbip/win_handle.h>
#include <libusbip/win_socket.h>
#include <libusbip/format_message.h>
#include <libusbip/src/usb_ids.h>
#include <libusbip/src/strconv.h>
#include <libusbip/src/file_ver.h>
#include <resources/messages.h>
#include <CLI11/CLI11.hpp>
#include "logsync.h"

using namespace usbip;

extern std::atomic<bool> is_datachannel_connected;

namespace {

const auto MAX_HUB_PORTS = 255;
constexpr auto &str_zero_copy = "zero-copy";
constexpr auto &str_low_latency = "low-latency";
auto &msgtable_dll = L"resources";

constexpr int kDefaultTunnelTcpPort = 3240;

std::unique_ptr<WebRtcManager_usbip> client_tunnel_connection = nullptr;
WebRtcTcpTunnelManager tunnel_manager(kDefaultTunnelTcpPort);
std::atomic<bool> client_tunnel_active{false};

// Callback storage & mutex protection
std::mutex g_callbacks_mutex;
OnDataChannelOpenCallback g_on_datachannel_open = nullptr;
OnTcpServerStartedCallback g_on_tcp_server_started = nullptr;

void AttachDataChannelCallbackForClient(WebRtcManager_usbip* manager) {
    if (!manager) return;
    manager->SetDataChannelStateCallback([](bool is_open) {
        client_tunnel_active = is_open;
        is_datachannel_connected = is_open;


            // Thread-safe access to GUI
        {
            std::lock_guard<std::mutex> lock(g_log_mutex);
            if (g_active_gui) {
              if (is_open) {
                g_active_gui->setLabelText("Data channel pipe opened");
              } else {
                g_active_gui->setLabelText("Data channel pipe closed");
              }
            }
        }

        
        if (is_open) {
            SPDLOG_INFO("[Tunnel Manager] DataChannel opened. Linking to TCP tunnel manager...");

            // 1. WebRTC DataChannel Open Callback
            OnDataChannelOpenCallback dc_cb;
            {
                std::lock_guard<std::mutex> lock(g_callbacks_mutex);
                dc_cb = g_on_datachannel_open;
            }
            if (dc_cb) {
                try {
                    dc_cb();
                } catch (const std::exception& e) {
                    SPDLOG_ERROR("Exception in OnDataChannelOpen callback: {}", e.what());
                }
            }

            if (client_tunnel_connection && client_tunnel_connection->GetDataChannel()) {
                if (!tunnel_manager.Start()) {
                    SPDLOG_ERROR("Failed to start tunnel manager because TCP bind failed.");
                } else {
                    // 2. TCP Server Started Callback
                    OnTcpServerStartedCallback tcp_cb;
                    {
                        std::lock_guard<std::mutex> lock(g_callbacks_mutex);
                        tcp_cb = g_on_tcp_server_started;
                    }
                    if (tcp_cb) {
                        try {
                            tcp_cb(kDefaultTunnelTcpPort);
                        } catch (const std::exception& e) {
                            SPDLOG_ERROR("Exception in OnTcpServerStarted callback: {}", e.what());
                        }
                    }

                    tunnel_manager.HandleDataChannel(client_tunnel_connection->GetDataChannel(), client_tunnel_connection->GetSignalingThread());
                }
            }
        } else {
            SPDLOG_WARN("[Tunnel Manager] DataChannel closed. Stopping TCP tunnel manager.");
            tunnel_manager.Stop();
        }
    });
}

auto get_ids_data() {
    win::Resource r(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDR_USB_IDS), RT_RCDATA);
    assert(r);
    return r.str();
}

auto get_version() {
    win::FileVersion fv;
    auto ver = fv.GetFileVersion();
    return wchar_to_utf8_or(ver);
}

template <typename Cmd, typename Args>
auto pack(Cmd &&cmd, std::shared_ptr<Args> args) {
    return [cmd = std::forward<Cmd>(cmd), args = std::move(args)] {
        if (!cmd(*args)) {
            throw CLI::RuntimeError(EXIT_FAILURE);
        }
    };
}

auto serial_validator(const std::string &serial) {
    std::string s;
    if (!validate_device_serial(serial)) {
        s = get_last_error_msg();
        if (s.empty()) {
            s = "invalid device serial number";
        }
    }
    return s;
}

void add_cmd_attach(CLI::App &app) {
    auto r = std::make_shared<attach_args>();
    auto cmd = app.add_subcommand("attach", "Attach remote/persistent USB device(s)")
            ->callback(pack(cmd_attach, r))
            ->require_option(1);

    auto rem = cmd->add_option_group("Remote", "Attach remote USB device");
    rem->add_option("-r,--remote", r->remote, "Hostname/IP of a USB/IP server")->required();
    auto busid_opt = rem->add_option("-b,--bus-id", r->busid, "Bus Id of the USB device");
    auto select_opt = rem->add_flag("--select", r->select_device, "Query server device list and select interactively");
    busid_opt->excludes(select_opt);
    select_opt->excludes(busid_opt);

    rem->add_option("--serial", r->serial, "device serial number")->check(serial_validator);
    rem->add_flag("-t,--terse", r->terse, "Show port number as a result");
    auto stop = rem->add_flag("-x,--stop", r->stop, "Stop attach attempts");
    rem->add_flag("--once", r->once, "Do not start automatic attach attempts if cannot connect")->excludes(stop);

    auto receive = [&mode = r->recv_mode, low_lat = str_low_latency] (const auto &choice) {
        mode = choice == low_lat ? receive_mode::low_latency : receive_mode::zero_copy;
    };
    rem->add_option_function<std::string>("--receive-mode", receive, "How to receive data")
        ->check(CLI::IsMember({str_zero_copy, str_low_latency}))
        ->default_str(str_zero_copy)
        ->excludes(stop);

    auto stop_all = cmd->add_option_group("Stop")->add_flag("-X,--stop-all", r->stop_all, "Stop all active attach attempts");
    stop->excludes(stop_all);
    cmd->add_option_group("Persistent", "Attach persistent USB device(s)")
        ->add_flag("-s,--stashed,--persistent", r->persistent, "Attach persistent device(s)");
}

void add_cmd_detach(CLI::App &app) {
    auto r = std::make_shared<detach_args>();
    auto cmd = app.add_subcommand("detach", "Detach a remote USB device")
            ->callback(pack(cmd_detach, r))
            ->require_option(1);

    auto opt_port = cmd->add_option("-p,--port", r->port, "Hub port number")->check(CLI::Range(1, MAX_HUB_PORTS));
    cmd->add_option("-a,--all", "Detach all devices")
        ->each([&port = r->port] (const auto&) noexcept { port = vhci::port_all_closeonly; })
        ->check(CLI::IsMember({"closeonly"}))
        ->expected(0, 1)
        ->excludes(opt_port);
}

void add_cmd_list(CLI::App &app) {
    auto r = std::make_shared<list_args>();
    auto cmd = app.add_subcommand("list", "List exportable/persistent USB devices")->callback(pack(cmd_list, r))->require_option(1);
    cmd->add_option_group("Remote", "List exportable devices on a remote")->add_option("-r,--remote", r->remote)->required();
    cmd->add_option_group("Persistent", "List persistent USB devices")->add_flag("-s,--stashed,--persistent", r->persistent);
}

void add_cmd_port(CLI::App &app) {
    auto r = std::make_shared<port_args>();
    auto cmd = app.add_subcommand("port", "Show/stash imported USB devices")->callback(pack(cmd_port, r));
    cmd->add_flag("-s,--stash,--persistent", r->persistent, "Persistent devices");
    cmd->add_option("number", r->ports, "Hub port number")->check(CLI::Range(1, MAX_HUB_PORTS))->expected(1, MAX_HUB_PORTS);
}

auto& get_resource_module() noexcept {
    static HModule mod(LoadLibraryEx(msgtable_dll, nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR));
    return mod;
}

void init(CLI::App &app) {
    static std::string version_str = get_version();
    app.set_version_flag("-V,--version", version_str);
    app.add_flag("-d,--debug", [] (auto) { log::set_debug(true); }, "Debug output");
    app.add_option("-t,--tcp-port", global_args.tcp_port, "TCP/IP port number")
        ->check(CLI::Range(1024, USHRT_MAX))
        ->default_str(global_args.tcp_port);

    add_cmd_attach(app);
    add_cmd_detach(app);
    add_cmd_list(app);
    add_cmd_port(app);
    app.require_subcommand(1);
}

CLI::App& get_usbip_app() {
    static CLI::App app("USB/IP Client & WebRTC Tunnel Utility");
    static bool initialized = false;
    if (!initialized) {
        init(app);
        initialized = true;
    }
    return app;
}

} // namespace

// Registration functions implementation
void RegisterOnDataChannelOpenCallback(OnDataChannelOpenCallback callback) {
    std::lock_guard<std::mutex> lock(g_callbacks_mutex);
    g_on_datachannel_open = std::move(callback);
}

void RegisterOnTcpServerStartedCallback(OnTcpServerStartedCallback callback) {
    std::lock_guard<std::mutex> lock(g_callbacks_mutex);
    g_on_tcp_server_started = std::move(callback);
}

void RegisterOnTcpServerStartedCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(g_callbacks_mutex);
    if (callback) {
        g_on_tcp_server_started = [cb = std::move(callback)](int /*port*/) { cb(); };
    } else {
        g_on_tcp_server_started = nullptr;
    }
}

namespace usbip {
std::string get_last_error_msg(DWORD msg_id) {
    auto &mod = get_resource_module();
    return format_message(mod.get(), msg_id);
}

const UsbIds& get_ids() {
    static UsbIds ids(get_ids_data());
    assert(ids);
    return ids;
}
}

void libusbip_spdlog_bridge(const std::string &msg) {
  std::string clean_msg = msg;
  while (!clean_msg.empty() &&
         (clean_msg.back() == '\n' || clean_msg.back() == '\r')) {
    clean_msg.pop_back();
  }
  if (!clean_msg.empty()) {
    SPDLOG_INFO("[libusbip] {}", clean_msg);
  }
}

int usbipCLI(int argc, wchar_t *argv[]) {
    log::init();
    libusbip::set_debug_output(libusbip_spdlog_bridge);

    InitWinSock2 ws2;
    if (!ws2) {
        log::critical("can't initialize Windows Sockets 2, {}", get_last_error_msg());
        return EXIT_FAILURE;
    }

    CLI::App &app = get_usbip_app();
    app.clear();

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        return app.exit(e);
    }

    return EXIT_SUCCESS;
}

void execute_usbip_command(const std::vector<std::string>& args_utf8) {
    std::vector<std::wstring> args_w;
    for (const auto &arg : args_utf8) {
        args_w.push_back(std::wstring(arg.begin(), arg.end()));
    }

    std::vector<wchar_t*> argv_vec;
    for (auto &wstr : args_w) {
        argv_vec.push_back(wstr.data());
    }

    try {
        usbipCLI(static_cast<int>(argv_vec.size()), argv_vec.data());
    } catch (const std::exception &e) {
        SPDLOG_ERROR("CLI execution exception: {}", e.what());
    }
}

int InitializeUsbIpClientTunnelSession() {
    if (client_tunnel_connection) return 0;
    
    extern std::string active_room_id;
    extern std::string global_client_id;

    if (active_room_id.empty()) {
        SPDLOG_ERROR("[Client Tunnel Init] Error: active_room_id is empty.");
        return -2;
    }

    SaveConfigToDisk(active_room_id, "info");

    SPDLOG_INFO("Evaluating capacity constraints for room matrix [{}]...", active_room_id);
    if (!VerifyAndReserveRoomCapacity(active_room_id, global_client_id)) {
        SPDLOG_ERROR("[CRITICAL REJECT] Access Denied: Room capacity limit reached.");
        return -1;
    }

    SPDLOG_INFO("[Client Tunnel Init] Instantiating WebRtcManager_usbip caller node...");
    client_tunnel_connection = std::make_unique<WebRtcManager_usbip>(active_room_id, global_client_id, true);
    AttachDataChannelCallbackForClient(client_tunnel_connection.get());
    client_tunnel_connection->SetRemotePeerId("ALL");

    RegisterOnRoomReadyEvent([&](const std::string& ready_room_id) {
        SPDLOG_WARN("=================================================================");
        SPDLOG_WARN("[EVENT TRIGGERED] Room [{}] is full and ready for meeting!", ready_room_id);
        SPDLOG_WARN("=================================================================");
    });

    RegisterOnPeerLeftEvent([&](const std::string& peer_client_id) {
        SPDLOG_ERROR("=================================================================");
        SPDLOG_ERROR("[EVENT NOTICE] Peer [{}] has disconnected or left the room.", peer_client_id);
        SPDLOG_ERROR("=================================================================");
        if (client_tunnel_connection) {
            client_tunnel_connection->ShutdownConnection();
            client_tunnel_connection = nullptr;
        }
        client_tunnel_active = false;
        is_datachannel_connected = false;
    });

    RegisterOnSdpReceivedEvent([&](const std::string& sender_id, const std::string& type, const std::string& sdp) {
        SPDLOG_INFO("[Signaling Event] Received remote SDP {} from peer: {}", type, sender_id);
        if (sender_id == global_client_id) return;
        if (client_tunnel_connection) {
            client_tunnel_connection->SetRemotePeerId(sender_id);
            client_tunnel_connection->HandleRemoteSdp(type, sdp);
        }
    });

    RegisterOnIceReceivedEvent([&](const std::string& sender_id, const std::string& mid, int mline_index, const std::string & sdp) {
        SPDLOG_INFO("[Signaling Event] Received remote ICE candidate (sdpMid: {}, sdpMLineIndex: {}) from peer: {}", mid, mline_index, sender_id);
        if (sender_id == global_client_id) return;
        if (client_tunnel_connection) {
            client_tunnel_connection->SetRemotePeerId(sender_id);
            client_tunnel_connection->HandleRemoteIceCandidate(mid, mline_index, sdp);
        }
    });

    SetUserPresence(active_room_id, global_client_id, true);
    ListenForTargetedMessages(active_room_id, global_client_id);

    if (client_tunnel_connection->Initialize()) {
        client_tunnel_connection->StartConnectionSequence();
    }

    return 0;
}

void ShutdownUsbIpClientTunnel() {
    tunnel_manager.Stop();
    if (client_tunnel_connection) {
        client_tunnel_connection->ShutdownConnection();
        client_tunnel_connection.reset();
        client_tunnel_connection = nullptr;
    }
}