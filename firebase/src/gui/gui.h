#ifndef GUI_H
#define GUI_H

#include <SDL.h>
#include <SDL_ttf.h>
#include <string>
#include <vector>
#include <functional>

struct UIState {
    std::string inputText = "enteryour@email.com";
    bool isInputActive = false;
    std::vector<std::string> listItems = {"click share for sharing local usb."};
    int selectedIndex = -1;
    std::string labelText = "No item selected";
    int scrollY = 0;
    int pressedButton = 0; // 0: None, 1: ShareUSB, 2: Bind, 3: Log
};

class GUI {
public:
    GUI();
    ~GUI();

    bool init();
    void run();
    void addListItem(const std::string& item);
    void clearListItems() { state.listItems.clear(); state.selectedIndex = -1; }
    
    using ShareUSBHandler = std::function<void(GUI&)>;
    using LogHandler = std::function<void(GUI&)>;

    void setShareUSBHandler(ShareUSBHandler handler) { share_usb_handler_ = handler; }
    void setLogHandler(LogHandler handler) { log_handler_ = handler; }
    void setLabelText(const std::string& text) { state.labelText = text; }

private:
    void handleEvents(bool& running);
    void render();

    SDL_Window* window;
    SDL_Renderer* renderer;
    TTF_Font* font;
    UIState state;
    bool isDraggingScrollbar;

    SDL_Rect textBoxRect;
    SDL_Rect buttonRect;
    SDL_Rect bindBtnRect;
    SDL_Rect logBtnRect;
    SDL_Rect listViewRect;
    SDL_Rect labelPos;

    ShareUSBHandler share_usb_handler_;
    LogHandler log_handler_;
};

#endif