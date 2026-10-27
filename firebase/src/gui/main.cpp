#include "gui.h"
#include "logsync.h"
#include "usbip_commands.h"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <csignal>
#include <memory>
#include <map>
#include <vector>

#include "signaling.h"
#include "../webrtc_usbipd.h"
#include "firebase/app.h"
#include "firebase/firestore.h"


#include "usbip_server.h"
#include "usb_device_passthrough.h"
#include "../vbox_service_manager.h"


std::atomic<bool> keep_running{true};
std::atomic<bool> is_datachannel_connected{false};
std::string shared_input_buffer = "";
std::mutex input_mutex;
std::mutex clients_mutex;

std::string active_room_id = ""; 
std::string global_client_id = "";

std::map<std::string, std::unique_ptr<WebRtcManager_usbipd>> connected_clients;
std::unique_ptr<WebRtcManager_usbipd> core_server_connection = nullptr;

bool host_server = false;
bool share_usb_triggered = false;
bool attach_triggered = false;

// USBIP Globals
usbip_server_t* g_server = nullptr;
bool g_have_passthrough = false;
std::string g_passthrough_busid = "";
uint16_t g_passthrough_vid = 0;
uint16_t g_passthrough_pid = 0;
bool g_dynamic_mode = true;


std::vector<std::string> usb_device_list_physical_devices(void);

void InitializeUsbIpServerSession() {
    if (g_server) return;

    g_server = usbip_server_create();
    if (!g_server) {
        SPDLOG_ERROR("Failed to create USB/IP WebRTC server context.");
        return;
    }

    usbip_server_enable_dynamic_devices(g_server, true);
    SPDLOG_INFO("USB/IP Server initialized in dynamic device mode.");
}

void AttachDataChannelCallback(WebRtcManager_usbipd* manager) {
    if (!manager) return;
    manager->SetDataChannelStateCallback([](bool is_open) {
        is_datachannel_connected = is_open;
        if (is_open) {
            SPDLOG_INFO("[Heartbeat Manager] DataChannel opened. Pausing Firebase polling & switching to 500ms loop.");
        } else {
            SPDLOG_WARN("[Heartbeat Manager] DataChannel closed. Resuming Firebase polling & switching to 30ms loop.");
        }
    });
}

static bool is_session_starting = false;

void OnShareUSBButtonClicked(GUI& gui) {
    std::cout << "\n[GUI Event] ShareUSB button clicked. Initializing session sequence...\n" << std::endl;
    share_usb_triggered = true;

    std::vector<std::string> usb_devices = usb_device_list_physical_devices();
    for (const auto& dev_str : usb_devices) {
        gui.addListItem(dev_str);
    }
    
    if (is_datachannel_connected || is_session_starting) {
      SPDLOG_INFO("[GUI Event] Session is already active or initializing. Ignoring re-init.");
      return;
    }

    is_session_starting = true; // Lock immediately to block concurrent clicks

    if (active_room_id.empty()) {
        active_room_id = GenerateRandomRoomId(20);
        host_server = true; 
        SPDLOG_INFO("[Room Creation] Generated fresh room key and set role to Host Server: {}", active_room_id);
    } else {
        host_server = IsRoomEmpty(active_room_id); 
        SPDLOG_INFO("[Room Join] Target room ID: {}. Assigned Role -> {}", active_room_id, host_server ? "Host Server" : "Client");
    }

    SaveConfigToDisk(active_room_id, "info");

    SPDLOG_INFO("Evaluating capacity constraints for room matrix [{}]...", active_room_id);
    if (!VerifyAndReserveRoomCapacity(active_room_id)) {
        std::cout << "\n[CRITICAL REJECT] Access Denied: Room capacity limit reached.\n" << std::endl;
        ShutdownFirebaseFramework(nullptr);
        return;
    }

    InitializeUsbIpServerSession();

    static std::unique_ptr<ScopedVBoxServiceManager> vbox_guard = nullptr;
    if (host_server) {
        vbox_guard = std::make_unique<ScopedVBoxServiceManager>();
    }

    if (!host_server) {
        SPDLOG_INFO("[Client Init] Instantiating WebRtcManager caller node to generate connection offer...");
        core_server_connection = std::make_unique<WebRtcManager_usbipd>(active_room_id, global_client_id, true);
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
                SPDLOG_INFO("[Server Hub] Spawning new WebRtcManager instance for incoming client: {}", sender_id);
                auto client_rtc = std::make_unique<WebRtcManager_usbipd>(active_room_id, global_client_id, false);
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
}





#if 0
void FetchAndDisplayRemoteDevices(GUI &gui) {
  // 1. Redirect std::cout to an in-memory string stream (zero disk I/O)
  std::streambuf *original_cout = std::cout.rdbuf();
  std::ostringstream oss;
  std::cout.rdbuf(oss.rdbuf());

  // 2. Execute the usbip command over the local tunnel proxy port
  execute_usbip_command({"usbip", "list", "-r", "127.0.0.1"});

  // 3. Restore the original std::cout buffer immediately
  std::cout.rdbuf(original_cout);

  // 4. Read and parse the captured text directly from RAM
  std::string output = oss.str();
  std::stringstream stream(output);
  std::string line;

  gui.clearListItems();
  gui.addListItem("--- Select a Device to Attach ---");

  std::string current_busid = "";
  std::string current_desc = "";

  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    // Group multi-line records into a single clean line
    size_t colon_pos = line.find(" : ");
    if (colon_pos != std::string::npos &&
        line.find("/sys/") == std::string::npos &&
        line.find("Type:") == std::string::npos) {
      if (!current_busid.empty()) {
        std::string formatted_item = "[" + current_busid + "] " + current_desc;
        gui.addListItem(formatted_item);
        SPDLOG_INFO("[Parsed Device] {}", formatted_item);
      }

      std::stringstream ss(line);
      ss >> current_busid;
      current_desc = line.substr(colon_pos + 3);
    }
  }

  // Push the final parsed device record
  if (!current_busid.empty()) {
    std::string formatted_item = "[" + current_busid + "] " + current_desc;
    gui.addListItem(formatted_item);
    SPDLOG_INFO("[Parsed Device] {}", formatted_item);
  }
}
#endif



void FetchAndDisplayRemoteDevices(GUI &gui) {
  std::string temp_file = "usbip_devices_temp.txt";

  FILE *fp = nullptr;
  if (freopen_s(&fp, temp_file.c_str(), "w", stdout) != 0) {
    SPDLOG_ERROR("Failed to redirect stdout for device listing.");
    return;
  }

  execute_usbip_command({"usbip", "list", "-r", "127.0.0.1"});

  fflush(stdout);
  if (fp) {
    fclose(fp);
  }

  std::ifstream infile(temp_file);
  std::string line;

  gui.clearListItems();
  gui.addListItem("--- Select a Device to Attach ---");

  std::string current_busid = "";
  std::string current_desc = "";

  while (std::getline(infile, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    // Check if line starts with a bus ID pattern (e.g., "   1-10    : ")
    size_t colon_pos = line.find(" : ");
    if (colon_pos != std::string::npos &&
        line.find("/sys/") == std::string::npos &&
        line.find("Type:") == std::string::npos) {
      // If we were already tracking a device, push it to the GUI first
      if (!current_busid.empty()) {
        std::string formatted_item = "[" + current_busid + "] " + current_desc;
        gui.addListItem(formatted_item);
        SPDLOG_INFO("[Parsed Device] {}", formatted_item);
      }

      // Extract new bus id and description
      std::stringstream ss(line);
      ss >> current_busid;
      current_desc = line.substr(colon_pos + 3);
    }
  }

  // Don't forget to push the last parsed device
  if (!current_busid.empty()) {
    std::string formatted_item = "[" + current_busid + "] " + current_desc;
    gui.addListItem(formatted_item);
    SPDLOG_INFO("[Parsed Device] {}", formatted_item);
  }

  infile.close();
  //std::remove(temp_file.c_str());
}

// --- ATTACH BUTTON ACTION HOOK (USB/IP Client Mode) ---
void OnAttachButtonClicked(GUI &gui) {
  std::cout << "\n[GUI Event] Attach button clicked. Initializing tunnel...\n"
            << std::endl;
  attach_triggered = true;
  gui.addListItem("Initializing WebRTC tunnel...");

  int ret = InitializeUsbIpClientTunnelSession();
  if (ret < 0)
  {
    if (ret == -1)
    gui.setLabelText("Room is fulled");

    if (ret == -2)
      gui.setLabelText("Enter your email id");

    return;
  }

    std::thread([](GUI& gui) {
    // Wait briefly for the TCP tunnel bridge to bind and connect
    while (!is_datachannel_connected)
    {
      gui.setLabelText("Waiting for WebRTC connection to establish...");
      std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
    
    
    if (is_datachannel_connected)
    {
      gui.setLabelText("Data channel connected");
      SPDLOG_INFO("Querying remote USB/IP server device list...");
      FetchAndDisplayRemoteDevices(gui);
    }
  
 

   // SPDLOG_INFO("Running automated 'detach -a'...");
  //  execute_usbip_command({"usbip", "detach", "-a"});
    }, std::ref(gui)).detach();
}

int main(int argc, char* argv[]) {

    rtc::LogMessage::LogToDebug(rtc::LoggingSeverity::LS_ERROR);
   // SetupLogging("info");
    SetupRotatingLogging("info");

    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    global_client_id = "cpp_peer_" + std::to_string(std::rand() % 90000 + 1000);

    if (!InitializeFirebaseFramework()) {
        std::cerr << "Firebase setup crash exception encountered." << std::endl;
        return -1;
    }

    LoadConfigFromDisk(active_room_id, global_client_id);

    // Initialize SDL GUI
    GUI gui;
    if (!gui.init()) {
        ShutdownFirebaseFramework(nullptr);
        return -1;
    }

    // Assign global GUI pointer so log sink can update labelText live
    g_active_gui = &gui;

    // Attach custom spdlog sink
    auto gui_sink = std::make_shared<GuiLogSink<std::mutex>>();
    spdlog::default_logger()->sinks().push_back(gui_sink);

    SPDLOG_INFO("Successfully authenticated with Cloud Firestore. Client ID: [{}]", global_client_id);

    // Bind ShareUSB Button Event Handler
    gui.setShareUSBHandler([](GUI& target_gui) {
        OnShareUSBButtonClicked(target_gui);
    });

    // Bind Attach Button Event Handler
    gui.setAttachHandler([](GUI& target_gui) {
        OnAttachButtonClicked(target_gui);
    });

    // Bind Log Button Event Handler to load history snapshots into list view
    gui.setLogHandler([](GUI& target_gui) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        target_gui.clearListItems();
        target_gui.addListItem("--- Recent Log Snapshot ---");
        for (const auto& log : g_log_buffer) {
            target_gui.addListItem(log);
        }
    });

    // Background thread for handling heartbeat polling/signaling loops while GUI runs smoothly
    std::thread background_worker([]() {
        auto last_heartbeat_time = std::chrono::steady_clock::now();
        while (keep_running) {
            int sleep_ms = is_datachannel_connected ? 500 : 30;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));

            if (!active_room_id.empty()) {
                if (is_datachannel_connected) {
                    last_heartbeat_time = std::chrono::steady_clock::now();
                } else {
                    auto now = std::chrono::steady_clock::now();
                    if (sleep_ms == 30 && std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat_time).count() >= 5) {
                        SetUserPresence(active_room_id, global_client_id, true);
                        last_heartbeat_time = now;
                    }
                }
            }
        }
    });

    // Run the main SDL event and render loop on the main thread
    gui.run();

    // Clean exit teardown
    keep_running = false;
    if (!active_room_id.empty()) {
        HandleGracefulExit(active_room_id, global_client_id);
    }

    // Clean up usbip client/tunnel code if Attach button was clicked
    if (attach_triggered) {
        ShutdownUsbIpClientTunnel();
        //execute_usbip_command({"usbip", "detach", "-a"});
    }

    // Clean up usbipd host/server code if ShareUSB button was clicked
    if (share_usb_triggered) {
        if (core_server_connection) {
            core_server_connection->ShutdownConnection();
            core_server_connection.reset();
        }
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            connected_clients.clear();
        }

        if (g_server) {
            usbip_server_destroy(g_server);
            g_server = nullptr;
        }
    }

    if (background_worker.joinable()) {
        background_worker.join();
    }

    ShutdownFirebaseFramework(nullptr);
    return 0;
}
