#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace xerces_core {
enum class log_level { trace, debug, info, warn, error };

const char *to_string(log_level level);

// One structured log line (DESIGN.md 15.1).
struct log_event {
  log_level level = log_level::info;
  std::string component; // "loader", "planner", ...
  std::string event;     // stable dotted name: "loader.zone.loaded"
  std::string message;
  std::string plan_id;
  std::string data_json; // a JSON object, or empty
};

// Replaces the profile folder (any case, either slash, JSON-escaped or not)
// with %USERPROFILE%, so logs can be shared.
std::string redact_profile(std::string_view text, std::string_view profile);

// JSON Lines, one file per session: <logs>/<date>-<time>-<role>.jsonl.
// Addresses never go into these files.
class json_log {
public:
  json_log(std::string role, std::string session);

  // The line for an event, without the newline.
  std::string format(const log_event &event, std::string_view timestamp) const;

  // Opens a new session file and deletes all but the newest `keep` sessions.
  bool open(const std::filesystem::path &logs_dir, std::size_t keep = 20);
  void write(const log_event &event);
  const std::filesystem::path &path() const { return path_; }

  void set_profile_path(std::string profile) { profile_ = std::move(profile); }

  static std::string now_utc(); // 2026-11-04T21:58:55.392Z

private:
  std::string role_;
  std::string session_;
  std::string profile_;
  std::filesystem::path path_;
  std::ofstream file_;
  std::mutex mutex_;
};
} // namespace xerces_core
