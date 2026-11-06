#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
// The launch journal and safe mode (DESIGN.md 5.6).
enum class launch_phase {
  launching,
  symbols,
  frontend,
  loading_map,
  in_game,
  clean_exit,
};

const char *to_string(launch_phase phase);
std::optional<launch_phase> parse_launch_phase(std::string_view text);

struct launch_journal {
  std::string plan_id;
  std::vector<std::string> stack; // mod ids, highest first
  launch_phase phase = launch_phase::launching;
  std::string started; // UTC timestamps
  std::string updated;
  bool safe_mode = false; // this launch ran with mods off
  // The game mode the launch used (zm, mp, cp), so a culprit search tests
  // the crashed stack in the mode it crashed in. Empty in older journals.
  std::string mode;
  // The one-time id the launcher also hands the game in XERCES_LAUNCH_ID.
  // The game clears it when it takes the journal over, so an id still here
  // means the game never did (see journal_claimable and exit_counts_as_clean).
  std::string launch_id;
  // The launch passed -xerces-db-pool, the experimental larger zone memory
  // (DESIGN.md 5.4), so a crash can be traced to it and not only to the mods.
  // False in older journals.
  bool db_pool = false;
  // The same for -xerces-precise-limiter, the experimental precise frame
  // limiter (DESIGN.md 12.4).
  bool precise_limiter = false;
};

std::string journal_to_json(const launch_journal &value);
// Journals written before a field existed parse with that field empty.
std::optional<launch_journal> parse_journal(std::string_view text);

// A fresh launch id: 32 hex digits from the system's random source.
std::string new_launch_id();

// The game's side: a game started with XERCES_LAUNCH_ID = launch_id takes
// the journal over only if it is the launch the launcher just recorded (same
// id, still "launching"). A game restarted from the crash dialog has no id,
// so it never overwrites the record of the launch that crashed.
bool journal_claimable(const std::optional<launch_journal> &journal,
                       std::string_view launch_id);

// The launcher's side when the game it started has exited. Exit code 0
// counts as a clean exit only when the game never took the journal over
// (it quit before Xerces' code ran). Otherwise the journal decides: the
// crash dialog's "Restart game" and fatal engine errors also end with
// code 0, and a normal quit writes clean_exit itself.
bool exit_counts_as_clean(const launch_journal &journal,
                          std::uint32_t exit_code);

// The game's side when it ends through its own ExitProcess (main.cpp's exit
// hook runs the components' shutdown, and with it the journal's last write).
// It ended cleanly when the player quit (the engine's quit command ran) or
// the code is 0. Any other code is a crash and the journal keeps the phase it
// had: on 2026-11-05 two custom maps ended the game with 0xFFFFFFFF in the
// middle of their load, which used to be written as clean_exit. Without a
// code (the process ended another way) it counts as clean as before.
bool game_exit_is_clean(bool quit_requested,
                        std::optional<std::uint32_t> exit_code);

// No journal, or one that does not end in clean_exit: the launch crashed.
bool journal_crashed(const std::optional<launch_journal> &journal);

struct safe_mode_decision {
  bool safe_mode = false;
  std::string crashed_plan_id;
  std::vector<std::string> crashed_stack;
  launch_phase crashed_in = launch_phase::launching;
  std::string crashed_mode;    // empty when an older journal did not say
  std::string crashed_started; // when the crashed launch started (UTC)
  // The crashed launch had the larger zone memory (-xerces-db-pool) or the
  // precise frame limiter (-xerces-precise-limiter): each is a suspect too,
  // and the culprit search tests them first.
  bool crashed_with_db_pool = false;
  bool crashed_with_precise_limiter = false;
  std::string message;
};

// After any crash with mods enabled, the next launch defaults to safe mode.
safe_mode_decision decide_safe_mode(const std::optional<launch_journal> &last);

// The experimental settings the crashed launch had, as flag names without
// the dash (settings_options' with_flag): "xerces-db-pool", then
// "xerces-precise-limiter".
std::vector<std::string> crashed_settings(const safe_mode_decision &decision);

// Whether "Find the culprit" has something to tell apart after this crash:
// two mods or more, or one mod and an experimental setting.
bool culprit_search_possible(const safe_mode_decision &decision);

// The experimental settings (crashed_settings) in a culprit search. Safe
// mode and every test launch run without them
// (launch_inputs::crash_followup), so they all see the stock pool and the
// engine's own frame limiter. When the crashed launch had one, the first test
// is the whole crashed stack without them.
enum class settings_suspect {
  none,     // the crashed launch had none of them
  untested, // the next test: the whole crashed stack without them
  cleared,  // that stack crashed without them too: the mods are bisected
  culprit,  // that stack ran fine without them: they caused the crash
};

// "Find the culprit": halve the crashed stack over a few launches, after
// testing the experimental settings when the crashed launch had any.
class culprit_search {
public:
  explicit culprit_search(std::vector<std::string> crashed_stack,
                          bool crashed_with_settings = false);
  // A search read back from disk (it spans several launches).
  culprit_search(std::vector<std::string> suspects, bool testing_second_half,
                 bool combination, std::size_t launches,
                 settings_suspect settings = settings_suspect::none);

  // Mods for the next test launch; empty when the search is over.
  std::vector<std::string> next_test() const;
  void record(bool crashed);

  bool done() const;
  // The one mod that crashes alone, if that is what the search found.
  std::optional<std::string> culprit() const;
  // The crash needs mods from both halves together (an interaction).
  bool needs_combination() const { return combination_; }
  // The next test is the whole crashed stack without the settings.
  bool testing_settings() const {
    return settings_ == settings_suspect::untested;
  }
  // The crashed stack ran fine without the settings.
  bool settings_culprit() const {
    return settings_ == settings_suspect::culprit;
  }
  settings_suspect settings_step() const { return settings_; }
  std::size_t launches() const { return launches_; }
  const std::vector<std::string> &suspects() const { return suspects_; }
  bool testing_second_half() const { return testing_second_half_; }

private:
  std::vector<std::string> suspects_;
  bool testing_second_half_ = false;
  bool combination_ = false;
  std::size_t launches_ = 0;
  settings_suspect settings_ = settings_suspect::none;
};
// A culprit search in progress, kept in <home>\config\culprit.json between
// launches: the mode and stack that crashed, the experimental settings it
// had (crashed_settings), the search, and the mods of the test launch whose
// result is still to be recorded (empty when none).
struct culprit_progress {
  std::string mode;
  std::vector<std::string> crashed_stack;
  std::vector<std::string> settings;
  std::vector<std::string> testing;
  culprit_search search{{}};
};

std::string culprit_progress_to_json(const culprit_progress &value);
std::optional<culprit_progress> parse_culprit_progress(std::string_view text);
} // namespace xerces_core
