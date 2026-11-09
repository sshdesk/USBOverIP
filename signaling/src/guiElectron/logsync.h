#ifndef LOGSYNC_H
#define LOGSYNC_H

#include <string>
#include <deque>
#include <vector>
#include <mutex>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include "gui.h"

extern std::deque<std::string> g_log_buffer;
extern std::mutex g_log_mutex;

inline void SetupRotatingLogging(bool host_server, const std::string &level_str = "info") {
    try {
        auto max_size = 1048576 * 5; // 5 MB
        auto max_files = 3;
        std::string log_file_name = host_server ? "host_log.txt" : "client_log.txt";

        auto rotating_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_file_name, max_size, max_files);
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        std::vector<spdlog::sink_ptr> sinks = { rotating_sink, console_sink };
        auto logger = std::make_shared<spdlog::logger>("usbip_logger", sinks.begin(), sinks.end());

        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::from_str(level_str));
        spdlog::flush_on(spdlog::level::info);
    } catch (const spdlog::spdlog_ex& ex) {
        std::printf("Log initialization failed: %s\n", ex.what());
    }
}

#endif // LOGSYNC_H
