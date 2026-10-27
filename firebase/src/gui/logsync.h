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

extern GUI *g_active_gui;
extern std::deque<std::string> g_log_buffer;
extern std::mutex g_log_mutex;

inline void
SetupRotatingLogging(bool host_server, const std::string &level_str = "info") {
    try {
        auto max_size = 1048576 * 5; // 5 MB
        auto max_files = 3;
        
        std::string log_file_name =    host_server ? "host_log.txt" : "client_log.txt";
     

        auto rotating_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_file_name, max_size, max_files);
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(rotating_sink);
        sinks.push_back(console_sink);
        
        auto logger = std::make_shared<spdlog::logger>("usbip_logger", sinks.begin(), sinks.end());
        
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::from_str(level_str));
        spdlog::flush_on(spdlog::level::info);
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