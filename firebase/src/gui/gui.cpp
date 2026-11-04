#include "gui.h"
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
    textBoxRect          = { 40, 50, 180, 36 };
    localUsbBtnRect      = { 230, 50, 110, 36 };
    remoteUsbBtnRect     = { 350, 50, 120, 36 };
    remoteAttachBtnRect  = { 480, 50, 130, 36 };
    remoteDetachBtnRect  = { 620, 50, 130, 36 };
    logBtnRect           = { 760, 50, 100, 36 };
    listViewRect         = { 40, 105, 820, 470 };
    labelPos             = { 40, 585, 0, 0 };
}

GUI::~GUI() {
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
                if (mx >= textBoxRect.x && mx <= textBoxRect.x + textBoxRect.w &&
                    my >= textBoxRect.y && my <= textBoxRect.y + textBoxRect.h) {
                    state.isInputActive = true;
                    SDL_StartTextInput();
                } else {
                    state.isInputActive = false;
                    SDL_StopTextInput();
                }

                // Only allow button clicks if buttons are currently enabled
                if (state.buttonsEnabled) {
                    if (mx >= localUsbBtnRect.x && mx <= localUsbBtnRect.x + localUsbBtnRect.w &&
                        my >= localUsbBtnRect.y && my <= localUsbBtnRect.y + localUsbBtnRect.h) {
                        state.pressedButton = 1;
                        state.listItems.clear();
                        state.selectedIndex = -1;
                        if (local_usb_handler_) {
                            local_usb_handler_(*this);
                        }    
                    }
                    else if (mx >= remoteUsbBtnRect.x && mx <= remoteUsbBtnRect.x + remoteUsbBtnRect.w &&
                             my >= remoteUsbBtnRect.y && my <= remoteUsbBtnRect.y + remoteUsbBtnRect.h) {
                        state.pressedButton = 2;
                        if (remote_usb_handler_) {
                            remote_usb_handler_(*this);
                        }
                    }
                    else if (mx >= remoteAttachBtnRect.x && mx <= remoteAttachBtnRect.x + remoteAttachBtnRect.w &&
                             my >= remoteAttachBtnRect.y && my <= remoteAttachBtnRect.y + remoteAttachBtnRect.h) {
                        state.pressedButton = 3;
                        if (remote_attach_handler_) {
                            remote_attach_handler_(*this);
                        }
                    }
                    else if (mx >= remoteDetachBtnRect.x && mx <= remoteDetachBtnRect.x + remoteDetachBtnRect.w &&
                             my >= remoteDetachBtnRect.y && my <= remoteDetachBtnRect.y + remoteDetachBtnRect.h) {
                        state.pressedButton = 4;
                        if (remote_detach_handler_) {
                            remote_detach_handler_(*this);
                        }
                    }
                    else if (mx >= logBtnRect.x && mx <= logBtnRect.x + logBtnRect.w &&
                             my >= logBtnRect.y && my <= logBtnRect.y + logBtnRect.h) {
                        state.pressedButton = 5;
                        if (log_handler_) {
                            log_handler_(*this);
                        }
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

    // TextBox
    SDL_SetRenderDrawColor(renderer, state.isInputActive ? 255 : 240, 240, 240, 255);
    SDL_RenderFillRect(renderer, &textBoxRect);
    if (font) renderText(renderer, font, state.inputText, textBoxRect.x + 8, textBoxRect.y + 8, black);

    // Helper lambda for button coloring depending on enabled state
    auto getButtonColor = [&](SDL_Color baseColor, int btnID) {
        if (!state.buttonsEnabled) {
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

    // 1. LocalUSB
    {
        bool isPressed = (state.pressedButton == 1);
        SDL_Rect rect = localUsbBtnRect;
        if (isPressed && state.buttonsEnabled) { rect.x += 2; rect.y += 2; }
        SDL_Color col = getButtonColor(localColor, 1);
        SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
        SDL_RenderFillRect(renderer, &rect);
        if (font) renderText(renderer, font, "LocalUSB", rect.x + 20, rect.y + 8, state.buttonsEnabled ? white : SDL_Color{160, 160, 160, 255});
    }

    // 2. RemoteUSB
    {
        bool isPressed = (state.pressedButton == 2);
        SDL_Rect rect = remoteUsbBtnRect;
        if (isPressed && state.buttonsEnabled) { rect.x += 2; rect.y += 2; }
        SDL_Color col = getButtonColor(remoteColor, 2);
        SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
        SDL_RenderFillRect(renderer, &rect);
        if (font) renderText(renderer, font, "RemoteUSB", rect.x + 18, rect.y + 8, state.buttonsEnabled ? white : SDL_Color{160, 160, 160, 255});
    }

    // 3. RemoteAttach
    {
        bool isPressed = (state.pressedButton == 3);
        SDL_Rect rect = remoteAttachBtnRect;
        if (isPressed && state.buttonsEnabled) { rect.x += 2; rect.y += 2; }
        SDL_Color col = getButtonColor(remoteColor, 3);
        SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
        SDL_RenderFillRect(renderer, &rect);
        if (font) renderText(renderer, font, "RemoteAttach", rect.x + 14, rect.y + 8, state.buttonsEnabled ? white : SDL_Color{160, 160, 160, 255});
    }

    // 4. RemoteDetach
    {
        bool isPressed = (state.pressedButton == 4);
        SDL_Rect rect = remoteDetachBtnRect;
        if (isPressed && state.buttonsEnabled) { rect.x += 2; rect.y += 2; }
        SDL_Color col = getButtonColor(remoteColor, 4);
        SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
        SDL_RenderFillRect(renderer, &rect);
        if (font) renderText(renderer, font, "RemoteDetach", rect.x + 14, rect.y + 8, state.buttonsEnabled ? white : SDL_Color{160, 160, 160, 255});
    }

    // 5. Log
    {
        bool isPressed = (state.pressedButton == 5);
        SDL_Rect rect = logBtnRect;
        if (isPressed && state.buttonsEnabled) { rect.x += 2; rect.y += 2; }
        SDL_Color col = getButtonColor(logColor, 5);
        SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
        SDL_RenderFillRect(renderer, &rect);
        if (font) renderText(renderer, font, "Log", rect.x + 36, rect.y + 8, state.buttonsEnabled ? white : SDL_Color{160, 160, 160, 255});
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