#ifndef LOGSYNC_H
#define LOGSYNC_H

#include <string>
#include <deque>
#include <vector>
#include <mutex>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include "gui.h"
#include <windows.h>
#include <shlobj.h>
#include <filesystem>


extern GUI *g_active_gui;
extern std::deque<std::string> g_log_buffer;
extern std::mutex g_log_mutex;




inline void SetupRotatingLogging(bool host_server, const std::string &level_str = "info") {
    try {
        // 1. Determine AppData path safely
        namespace fs = std::filesystem;
        fs::path log_dir;
        
        char appdata_path[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata_path))) {
            log_dir = fs::path(appdata_path) / "USBIP" / "logs";
        } else {
            // Fallback to temp directory if AppData is inaccessible
            log_dir = fs::temp_directory_path() / "USBIP" / "logs";
        }

        // 2. Ensure the log directory exists
        if (!fs::exists(log_dir)) {
            fs::create_directories(log_dir);
        }

        // 3. Construct absolute path for the log file
        std::string log_file_name = host_server ? "host_log.txt" : "client_log.txt";
        fs::path full_log_path = log_dir / log_file_name;

        auto max_size = 1048576 ; // 1 MB
        auto max_files = 3;
        
        auto rotating_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(full_log_path.string(), max_size, max_files);
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(rotating_sink);
        sinks.push_back(console_sink);
        
        auto logger = std::make_shared<spdlog::logger>("usbip_logger", sinks.begin(), sinks.end());
        
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::from_str(level_str));
        spdlog::flush_on(spdlog::level::info);
        
        spdlog::info("Logging initialized at: {}", full_log_path.string());
    } catch (const spdlog::spdlog_ex& ex) {
        std::printf("Log initialization failed: %s\n", ex.what());
    }
}

template <typename Mutex>
class GuiLogSink : public spdlog::sinks::base_sink<Mutex> {
protected:
  void sink_it_(const spdlog::details::log_msg &msg) override {
    spdlog::memory_buf_t formatted;
    spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);
    std::string log_str = fmt::to_string(formatted);

    while (!log_str.empty() &&
           (log_str.back() == '\n' || log_str.back() == '\r')) {
      log_str.pop_back();
    }

    /*if (g_active_gui) {
      g_active_gui->setLabelText(log_str);
    }*/

    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_buffer.push_back(log_str);
    if (g_log_buffer.size() > 500)
      g_log_buffer.pop_front();
  }
  void flush_() override {}
};

#endif // LOGSYNC_H
