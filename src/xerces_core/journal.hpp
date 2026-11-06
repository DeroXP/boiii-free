#pragma once

#include <cstddef>
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
  // The crashed launch had the larger zone memory (-xerces-db-pool): it is a
  // suspect too, and the culprit search tests it first.
  bool crashed_with_db_pool = false;
  // What the screens say: one of a fixed set of sentences (per phase, with
  // or without the larger zone memory), so it can be translated as it is.
  std::string message;
};

// After any crash with mods enabled, the next launch defaults to safe mode.
safe_mode_decision decide_safe_mode(const std::optional<launch_journal> &last);

// Whether "Find the culprit" has something to tell apart after this crash:
// two mods or more, or one mod and the larger zone memory.
bool culprit_search_possible(const safe_mode_decision &decision);

// The larger zone memory (-xerces-db-pool) in a culprit search. Safe mode and
// every test launch run without it (launch_inputs::crash_followup), so they
// all see the stock pool. When the crashed launch had it, the first test is
// the whole crashed stack without it.
enum class db_pool_suspect {
  none,     // the crashed launch did not have it
  untested, // the next test: the whole crashed stack without it
  cleared,  // that stack crashed without it too: the mods are bisected
  culprit,  // that stack ran fine without it: it caused the crash
};

// "Find the culprit": halve the crashed stack over a few launches, after
// testing the larger zone memory when the crashed launch had it.
//
// The suspects are the whole crashed stack, the dependencies the planner
// pulled in included, in stack order (highest first). A test asks the
// planner for some of them as the plan's mods, keeping the profile's map, so
// it also loads what they need, and what the map needs: those of the map's
// dependencies that are suspects load in every test ("always"). Every split
// tests the lower part of the suspects first. The planner puts each
// dependency below the mods that need it, so the lower part loads nothing
// from the upper part. A crash narrows the suspects to what the test loaded;
// a test that runs fine clears what it loaded, and the rest is tested next,
// as a whole, to see that it crashes without them. So a dependency that
// crashes is found itself, not blamed on the mod that needs it.
//
// The map's dependencies cannot be left out of a test, so they are split
// off last: when one suspect is left besides them, they are tested alone
// (with the map). Suspects that only ever load together, with the map, end
// the search without a culprit (inseparable()). A test that loads every
// suspect although it did not ask for them all shows which ones the map
// brings (a map changed during the search, or a search saved without
// "always"): they join "always", and the next split leaves them aside.
class culprit_search {
public:
  // always: the suspects every test loads, whatever it asks for (the
  // dependencies of the profile's map); empty when the profile has no map.
  explicit culprit_search(std::vector<std::string> crashed_stack,
                          bool crashed_with_db_pool = false,
                          std::vector<std::string> always = {});
  // A search read back from disk (it spans several launches).
  culprit_search(std::vector<std::string> suspects,
                 std::vector<std::string> cleared, bool combination,
                 std::size_t launches,
                 db_pool_suspect db_pool = db_pool_suspect::none,
                 std::vector<std::string> always = {}, bool ran_fine = false);

  // Mods for the next test launch, to be asked for as the plan's mods;
  // empty when the search is over.
  std::vector<std::string> next_test() const;
  // The result of the test launch of next_test(). loaded: every mod that
  // launch's plan loaded (the planner adds the dependencies of the mods
  // asked for, and the map's); empty means next_test() alone.
  void record(bool crashed, const std::vector<std::string> &loaded = {});

  bool done() const;
  // The one mod that crashes alone, if that is what the search found. It
  // may be a dependency the player never enabled (culprit_switches).
  std::optional<std::string> culprit() const;
  // No single mod: the crash needs mods from both parts together, or a test
  // with every suspect ran fine (the crash did not come back).
  bool needs_combination() const { return combination_; }
  // No single mod either: the suspects left all load in every test (the
  // map needs them), so no test can tell them apart.
  bool inseparable() const;
  // The next test is the whole crashed stack without the larger zone memory.
  bool testing_db_pool() const { return db_pool_ == db_pool_suspect::untested; }
  // The crashed stack ran fine without the larger zone memory.
  bool db_pool_culprit() const { return db_pool_ == db_pool_suspect::culprit; }
  db_pool_suspect db_pool() const { return db_pool_; }
  std::size_t launches() const { return launches_; }
  const std::vector<std::string> &suspects() const { return suspects_; }
  // Suspects the lower part's test loaded, and it ran fine: the next test
  // is the rest of the suspects. Empty while a split is still to be tested.
  const std::vector<std::string> &cleared() const { return cleared_; }
  const std::vector<std::string> &always() const { return always_; }
  // A test of the mods ran fine. Every test keeps the profile's map, so
  // until one does, the map is as much a suspect as the culprit found.
  bool ran_fine() const { return ran_fine_; }

private:
  // The suspects a test can leave out (not in always_), in stack order.
  std::vector<std::string> apart() const;

  std::vector<std::string> suspects_;
  std::vector<std::string> cleared_;
  std::vector<std::string> always_;
  bool combination_ = false;
  bool ran_fine_ = false;
  std::size_t launches_ = 0;
  db_pool_suspect db_pool_ = db_pool_suspect::none;
};
// A culprit search in progress, kept in <home>\config\culprit.json between
// launches: the mode and stack that crashed, the search, and the test launch
// whose result is still to be recorded (empty when none): the mods it asked
// for and every mod its plan loaded.
struct culprit_progress {
  std::string mode;
  std::vector<std::string> crashed_stack;
  std::vector<std::string> testing;
  std::vector<std::string> loaded;
  culprit_search search{{}};
};

std::string culprit_progress_to_json(const culprit_progress &value);
std::optional<culprit_progress> parse_culprit_progress(std::string_view text);
} // namespace xerces_core
