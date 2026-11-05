#pragma once

// What xerces_cli's files share: arguments, exit codes (DESIGN.md 15.2) and
// the printing helpers. main.cpp has the offline commands and the pipe;
// home_commands.cpp the ones that work on <home> as Xerces.exe does.

#include <filesystem>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <xerces_core/diagnostics.hpp>
#include <xerces_core/pack.hpp>
#include <xerces_core/planner.hpp>

namespace xerces_cli {
namespace fs = std::filesystem;

enum exit_code {
  exit_ok = 0,
  exit_error = 1,
  exit_failed = 2, // validation or conflict failures
  exit_not_running = 3,
  exit_signatures = 4,
};

struct arguments {
  std::vector<std::string> words;
  std::map<std::string, std::vector<std::string>> options;
  std::set<std::string> flags;
  // What parse_arguments refused: a flag written with a value
  // ("--dry-launch=yes"), an option without its value ("--home --dry-launch").
  std::vector<std::string> errors;

  bool has(const std::string &flag) const { return flags.contains(flag); }
  std::optional<std::string> get(const std::string &name) const {
    const auto it = options.find(name);
    if (it == options.end() || it->second.empty()) {
      return std::nullopt;
    }
    return it->second.back();
  }
  std::vector<std::string> all(const std::string &name) const {
    const auto it = options.find(name);
    return it == options.end() ? std::vector<std::string>{} : it->second;
  }
  std::string word(std::size_t index) const {
    return index < words.size() ? words[index] : std::string{};
  }
};

// The words, options and flags a command takes, "--home" included when it
// is one of them. Anything else (an unknown or misspelled option, a stray
// word) is named on stderr and the command must stop with exit_error before
// doing any work: "--dry-run" for "--dry-launch" must not start the game.
bool accept_only(const arguments &args, std::size_t words,
                 std::initializer_list<std::string_view> options,
                 std::initializer_list<std::string_view> flags);

// '/' separated, UTF-8.
std::string utf8(const fs::path &path);
void print_diagnostics(const xerces_core::diagnostics &diags);
std::string diagnostics_json(const xerces_core::diagnostics &diags);
void print_plan(const xerces_core::plan &p, bool explain);
// The plan's zone memory line, and the installed maps that do not fit.
void print_zone_memory(const xerces_core::plan &p);
// A share code (XRC1.) or a pack file.
xerces_core::pack_result load_pack(const std::string &source);

// home_commands.cpp: <home> as Xerces.exe sees it (--home, else XERCES_HOME,
// else %LOCALAPPDATA%\Xerces).
int cmd_mods_home_list(const arguments &args);
int cmd_mods_info(const arguments &args);
int cmd_mods_edit(const arguments &args, const std::string &action);
int cmd_plan_apply(const arguments &args);
int cmd_packs_apply(const arguments &args);
int cmd_logs_tail(const arguments &args);
int cmd_sig_report(const arguments &args);
} // namespace xerces_cli
