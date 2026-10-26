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
};

class GUI {
public:
    GUI();
    ~GUI();

    bool init();
    void run();
    void addListItem(const std::string& item);
    
    // Callback hook triggered when Connect is pressed
    using ConnectHandler = std::function<void(GUI&)>;
    void setConnectHandler(ConnectHandler handler) { connect_handler_ = handler; }

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
    SDL_Rect listViewRect;
    SDL_Rect labelPos;

    ConnectHandler connect_handler_;
};

#endif