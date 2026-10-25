#include "gui.h"

int main(int argc, char* argv[]) {
    GUI gui;
    if (!gui.init()) {
        return -1;
    }

    // You can programmatically add items from main.cpp if needed
     gui.addListItem("Hello from main.cpp!");

    gui.run();
    return 0;
}