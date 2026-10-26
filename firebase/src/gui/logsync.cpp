#include "logsync.h"

GUI *g_active_gui = nullptr;
std::deque<std::string> g_log_buffer;
std::mutex g_log_mutex;