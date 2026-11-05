#pragma once

// The launcher's structured log (DESIGN.md 15.1): <home>\logs\<date>-<time>-
// launcher.jsonl through xerces_core::json_log, plus OutputDebugString. Safe
// to call from any thread.

#include <string>
#include <string_view>

#include <xerces_core/log.hpp>

namespace xerces_launcher {
// Opens the session file; before this, events go only to the debugger.
bool open_log(const std::string &logs_dir);
std::string log_path();

void log(xerces_core::log_level level, std::string_view event,
         std::string_view message, std::string_view data_json = {});
inline void log_info(std::string_view event, std::string_view message,
                     std::string_view data_json = {}) {
  log(xerces_core::log_level::info, event, message, data_json);
}
inline void log_warn(std::string_view event, std::string_view message,
                     std::string_view data_json = {}) {
  log(xerces_core::log_level::warn, event, message, data_json);
}
inline void log_error(std::string_view event, std::string_view message,
                      std::string_view data_json = {}) {
  log(xerces_core::log_level::error, event, message, data_json);
}
} // namespace xerces_launcher
