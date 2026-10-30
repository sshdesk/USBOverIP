#ifndef GUI_H
#define GUI_H

#include <string>

class GUI {
public:
    void setLabelText(const std::string& text);
};

extern GUI* g_active_gui;

#endif // GUI_H
