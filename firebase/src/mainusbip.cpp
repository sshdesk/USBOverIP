#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <csignal>
#include <print>

#include "signaling.h"
#include "webrtc_stats.h"

#include "api/create_peerconnection_factory.h"
#include "api/peer_connection_interface.h"
#include "rtc_base/thread.h"

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

// Global WebRTC / State definitions
rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection = nullptr;
rtc::scoped_refptr<webrtc::DataChannelInterface> native_data_channel = nullptr;
webrtc::DataChannelObserver* global_dc_observer = nullptr;
WebRtcTcpTunnelManager tunnel_manager(3240);

std::atomic<bool> keep_running{true};
std::atomic<bool> restart_session{false};
int global_choice = 1;
rtc::Thread* global_signaling_thread_ptr = nullptr;
std::string global_saved_room_id;
std::string global_saved_log_level;

// Graceful exit loop capture
void SignalHandler(int signum) {
    SPDLOG_WARN("Interrupt signal ({}) received. Shutting down gracefully...", signum);
    keep_running = false;
    restart_session = false;
}


extern "C" {
    bool rtc_data_channel_send(rtc_data_channel_t *dc, const void *data, size_t size) {
        (void)dc;
        if (!native_data_channel || native_data_channel->state() != webrtc::DataChannelInterface::kOpen) {
            return false;
        }
        rtc::CopyOnWriteBuffer buffer(reinterpret_cast<const uint8_t*>(data), size);
        webrtc::DataBuffer data_buffer(buffer, true);
        return native_data_channel->Send(data_buffer);
    }
}

// WebRTC Observers
class CustomDataChannelObserver : public webrtc::DataChannelObserver {
public:
    void OnStateChange() override {
        if (native_data_channel) {
            SPDLOG_INFO("DataChannel state changed: {}", webrtc::DataChannelInterface::DataStateString(native_data_channel->state()));
        }
    }
    void OnMessage(const webrtc::DataBuffer &buffer) override {
        size_t data_size = buffer.data.size();
        SPDLOG_INFO("PEER BINARY MESSAGE [{} bytes] dropped because server-side engines are inactive.", data_size);
    }
    void OnBufferedAmountChange(uint64_t previous_amount) override {}
};
CustomDataChannelObserver data_channel_observer;

class CustomPeerConnectionObserver : public webrtc::PeerConnectionObserver {
public:
    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
        if (active_room_id.empty() || !db) return;

        std::string candidate_str;
        if (!candidate->ToString(&candidate_str)) return;

        std::string sdp_mid = candidate->sdp_mid();
        int sdp_mline_index = candidate->sdp_mline_index();
        std::string protocol = (candidate_str.find(" tcp ") != std::string::npos) ? "TCP" : "UDP";

        SPDLOG_INFO("[LOCAL ICE CANDIDATE GATHERED]");
        SPDLOG_INFO("  |--> Protocol:      {}", protocol);
        SPDLOG_INFO("  |--> sdpMid:        {}", sdp_mid);
        SPDLOG_INFO("  |--> sdpMLineIndex: {}", sdp_mline_index);
        SPDLOG_INFO("  |--> Candidate SDP: {}", candidate_str);

        if (active_tcp_only && protocol == "UDP") {
            SPDLOG_WARN("TCP_ONLY mode active: Filtered out local UDP ICE candidate.");
            return;
        }

        firebase::firestore::MapFieldValue cand_payload = {
            {"candidate", firebase::firestore::FieldValue::String(candidate_str)},
            {"sdpMid", firebase::firestore::FieldValue::String(sdp_mid)},
            {"sdpMLineIndex", firebase::firestore::FieldValue::Integer(sdp_mline_index)}
        };

        std::string sub_collection = (native_data_channel != nullptr) ? "callerCandidates" : "calleeCandidates";
        
        db->Collection("rooms").Document(active_room_id)
          .Collection(sub_collection).Add(cand_payload);
    }

    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {
        std::string state_str;
        switch (new_state) {
            case webrtc::PeerConnectionInterface::kIceConnectionNew:          state_str = "NEW"; break;
            case webrtc::PeerConnectionInterface::kIceConnectionChecking:     state_str = "CHECKING"; break;
            case webrtc::PeerConnectionInterface::kIceConnectionConnected:    state_str = "CONNECTED"; break;
            case webrtc::PeerConnectionInterface::kIceConnectionCompleted:    state_str = "COMPLETED"; break;
            case webrtc::PeerConnectionInterface::kIceConnectionFailed:       state_str = "FAILED"; break;
            case webrtc::PeerConnectionInterface::kIceConnectionDisconnected: state_str = "DISCONNECTED"; break;
            case webrtc::PeerConnectionInterface::kIceConnectionClosed:       state_str = "CLOSED"; break;
            default: state_str = "UNKNOWN"; break;
        }
        SPDLOG_INFO("==> [ICE CONNECTION STATE]: {}", state_str);

        if (new_state == webrtc::PeerConnectionInterface::kIceConnectionConnected && peer_connection) {
            peer_connection->GetStats(NominatedCandidatePairStatsCallback::Create());
        }
    }

    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override {
        std::string g_state = (new_state == webrtc::PeerConnectionInterface::kIceGatheringGathering) ? "GATHERING" : "COMPLETE";
        SPDLOG_INFO("==> [ICE GATHERING STATE]: {}", g_state);
    }

    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override {
        std::string pc_state;
        switch (new_state) {
            case webrtc::PeerConnectionInterface::PeerConnectionState::kNew:          pc_state = "New"; break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kConnecting:   pc_state = "Connecting"; break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:    pc_state = "Connected"; break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected: pc_state = "Disconnected"; break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:       pc_state = "Failed"; break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:       pc_state = "Closed"; break;
        }
        SPDLOG_INFO("==> [PEER CONNECTION STATE]: {}", pc_state);

        // Trigger session restart when client disconnects, fails, or closes
        if (new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected ||
            new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kFailed ||
            new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kClosed) {
            SPDLOG_WARN("Peer connection state indicates client disconnection. Scheduling session restart...");
            restart_session = true;
        }
    }

    void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {
        SPDLOG_INFO("--> Remote DataChannel negotiated and attached.");
        native_data_channel = data_channel;
        native_data_channel->RegisterObserver(global_dc_observer);
    }
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override {}
    void OnIceCandidatesRemoved(const std::vector<cricket::Candidate>& candidates) override {}
    void OnRenegotiationNeeded() override {}
};
CustomPeerConnectionObserver peer_connection_observer;


namespace {
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
            if (s.empty()) s = "invalid device serial number";
        }
        return s;
    }

    void add_cmd_attach(CLI::App &app) {
        auto r = std::make_shared<attach_args>();
        auto cmd = app.add_subcommand("attach", "Attach remote/persistent USB device(s)")
            ->callback(pack(cmd_attach, r))
            ->require_option(1);

        auto rem = cmd->add_option_group("Remote", "Attach remote USB device");
        rem->add_option("-r,--remote", r->remote, "Hostname/IP of a USB/IP server with exported USB devices")->required();
        auto busid_opt = rem->add_option("-b,--bus-id", r->busid, "Bus Id of the USB device on a server");
        auto select_opt = rem->add_flag("--select", r->select_device, "Query the server device list and interactively select a device to attach");
        busid_opt->excludes(select_opt);
        select_opt->excludes(busid_opt);

        rem->add_option("--serial", r->serial, "device serial number")->check(serial_validator);
        rem->add_flag("-t,--terse", r->terse, "Show port number as a result");
        auto stop = rem->add_flag("-x,--stop", r->stop, "Stop attach attempts to this device");
        rem->add_flag("--once", r->once, "Do not start automatic attach attempts if cannot connect")->excludes(stop);

        auto receive = [&mode = r->recv_mode, low_lat = str_low_latency](const auto &choice) {
            mode = choice == low_lat ? receive_mode::low_latency : receive_mode::zero_copy;
        };
        rem->add_option_function<std::string>("--receive-mode", receive, "How to receive data from the network")
            ->check(CLI::IsMember({str_zero_copy, str_low_latency}))
            ->default_str(str_zero_copy)
            ->excludes(stop);

        auto stop_all = cmd->add_option_group("Stop")->add_flag("-X,--stop-all", r->stop_all, "Stop all active attach attempts");
        stop->excludes(stop_all);
        cmd->add_option_group("Persistent", "Attach persistent USB device(s)")
            ->add_flag("-s,--stashed,--persistent", r->persistent, "Attach persistent device(s) stashed by 'port --stash'");
    }

    void add_cmd_detach(CLI::App &app) {
        auto r = std::make_shared<detach_args>();
        auto cmd = app.add_subcommand("detach", "Detach a remote USB device")
            ->callback(pack(cmd_detach, r))
            ->require_option(1);

        auto opt_port = cmd->add_option("-p,--port", r->port, "Hub port number the device is plugged in")->check(CLI::Range(1, MAX_HUB_PORTS));
        cmd->add_option("-a,--all", "Detach all devices")
            ->each([&port = r->port](const auto&) noexcept { port = vhci::port_all_closeonly; })
            ->check(CLI::IsMember({"closeonly"}))
            ->expected(0, 1)
            ->excludes(opt_port);
    }

    void add_cmd_list(CLI::App &app) {
        auto r = std::make_shared<list_args>();
        auto cmd = app.add_subcommand("list", "List exportable/persistent USB devices")
            ->callback(pack(cmd_list, r))
            ->require_option(1);

        cmd->add_option_group("Remote", "List exportable USB devices")
            ->add_option("-r,--remote", r->remote, "List exportable devices on a remote")->required();
        cmd->add_option_group("Persistent", "List persistent USB devices")
            ->add_flag("-s,--stashed,--persistent", r->persistent, "List persistent devices stashed by 'port --stash'");
    }

    void add_cmd_port(CLI::App &app) {
        auto r = std::make_shared<port_args>();
        auto cmd = app.add_subcommand("port", "Show/stash imported USB devices")->callback(pack(cmd_port, r));
        cmd->add_flag("-s,--stash,--persistent", r->persistent, "Devices listed by the command will be attached every time the driver is loaded");
        cmd->add_option("number", r->ports, "Hub port number")->check(CLI::Range(1, MAX_HUB_PORTS))->expected(1, MAX_HUB_PORTS);
    }

    auto& get_resource_module() noexcept {
        static HModule mod(LoadLibraryEx(msgtable_dll, nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR));
        return mod;
    }

    void init_cli_options(CLI::App &app) {
      app.set_version_flag("-V,--version", "1");
        app.add_flag("-d,--debug", [](auto) { log::set_debug(true); }, "Debug output");
        app.add_option("-t,--tcp-port", global_args.tcp_port, "TCP/IP port number of USB/IP server")
            ->check(CLI::Range(1024, USHRT_MAX))
            ->default_str(global_args.tcp_port);

        add_cmd_attach(app);
        add_cmd_detach(app);
        add_cmd_list(app);
        add_cmd_port(app);
    }
}

std::string usbip::get_last_error_msg(DWORD msg_id) {
    auto &mod = get_resource_module();
    return format_message(mod.get(), msg_id);
}

const UsbIds& usbip::get_ids() {
    static UsbIds ids(get_ids_data());
    assert(ids);
    return ids;
}

// Unified Execution Entry Point matching wmain requirements
int wmain(int argc, wchar_t *argv[]) {
    // 1. Fire up baseline driver hooks and system environment checks
    log::init();
    libusbip::set_debug_output(log::debug_msg);

    //if (!get_resource_module()) {
    //    auto err = GetLastError();
    //    log::critical("can't load '{}.dll', error {:#x} {}", "resources", err, format_message(err));
    //    return EXIT_FAILURE;
    //}

    InitWinSock2 ws2;
    if (!ws2) {
        log::critical("can't initialize Windows Sockets 2, {}", get_last_error_msg());
        return EXIT_FAILURE;
    }

    // 2. Build and parse argument layout vectors
    CLI::App app("USB/IP Client & WebRTC Tunnel Utility");
    init_cli_options(app);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        return app.exit(e);
    }

    // 3. Fall into active WebRTC Tunnel loop infrastructure if any commands executed fine
    std::signal(SIGINT, SignalHandler);
    bool tcp_only_flag = false;


    LoadConfigFromDisk(global_saved_room_id, global_saved_log_level, tcp_only_flag);
    SetupLogging(global_saved_log_level);
    active_tcp_only = tcp_only_flag;

    rtc::LogMessage::LogToDebug(rtc::LoggingSeverity::LS_ERROR);

    SPDLOG_INFO("=== USB/IP Server over WebRTC TCP Tunnel App ===");
    SPDLOG_INFO("TCP_ONLY Mode: [{}]", active_tcp_only ? "ENABLED" : "DISABLED");
    global_dc_observer = &data_channel_observer;

    if (!InitializeFirebaseFramework()) {
        SPDLOG_ERROR("Initialization Fault: Check google-services.json context.");
        return -1;
    }
    SPDLOG_INFO("Successfully authenticated with Firebase Database.");

    std::unique_ptr<rtc::Thread> network_thread = rtc::Thread::CreateWithSocketServer();
    std::unique_ptr<rtc::Thread> worker_thread = rtc::Thread::Create();
    std::unique_ptr<rtc::Thread> signaling_thread = rtc::Thread::Create();
    network_thread->Start(); worker_thread->Start(); signaling_thread->Start();

    global_signaling_thread_ptr = signaling_thread.get();

    webrtc::PeerConnectionInterface::RTCConfiguration config;
    webrtc::PeerConnectionInterface::IceServer server;
    
    // TURN/STUN Server assignment (use TCP port for STUN/TURN if TCP_ONLY is true)
    if (active_tcp_only) {
        server.urls.push_back("stun:stun.l.google.com:19302");
        //server.urls.push_back("stun:global.stun.twilio.com:3478?transport=tcp");
        config.tcp_candidate_policy = webrtc::PeerConnectionInterface::kTcpCandidatePolicyEnabled;
        
    } else {
        server.urls.push_back("stun:stun.l.google.com:19302");
    }
    config.servers.push_back(server);

    webrtc::PeerConnectionFactoryDependencies factory_deps;
    factory_deps.network_thread = network_thread.get();
    factory_deps.worker_thread = worker_thread.get();
    factory_deps.signaling_thread = signaling_thread.get();

    auto factory = webrtc::CreateModularPeerConnectionFactory(std::move(factory_deps));

    if (!factory) {
        SPDLOG_ERROR("Fatal Error: Failed to build modular WebRTC PC Factory Interface.");
        return -1;
    }

    std::cout << "\nSelect Execution Profile:\n1. Host Room (Caller Mode)\n2. Connect Room (Callee Mode)\nChoice: ";
    std::cin >> global_choice;

    auto initialize_session = [&]() {
        webrtc::PeerConnectionDependencies dependencies(&peer_connection_observer);
        auto pc_result = factory->CreatePeerConnectionOrError(config, std::move(dependencies));
        if (!pc_result.ok()) {
            SPDLOG_ERROR("Failed to create PeerConnection.");
            return;
        }
        peer_connection = pc_result.MoveValue();

        if (global_choice == 1) {
            webrtc::DataChannelInit dc_config;
            dc_config.maxRetransmits = std::nullopt; 
            dc_config.maxRetransmitTime = std::nullopt;
            dc_config.ordered = true;

            auto dc_result = peer_connection->CreateDataChannelOrError("usbip-tcp-channel", &dc_config);
            if (dc_result.ok()) {
                native_data_channel = dc_result.MoveValue();
                native_data_channel->RegisterObserver(global_dc_observer);

                if (!tunnel_manager.Start()) {
                    SPDLOG_ERROR("Failed to start tunnel manager because TCP bind failed on Caller.");
                } else {
                    SPDLOG_INFO("Caller DataChannel created. Linking to TCP tunnel manager...");
                    tunnel_manager.HandleDataChannel(native_data_channel, global_signaling_thread_ptr);
                }
            }

            if (!global_saved_room_id.empty()) {
                SPDLOG_INFO("Found cached room ID: {}. Validating with Firebase...", global_saved_room_id);
                ValidateAndInitializeCaller(global_saved_room_id);
            } else {
                SPDLOG_INFO("No saved room found. Creating new room...");
                peer_connection->CreateOffer(
                    new rtc::RefCountedObject<CallerCreateSessionDescriptionObserver>(), 
                    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions()
                );
            }
        } else {
      
            std::string input_room = global_saved_room_id;
            if (input_room.empty()) {
                std::cout << "Enter target room identifier string token: ";
                std::cin >> input_room;
            }
            SaveConfigToDisk(input_room, global_saved_log_level, active_tcp_only);
            InitializeCalleeSignaling(input_room);
        }
    };

    initialize_session();

    while (keep_running) {
        if (restart_session) {
            restart_session = false;
            tunnel_manager.Stop();
            native_data_channel = nullptr;
            peer_connection = nullptr;
            initialize_session();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    tunnel_manager.Stop();
    peer_connection = nullptr;
    native_data_channel = nullptr;
    ShutdownFirebaseFramework(nullptr);
    return EXIT_SUCCESS;
}
