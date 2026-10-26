#ifndef LOGSYNC_H
#define LOGSYNC_H

#include <string>
#include <deque>
#include <mutex>
#include <spdlog/sinks/base_sink.h>
#include "gui.h"

extern GUI *g_active_gui;
extern std::deque<std::string> g_log_buffer;
extern std::mutex g_log_mutex;

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

    // Update live status bar (labelText)
    if (g_active_gui) {
      g_active_gui->setLabelText(log_str);
    }

    // Store in background log queue for the Log button
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_buffer.push_back(log_str);
    if (g_log_buffer.size() > 500)
      g_log_buffer.pop_front();
  }
  void flush_() override {}
};

#endif // LOGSYNC_H