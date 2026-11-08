#pragma once

// Getting Workshop items through Steam (docs/DESIGN.md 9.2): subscribe and
// download them with Steam's own Workshop API, as Steam's Subscribe button
// does, under the player's Steam account. The player chose this on
// 2026-11-08 over opening each item's page by hand.
//
// It runs in its own short process (`xerces_cli workshop get`), which loads
// Black Ops III's own steam_api64.dll from the game folder and starts the
// Steamworks API as Black Ops III (app 311210): Steam allows that only to an
// account that owns the game, and while the process runs Steam may show the
// player as playing it. Nothing goes around Steam: Steam itself decides
// whether to subscribe and download, and puts the items in its Workshop
// folder, where the game and Xerces already find them. The launcher and the
// game start the process and read its progress, one JSON line per change.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core::workshop_get {
inline constexpr std::uint32_t black_ops_3_app_id = 311210;

// One line of progress. state: "ready" (Steam answered; id 0), "subscribing",
// "queued", "downloading", "installed", "failed" (message says why), or
// "error" (Steam could not be started; id 0; message says why).
struct progress {
  std::uint64_t id = 0;
  std::string state;
  std::uint64_t done = 0;  // bytes, while downloading
  std::uint64_t total = 0; // bytes, 0 while Steam does not know yet
  std::string message;
  bool operator==(const progress &) const = default;
};
std::string to_json(const progress &value);
std::optional<progress> parse_progress(std::string_view line);

// What Steam's item state flags (EItemState) mean for a download.
enum class item_phase { none, subscribing, queued, downloading, installed };
item_phase phase_of(std::uint32_t state, bool subscribe_asked);
const char *to_string(item_phase phase);

// "123,456" or "123 456" -> ids; nullopt when one is not a Workshop id.
std::optional<std::vector<std::uint64_t>> parse_ids(std::string_view text);

struct options {
  std::filesystem::path game_dir; // holds steam_api64.dll
  std::vector<std::uint64_t> ids;
  // An item Steam does not subscribe to within subscribe_limit fails (taken
  // down, or private); the rest fail when none of them has moved on for
  // stall_limit (Steam downloads one at a time, so a queued item may wait).
  std::chrono::seconds subscribe_limit{90};
  std::chrono::seconds stall_limit{300};
  std::function<void(const progress &)> report;
  std::function<bool()> cancelled; // optional
};
// Returns 0 when every item is installed, 1 when some failed, 2 when Steam
// could not be started (no Steam, not signed in, the game not owned, or no
// steam_api64.dll in game_dir).
int run(const options &value);

// Starts `<cli> workshop get <ids> --game-dir <game_dir>` without a window
// and hands each progress line to on_line, on this thread, until it ends.
// stderr is thrown away (Steam's library prints the player's Steam id on
// it). process, when given, gets the process handle while it runs (for
// TerminateProcess to cancel it), and nullptr after. Returns its exit code,
// or -1 when it could not be started (error says why).
int run_helper(const std::filesystem::path &cli,
               const std::filesystem::path &game_dir,
               const std::vector<std::uint64_t> &ids,
               const std::function<void(const progress &)> &on_line,
               std::atomic<void *> *process, std::string *error);
} // namespace xerces_core::workshop_get
