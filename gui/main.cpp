#include <SDL.h>
#include <SDL_ttf.h>
#include <iostream>
#include <string>
#include <vector>

const int WINDOW_WIDTH = 640;
const int WINDOW_HEIGHT = 480;

struct UIState {
    std::string inputText = "";
    bool isInputActive = false;
    std::vector<std::string> listItems = {"Item 1", "Item 2", "Item 3"};
    int selectedIndex = -1;
    std::string labelText = "No item selected";
};

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
    if (SDL_Init(SDL_INIT_VIDEO) < 0 || TTF_Init() < 0) {
        std::cerr << "SDL/TTF Init Failed: " << SDL_GetError() << std::endl;
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow("Cross-Platform SDL GUI", 
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    // Cross-Platform safe font path traversal
    TTF_Font* font = TTF_OpenFont("assets/arial.ttf", 20);
    if (!font) {
        font = TTF_OpenFont("arial.ttf", 20); // Android assets root fallback
    }
    if (!font) {
        // Ultimate Linux fallback if local assets are missing
        font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 20);
    }

    UIState state;
    bool running = true;
    SDL_Event e;

    SDL_Rect textBoxRect = { 50, 50, 300, 40 };
    SDL_Rect buttonRect  = { 370, 50, 100, 40 };
    SDL_Rect bindBtnRect = { 480, 50, 100, 40 };  
    SDL_Rect listViewRect= { 50, 120, 530, 220 }; 
    SDL_Rect labelPos     = { 50, 360, 0, 0 };

    while (running) {
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_MOUSEBUTTONDOWN) {
                int mx = e.button.x;
                int my = e.button.y;

                // Text Box boundaries check
                if (mx >= textBoxRect.x && mx <= textBoxRect.x + textBoxRect.w &&
                    my >= textBoxRect.y && my <= textBoxRect.y + textBoxRect.h) {
                    state.isInputActive = true;
                    SDL_StartTextInput();
                } else {
                    state.isInputActive = false;
                    SDL_StopTextInput();
                }

                // Add button action
                if (mx >= buttonRect.x && mx <= buttonRect.x + buttonRect.w &&
                    my >= buttonRect.y && my <= buttonRect.y + buttonRect.h) {
                    if (!state.inputText.empty()) {
                        state.listItems.push_back(state.inputText);
                        state.inputText.clear();
                    }
                }

                // Bind button action (Triggers Native OS Popup window)
                if (mx >= bindBtnRect.x && mx <= bindBtnRect.x + bindBtnRect.w &&
                    my >= bindBtnRect.y && my <= bindBtnRect.y + bindBtnRect.h) {
                    
                    if (state.selectedIndex >= 0 && state.selectedIndex < (int)state.listItems.size()) {
                        std::string message = "Selected Item: " + state.listItems[state.selectedIndex];
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Bind Info", message.c_str(), window);
                    } else {
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Warning", "Please select a list item first!", window);
                    }
                }

                // List View index tracking selection
                if (mx >= listViewRect.x && mx <= listViewRect.x + listViewRect.w &&
                    my >= listViewRect.y && my <= listViewRect.y + listViewRect.h) {
                    int relativeY = my - listViewRect.y;
                    int clickedIndex = relativeY / 30;
                    if (clickedIndex >= 0 && clickedIndex < (int)state.listItems.size()) {
                        state.selectedIndex = clickedIndex;
                        state.inputText = state.listItems[clickedIndex];
                        state.labelText = "Selected: " + state.listItems[clickedIndex];
                    }
                }
            } else if (e.type == SDL_TEXTINPUT && state.isInputActive) {
                #if defined(__APPLE__)
                bool ctrlPressed = (SDL_GetModState() & KMOD_GUI); // Command key mapping
                #else
                bool ctrlPressed = (SDL_GetModState() & KMOD_CTRL);
                #endif
                if (!ctrlPressed) {
                    state.inputText += e.text.text;
                }
            } else if (e.type == SDL_KEYDOWN && state.isInputActive) {
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

        SDL_SetRenderDrawColor(renderer, 40, 44, 52, 255);
        SDL_RenderClear(renderer);

        SDL_Color white     = { 255, 255, 255, 255 };
        SDL_Color black     = { 0, 0, 0, 255 };
        SDL_Color blue      = { 70, 130, 180, 255 };
        SDL_Color purple    = { 138, 43, 226, 255 }; 
        SDL_Color gray      = { 60, 64, 72, 255 };
        SDL_Color highlight = { 100, 149, 237, 255 };
        SDL_Color green     = { 152, 195, 121, 255 };

        // Render TextBox background & text string
        SDL_SetRenderDrawColor(renderer, state.isInputActive ? 255 : 240, 240, 240, 255);
        SDL_RenderFillRect(renderer, &textBoxRect);
        if (font) renderText(renderer, font, state.inputText, textBoxRect.x + 8, textBoxRect.y + 8, black);

        // Render Add Button
        SDL_SetRenderDrawColor(renderer, blue.r, blue.g, blue.b, 255);
        SDL_RenderFillRect(renderer, &buttonRect);
        if (font) renderText(renderer, font, "Add", buttonRect.x + 32, buttonRect.y + 8, white);

        // Render Bind Button
        SDL_SetRenderDrawColor(renderer, purple.r, purple.g, purple.b, 255);
        SDL_RenderFillRect(renderer, &bindBtnRect);
        if (font) renderText(renderer, font, "Bind", bindBtnRect.x + 28, bindBtnRect.y + 8, white);

        // Render ListView panel & elements
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

        // Render Output Label block info
        if (font) renderText(renderer, font, state.labelText, labelPos.x, labelPos.y, green);

        SDL_RenderPresent(renderer);
    }

    if (font) TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
