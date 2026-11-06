#pragma once

// Home's "Launch" (DESIGN.md 5.4, 5.6), shared by Xerces.exe and
// `xerces_cli plan apply` so the two can never start the game differently:
// a mode's profile turned into a plan request, the command line, the plan
// file in <home>\config\plans, the journal with a fresh launch id, and the
// client started from the game folder with XERCES_HOME, XERCES_DATA_PATH and
// XERCES_LAUNCH_ID in its environment only. Headless: no UI.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "diagnostics.hpp"
#include "event_log.hpp"
#include "launcher_config.hpp"
#include "launcher_home.hpp"
#include "launcher_state.hpp"
#include "planner.hpp"

namespace xerces_core {
// The plan request for a mode's profile: its mods, map, pins, allowed
// conflicts and settings, the planner's saved positions, the Steam build
// (game_build), this Xerces version, the game's launcher_settings.json text
// (read_launcher_settings) for the asset pool report, and the mode's zone
// memory budget from the build's figures (game_state::zone_memory).
plan_request
profile_request(const launcher_config &config, const std::string &mode,
                const std::optional<std::uint64_t> &steam_build,
                const std::string &launcher_settings = {},
                const std::optional<zone_memory_build> &zone_memory = {});

// The profile resolved against the catalog, every installed map included
// (the zone memory check names the maps that do not fit); mods, when given,
// replace the profile's list (a culprit-search test launch).
plan resolve_profile(const std::vector<installed_content> &catalog,
                     const launcher_config &config, const std::string &mode,
                     const std::optional<std::uint64_t> &steam_build,
                     const std::vector<std::string> *mods = nullptr,
                     const std::string &launcher_settings = {},
                     const std::optional<zone_memory_build> &zone_memory = {});

// What stopped a launch, so callers can tell the player's plan from the PC.
enum class launch_refusal {
  none,
  plan,    // the plan has problems, or no mods are enabled
  command, // a setting the command line cannot carry (build_launch_command)
  game,    // the game was not found, or its build is not supported
  running, // a game is already running
  client,  // boiii.exe is missing next to the program
  start,   // Windows refused to start it
};

struct launch_outcome {
  bool started = false;
  bool dry_run = false; // --dry-launch: built and logged, not started
  std::string message;  // what the screen shows
  std::string command_line;
  std::string plan_id;
  std::filesystem::path plan_file; // written for a modded launch
  unsigned long pid = 0;
  diagnostics diags;
  launch_refusal refusal = launch_refusal::none;
  std::optional<plan> resolved; // a modded launch's plan
  // The command line passes -xerces-db-pool (larger zone memory) or
  // -xerces-flip-model (flip-model presentation); the journal records both.
  bool db_pool = false;
  bool flip_model = false;
};

struct launch_inputs {
  const launcher_paths *paths = nullptr;
  const launcher_config *config = nullptr;
  const game_state *game = nullptr;
  const std::vector<installed_content> *catalog = nullptr;
  std::string mode = "zm";
  bool modded = true;    // false: vanilla
  // Safe mode: the last modded launch crashed, so mods are off. Implies
  // crash_followup.
  bool mods_off = false;
  // A launch that judges a crash: safe mode, or a culprit-search test (5.6).
  // It leaves the experimental settings out whatever the settings say: the
  // larger zone memory (-xerces-db-pool) and flip-model presentation
  // (-xerces-flip-model). Safe mode and every test then run on the same
  // stock pool and the engine's own swap chain, and a crash a setting causes
  // is never blamed on a mod.
  bool crash_followup = false;
  std::optional<std::string> connect; // "+connect host:port" (Servers)
  // Instead of the profile's (a culprit-search test, with crash_followup).
  const std::vector<std::string> *mods = nullptr;
  bool dry_launch = false;
};

// The command line the launch would use, without starting or writing
// anything. A plan with problems, an empty stack and values the command line
// cannot carry are refused here.
launch_outcome preview_launch(const launch_inputs &in);

// The launch: preview_launch, then the game (found and supported), no game
// running already, and the client next to the program. Writes the plan file
// (modded) and the journal (phase launching, the mode, a fresh launch id and
// whether the command passes -xerces-db-pool and -xerces-flip-model),
// then starts the client from the game folder with XERCES_HOME,
// XERCES_DATA_PATH (development builds) and XERCES_LAUNCH_ID set for it alone.
// If Windows refuses, the old journal is put back. game_running is asked
// after the plan and game checks. On success *process (when given) receives
// the process handle, which the caller closes.
//
// A dry launch (in.dry_launch) is refused where a real one is (the plan, the
// command line, the game not found or not supported, a running game), then
// writes the plan file but no journal, starts nothing and logs the command
// (launch.dry). Only a missing client does not stop it; the message says a
// real launch would stop there. Logs launch.refused, launch.dry,
// launch.started and launch.failed.
launch_outcome launch_game(const launch_inputs &in,
                           const std::function<bool()> &game_running,
                           const event_log &log, void **process = nullptr);

// A game announces itself in <home>\run while it runs (15.2), also one this
// program did not start.
bool game_in_run_folder(const std::filesystem::path &home);

// A process named like the client (boiii.exe) or BlackOps3.exe runs on this
// PC. A game started seconds ago, by Xerces.exe, xerces_cli or anything else,
// has no entry in <home>\run until it reaches post_unpack, but its process is
// there from the start. Both programs ask it before a real launch, so neither
// starts a second game.
bool game_process_running(const std::filesystem::path &client);

// The environment block for a child process: block (as from
// GetEnvironmentStringsW, double-NUL terminated) with each change applied (a
// value sets the variable, nullopt removes it; names compare ignoring case, as
// Windows does), sorted by name as CreateProcessW asks. The drive entries
// ("=C:=C:\...") stay first.
using environment_changes =
    std::vector<std::pair<std::wstring, std::optional<std::wstring>>>;
std::wstring child_environment(const wchar_t *block,
                               const environment_changes &changes);
} // namespace xerces_core
