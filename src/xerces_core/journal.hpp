#pragma once

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
};

std::string journal_to_json(const launch_journal &value);
std::optional<launch_journal> parse_journal(std::string_view text);

struct safe_mode_decision {
  bool safe_mode = false;
  std::string crashed_plan_id;
  std::vector<std::string> crashed_stack;
  launch_phase crashed_in = launch_phase::launching;
  std::string message;
};

// After any crash with mods enabled, the next launch defaults to safe mode.
safe_mode_decision decide_safe_mode(const std::optional<launch_journal> &last);

// "Find the culprit": halve the crashed stack over a few launches.
class culprit_search {
public:
  explicit culprit_search(std::vector<std::string> crashed_stack);

  // Mods for the next test launch; empty when the search is over.
  std::vector<std::string> next_test() const;
  void record(bool crashed);

  bool done() const;
  // The one mod that crashes alone, if that is what the search found.
  std::optional<std::string> culprit() const;
  // The crash needs mods from both halves together (an interaction).
  bool needs_combination() const { return combination_; }
  std::size_t launches() const { return launches_; }

private:
  std::vector<std::string> suspects_;
  bool testing_second_half_ = false;
  bool combination_ = false;
  std::size_t launches_ = 0;
};
} // namespace xerces_core
