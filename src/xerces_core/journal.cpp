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

// The safe-mode notice for a crash in a phase, with or without the larger
// zone memory. Whole sentences, so that the launcher translates each as one
// string (Home, Servers and Diagnostics show it through app::tr();
// tools/i18n/extract_strings.py reads the sentences from here).
const char *safe_mode_message(launch_phase phase, bool db_pool) {
  switch (phase) {
  case launch_phase::symbols:
    return db_pool
               ? "The last launch with mods and larger zone memory "
                 "(experimental) crashed while finding the game's functions. "
                 "This launch starts with mods and larger zone memory off. "
                 "Larger zone memory may be the cause: find the culprit runs "
                 "the same mods without it first. Or retry with mods, report "
                 "the crash, or turn larger zone memory off in Settings > "
                 "Launch."
               : "The last launch with mods crashed while finding the game's "
                 "functions. This launch starts with mods off. Retry with "
                 "mods, report the crash, or find the mod that causes it.";
  case launch_phase::frontend:
    return db_pool
               ? "The last launch with mods and larger zone memory "
                 "(experimental) crashed in the menus. This launch starts "
                 "with mods and larger zone memory off. Larger zone memory "
                 "may be the cause: find the culprit runs the same mods "
                 "without it first. Or retry with mods, report the crash, or "
                 "turn larger zone memory off in Settings > Launch."
               : "The last launch with mods crashed in the menus. This launch "
                 "starts with mods off. Retry with mods, report the crash, or "
                 "find the mod that causes it.";
  case launch_phase::loading_map:
    return db_pool
               ? "The last launch with mods and larger zone memory "
                 "(experimental) crashed while loading a map. This launch "
                 "starts with mods and larger zone memory off. Larger zone "
                 "memory may be the cause: find the culprit runs the same "
                 "mods without it first. Or retry with mods, report the "
                 "crash, or turn larger zone memory off in Settings > Launch."
               : "The last launch with mods crashed while loading a map. This "
                 "launch starts with mods off. Retry with mods, report the "
                 "crash, or find the mod that causes it.";
  case launch_phase::in_game:
    return db_pool
               ? "The last launch with mods and larger zone memory "
                 "(experimental) crashed in a match. This launch starts with "
                 "mods and larger zone memory off. Larger zone memory may be "
                 "the cause: find the culprit runs the same mods without it "
                 "first. Or retry with mods, report the crash, or turn larger "
                 "zone memory off in Settings > Launch."
               : "The last launch with mods crashed in a match. This launch "
                 "starts with mods off. Retry with mods, report the crash, or "
                 "find the mod that causes it.";
  case launch_phase::launching:
  case launch_phase::clean_exit: // not a crash; never asked for
    break;
  }
  return db_pool
             ? "The last launch with mods and larger zone memory "
               "(experimental) crashed while starting. This launch starts "
               "with mods and larger zone memory off. Larger zone memory may "
               "be the cause: find the culprit runs the same mods without it "
               "first. Or retry with mods, report the crash, or turn larger "
               "zone memory off in Settings > Launch."
             : "The last launch with mods crashed while starting. This launch "
               "starts with mods off. Retry with mods, report the crash, or "
               "find the mod that causes it.";
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
  out.safe_mode = true;
  out.crashed_plan_id = last->plan_id;
  out.crashed_stack = last->stack;
  out.crashed_in = last->phase;
  out.crashed_mode = last->mode;
  out.crashed_started = last->started;
  out.crashed_with_db_pool = last->db_pool;
  // The experimental setting is as much a suspect as the mods (5.4).
  out.message = safe_mode_message(last->phase, last->db_pool);
  return out;
}

bool culprit_search_possible(const safe_mode_decision &decision) {
  return decision.safe_mode &&
         decision.crashed_stack.size() +
                 (decision.crashed_with_db_pool ? 1 : 0) >=
             2;
}

namespace {
bool holds(const std::vector<std::string> &ids, const std::string &id) {
  return std::ranges::find(ids, id) != ids.end();
}
} // namespace

culprit_search::culprit_search(std::vector<std::string> crashed_stack,
                               bool crashed_with_db_pool,
                               std::vector<std::string> always)
    : suspects_(std::move(crashed_stack)), always_(std::move(always)),
      db_pool_(crashed_with_db_pool ? db_pool_suspect::untested
                                    : db_pool_suspect::none) {}

culprit_search::culprit_search(std::vector<std::string> suspects,
                               std::vector<std::string> cleared,
                               bool combination, std::size_t launches,
                               db_pool_suspect db_pool,
                               std::vector<std::string> always)
    : suspects_(std::move(suspects)), always_(std::move(always)),
      combination_(combination), launches_(launches), db_pool_(db_pool) {
  // Only suspects can be cleared, and never all of them (that ends the
  // search as a combination instead).
  for (const auto &id : cleared) {
    if (holds(suspects_, id) && !holds(cleared_, id)) {
      cleared_.push_back(id);
    }
  }
  if (cleared_.size() >= suspects_.size()) {
    cleared_.clear();
  }
}

std::vector<std::string> culprit_search::apart() const {
  std::vector<std::string> out;
  for (const auto &id : suspects_) {
    if (!holds(always_, id)) {
      out.push_back(id);
    }
  }
  return out;
}

std::vector<std::string> culprit_search::next_test() const {
  if (done()) {
    return {};
  }
  if (db_pool_ == db_pool_suspect::untested) {
    return suspects_; // the whole crashed stack, without the setting
  }
  if (!cleared_.empty()) {
    // The lower part ran fine: the rest, as a whole.
    std::vector<std::string> rest;
    for (const auto &id : suspects_) {
      if (!holds(cleared_, id)) {
        rest.push_back(id);
      }
    }
    return rest;
  }
  const std::vector<std::string> split = apart();
  if (split.size() >= 2) {
    // The lower part; the upper part is the first size/2.
    return {split.begin() + static_cast<std::ptrdiff_t>(split.size() / 2),
            split.end()};
  }
  // One suspect besides the map's dependencies: those alone, with the map.
  std::vector<std::string> with_map;
  for (const auto &id : suspects_) {
    if (holds(always_, id)) {
      with_map.push_back(id);
    }
  }
  return with_map;
}

void culprit_search::record(bool crashed,
                            const std::vector<std::string> &loaded) {
  if (done()) {
    return;
  }
  ++launches_;
  if (db_pool_ == db_pool_suspect::untested) {
    db_pool_ = crashed ? db_pool_suspect::cleared : db_pool_suspect::culprit;
    return;
  }
  const std::vector<std::string> asked = next_test();
  // The suspects the test loaded, in stack order.
  const std::vector<std::string> &test = loaded.empty() ? asked : loaded;
  std::vector<std::string> ran;
  for (const auto &id : suspects_) {
    if (holds(test, id)) {
      ran.push_back(id);
    }
  }
  if (!cleared_.empty()) {
    // The rest, after a lower part that ran fine.
    if (!crashed) {
      combination_ = true; // neither part crashes without the other
      return;
    }
    std::vector<std::string> narrowed;
    for (const auto &id : ran) {
      if (!holds(cleared_, id)) {
        narrowed.push_back(id);
      }
    }
    if (!narrowed.empty()) {
      suspects_ = std::move(narrowed);
    }
    cleared_.clear();
    return;
  }
  if (!crashed) {
    if (ran.size() >= suspects_.size()) {
      combination_ = true; // it loaded them all and ran fine
    } else {
      cleared_ = std::move(ran);
    }
    return;
  }
  if (ran.size() < suspects_.size()) {
    suspects_ = std::move(ran);
    return;
  }
  // It loaded every suspect although it asked for fewer: the ones it did not
  // ask for come with every test (the map needs them).
  bool learned = false;
  for (const auto &id : suspects_) {
    if (!holds(asked, id) && !holds(always_, id)) {
      always_.push_back(id);
      learned = true;
    }
  }
  if (!learned) {
    combination_ = true; // nothing left that a test could leave out
  }
}

bool culprit_search::done() const {
  if (db_pool_ == db_pool_suspect::untested) {
    return false;
  }
  if (db_pool_ == db_pool_suspect::culprit || combination_ ||
      suspects_.size() <= 1) {
    return true;
  }
  return cleared_.empty() && apart().empty();
}

std::optional<std::string> culprit_search::culprit() const {
  if (done() && db_pool_ != db_pool_suspect::culprit && !combination_ &&
      suspects_.size() == 1) {
    return suspects_.front();
  }
  return std::nullopt;
}

bool culprit_search::inseparable() const {
  return done() && db_pool_ != db_pool_suspect::culprit && !combination_ &&
         suspects_.size() >= 2;
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
  json::set(doc, "schema", std::int64_t{2}, alloc);
  json::set(doc, "mode", value.mode, alloc);
  json::set(doc, "crashed_stack",
            json::string_array(value.crashed_stack, alloc), alloc);
  json::set(doc, "testing", json::string_array(value.testing, alloc), alloc);
  json::set(doc, "loaded", json::string_array(value.loaded, alloc), alloc);
  json::set(doc, "suspects", json::string_array(value.search.suspects(), alloc),
            alloc);
  json::set(doc, "cleared", json::string_array(value.search.cleared(), alloc),
            alloc);
  json::set(doc, "always", json::string_array(value.search.always(), alloc),
            alloc);
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
  out.loaded = strings_at(root, "loaded");
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
  std::vector<std::string> cleared = strings_at(root, "cleared");
  std::int64_t schema = 1;
  if (const auto *number = json::member(root, "schema");
      number != nullptr && number->IsInt64()) {
    schema = number->GetInt64();
  }
  if (schema < 2 && bool_at(root, "second_half")) {
    // Saved by the search that tested the upper half first: "second_half"
    // meant that half had run fine.
    cleared.assign(suspects.begin(),
                   suspects.begin() +
                       static_cast<std::ptrdiff_t>(suspects.size() / 2));
  }
  out.search = culprit_search(std::move(suspects), std::move(cleared),
                              bool_at(root, "combination"), launches, db_pool,
                              strings_at(root, "always"));
  return out;
}
} // namespace xerces_core
