#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
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

// The text written to logs and diagnostics bundles is shared in bug reports,
// and a server address there can be a friend's home IP (DESIGN.md 5.10, 15.1).
// Replaced by redacted_address:
//   - IPv4 addresses, with a ":port" if one follows;
//   - IPv6 addresses in brackets ("[::1]:27017"), and without brackets when
//     they hold "::" or all eight groups;
//   - a host name with a dot and a port ("friend.example.org:27017");
//   - whatever follows "+connect" or "xerces_join", the join commands;
//   - each of `known` (addresses the caller knows the person used, such as
//     their favourite and recent servers), with or without its port, where
//     it stands as a whole word.
// Any other host name without a port is left alone: it cannot be told apart
// from file and event names such as "plan.json". The patterns may also catch
// the odd "file.cpp:91"; losing that is the price of never leaking an address.
//
// For plain text only. In JSON text an escape such as "\n" glues its letter
// to the address that follows ("\n203.0.113.7" reads as one word), so JSON
// goes through redact_addresses_in_json instead.
inline constexpr std::string_view redacted_address = "<address>";
std::string redact_addresses(std::string_view text,
                             std::span<const std::string> known = {});

// redact_addresses for JSON text: a document or JSON Lines. Every string in
// it is decoded, scrubbed, and written back escaped only if it changed;
// everything else is kept byte for byte, so valid JSON stays valid. The
// scanner is lexical, so a truncated last line (a crash mid-write) or an old
// file that no longer parses is still scrubbed. Text outside strings, which
// in JSON is only numbers and punctuation, gets the patterns but not `known`:
// a server called "null" must not break the document.
std::string redact_addresses_in_json(std::string_view json_text,
                                     std::span<const std::string> known = {});

// JSON Lines, one file per session: <logs>/<date>-<time>-<role>.jsonl.
// format() passes every message through redact_addresses and the data object
// through redact_addresses_in_json. A host name without a port is not
// recognised there, so a caller that logs a server address it was given logs
// redacted_address (or nothing) in its place.
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
