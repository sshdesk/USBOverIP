#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <csignal>

#include "signaling.h"
#include "webrtc_stats.h"
#include "usbip_server.h"
#include "usb_device_passthrough.h"

#include "api/create_peerconnection_factory.h"
#include "api/peer_connection_interface.h"
#include "rtc_base/thread.h"

#include "firebase/app.h"
#include "firebase/firestore.h"
#include "vbox_service_manager.h"

//#define COUNTER 1

rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection = nullptr;
rtc::scoped_refptr<webrtc::DataChannelInterface> native_data_channel = nullptr;
webrtc::DataChannelObserver* global_dc_observer = nullptr;

usbip_server_t* g_server = nullptr;

std::atomic<bool> keep_running{true};
std::atomic<bool> restart_session{false};
int global_choice = 1;
rtc::Thread* global_signaling_thread_ptr = nullptr;
std::string global_saved_room_id;
std::string global_saved_log_level;

// Global parameters for passthrough vs dynamic mode
static bool g_have_passthrough = false;
static std::string g_passthrough_busid = "";
static uint16_t g_passthrough_vid = 0;
static uint16_t g_passthrough_pid = 0;
static bool g_dynamic_mode = false;

// Signal handler for Ctrl+C (SIGINT)
void SignalHandler(int signum) {
    SPDLOG_WARN("Interrupt signal ({}) received. Shutting down gracefully...", signum);
    keep_running = false;
    restart_session = false; 
}

// External C linkage for transmitting data back via DataChannel
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

void InitializeUsbIpServerSession() {
    if (g_server) return;

    // Create server instance
    g_server = usbip_server_create();
    if (!g_server) {
        SPDLOG_ERROR("Failed to create USB/IP WebRTC server context.");
        return;
    }

    if (g_have_passthrough) {
        usb_device_t *dev = usb_device_create_passthrough(g_passthrough_busid.c_str(), g_passthrough_vid, g_passthrough_pid, INVALID_HANDLE_VALUE);
        if (dev) {
            usbip_server_register_device(g_server, dev);
            SPDLOG_INFO("Successfully registered fixed USB passthrough device [BusID: {}, VID:PID={:04X}:{:04X}].", g_passthrough_busid, g_passthrough_vid, g_passthrough_pid);
        } else {
            SPDLOG_WARN("Warning: Failed to capture physical USB device during startup.");
        }
    } else if (g_dynamic_mode) {
        usbip_server_enable_dynamic_devices(g_server, true);
        SPDLOG_INFO("USB/IP Server initialized in dynamic device mode. Devices will be captured upon client selection/import.");
    } else {
        // Fallback default: enable dynamic devices if no passthrough specified
        usbip_server_enable_dynamic_devices(g_server, true);
        SPDLOG_INFO("USB/IP Server initialized with dynamic device selection enabled by default.");
    }
}

class CustomDataChannelObserver : public webrtc::DataChannelObserver {
public:
    void OnStateChange() override {
        if (native_data_channel) {
            SPDLOG_INFO("DataChannel state changed: {}", webrtc::DataChannelInterface::DataStateString(native_data_channel->state()));
            if (native_data_channel->state() == webrtc::DataChannelInterface::kOpen) {
                SPDLOG_INFO("DataChannel Open! USB/IP starting usbip_server_on_channel_open.");
                if (g_server) {
                    usbip_server_on_channel_open(g_server);
                }
            } else if (native_data_channel->state() == webrtc::DataChannelInterface::kClosed ||
                       native_data_channel->state() == webrtc::DataChannelInterface::kClosing) {
                if (g_server) {
                    usbip_server_on_channel_closed(g_server);
                }
            }
        }
    }

#if COUNTER
    void OnMessage(const webrtc::DataBuffer& buffer) override {
        const uint8_t* data_ptr = buffer.data.data();
        size_t data_size = buffer.data.size();

        if (data_size >= sizeof(uint64_t)) {
            uint64_t client_counter = 0;
            std::memcpy(&client_counter, data_ptr, sizeof(uint64_t));
            SPDLOG_INFO("[SERVER RECEIVED] Client Counter #{}", client_counter);
        } else {
            SPDLOG_INFO("PEER BINARY MESSAGE [{} bytes]", data_size);
        }

        static uint64_t server_packet_counter = 0;
        uint64_t current_server_count = ++server_packet_counter;

        std::vector<uint8_t> response_data;
        const uint8_t* count_bytes = reinterpret_cast<const uint8_t*>(&current_server_count);
        response_data.insert(response_data.end(), count_bytes, count_bytes + sizeof(current_server_count));

        if (native_data_channel && native_data_channel->state() == webrtc::DataChannelInterface::kOpen) {
            rtc::CopyOnWriteBuffer reply_buffer(response_data.data(), response_data.size());
            webrtc::DataBuffer data_buffer(reply_buffer, true);
            native_data_channel->Send(data_buffer);
            SPDLOG_INFO("[SERVER SENT] Echo Counter #{}", current_server_count);
        }
    }
#else
    void OnMessage(const webrtc::DataBuffer &buffer) override {
      const uint8_t *data_ptr = buffer.data.data();
      size_t data_size = buffer.data.size();

      SPDLOG_INFO("PEER BINARY MESSAGE [{} bytes] routed through data channel", data_size);

      if (g_server && buffer.binary) {
        usbip_server_on_data_received(g_server, data_ptr, data_size);
      }
    }
#endif

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

        InitializeUsbIpServerSession();

        if (g_server) {
            g_server->data_channel = (rtc_data_channel_t*)native_data_channel.get();
        }
    }

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override {}
    void OnIceCandidatesRemoved(const std::vector<cricket::Candidate>& candidates) override {}
    void OnRenegotiationNeeded() override {}
};
CustomPeerConnectionObserver peer_connection_observer;

static void print_usage(const char *prog) {
    std::cout << "Usage:\n";
    std::cout << "  " << prog << " [--list/-l]\n";
    std::cout << "      List physical USB devices visible to the system.\n\n";
    std::cout << "  " << prog << " [--passthrough <busid> <vid_hex> <pid_hex>]\n";
    std::cout << "      Optional fixed mode: capture a single device at startup.\n\n";
    std::cout << "  " << prog << " [--dynamic]\n";
    std::cout << "      Dynamic mode: client requests device list and selects a device dynamically.\n";
}

int main(int argc, char *argv[]) {
    // Parse command line arguments for USB config options
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--list" || arg == "-l") {
            usb_device_list_physical_devices();
            return 0;
        }
        if (arg == "--passthrough") {
            if (i + 3 >= argc) {
                std::cerr << "--passthrough requires BUSID VID PID\n";
                return 2;
            }
            g_passthrough_busid = argv[++i];
            g_passthrough_vid = (uint16_t)strtoul(argv[++i], NULL, 16);
            g_passthrough_pid = (uint16_t)strtoul(argv[++i], NULL, 16);
            g_have_passthrough = true;
            g_dynamic_mode = false;
        }
        if (arg == "--dynamic") {
            g_dynamic_mode = true;
            g_have_passthrough = false;
        }
    }

    // Default to dynamic mode if neither was explicitly specified
    if (!g_have_passthrough && !g_dynamic_mode) {
        g_dynamic_mode = true;
    }

    // Register Ctrl+C (SIGINT) handler for graceful shutdown
    std::signal(SIGINT, SignalHandler);

    bool tcp_only_flag = false;
    std::unique_ptr<ScopedVBoxServiceManager> vbox_guard = nullptr;
    
    LoadConfigFromDisk(global_saved_room_id, global_saved_log_level, tcp_only_flag);
    SetupLogging(global_saved_log_level);
    active_tcp_only = tcp_only_flag;

    rtc::LogMessage::LogToDebug(rtc::LoggingSeverity::LS_ERROR);

    SPDLOG_INFO("=== USB/IP Server over WebRTC TCP Tunnel App ===");
    SPDLOG_INFO("TCP_ONLY Mode: [{}]", active_tcp_only ? "ENABLED" : "DISABLED");
    SPDLOG_INFO("USBIP Mode: [{}]", g_have_passthrough ? "Fixed Passthrough" : "Dynamic Client-Selected");
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

    // Initial setup lambda/block helper
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

                SPDLOG_INFO("Caller DataChannel created.");
  /*              if (!tunnel_manager.Start()) {
                    SPDLOG_ERROR("Failed to start tunnel manager because TCP bind failed on Caller.");
                } else {
                    SPDLOG_INFO("Caller DataChannel created. Linking to TCP tunnel manager...");
                    tunnel_manager.HandleDataChannel(native_data_channel, global_signaling_thread_ptr);
                }*/
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
            vbox_guard = std::make_unique<ScopedVBoxServiceManager>();

            std::string input_room;
            if (!global_saved_room_id.empty()) {
                char use_saved = 'y';
                std::cout << "Found cached room identifier token in settings file: [" << global_saved_room_id << "]\n";
                std::cout << "Do you want to use this saved token? (y/n): ";
                std::cin >> use_saved;
                
                if (use_saved == 'y' || use_saved == 'Y') {
                    input_room = global_saved_room_id;
                }
            }

            if (input_room.empty()) {
                std::cout << "Enter target room identifier string token: ";
                std::cin >> input_room;
            }

            SaveConfigToDisk(input_room, global_saved_log_level, active_tcp_only);
            InitializeCalleeSignaling(input_room);
        }
    };

    // Run initial session
    initialize_session();

    std::cout << "\n>>> USB/IP Server Active. Press Ctrl+C to quit.\n" << std::endl;

    while (keep_running) {
        if (restart_session) {
            restart_session = false;
            SPDLOG_INFO("=== CLIENT DISCONNECTED: Restarting Server & Tunnel Manager ===");


            native_data_channel = nullptr;
            peer_connection = nullptr;

            if (g_server) {
                usbip_server_on_channel_closed(g_server);
            }

            initialize_session();
            SPDLOG_INFO("=== Server session restarted successfully ===");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Final shutdown teardown on Ctrl+C exit
    SPDLOG_INFO("Performing final cleanup teardown...");
    peer_connection = nullptr;
    native_data_channel = nullptr;

    if (g_server) {
        usbip_server_destroy(g_server);
        g_server = nullptr;
    }

    ShutdownFirebaseFramework(nullptr); 
    SPDLOG_INFO("Application exited cleanly.");
    return 0;
}
