#pragma once

// What every Xerces part of the game role shares (docs/DESIGN.md 3.5, 15.1):
// <home> (XERCES_HOME, set by the launcher, else %LOCALAPPDATA%\Xerces) and
// the game role's JSON log, <home>\logs\<date>-<time>-game-<pid>.jsonl.

#include <filesystem>
#include <string>
#include <string_view>

#include <xerces_core/log.hpp>

namespace xerces_runtime {
std::filesystem::path home();
// The launcher sets XERCES_HOME for the game it starts; a game started any
// other way leaves the launcher's journal alone.
bool started_by_launcher();

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

// Writes a file through a temporary file, so a crash never leaves half of
// it behind.
bool write_file(const std::filesystem::path &path, std::string_view text);
std::string read_file(const std::filesystem::path &path);

// The value after a flag on the game's own command line ("-xerces-plan
// <id>"), looking only before the first "+" command.
std::string flag_value(std::string_view flag);
} // namespace xerces_runtime
