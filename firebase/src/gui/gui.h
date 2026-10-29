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
    std::vector<std::string> listItems = {"Select an action to begin."};
    int selectedIndex = -1;
    std::string labelText = "No item selected";
    int scrollY = 0;
    int pressedButton = 0; // 0: None, 1: LocalUSB, 2: RemoteUSB, 3: RemoteAttach, 4: RemoteDetach, 5: Log
    bool buttonsEnabled = true; // Tracks whether buttons accept input and render as enabled
};

class GUI {
public:
    GUI();
    ~GUI();

    bool init();
    void run();
    void addListItem(const std::string& item);
    void clearListItems() { state.listItems.clear(); state.selectedIndex = -1; }
    
    using ButtonHandler = std::function<void(GUI&)>;

    void setLocalUSBHandler(ButtonHandler handler)     { local_usb_handler_ = handler; }
    void setRemoteUSBHandler(ButtonHandler handler)    { remote_usb_handler_ = handler; }
    void setRemoteAttachHandler(ButtonHandler handler) { remote_attach_handler_ = handler; }
    void setRemoteDetachHandler(ButtonHandler handler) { remote_detach_handler_ = handler; }
    void setLogHandler(ButtonHandler handler)          { log_handler_ = handler; }

    void setLabelText(const std::string& text) { state.labelText = text; }
    void setButtonsEnabled(bool enabled) { state.buttonsEnabled = enabled; }

    std::string getSelectedItem() const {
        if (state.selectedIndex >= 0 && state.selectedIndex < static_cast<int>(state.listItems.size())) {
            return state.listItems[state.selectedIndex];
        }
        return "";
    }
    int getSelectedIndex() const { return state.selectedIndex; }
    std::string getInputText() const { return state.inputText; }

private:
    void handleEvents(bool& running);
    void render();

    SDL_Window* window;
    SDL_Renderer* renderer;
    TTF_Font* font;
    UIState state;
    bool isDraggingScrollbar;

    SDL_Rect textBoxRect;
    SDL_Rect localUsbBtnRect;
    SDL_Rect remoteUsbBtnRect;
    SDL_Rect remoteAttachBtnRect;
    SDL_Rect remoteDetachBtnRect;
    SDL_Rect logBtnRect;
    SDL_Rect listViewRect;
    SDL_Rect labelPos;

    ButtonHandler local_usb_handler_;
    ButtonHandler remote_usb_handler_;
    ButtonHandler remote_attach_handler_;
    ButtonHandler remote_detach_handler_;
    ButtonHandler log_handler_;
};

#endif