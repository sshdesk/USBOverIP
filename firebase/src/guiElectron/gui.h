#ifndef GUI_H
#define GUI_H

#include <string>

class GUI {
public:
    void setLabelText(const std::string& text);

    void setInputText(const std::string& text);

    std::string getInputText() const;

};

extern GUI* g_active_gui;

#endif // GUI_H
