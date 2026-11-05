#pragma once

// Reading the structured logs back (DESIGN.md 15.1, `xerces_cli logs tail`):
// which file is the newest of a role, one line per event, and the CLI's own
// log, which is one file per day so scripts that call xerces_cli often do not
// push the launcher's and the game's sessions out of the last 20. Headless.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
// One event as "time level event | msg", with the data object after two
// spaces when there is one, as launcher-checks\showlog.ps1 prints them:
//   21:58:55.392 info  launcher.start | Xerces 0.1.0-dev  {"home":"..."}
// A line that is not a log event comes back as it is.
std::string format_log_line(std::string_view line);

// The role in a log file's name, <date>-<time>-<role>-<session>.jsonl
// ("launcher", "game", "cli", ...); empty when the name is not one.
std::string log_role(const std::filesystem::path &file);

// The newest log of a role (any role when role is empty) in a logs folder: by
// the start time in its name, then by its last write.
std::optional<std::filesystem::path>
newest_log(const std::filesystem::path &logs_dir, std::string_view role);

// The last n complete lines of text (all of them when n is 0). A last line
// without its newline is left out: it may still be being written.
std::vector<std::string> last_lines(std::string_view text, std::size_t n);

// Today's CLI log (UTC date in the name) to append to, if one exists.
std::optional<std::filesystem::path>
todays_cli_log(const std::filesystem::path &logs_dir, std::string_view today);
} // namespace xerces_core
