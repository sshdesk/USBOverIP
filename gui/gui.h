#ifndef GUI_H
#define GUI_H

#include <SDL.h>
#include <SDL_ttf.h>
#include <string>
#include <vector>

struct UIState {
    std::string inputText = "enteryour@.email.com"; // Pre-filled email placeholder/text
    bool isInputActive = false;
    std::vector<std::string> listItems = {"Item 1", "Item 2", "Item 3", "Item 4", "Item 5", "Item 6", "Item 7", "Item 8", "Item 9", "Item 10"};
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
};

#endif