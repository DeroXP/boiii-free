#include "journal.hpp"

#include "json.hpp"

#include <algorithm>
#include <cstdio>
#include <random>

namespace xerces_core {
namespace {
constexpr std::pair<launch_phase, const char *> phase_names[] = {
    {launch_phase::launching, "launching"},
    {launch_phase::symbols, "symbols"},
    {launch_phase::frontend, "frontend"},
    {launch_phase::loading_map, "loading_map"},
    {launch_phase::in_game, "in_game"},
    {launch_phase::clean_exit, "clean_exit"},
};

std::string describe(launch_phase phase) {
  switch (phase) {
  case launch_phase::launching:
    return "while starting";
  case launch_phase::symbols:
    return "while finding the game's functions";
  case launch_phase::frontend:
    return "in the menus";
  case launch_phase::loading_map:
    return "while loading a map";
  case launch_phase::in_game:
    return "in a match";
  case launch_phase::clean_exit:
    return "after exiting";
  }
  return "";
}
} // namespace

const char *to_string(launch_phase phase) {
  for (const auto &[value, name] : phase_names) {
    if (value == phase) {
      return name;
    }
  }
  return "launching";
}

std::optional<launch_phase> parse_launch_phase(std::string_view text) {
  for (const auto &[value, name] : phase_names) {
    if (text == name) {
      return value;
    }
  }
  return std::nullopt;
}

std::string journal_to_json(const launch_journal &value) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "schema", std::int64_t{1}, alloc);
  json::set(doc, "plan_id", value.plan_id, alloc);
  json::set(doc, "stack", json::string_array(value.stack, alloc), alloc);
  json::set(doc, "phase", to_string(value.phase), alloc);
  json::set(doc, "started", value.started, alloc);
  json::set(doc, "updated", value.updated, alloc);
  json::set(doc, "safe_mode", value.safe_mode, alloc);
  json::set(doc, "mode", value.mode, alloc);
  if (value.joined) {
    json::set(doc, "joined", true, alloc);
    json::set(doc, "joined_stack",
              json::string_array(value.joined_stack, alloc), alloc);
  }
  json::set(doc, "launch_id", value.launch_id, alloc);
  json::set(doc, "db_pool", value.db_pool, alloc);
  return json::canonical(doc);
}

std::optional<launch_journal> parse_journal(std::string_view text) {
  const auto parsed = json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  const auto &root = parsed.document;
  launch_journal out;
  out.plan_id = json::get_string(root, "plan_id").value_or("");
  const auto phase =
      parse_launch_phase(json::get_string(root, "phase").value_or(""));
  if (!phase) {
    return std::nullopt;
  }
  out.phase = *phase;
  out.started = json::get_string(root, "started").value_or("");
  out.updated = json::get_string(root, "updated").value_or("");
  if (const auto *stack = json::member(root, "stack");
      stack != nullptr && stack->IsArray()) {
    for (const auto &item : stack->GetArray()) {
      if (item.IsString()) {
        out.stack.emplace_back(item.GetString(), item.GetStringLength());
      }
    }
  }
  if (const auto *safe = json::member(root, "safe_mode");
      safe != nullptr && safe->IsBool()) {
    out.safe_mode = safe->GetBool();
  }
  out.mode = json::get_string(root, "mode").value_or("");
  if (const auto *joined = json::member(root, "joined");
      joined != nullptr && joined->IsBool()) {
    out.joined = joined->GetBool();
  }
  if (const auto *stack = json::member(root, "joined_stack");
      out.joined && stack != nullptr && stack->IsArray()) {
    for (const auto &item : stack->GetArray()) {
      if (item.IsString()) {
        out.joined_stack.emplace_back(item.GetString(), item.GetStringLength());
      }
    }
  }
  out.launch_id = json::get_string(root, "launch_id").value_or("");
  if (const auto *db_pool = json::member(root, "db_pool");
      db_pool != nullptr && db_pool->IsBool()) {
    out.db_pool = db_pool->GetBool();
  }
  return out;
}

std::string new_launch_id() {
  // It only has to differ from every other launch's id; the standard
  // library's random_device reads the system's random source on Windows.
  std::random_device source;
  std::string out;
  for (int i = 0; i < 4; ++i) {
    char part[9];
    std::snprintf(part, sizeof(part), "%08x", static_cast<unsigned>(source()));
    out += part;
  }
  return out;
}

bool journal_claimable(const std::optional<launch_journal> &journal,
                       std::string_view launch_id) {
  return journal && !launch_id.empty() && journal->launch_id == launch_id &&
         journal->phase == launch_phase::launching;
}

bool exit_counts_as_clean(const launch_journal &journal,
                          std::uint32_t exit_code) {
  return exit_code == 0 && journal.phase == launch_phase::launching &&
         !journal.launch_id.empty();
}

bool game_exit_is_clean(bool quit_requested,
                        std::optional<std::uint32_t> exit_code) {
  return quit_requested || !exit_code || *exit_code == 0;
}

bool journal_crashed(const std::optional<launch_journal> &journal) {
  return !journal || journal->phase != launch_phase::clean_exit;
}

safe_mode_decision decide_safe_mode(const std::optional<launch_journal> &last) {
  safe_mode_decision out;
  if (!last || last->phase == launch_phase::clean_exit || last->stack.empty() ||
      last->safe_mode) {
    return out; // no crash, or a crash with mods already off
  }
  if (last->joined && last->joined_stack != last->stack) {
    // It crashed on another player's game, with the host's mods: this
    // plan's mods were not running, so they stay on.
    out.crashed_after_join = true;
    return out;
  }
  out.safe_mode = true;
  out.crashed_plan_id = last->plan_id;
  out.crashed_stack = last->stack;
  out.crashed_in = last->phase;
  out.crashed_mode = last->mode;
  out.crashed_started = last->started;
  out.crashed_with_db_pool = last->db_pool;
  const std::string mods = std::to_string(last->stack.size()) +
                           (last->stack.size() == 1 ? " mod" : " mods");
  if (last->db_pool) {
    // The experimental setting is as much a suspect as the mods (5.4).
    out.message = "The last launch with " + mods +
                  " and larger zone memory (experimental) crashed " +
                  describe(last->phase) +
                  ". This launch starts with mods and larger zone memory "
                  "off. Larger zone memory may be the cause: find the "
                  "culprit runs the same mods without it first. Or retry "
                  "with mods, report the crash, or turn larger zone memory "
                  "off in Settings > Launch.";
    return out;
  }
  out.message = "The last launch with " + mods + " crashed " +
                describe(last->phase) +
                ". This launch starts with mods off. Retry with mods, report "
                "the crash, or find the mod that causes it.";
  return out;
}

bool culprit_search_possible(const safe_mode_decision &decision) {
  return decision.safe_mode &&
         decision.crashed_stack.size() +
                 (decision.crashed_with_db_pool ? 1 : 0) >=
             2;
}

culprit_search::culprit_search(std::vector<std::string> crashed_stack,
                               bool crashed_with_db_pool)
    : suspects_(std::move(crashed_stack)),
      db_pool_(crashed_with_db_pool ? db_pool_suspect::untested
                                    : db_pool_suspect::none) {}

culprit_search::culprit_search(std::vector<std::string> suspects,
                               bool testing_second_half, bool combination,
                               std::size_t launches, db_pool_suspect db_pool)
    : suspects_(std::move(suspects)), testing_second_half_(testing_second_half),
      combination_(combination), launches_(launches), db_pool_(db_pool) {}

std::vector<std::string> culprit_search::next_test() const {
  if (done()) {
    return {};
  }
  if (db_pool_ == db_pool_suspect::untested) {
    return suspects_; // the whole crashed stack, without the setting
  }
  const std::size_t half = suspects_.size() / 2;
  return testing_second_half_
             ? std::vector<std::string>(suspects_.begin() + half,
                                        suspects_.end())
             : std::vector<std::string>(suspects_.begin(),
                                        suspects_.begin() + half);
}

void culprit_search::record(bool crashed) {
  if (done()) {
    return;
  }
  ++launches_;
  if (db_pool_ == db_pool_suspect::untested) {
    db_pool_ = crashed ? db_pool_suspect::cleared : db_pool_suspect::culprit;
    return;
  }
  const std::vector<std::string> tested = next_test();
  if (crashed) {
    suspects_ = tested;
    testing_second_half_ = false;
    return;
  }
  if (!testing_second_half_) {
    testing_second_half_ = true; // the first half is fine; try the second
    return;
  }
  combination_ = true; // neither half crashes alone
}

bool culprit_search::done() const {
  if (db_pool_ == db_pool_suspect::untested) {
    return false;
  }
  return db_pool_ == db_pool_suspect::culprit || combination_ ||
         suspects_.size() <= 1;
}

std::optional<std::string> culprit_search::culprit() const {
  if (done() && db_pool_ != db_pool_suspect::culprit && !combination_ &&
      suspects_.size() == 1) {
    return suspects_.front();
  }
  return std::nullopt;
}
namespace {
constexpr std::pair<db_pool_suspect, const char *> db_pool_names[] = {
    {db_pool_suspect::none, "none"},
    {db_pool_suspect::untested, "untested"},
    {db_pool_suspect::cleared, "cleared"},
    {db_pool_suspect::culprit, "culprit"},
};

std::vector<std::string> strings_at(const rapidjson::Value &root,
                                    std::string_view key) {
  std::vector<std::string> out;
  if (const auto *array = json::member(root, key);
      array != nullptr && array->IsArray()) {
    for (const auto &item : array->GetArray()) {
      if (item.IsString()) {
        out.emplace_back(item.GetString(), item.GetStringLength());
      }
    }
  }
  return out;
}

bool bool_at(const rapidjson::Value &root, std::string_view key) {
  const auto *value = json::member(root, key);
  return value != nullptr && value->IsBool() && value->GetBool();
}
} // namespace

std::string culprit_progress_to_json(const culprit_progress &value) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "schema", std::int64_t{1}, alloc);
  json::set(doc, "mode", value.mode, alloc);
  json::set(doc, "crashed_stack",
            json::string_array(value.crashed_stack, alloc), alloc);
  json::set(doc, "testing", json::string_array(value.testing, alloc), alloc);
  json::set(doc, "suspects", json::string_array(value.search.suspects(), alloc),
            alloc);
  json::set(doc, "second_half", value.search.testing_second_half(), alloc);
  json::set(doc, "combination", value.search.needs_combination(), alloc);
  json::set(doc, "launches", static_cast<std::int64_t>(value.search.launches()),
            alloc);
  for (const auto &[state, name] : db_pool_names) {
    if (state == value.search.db_pool()) {
      json::set(doc, "db_pool", name, alloc);
    }
  }
  return json::canonical(doc);
}

std::optional<culprit_progress> parse_culprit_progress(std::string_view text) {
  const auto parsed = json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  const auto &root = parsed.document;
  culprit_progress out;
  out.mode = json::get_string(root, "mode").value_or("");
  out.crashed_stack = strings_at(root, "crashed_stack");
  out.testing = strings_at(root, "testing");
  std::size_t launches = 0;
  if (const auto *count = json::member(root, "launches");
      count != nullptr && count->IsUint64()) {
    launches = static_cast<std::size_t>(count->GetUint64());
  }
  std::vector<std::string> suspects = strings_at(root, "suspects");
  if (out.mode.empty() || suspects.empty()) {
    return std::nullopt;
  }
  // Searches saved before the larger zone memory was a suspect have no
  // "db_pool"; a value this version does not know ends the search.
  db_pool_suspect db_pool = db_pool_suspect::none;
  if (const auto name = json::get_string(root, "db_pool")) {
    const auto *known =
        std::ranges::find_if(db_pool_names, [&](const auto &entry) {
          return *name == entry.second;
        });
    if (known == std::ranges::end(db_pool_names)) {
      return std::nullopt;
    }
    db_pool = known->first;
  }
  out.search = culprit_search(std::move(suspects), bool_at(root, "second_half"),
                              bool_at(root, "combination"), launches, db_pool);
  return out;
}
} // namespace xerces_core
