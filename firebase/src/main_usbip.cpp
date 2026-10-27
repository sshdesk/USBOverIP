#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <csignal>
#include <print>
#include <sstream>
#include <map>
#include <chrono>
#include <cstdlib>
#include <ctime>

#include "signaling.h"
#include "webrtc_usbip.h"
#include "webrtc_stats.h"

#include "firebase/app.h"
#include "firebase/firestore.h"
#include "tcptunnel.h"

// USB/IP Core & CLI Header References
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

using namespace usbip;

const auto MAX_HUB_PORTS = 255;
constexpr auto &str_zero_copy = "zero-copy";
constexpr auto &str_low_latency = "low-latency";
auto &msgtable_dll = L"resources";

typedef struct rtc_data_channel rtc_data_channel_t;

std::atomic<bool> keep_running{true};
std::atomic<bool> is_datachannel_connected{false};
std::string shared_input_buffer = "";
std::mutex input_mutex;
std::mutex clients_mutex;

std::string active_room_id = ""; 
std::string global_client_id = "";

std::map<std::string, std::unique_ptr<WebRtcManager_usbip>> connected_clients;
std::unique_ptr<WebRtcManager_usbip> core_server_connection = nullptr;

bool host_server = false;
WebRtcTcpTunnelManager tunnel_manager(3240);

void SignalExitHandler(int signum) {
    SPDLOG_WARN("\nProcess termination signal [{}] intercepted!", signum);
    keep_running = false;
}

void AttachDataChannelCallback(WebRtcManager_usbip* manager) {
    if (!manager) return;
    manager->SetDataChannelStateCallback([](bool is_open) {
        is_datachannel_connected = is_open;
        if (is_open) {
            SPDLOG_INFO("[Heartbeat Manager] DataChannel opened. Pausing Firebase polling & switching to 500ms loop.");
            if (core_server_connection && core_server_connection->GetDataChannel()) {
                if (!tunnel_manager.Start()) {
                    SPDLOG_ERROR("Failed to start tunnel manager because TCP bind failed.");
                } else {
                    SPDLOG_INFO("Caller DataChannel linked. Linking to TCP tunnel manager...");
                    tunnel_manager.HandleDataChannel(core_server_connection->GetDataChannel(), core_server_connection->GetSignalingThread());
                }
            }
        } else {
            SPDLOG_WARN("[Heartbeat Manager] DataChannel closed. Resuming Firebase polling & switching to 30ms loop.");
            tunnel_manager.Stop();
        }
    });
}

namespace {

auto get_ids_data()
{
        win::Resource r(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDR_USB_IDS), RT_RCDATA);
        assert(r);
        return r.str();
}

auto get_version()
{
        win::FileVersion fv;
        auto ver = fv.GetFileVersion();
        return wchar_to_utf8_or(ver);
}

template <typename Cmd, typename Args>
auto pack(Cmd &&cmd, std::shared_ptr<Args> args)
{
        return [cmd = std::forward<Cmd>(cmd), args = std::move(args)] {
                if (!cmd(*args)) {
                        throw CLI::RuntimeError(EXIT_FAILURE);
                }
        };
}

auto serial_validator(const std::string &serial)
{
        std::string s;
        if (!validate_device_serial(serial)) {
                s = get_last_error_msg();
                if (s.empty()) {
                        s = "invalid device serial number";
                }
        }
        return s;
}

void add_cmd_attach(CLI::App &app)
{
        auto r = std::make_shared<attach_args>();

        auto cmd = app.add_subcommand("attach", "Attach remote/persistent USB device(s)")
                ->callback(pack(cmd_attach, r))
                ->require_option(1);

        auto rem = cmd->add_option_group("Remote", "Attach remote USB device");

        rem->add_option("-r,--remote", r->remote, "Hostname/IP of a USB/IP server with exported USB devices")
                ->required();

        auto busid_opt = rem->add_option("-b,--bus-id", r->busid, "Bus Id of the USB device on a server");
        auto select_opt = rem->add_flag("--select", r->select_device,
                "Query the server device list and interactively select a device to attach");
        busid_opt->excludes(select_opt);
        select_opt->excludes(busid_opt);

        rem->add_option("--serial", r->serial, "device serial number")->check(serial_validator);
        rem->add_flag("-t,--terse", r->terse, "Show port number as a result");

        auto stop = rem->add_flag("-x,--stop", r->stop, "Stop attach attempts to this device");
        rem->add_flag("--once", r->once, "Do not start automatic attach attempts if cannot connect")->excludes(stop);

        auto receive = [&mode = r->recv_mode, low_lat = str_low_latency] (const auto &choice)
        {
                mode = choice == low_lat ? receive_mode::low_latency : receive_mode::zero_copy;
        };

        rem->add_option_function<std::string>("--receive-mode", receive, "How to receive data from the network")
                ->check(CLI::IsMember({str_zero_copy, str_low_latency}))
                ->default_str(str_zero_copy)
                ->excludes(stop);

        auto stop_all = cmd->add_option_group("Stop")
                ->add_flag("-X,--stop-all", r->stop_all, "Stop all active attach attempts");

        stop->excludes(stop_all);

        cmd->add_option_group("Persistent", "Attach persistent USB device(s)")
                ->add_flag("-s,--stashed,--persistent", r->persistent, "Attach persistent device(s) stashed by 'port --stash'");
}

void add_cmd_detach(CLI::App &app)
{
        auto r = std::make_shared<detach_args>();

        auto cmd = app.add_subcommand("detach", "Detach a remote USB device")
                ->callback(pack(cmd_detach, r))
                ->require_option(1);

        auto opt_port = cmd->add_option("-p,--port", r->port, "Hub port number the device is plugged in")
                ->check(CLI::Range(1, MAX_HUB_PORTS));

        cmd->add_option("-a,--all", "Detach all devices")
                ->each([&port = r->port] (const auto&) noexcept { port = vhci::port_all_closeonly; })
                ->check(CLI::IsMember({"closeonly"}))
                ->expected(0, 1)
                ->excludes(opt_port);
}

void add_cmd_list(CLI::App &app)
{
        auto r = std::make_shared<list_args>();

        auto cmd = app.add_subcommand("list", "List exportable/persistent USB devices")
                ->callback(pack(cmd_list, r))
                ->require_option(1);

        cmd->add_option_group("Remote", "List exportable devices on a remote")
                ->add_option("-r,--remote", r->remote, "List exportable devices on a remote")
                ->required();

        cmd->add_option_group("Persistent", "List persistent USB devices")
                ->add_flag("-s,--stashed,--persistent", r->persistent, "List persistent devices stashed by 'port --stash'");
}

void add_cmd_port(CLI::App &app)
{
        auto r = std::make_shared<port_args>();

        auto cmd = app.add_subcommand("port", "Show/stash imported USB devices")
                ->callback(pack(cmd_port, r));

        cmd->add_flag("-s,--stash,--persistent", r->persistent,
                      "Devices listed by the command will be attached every time the driver is loaded (aka persistent devices)");

        cmd->add_option("number", r->ports, "Hub port number")
                ->check(CLI::Range(1, MAX_HUB_PORTS))
                ->expected(1, MAX_HUB_PORTS);
}

auto& get_resource_module() noexcept
{
        static HModule mod(LoadLibraryEx(msgtable_dll, nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR));
        return mod;
}

void init(CLI::App &app)
{
        static std::string version_str = get_version();
        app.set_version_flag("-V,--version", version_str);

        app.add_flag("-d,--debug",
                [] (auto) { log::set_debug(true); }, "Debug output");

        app.add_option("-t,--tcp-port", global_args.tcp_port, "TCP/IP port number of USB/IP server")
                ->check(CLI::Range(1024, USHRT_MAX))
                ->default_str(global_args.tcp_port);

        add_cmd_attach(app);
        add_cmd_detach(app);
        add_cmd_list(app);
        add_cmd_port(app);

        app.require_subcommand(1);
}

// Keep a single persistent CLI app instance to avoid repeated dynamic allocations and destructor crashes
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

std::string usbip::get_last_error_msg(DWORD msg_id) {
    auto &mod = get_resource_module();
    return format_message(mod.get(), msg_id);
}

const UsbIds& usbip::get_ids() {
    static UsbIds ids(get_ids_data());
    assert(ids);
    return ids;
}

int usbipCLI(int argc, wchar_t *argv[]) {
    log::init();
    libusbip::set_debug_output(log::debug_msg);

    InitWinSock2 ws2;
    if (!ws2) {
        log::critical("can't initialize Windows Sockets 2, {}", get_last_error_msg());
        return EXIT_FAILURE;
    }

    CLI::App &app = get_usbip_app();
    app.clear(); // Reset subcommand states and parsed values for the new invocation

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        return app.exit(e);
    }

    return EXIT_SUCCESS;
}

int wmain(int argc, wchar_t *argv[]) {
    SetupLogging("info");
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    rtc::LogMessage::LogToDebug(rtc::LoggingSeverity::LS_ERROR);

    std::signal(SIGINT, SignalExitHandler);
    std::signal(SIGTERM, SignalExitHandler);

    global_client_id = "cpp_peer_" + std::to_string(std::rand() % 90000 + 1000);

    if (!InitializeFirebaseFramework()) {
        SPDLOG_ERROR("Firebase setup crash exception encountered.");
        return -1;
    }
    SPDLOG_INFO("Successfully authenticated with Cloud Firestore.");
    SPDLOG_INFO("Your Client Identity Token: [{}]", global_client_id);

    std::string saved_room_id = "";
    std::string saved_log_level = "info";
    LoadConfigFromDisk(saved_room_id, saved_log_level);

    std::string input_room;
    if (!saved_room_id.empty()) {
        char use_saved = 'y';
        std::cout << "Found cached room identifier token: [" << saved_room_id << "]\n";
        std::cout << "Use saved token? (y/n): ";
        std::cin >> use_saved;
        if (use_saved == 'y' || use_saved == 'Y') {
            input_room = saved_room_id;
        }
        std::cin.ignore();
    }

    if (input_room.empty()) {
        std::cout << "Enter target room identifier string token (or leave blank to create a new room): ";
        std::getline(std::cin, input_room);
    }

    if (input_room.empty()) {
        active_room_id = GenerateRandomRoomId(20);
        host_server = true; 
        SPDLOG_INFO("[Room Creation] Generated fresh room key and set role to Host Server: {}", active_room_id);
    } else {
        active_room_id = input_room;
        host_server = IsRoomEmpty(active_room_id); 
        SPDLOG_INFO("[Room Join] Target room ID: {}. Assigned Role -> {}", active_room_id, host_server ? "Host Server" : "Client");
    }

    SaveConfigToDisk(active_room_id, "info");

    SPDLOG_INFO("Evaluating capacity constraints for room matrix [{}]...", active_room_id);
    if (!VerifyAndReserveRoomCapacity(active_room_id)) {
        std::cout << "\n[CRITICAL REJECT] Access Denied: Room capacity limit reached.\n" << std::endl;
        ShutdownFirebaseFramework(nullptr);
        return -1;
    }

    if (!host_server) {
        SPDLOG_INFO("[Client Init] Instantiating WebRtcManager_usbip caller node to generate connection offer...");
        core_server_connection = std::make_unique<WebRtcManager_usbip>(active_room_id, global_client_id, true);
        AttachDataChannelCallback(core_server_connection.get());
        core_server_connection->SetRemotePeerId("ALL");
        if (core_server_connection->Initialize()) {
            core_server_connection->StartConnectionSequence();
        }
    }

    RegisterOnRoomReadyEvent([&](const std::string& ready_room_id) {
        SPDLOG_WARN("=================================================================");
        SPDLOG_WARN("[EVENT TRIGGERED] Room [{}] is full and ready for meeting!", ready_room_id);
        SPDLOG_WARN("=================================================================");
    });

    RegisterOnPeerLeftEvent([&](const std::string& peer_client_id) {
        SPDLOG_ERROR("=================================================================");
        SPDLOG_ERROR("[EVENT NOTICE] Peer [{}] has disconnected or left the room.", peer_client_id);
        SPDLOG_ERROR("=================================================================");
        
        if (host_server) {
            std::lock_guard<std::mutex> lock(clients_mutex);
            auto it = connected_clients.find(peer_client_id);
            if (it != connected_clients.end()) {
                it->second->ShutdownConnection();
                connected_clients.erase(it);
            }
            is_datachannel_connected = false;
            SPDLOG_INFO("[WebRTC State Reset] Standing by as Host. Waiting for new peer connection...");
        } else {
            if (core_server_connection) {
                core_server_connection->ShutdownConnection();
                core_server_connection = nullptr;
            }
            SPDLOG_WARN("Host disconnected. Exiting signaling session room.");
            keep_running = false;
        }
    });

    RegisterOnSdpReceivedEvent([&](const std::string& sender_id, const std::string& type, const std::string& sdp) {
        SPDLOG_INFO("[Signaling Event] Received remote SDP {} from peer: {}", type, sender_id);
        if (sender_id == global_client_id) return;

        if (host_server) {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (connected_clients.find(sender_id) == connected_clients.end()) {
                SPDLOG_INFO("[Server Hub] Spawning new WebRtcManager_usbip instance for incoming client: {}", sender_id);
                auto client_rtc = std::make_unique<WebRtcManager_usbip>(active_room_id, global_client_id, false);
                AttachDataChannelCallback(client_rtc.get());
                client_rtc->SetRemotePeerId(sender_id);
                if (client_rtc->Initialize()) {
                    connected_clients[sender_id] = std::move(client_rtc);
                }
            }
            connected_clients[sender_id]->SetRemotePeerId(sender_id);
            connected_clients[sender_id]->HandleRemoteSdp(type, sdp);
        } else {
            if (core_server_connection) {
                core_server_connection->SetRemotePeerId(sender_id);
                core_server_connection->HandleRemoteSdp(type, sdp);
            }
        }
    });

    RegisterOnIceReceivedEvent([&](const std::string& sender_id, const std::string& mid, int mline_index, const std::string & sdp) {
        SPDLOG_INFO("[Signaling Event] Received remote ICE candidate (sdpMid: {}, sdpMLineIndex: {}) from peer: {}", mid, mline_index, sender_id);
        if (sender_id == global_client_id) return;

        if (host_server) {
            std::lock_guard<std::mutex> lock(clients_mutex);
            auto it = connected_clients.find(sender_id);
            if (it != connected_clients.end() && it->second) {
                it->second->SetRemotePeerId(sender_id);
                it->second->HandleRemoteIceCandidate(mid, mline_index, sdp);
            }
        } else {
            if (core_server_connection) {
                core_server_connection->SetRemotePeerId(sender_id);
                core_server_connection->HandleRemoteIceCandidate(mid, mline_index, sdp);
            }
        }
    });

    SetUserPresence(active_room_id, global_client_id, true);
    ListenForTargetedMessages(active_room_id, global_client_id);

    std::cout << "\n=======================================================" << std::endl;
    std::cout << "Waiting for Peer-to-Peer WebRTC connection to establish..." << std::endl;
    std::cout << "=======================================================\n" << std::endl;

    auto last_heartbeat_time = std::chrono::steady_clock::now();

    while (keep_running && !is_datachannel_connected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto now = std::chrono::steady_clock::now();
        if (/*sleep_ms == 30 &&*/ std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat_time).count() >= 5) {
            SetUserPresence(active_room_id, global_client_id, true);
            last_heartbeat_time = now;
        }
    }

    if (is_datachannel_connected) {
        SPDLOG_INFO("WebRTC connection established successfully. Starting interactive USB/IP CLI shell...");
        std::cout << "\n[Connected via WebRTC] Enter USB/IP commands (e.g., attach -r 127.0.0.1 -b 1-1, or 'exit' to quit):\n";

        std::string command_line;
        while (keep_running && is_datachannel_connected) {
            std::cout << "usbip> ";
            if (!std::getline(std::cin, command_line)) {
                break;
            }

            if (command_line.empty()) {
                continue;
            }

            if (command_line == "exit" || command_line == "quit") {
                keep_running = false;
                break;
            }

            std::vector<std::string> args_utf8;
            args_utf8.push_back("usbip");

            std::istringstream iss(command_line);
            std::string token;
            while (iss >> token) {
                args_utf8.push_back(token);
            }

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
                std::println(stderr, "CLI execution exception: {}", e.what());
            }
        }
    }

    HandleGracefulExit(active_room_id, global_client_id);
    tunnel_manager.Stop();

    // Ensure 'detach -a' is called before exiting the application
    {
        std::vector<std::wstring> detach_args_w = { L"usbip", L"detach", L"-a" };
        std::vector<wchar_t*> detach_argv_vec;
        for (auto &wstr : detach_args_w) {
            detach_argv_vec.push_back(wstr.data());
        }
        try {
            usbipCLI(static_cast<int>(detach_argv_vec.size()), detach_argv_vec.data());
            SPDLOG_INFO("Successfully executed automatic 'detach -a' on exit.");
        } catch (const std::exception &e) {
            SPDLOG_ERROR("Failed to execute 'detach -a' on exit: {}", e.what());
        }
    }

    if (core_server_connection) {
        core_server_connection->ShutdownConnection();
        core_server_connection.reset();
    }
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (auto& pair : connected_clients) {
            if (pair.second) {
                pair.second->ShutdownConnection();
            }
        }
        connected_clients.clear();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    ShutdownFirebaseFramework(nullptr);
    return EXIT_SUCCESS;
}
