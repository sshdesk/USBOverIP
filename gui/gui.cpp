#include "gui.h"
#include <iostream>
#include <algorithm>

const int WINDOW_WIDTH = 640;
const int WINDOW_HEIGHT = 480;
const int ITEM_HEIGHT = 30;

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
    textBoxRect  = { 50, 50, 300, 40 };
    buttonRect   = { 370, 50, 100, 40 };
    bindBtnRect  = { 480, 50, 100, 40 };
    listViewRect = { 50, 120, 530, 220 };
    labelPos     = { 50, 360, 0, 0 };
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

    window = SDL_CreateWindow("Cross-Platform SDL GUI", 
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    if (!window) return false;

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) return false;

    font = TTF_OpenFont("assets/arial.ttf", 20);
    if (!font) font = TTF_OpenFont("arial.ttf", 20);
    if (!font) font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 20);

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
                
                int scrollSpeed = 15;
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

                // Connect Button Click Event (formerly Add)
                if (mx >= buttonRect.x && mx <= buttonRect.x + buttonRect.w &&
                    my >= buttonRect.y && my <= buttonRect.y + buttonRect.h) {
                    addListItem(state.inputText);
                    state.inputText.clear();
                }

                // Bind Button Click Event
                if (mx >= bindBtnRect.x && mx <= bindBtnRect.x + bindBtnRect.w &&
                    my >= bindBtnRect.y && my <= bindBtnRect.y + bindBtnRect.h) {
                    
                    if (state.selectedIndex >= 0 && state.selectedIndex < (int)state.listItems.size()) {
                        std::string message = "Selected Item: " + state.listItems[state.selectedIndex];
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Bind Info", message.c_str(), window);
                    } else {
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Warning", "Please select a list item first!", window);
                    }
                }

                if (mx >= listViewRect.x && mx <= listViewRect.x + listViewRect.w - scrollTrackW &&
                    my >= listViewRect.y && my <= listViewRect.y + listViewRect.h) {
                    
                    int relativeY = (my - listViewRect.y) + state.scrollY;
                    int clickedIndex = relativeY / ITEM_HEIGHT;
                    
                    if (clickedIndex >= 0 && clickedIndex < (int)state.listItems.size()) {
                        state.selectedIndex = clickedIndex;
                        state.inputText = state.listItems[clickedIndex];
                        state.labelText = "Selected: " + state.listItems[clickedIndex];
                    }
                }
            }
        } 
        else if (e.type == SDL_MOUSEBUTTONUP) {
            if (e.button.button == SDL_BUTTON_LEFT) {
                isDraggingScrollbar = false;
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

    // Connect Button (adjusted text position for label length)
    SDL_SetRenderDrawColor(renderer, blue.r, blue.g, blue.b, 255);
    SDL_RenderFillRect(renderer, &buttonRect);
    if (font) renderText(renderer, font, "Connect", buttonRect.x + 18, buttonRect.y + 8, white);

    // Bind Button
    SDL_SetRenderDrawColor(renderer, purple.r, purple.g, purple.b, 255);
    SDL_RenderFillRect(renderer, &bindBtnRect);
    if (font) renderText(renderer, font, "Bind", bindBtnRect.x + 28, bindBtnRect.y + 8, white);

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
                renderText(renderer, font, state.listItems[i], itemRect.x + 10, itemRect.y + 4, white);
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