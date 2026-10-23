#include <SDL.h>
#include <SDL_ttf.h>
#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <csignal>
#include <memory>
#include <map>

#include "signaling.h"
#include "webrtc.h"
#include "firebase/app.h"
#include "firebase/firestore.h"

const int WINDOW_WIDTH = 640;
const int WINDOW_HEIGHT = 480;

struct UIState {
    std::string inputText = "";
    bool isInputActive = false;
    std::vector<std::string> listItems = {"ALL: Hello Room", "peer_id: Test Private Message"};
    int selectedIndex = -1;
    std::string labelText = "System Status: Initializing...";
};

// Global application synchronization state flags
std::atomic<bool> keep_running{true};
std::atomic<bool> is_datachannel_connected{false};
std::mutex clients_mutex;

std::string active_room_id = ""; 
std::string global_client_id = "";

std::map<std::string, std::unique_ptr<WebRtcManager>> connected_clients;
std::unique_ptr<WebRtcManager> core_server_connection = nullptr;
bool host_server = false;

void SignalExitHandler(int signum) {
    SPDLOG_WARN("\nProcess termination signal [{}] intercepted!", signum);
    keep_running = false;
}

void AttachDataChannelCallback(WebRtcManager* manager) {
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

void renderText(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, int x, int y, SDL_Color color) {
    if (text.empty()) return;
    SDL_Surface* surface = TTF_RenderText_Blended(font, text.c_str(), color);
    if (!surface) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_Rect destRect = { x, y, surface->w, surface->h };
    SDL_RenderCopy(renderer, texture, NULL, &destRect);
    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

int main(int argc, char* argv[]) {
    SetupLogging("info");
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

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

    // Auto-fallback or generate room ID for GUI environment
    active_room_id = saved_room_id.empty() ? GenerateRandomRoomId(20) : saved_room_id;
    host_server = IsRoomEmpty(active_room_id); 
    SPDLOG_INFO("[Room Join] Target room ID: {}. Assigned Role -> {}", active_room_id, host_server ? "Host Server" : "Client");

    SaveConfigToDisk(active_room_id, "info");

    SPDLOG_INFO("Evaluating capacity constraints for room matrix [{}]...", active_room_id);
    if (!VerifyAndReserveRoomCapacity(active_room_id)) {
        SPDLOG_ERROR("Access Denied: Room capacity limit reached.");
        ShutdownFirebaseFramework(nullptr);
        return -1;
    }

    if (!host_server) {
        SPDLOG_INFO("[Client Init] Instantiating WebRtcManager caller node to generate connection offer...");
        core_server_connection = std::make_unique<WebRtcManager>(active_room_id, global_client_id, true);
        AttachDataChannelCallback(core_server_connection.get());
        core_server_connection->SetRemotePeerId("ALL");
        if (core_server_connection->Initialize()) {
            core_server_connection->StartConnectionSequence();
        }
    }

    RegisterOnRoomReadyEvent([&](const std::string& ready_room_id) {
        SPDLOG_WARN("[EVENT TRIGGERED] Room [{}] is full and ready for meeting!", ready_room_id);
    });

    RegisterOnPeerLeftEvent([&](const std::string& peer_client_id) {
        SPDLOG_ERROR("[EVENT NOTICE] Peer [{}] has disconnected or left the room.", peer_client_id);
        if (host_server) {
            std::lock_guard<std::mutex> lock(clients_mutex);
            auto it = connected_clients.find(peer_client_id);
            if (it != connected_clients.end()) {
                it->second->ShutdownConnection();
                connected_clients.erase(it);
            }
            is_datachannel_connected = false;
        } else {
            if (core_server_connection) {
                core_server_connection->ShutdownConnection();
                core_server_connection = nullptr;
            }
            keep_running = false;
        }
    });

    RegisterOnSdpReceivedEvent([&](const std::string& sender_id, const std::string& type, const std::string& sdp) {
        if (sender_id == global_client_id) return;
        if (host_server) {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (connected_clients.find(sender_id) == connected_clients.end()) {
                auto client_rtc = std::make_unique<WebRtcManager>(active_room_id, global_client_id, false);
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

    // Initialize SDL & TTF GUI Subsystems
    if (SDL_Init(SDL_INIT_VIDEO) < 0 || TTF_Init() < 0) {
        std::cerr << "SDL/TTF Init Failed: " << SDL_GetError() << std::endl;
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow("Cross-Platform WebRTC Signaling GUI", 
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    TTF_Font* font = TTF_OpenFont("assets/arial.ttf", 20);
    if (!font) font = TTF_OpenFont("arial.ttf", 20);
    if (!font) font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 20);

    UIState state;
    state.labelText = "Connected Room: " + active_room_id;
    SDL_Event e;

    SDL_Rect textBoxRect = { 50, 50, 300, 40 };
    SDL_Rect buttonRect  = { 370, 50, 100, 40 };
    SDL_Rect bindBtnRect = { 480, 50, 100, 40 };  
    SDL_Rect listViewRect= { 50, 120, 530, 220 }; 
    SDL_Rect labelPos    = { 50, 360, 0, 0 };

    auto last_heartbeat_time = std::chrono::steady_clock::now();

    while (keep_running ) {
        // Non-blocking poll events for SDL GUI responsiveness
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                keep_running = false;
            } else if (e.type == SDL_MOUSEBUTTONDOWN) {
                int mx = e.button.x;
                int my = e.button.y;

                if (mx >= textBoxRect.x && mx <= textBoxRect.x + textBoxRect.w &&
                    my >= textBoxRect.y && my <= textBoxRect.y + textBoxRect.h) {
                    state.isInputActive = true;
                    SDL_StartTextInput();
                } else {
                    state.isInputActive = false;
                    SDL_StopTextInput();
                }

                // Send Message Action [Add Button]
                if (mx >= buttonRect.x && mx <= buttonRect.x + buttonRect.w &&
                    my >= buttonRect.y && my <= buttonRect.y + buttonRect.h) {
                    if (!state.inputText.empty()) {
                        state.listItems.push_back(state.inputText);
                        
                        // Parse target format "target_id:message" and dispatch via Firebase Signaling
                        size_t delimiter_pos = state.inputText.find(':');
                        if (delimiter_pos != std::string::npos) {
                            std::string target_peer = state.inputText.substr(0, delimiter_pos);
                            std::string text_payload = state.inputText.substr(delimiter_pos + 1);
                            SendTargetedMessage(active_room_id, global_client_id, target_peer, text_payload);
                            state.labelText = "Sent to " + target_peer;
                        } else {
                            state.labelText = "Error: Use format target_id:message";
                        }
                        state.inputText.clear();
                    }
                }

                // Info Button Action [Bind Button]
                if (mx >= bindBtnRect.x && mx <= bindBtnRect.x + bindBtnRect.w &&
                    my >= bindBtnRect.y && my <= bindBtnRect.y + bindBtnRect.h) {
                    if (state.selectedIndex >= 0 && state.selectedIndex < (int)state.listItems.size()) {
                        std::string message = "Selected Log/Command: " + state.listItems[state.selectedIndex];
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "WebRTC Status", message.c_str(), window);
                    } else {
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Warning", "Please select a log item first!", window);
                    }
                }

                if (mx >= listViewRect.x && mx <= listViewRect.x + listViewRect.w &&
                    my >= listViewRect.y && my <= listViewRect.y + listViewRect.h) {
                    int relativeY = my - listViewRect.y;
                    int clickedIndex = relativeY / 30;
                    if (clickedIndex >= 0 && clickedIndex < (int)state.listItems.size()) {
                        state.selectedIndex = clickedIndex;
                        state.inputText = state.listItems[clickedIndex];
                    }
                }
            } else if (e.type == SDL_TEXTINPUT && state.isInputActive) {
                state.inputText += e.text.text;
            } else if (e.type == SDL_KEYDOWN && state.isInputActive) {
                if (e.key.keysym.sym == SDLK_BACKSPACE && !state.inputText.empty()) {
                    state.inputText.pop_back();
                }
            }
        }

        // Heartbeat logic
        int sleep_ms = is_datachannel_connected ? 500 : 30;
        SDL_Delay(sleep_ms);

        if (is_datachannel_connected) {
            last_heartbeat_time = std::chrono::steady_clock::now();
        } else {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat_time).count() >= 5) {
                SetUserPresence(active_room_id, global_client_id, true);
                last_heartbeat_time = now;
            }
        }

        // Render UI Pipeline
        SDL_SetRenderDrawColor(renderer, 40, 44, 52, 255);
        SDL_RenderClear(renderer);

        SDL_Color white     = { 255, 255, 255, 255 };
        SDL_Color black     = { 0, 0, 0, 255 };
        SDL_Color blue      = { 70, 130, 180, 255 };
        SDL_Color purple    = { 138, 43, 226, 255 }; 
        SDL_Color gray      = { 60, 64, 72, 255 };
        SDL_Color highlight = { 100, 149, 237, 255 };
        SDL_Color green     = { 152, 195, 121, 255 };

        // TextBox
        SDL_SetRenderDrawColor(renderer, state.isInputActive ? 255 : 240, 240, 240, 255);
        SDL_RenderFillRect(renderer, &textBoxRect);
        if (font) renderText(renderer, font, state.inputText, textBoxRect.x + 8, textBoxRect.y + 8, black);

        // Send Button ("Add")
        SDL_SetRenderDrawColor(renderer, blue.r, blue.g, blue.b, 255);
        SDL_RenderFillRect(renderer, &buttonRect);
        if (font) renderText(renderer, font, "Send", buttonRect.x + 28, buttonRect.y + 8, white);

        // Info Button ("Bind")
        SDL_SetRenderDrawColor(renderer, purple.r, purple.g, purple.b, 255);
        SDL_RenderFillRect(renderer, &bindBtnRect);
        if (font) renderText(renderer, font, "Info", bindBtnRect.x + 32, bindBtnRect.y + 8, white);

        // ListView Box
        SDL_SetRenderDrawColor(renderer, gray.r, gray.g, gray.b, 255);
        SDL_RenderFillRect(renderer, &listViewRect);

        if (font) {
            for (size_t i = 0; i < state.listItems.size(); ++i) {
                SDL_Rect itemRect = { listViewRect.x, listViewRect.y + (int)i * 30, listViewRect.w, 28 };
                if ((int)i == state.selectedIndex) {
                    SDL_SetRenderDrawColor(renderer, highlight.r, highlight.g, highlight.b, 255);
                    SDL_RenderFillRect(renderer, &itemRect);
                }
                renderText(renderer, font, state.listItems[i], itemRect.x + 10, itemRect.y + 4, white);
            }
        }

        if (font) renderText(renderer, font, state.labelText, labelPos.x, labelPos.y, green);

        SDL_RenderPresent(renderer);
    }

    // Graceful Shutdown Sequence
    HandleGracefulExit(active_room_id, global_client_id);

    if (core_server_connection) {
        core_server_connection->ShutdownConnection();
        core_server_connection.reset();
    }
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        connected_clients.clear();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (font) TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    ShutdownFirebaseFramework(nullptr);
    return 0;
}
