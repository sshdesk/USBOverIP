#include "gui.h"
#include "signaling.h"
#include <iostream>
#include <algorithm>

const int WINDOW_WIDTH = 900;
const int WINDOW_HEIGHT = 650;
const int ITEM_HEIGHT = 22;

static void renderText(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, int x, int y, SDL_Color color) {
    if (text.empty()) return;
    SDL_Surface* surface = TTF_RenderText_Blended(font, text.c_str(), color);
    if (!surface) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_Rect destRect = { x, y, surface->w, surface->h };
    SDL_RenderCopy(renderer, texture, NULL, &destRect);
    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

GUI::GUI() 
    : window(nullptr), renderer(nullptr), font(nullptr), isDraggingScrollbar(false) {
    checkboxRect         = { 40, 40, 20, 20 };
    textBoxRect          = { 40, 75, 180, 36 };
    localUsbBtnRect      = { 230, 75, 110, 36 };
    remoteUsbBtnRect     = { 230, 75, 120, 36 };
    remoteAttachBtnRect  = { 360, 75, 130, 36 };
    remoteDetachBtnRect  = { 500, 75, 130, 36 };
    logBtnRect           = { 760, 75, 100, 36 };
    listViewRect         = { 40, 130, 820, 445 };
    labelPos             = { 40, 585, 0, 0 };

    // Load configuration from disk using SignalingClient
    std::string loaded_token_id;
    bool loaded_remote_usb = false;
    std::string loaded_log_level;
    SignClient_LoadConfigHelper_placeholder: // or use SignalingClient directly
    {
        SignalingClient client;
        client.LoadConfigFromDisk(loaded_token_id, loaded_remote_usb, loaded_log_level);
    }
    state.inputText = loaded_token_id;
    state.forRemoteUsb = loaded_remote_usb;
}

GUI::~GUI() {
    // Save configuration to disk using SignalingClient upon exit
    {
        SignalingClient client;
        client.SaveConfigToDisk(state.inputText, state.forRemoteUsb);
    }

    if (font) TTF_CloseFont(font);
    TTF_Quit();
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
}

bool GUI::init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0 || TTF_Init() < 0) {
        std::cerr << "SDL/TTF Init Failed: " << SDL_GetError() << std::endl;
        return false;
    }

    window = SDL_CreateWindow("Stim USB Share", 
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    if (!window) return false;

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) return false;

    font = TTF_OpenFont("assets/arial.ttf", 14);
    if (!font) font = TTF_OpenFont("arial.ttf", 14);
    if (!font) font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14);

    return true;
}

void GUI::addListItem(const std::string& item) {
    if (!item.empty()) {
        state.listItems.push_back(item);
    }
}

void GUI::handleEvents(bool& running) {
    int totalContentHeight = (int)state.listItems.size() * ITEM_HEIGHT;
    int maxScrollY = std::max(0, totalContentHeight - listViewRect.h);

    int scrollTrackW = 12;
    SDL_Rect scrollTrackRect = { listViewRect.x + listViewRect.w - scrollTrackW, listViewRect.y, scrollTrackW, listViewRect.h };
    
    SDL_Rect scrollThumbRect = scrollTrackRect;
    if (totalContentHeight > listViewRect.h) {
        scrollThumbRect.h = (int)(((float)listViewRect.h / totalContentHeight) * listViewRect.h);
        if (scrollThumbRect.h < 20) scrollThumbRect.h = 20;
        float scrollRatio = (float)state.scrollY / maxScrollY;
        scrollThumbRect.y = listViewRect.y + (int)(scrollRatio * (listViewRect.h - scrollThumbRect.h));
    } else {
        scrollThumbRect.h = listViewRect.h;
    }

    SDL_Event e;
    while (SDL_PollEvent(&e) != 0) {
        if (e.type == SDL_QUIT) {
            running = false;
        } 
        else if (e.type == SDL_MOUSEWHEEL) {
            int mx, my;
            SDL_GetMouseState(&mx, &my);

            if (mx >= listViewRect.x && mx <= listViewRect.x + listViewRect.w &&
                my >= listViewRect.y && my <= listViewRect.y + listViewRect.h) {
                
                int scrollSpeed = 20;
                state.scrollY -= e.wheel.y * scrollSpeed;
                state.scrollY = std::clamp(state.scrollY, 0, maxScrollY);
            }
        }
        else if (e.type == SDL_MOUSEBUTTONDOWN) {
            int mx = e.button.x;
            int my = e.button.y;

            if (totalContentHeight > listViewRect.h &&
                mx >= scrollTrackRect.x && mx <= scrollTrackRect.x + scrollTrackRect.w &&
                my >= scrollTrackRect.y && my <= scrollTrackRect.y + scrollTrackRect.h) {
                
                isDraggingScrollbar = true;
                float clickRatio = (float)(my - listViewRect.y - scrollThumbRect.h / 2) / (listViewRect.h - scrollThumbRect.h);
                state.scrollY = (int)(clickRatio * maxScrollY);
                state.scrollY = std::clamp(state.scrollY, 0, maxScrollY);
            }
            else {
                // Checkbox click detection (including label area for convenience)
                SDL_Rect fullCheckboxArea = { checkboxRect.x, checkboxRect.y, 115, checkboxRect.h };
                if (mx >= fullCheckboxArea.x && mx <= fullCheckboxArea.x + fullCheckboxArea.w &&
                    my >= fullCheckboxArea.y && my <= fullCheckboxArea.y + fullCheckboxArea.h) {
                    state.forRemoteUsb = !state.forRemoteUsb;
                }

                if (mx >= textBoxRect.x && mx <= textBoxRect.x + textBoxRect.w &&
                    my >= textBoxRect.y && my <= textBoxRect.y + textBoxRect.h) {
                    state.isInputActive = true;
                    SDL_StartTextInput();
                } else {
                    state.isInputActive = false;
                    SDL_StopTextInput();
                }

                if (!state.forRemoteUsb) {
                    // LocalUSB branch
                    if (state.localUsbEnabled &&
                        mx >= localUsbBtnRect.x && mx <= localUsbBtnRect.x + localUsbBtnRect.w &&
                        my >= localUsbBtnRect.y && my <= localUsbBtnRect.y + localUsbBtnRect.h) {
                        state.pressedButton = 1;
                        state.listItems.clear();
                        state.selectedIndex = -1;
                        if (local_usb_handler_) {
                            local_usb_handler_(*this);
                        }    
                    }
                } else {
                    // RemoteUSB branch
                    if (state.remoteUsbEnabled &&
                        mx >= remoteUsbBtnRect.x && mx <= remoteUsbBtnRect.x + remoteUsbBtnRect.w &&
                        my >= remoteUsbBtnRect.y && my <= remoteUsbBtnRect.y + remoteUsbBtnRect.h) {
                        state.pressedButton = 2;
                        if (remote_usb_handler_) {
                            remote_usb_handler_(*this);
                        }
                    }
                    else if (state.remoteAttachEnabled &&
                             mx >= remoteAttachBtnRect.x && mx <= remoteAttachBtnRect.x + remoteAttachBtnRect.w &&
                             my >= remoteAttachBtnRect.y && my <= remoteAttachBtnRect.y + remoteAttachBtnRect.h) {
                        state.pressedButton = 3;
                        if (remote_attach_handler_) {
                            remote_attach_handler_(*this);
                        }
                    }
                    else if (state.remoteDetachEnabled &&
                             mx >= remoteDetachBtnRect.x && mx <= remoteDetachBtnRect.x + remoteDetachBtnRect.w &&
                             my >= remoteDetachBtnRect.y && my <= remoteDetachBtnRect.y + remoteDetachBtnRect.h) {
                        state.pressedButton = 4;
                        if (remote_detach_handler_) {
                            remote_detach_handler_(*this);
                        }
                    }
                }

                // Log button is always active
                if (mx >= logBtnRect.x && mx <= logBtnRect.x + logBtnRect.w &&
                    my >= logBtnRect.y && my <= logBtnRect.y + logBtnRect.h) {
                    state.pressedButton = 5;
                    if (log_handler_) {
                        log_handler_(*this);
                    }
                }

                if (mx >= listViewRect.x && mx <= listViewRect.x + listViewRect.w - scrollTrackW &&
                    my >= listViewRect.y && my <= listViewRect.y + listViewRect.h) {
                    
                    int relativeY = (my - listViewRect.y) + state.scrollY;
                    int clickedIndex = relativeY / ITEM_HEIGHT;
                    
                    if (clickedIndex >= 0 && clickedIndex < (int)state.listItems.size()) {
                        state.selectedIndex = clickedIndex;
                        state.labelText = "Selected: " + state.listItems[clickedIndex];
                    }
                }
            }
        } 
        else if (e.type == SDL_MOUSEBUTTONUP) {
            if (e.button.button == SDL_BUTTON_LEFT) {
                isDraggingScrollbar = false;
                state.pressedButton = 0;
            }
        }
        else if (e.type == SDL_MOUSEMOTION && isDraggingScrollbar) {
            int my = e.motion.y;
            float clickRatio = (float)(my - listViewRect.y - scrollThumbRect.h / 2) / (listViewRect.h - scrollThumbRect.h);
            state.scrollY = (int)(clickRatio * maxScrollY);
            state.scrollY = std::clamp(state.scrollY, 0, maxScrollY);
        }
        else if (e.type == SDL_TEXTINPUT && state.isInputActive) {
            #if defined(__APPLE__)
            bool ctrlPressed = (SDL_GetModState() & KMOD_GUI);
            #else
            bool ctrlPressed = (SDL_GetModState() & KMOD_CTRL);
            #endif
            if (!ctrlPressed) {
                state.inputText += e.text.text;
            }
        } 
        else if (e.type == SDL_KEYDOWN && state.isInputActive) {
            #if defined(__APPLE__)
            bool ctrlPressed = (SDL_GetModState() & KMOD_GUI);
            #else
            bool ctrlPressed = (SDL_GetModState() & KMOD_CTRL);
            #endif

            if (e.key.keysym.sym == SDLK_BACKSPACE && !state.inputText.empty()) {
                state.inputText.pop_back();
            }
            else if (ctrlPressed && e.key.keysym.sym == SDLK_v) {
                if (SDL_HasClipboardText()) {
                    char* clipboardText = SDL_GetClipboardText();
                    if (clipboardText) {
                        state.inputText += clipboardText;
                        SDL_free(clipboardText);
                    }
                }
            }
            else if (ctrlPressed && e.key.keysym.sym == SDLK_c) {
                if (!state.inputText.empty()) {
                    SDL_SetClipboardText(state.inputText.c_str());
                }
            }
            else if (ctrlPressed && e.key.keysym.sym == SDLK_x) {
                if (!state.inputText.empty()) {
                    SDL_SetClipboardText(state.inputText.c_str());
                    state.inputText.clear();
                }
            }
        }
    }
}

void GUI::render() {
    SDL_SetRenderDrawColor(renderer, 40, 44, 52, 255);
    SDL_RenderClear(renderer);

    SDL_Color white       = { 255, 255, 255, 255 };
    SDL_Color black       = { 0, 0, 0, 255 };
    SDL_Color localColor  = { 70, 130, 180, 255 };   // Steel Blue
    SDL_Color remoteColor = { 138, 43, 226, 255 };  // Purple
    SDL_Color logColor    = { 209, 154, 102, 255 };  // Orange
    SDL_Color gray        = { 60, 64, 72, 255 };
    SDL_Color highlight   = { 100, 149, 237, 255 };
    SDL_Color green       = { 152, 195, 121, 255 };

    // Checkbox FOR_REMOTE_USB (Top Row)
    SDL_SetRenderDrawColor(renderer, 240, 240, 240, 255);
    SDL_RenderFillRect(renderer, &checkboxRect);
    if (state.forRemoteUsb) {
        SDL_SetRenderDrawColor(renderer, 70, 130, 180, 255);
        SDL_Rect checkMark = { checkboxRect.x + 4, checkboxRect.y + 4, checkboxRect.w - 8, checkboxRect.h - 8 };
        SDL_RenderFillRect(renderer, &checkMark);
    }
    if (font) renderText(renderer, font, "For remote USB", checkboxRect.x + 28, checkboxRect.y + 1, white);

    // TextBox (Next Row)
    SDL_SetRenderDrawColor(renderer, state.isInputActive ? 255 : 240, 240, 240, 255);
    SDL_RenderFillRect(renderer, &textBoxRect);
    if (font) renderText(renderer, font, state.inputText, textBoxRect.x + 8, textBoxRect.y + 8, black);

    // Helper lambda for button coloring depending on individual enabled states
    auto getButtonColor = [&](SDL_Color baseColor, int btnID, bool isEnabled) {
        if (!isEnabled) {
            return SDL_Color{ 90, 95, 105, 255 }; // Disabled Gray
        }
        bool isPressed = (state.pressedButton == btnID);
        return SDL_Color{ 
            (Uint8)(baseColor.r - (isPressed ? 30 : 0)), 
            (Uint8)(baseColor.g - (isPressed ? 30 : 0)), 
            (Uint8)(baseColor.b - (isPressed ? 30 : 0)), 
            255 
        };
    };

    if (!state.forRemoteUsb) {
        // Show LocalUSB
        bool isPressed = (state.pressedButton == 1);
        SDL_Rect rect = localUsbBtnRect;
        if (isPressed && state.localUsbEnabled) { rect.x += 2; rect.y += 2; }
        SDL_Color col = getButtonColor(localColor, 1, state.localUsbEnabled);
        SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
        SDL_RenderFillRect(renderer, &rect);
        if (font) renderText(renderer, font, "Connect", rect.x + 20, rect.y + 8, state.localUsbEnabled ? white : SDL_Color{160, 160, 160, 255});
    } else {
        // Show RemoteUSB, RemoteAttach, RemoteDetach
        // 2. RemoteUSB
        {
            bool isPressed = (state.pressedButton == 2);
            SDL_Rect rect = remoteUsbBtnRect;
            if (isPressed && state.remoteUsbEnabled) { rect.x += 2; rect.y += 2; }
            SDL_Color col = getButtonColor(remoteColor, 2, state.remoteUsbEnabled);
            SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
            SDL_RenderFillRect(renderer, &rect);
            if (font) renderText(renderer, font, "Connect", rect.x + 18, rect.y + 8, state.remoteUsbEnabled ? white : SDL_Color{160, 160, 160, 255});
        }

        // 3. RemoteAttach
        {
            bool isPressed = (state.pressedButton == 3);
            SDL_Rect rect = remoteAttachBtnRect;
            if (isPressed && state.remoteAttachEnabled) { rect.x += 2; rect.y += 2; }
            SDL_Color col = getButtonColor(remoteColor, 3, state.remoteAttachEnabled);
            SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
            SDL_RenderFillRect(renderer, &rect);
            if (font) renderText(renderer, font, "RemoteAttach", rect.x + 14, rect.y + 8, state.remoteAttachEnabled ? white : SDL_Color{160, 160, 160, 255});
        }

        // 4. RemoteDetach
        {
            bool isPressed = (state.pressedButton == 4);
            SDL_Rect rect = remoteDetachBtnRect;
            if (isPressed && state.remoteDetachEnabled) { rect.x += 2; rect.y += 2; }
            SDL_Color col = getButtonColor(remoteColor, 4, state.remoteDetachEnabled);
            SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
            SDL_RenderFillRect(renderer, &rect);
            if (font) renderText(renderer, font, "RemoteDetach", rect.x + 14, rect.y + 8, state.remoteDetachEnabled ? white : SDL_Color{160, 160, 160, 255});
        }
    }

    // 5. Log (Always enabled)
    {
        bool isPressed = (state.pressedButton == 5);
        SDL_Rect rect = logBtnRect;
        if (isPressed) { rect.x += 2; rect.y += 2; }
        SDL_Color col = getButtonColor(logColor, 5, true);
        SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
        SDL_RenderFillRect(renderer, &rect);
        if (font) renderText(renderer, font, "Log", rect.x + 36, rect.y + 8, white);
    }

    // ListView Panel
    SDL_SetRenderDrawColor(renderer, gray.r, gray.g, gray.b, 255);
    SDL_RenderFillRect(renderer, &listViewRect);

    int totalContentHeight = (int)state.listItems.size() * ITEM_HEIGHT;
    int maxScrollY = std::max(0, totalContentHeight - listViewRect.h);
    int scrollTrackW = 12;
    SDL_Rect scrollTrackRect = { listViewRect.x + listViewRect.w - scrollTrackW, listViewRect.y, scrollTrackW, listViewRect.h };
    
    SDL_Rect scrollThumbRect = scrollTrackRect;
    if (totalContentHeight > listViewRect.h) {
        scrollThumbRect.h = (int)(((float)listViewRect.h / totalContentHeight) * listViewRect.h);
        if (scrollThumbRect.h < 20) scrollThumbRect.h = 20;
        float scrollRatio = (float)state.scrollY / maxScrollY;
        scrollThumbRect.y = listViewRect.y + (int)(scrollRatio * (listViewRect.h - scrollThumbRect.h));
    } else {
        scrollThumbRect.h = listViewRect.h;
    }

    if (font) {
        SDL_Rect oldClip;
        SDL_bool wasClipped = SDL_RenderIsClipEnabled(renderer);
        if (wasClipped) {
            SDL_RenderGetClipRect(renderer, &oldClip);
        }

        SDL_RenderSetClipRect(renderer, &listViewRect);

        int contentWidth = listViewRect.w - scrollTrackW;

        for (size_t i = 0; i < state.listItems.size(); ++i) {
            int itemTopY = listViewRect.y + ((int)i * ITEM_HEIGHT) - state.scrollY;
            SDL_Rect itemRect = { listViewRect.x, itemTopY, contentWidth, ITEM_HEIGHT - 2 };

            if (itemRect.y + itemRect.h >= listViewRect.y && itemRect.y <= listViewRect.y + listViewRect.h) {
                if ((int)i == state.selectedIndex) {
                    SDL_SetRenderDrawColor(renderer, highlight.r, highlight.g, highlight.b, 255);
                    SDL_RenderFillRect(renderer, &itemRect);
                }
                renderText(renderer, font, state.listItems[i], itemRect.x + 10, itemRect.y + 3, white);
            }
        }

        SDL_RenderSetClipRect(renderer, wasClipped ? &oldClip : nullptr);
    }

    if (totalContentHeight > listViewRect.h) {
        SDL_SetRenderDrawColor(renderer, 45, 48, 54, 255);
        SDL_RenderFillRect(renderer, &scrollTrackRect);

        SDL_SetRenderDrawColor(renderer, 90, 95, 105, 255);
        SDL_RenderFillRect(renderer, &scrollThumbRect);
    }

    if (font) renderText(renderer, font, state.labelText, labelPos.x, labelPos.y, green);

    SDL_RenderPresent(renderer);
}

void GUI::run() {
    bool running = true;
    while (running) {
        handleEvents(running);
        render();
    }
}