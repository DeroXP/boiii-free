#include "journal.hpp"

#include "db_pool.hpp"
#include "flip_model.hpp"
#include "frame_pacing.hpp"
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

// The safe-mode notice for a crash in a phase, without experimental settings
// or with two or more (the sentences naming them follow,
// setting_on_sentence); one_setting_message is the notice for one. Whole
// sentences, so that the launcher translates each as one string (Home,
// Servers and Diagnostics show them through app::tr();
// tools/i18n/extract_strings.py reads the sentences from here).
const char *safe_mode_message(launch_phase phase, bool settings) {
  switch (phase) {
  case launch_phase::symbols:
    return settings
               ? "The last launch with mods and experimental settings "
                 "crashed while finding the game's functions. This launch "
                 "starts with mods and those settings off. They may be the "
                 "cause: find the culprit runs the same mods without them "
                 "first. Or retry with mods, report the crash, or turn them "
                 "off in Settings."
               : "The last launch with mods crashed while finding the game's "
                 "functions. This launch starts with mods off. Retry with "
                 "mods, report the crash, or find the mod that causes it.";
  case launch_phase::frontend:
    return settings
               ? "The last launch with mods and experimental settings "
                 "crashed in the menus. This launch starts with mods and "
                 "those settings off. They may be the cause: find the "
                 "culprit runs the same mods without them first. Or retry "
                 "with mods, report the crash, or turn them off in Settings."
               : "The last launch with mods crashed in the menus. This launch "
                 "starts with mods off. Retry with mods, report the crash, or "
                 "find the mod that causes it.";
  case launch_phase::loading_map:
    return settings
               ? "The last launch with mods and experimental settings "
                 "crashed while loading a map. This launch starts with mods "
                 "and those settings off. They may be the cause: find the "
                 "culprit runs the same mods without them first. Or retry "
                 "with mods, report the crash, or turn them off in Settings."
               : "The last launch with mods crashed while loading a map. This "
                 "launch starts with mods off. Retry with mods, report the "
                 "crash, or find the mod that causes it.";
  case launch_phase::in_game:
    return settings
               ? "The last launch with mods and experimental settings "
                 "crashed in a match. This launch starts with mods and those "
                 "settings off. They may be the cause: find the culprit runs "
                 "the same mods without them first. Or retry with mods, "
                 "report the crash, or turn them off in Settings."
               : "The last launch with mods crashed in a match. This launch "
                 "starts with mods off. Retry with mods, report the crash, or "
                 "find the mod that causes it.";
  case launch_phase::launching:
  case launch_phase::clean_exit: // not a crash; never asked for
    break;
  }
  return settings
             ? "The last launch with mods and experimental settings crashed "
               "while starting. This launch starts with mods and those "
               "settings off. They may be the cause: find the culprit runs "
               "the same mods without them first. Or retry with mods, report "
               "the crash, or turn them off in Settings."
             : "The last launch with mods crashed while starting. This launch "
               "starts with mods off. Retry with mods, report the crash, or "
               "find the mod that causes it.";
}

// The safe-mode notice for a crash in a phase with one experimental setting
// on (flag, one of suspect_settings()): it names the setting and where it is
// turned off, so no setting_on_sentence follows. Larger zone memory's are
// the sentences from when it was the only suspect setting. Empty for any
// other flag.
const char *one_setting_message(launch_phase phase, std::string_view flag) {
  const bool pool = flag == db_pool::flag_name;
  const bool limiter = flag == frame_pacing::flag_name;
  const bool flip = flag == flip_model::flag_name;
  switch (phase) {
  case launch_phase::symbols:
    return pool ? "The last launch with mods and larger zone memory "
                  "(experimental) crashed while finding the game's functions. "
                  "This launch starts with mods and larger zone memory off. "
                  "Larger zone memory may be the cause: find the culprit runs "
                  "the same mods without it first. Or retry with mods, report "
                  "the crash, or turn larger zone memory off in Settings > "
                  "Launch."
           : limiter
               ? "The last launch with mods and the precise FPS limit "
                 "(experimental) crashed while finding the game's functions. "
                 "This launch starts with mods and the precise FPS limit off. "
                 "The precise FPS limit may be the cause: find the culprit "
                 "runs the same mods without it first. Or retry with mods, "
                 "report the crash, or turn the precise FPS limit off in "
                 "Settings > Display."
           : flip
               ? "The last launch with mods and flip-model presentation "
                 "(experimental) crashed while finding the game's functions. "
                 "This launch starts with mods and flip-model presentation "
                 "off. Flip-model presentation may be the cause: find the "
                 "culprit runs the same mods without it first. Or retry with "
                 "mods, report the crash, or turn flip-model presentation off "
                 "in Settings > Display."
               : "";
  case launch_phase::frontend:
    return pool ? "The last launch with mods and larger zone memory "
                  "(experimental) crashed in the menus. This launch starts "
                  "with mods and larger zone memory off. Larger zone memory "
                  "may be the cause: find the culprit runs the same mods "
                  "without it first. Or retry with mods, report the crash, or "
                  "turn larger zone memory off in Settings > Launch."
           : limiter
               ? "The last launch with mods and the precise FPS limit "
                 "(experimental) crashed in the menus. This launch starts "
                 "with mods and the precise FPS limit off. The precise FPS "
                 "limit may be the cause: find the culprit runs the same mods "
                 "without it first. Or retry with mods, report the crash, or "
                 "turn the precise FPS limit off in Settings > Display."
           : flip
               ? "The last launch with mods and flip-model presentation "
                 "(experimental) crashed in the menus. This launch starts "
                 "with mods and flip-model presentation off. Flip-model "
                 "presentation may be the cause: find the culprit runs the "
                 "same mods without it first. Or retry with mods, report the "
                 "crash, or turn flip-model presentation off in Settings > "
                 "Display."
               : "";
  case launch_phase::loading_map:
    return pool ? "The last launch with mods and larger zone memory "
                  "(experimental) crashed while loading a map. This launch "
                  "starts with mods and larger zone memory off. Larger zone "
                  "memory may be the cause: find the culprit runs the same "
                  "mods without it first. Or retry with mods, report the "
                  "crash, or turn larger zone memory off in Settings > Launch."
           : limiter
               ? "The last launch with mods and the precise FPS limit "
                 "(experimental) crashed while loading a map. This launch "
                 "starts with mods and the precise FPS limit off. The precise "
                 "FPS limit may be the cause: find the culprit runs the same "
                 "mods without it first. Or retry with mods, report the "
                 "crash, or turn the precise FPS limit off in Settings > "
                 "Display."
           : flip
               ? "The last launch with mods and flip-model presentation "
                 "(experimental) crashed while loading a map. This launch "
                 "starts with mods and flip-model presentation off. "
                 "Flip-model presentation may be the cause: find the culprit "
                 "runs the same mods without it first. Or retry with mods, "
                 "report the crash, or turn flip-model presentation off in "
                 "Settings > Display."
               : "";
  case launch_phase::in_game:
    return pool ? "The last launch with mods and larger zone memory "
                  "(experimental) crashed in a match. This launch starts with "
                  "mods and larger zone memory off. Larger zone memory may be "
                  "the cause: find the culprit runs the same mods without it "
                  "first. Or retry with mods, report the crash, or turn larger "
                  "zone memory off in Settings > Launch."
           : limiter
               ? "The last launch with mods and the precise FPS limit "
                 "(experimental) crashed in a match. This launch starts with "
                 "mods and the precise FPS limit off. The precise FPS limit "
                 "may be the cause: find the culprit runs the same mods "
                 "without it first. Or retry with mods, report the crash, or "
                 "turn the precise FPS limit off in Settings > Display."
           : flip
               ? "The last launch with mods and flip-model presentation "
                 "(experimental) crashed in a match. This launch starts with "
                 "mods and flip-model presentation off. Flip-model "
                 "presentation may be the cause: find the culprit runs the "
                 "same mods without it first. Or retry with mods, report the "
                 "crash, or turn flip-model presentation off in Settings > "
                 "Display."
               : "";
  case launch_phase::launching:
  case launch_phase::clean_exit: // not a crash; never asked for
    break;
  }
  return pool ? "The last launch with mods and larger zone memory "
                "(experimental) crashed while starting. This launch starts "
                "with mods and larger zone memory off. Larger zone memory may "
                "be the cause: find the culprit runs the same mods without it "
                "first. Or retry with mods, report the crash, or turn larger "
                "zone memory off in Settings > Launch."
         : limiter
             ? "The last launch with mods and the precise FPS limit "
               "(experimental) crashed while starting. This launch starts "
               "with mods and the precise FPS limit off. The precise FPS "
               "limit may be the cause: find the culprit runs the same mods "
               "without it first. Or retry with mods, report the crash, or "
               "turn the precise FPS limit off in Settings > Display."
         : flip
             ? "The last launch with mods and flip-model presentation "
               "(experimental) crashed while starting. This launch starts "
               "with mods and flip-model presentation off. Flip-model "
               "presentation may be the cause: find the culprit runs the same "
               "mods without it first. Or retry with mods, report the crash, "
               "or turn flip-model presentation off in Settings > Display."
             : "";
}

// The sentence after safe_mode_message for each setting the crashed launch
// had: which one, and where it is turned off.
const char *setting_on_sentence(std::string_view flag) {
  if (flag == db_pool::flag_name) {
    return "Larger zone memory was on (Settings > Launch).";
  }
  if (flag == frame_pacing::flag_name) {
    return "The precise FPS limit was on (Settings > Display).";
  }
  if (flag == flip_model::flag_name) {
    return "Flip-model presentation was on (Settings > Display).";
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
  json::set(doc, "launch_id", value.launch_id, alloc);
  json::set(doc, "db_pool", value.db_pool, alloc);
  json::set(doc, "precise_limiter", value.precise_limiter, alloc);
  json::set(doc, "flip_model", value.flip_model, alloc);
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
  if (const auto *limiter = json::member(root, "precise_limiter");
      limiter != nullptr && limiter->IsBool()) {
    out.precise_limiter = limiter->GetBool();
  }
  if (const auto *flip = json::member(root, "flip_model");
      flip != nullptr && flip->IsBool()) {
    out.flip_model = flip->GetBool();
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
  out.crashed_with_precise_limiter = last->precise_limiter;
  out.crashed_with_flip_model = last->flip_model;
  // The experimental settings are as much suspects as the mods (5.4, 12.4):
  // with one, the phase's sentence names it; with more, the phase's sentence
  // for settings, then one naming each.
  const std::vector<std::string> settings = crashed_settings(out);
  if (settings.size() == 1) {
    out.message_parts.emplace_back(
        one_setting_message(last->phase, settings.front()));
  } else {
    out.message_parts.emplace_back(
        safe_mode_message(last->phase, !settings.empty()));
    for (const auto &flag : settings) {
      out.message_parts.emplace_back(setting_on_sentence(flag));
    }
  }
  for (const auto &part : out.message_parts) {
    out.message += (out.message.empty() ? "" : " ") + part;
  }
  return out;
}

std::vector<std::string> crashed_settings(const safe_mode_decision &decision) {
  std::vector<std::string> out;
  if (decision.crashed_with_db_pool) {
    out.emplace_back(db_pool::flag_name);
  }
  if (decision.crashed_with_precise_limiter) {
    out.emplace_back(frame_pacing::flag_name);
  }
  if (decision.crashed_with_flip_model) {
    out.emplace_back(flip_model::flag_name);
  }
  return out;
}

const std::vector<std::string> &suspect_settings() {
  static const std::vector<std::string> flags{
      std::string(db_pool::flag_name), std::string(frame_pacing::flag_name),
      std::string(flip_model::flag_name)};
  return flags;
}

const char *setting_name(std::string_view flag) {
  if (flag == db_pool::flag_name) {
    return "larger zone memory";
  }
  if (flag == frame_pacing::flag_name) {
    return "the precise FPS limit";
  }
  if (flag == flip_model::flag_name) {
    return "flip-model presentation";
  }
  return "";
}

bool culprit_search_possible(const safe_mode_decision &decision) {
  // The settings are one suspect: the first test leaves them all out.
  return decision.safe_mode &&
         decision.crashed_stack.size() +
                 (decision.crashed_with_settings() ? 1 : 0) >=
             2;
}

namespace {
bool holds(const std::vector<std::string> &ids, const std::string &id) {
  return std::ranges::find(ids, id) != ids.end();
}
} // namespace

culprit_search::culprit_search(std::vector<std::string> crashed_stack,
                               bool crashed_with_settings,
                               std::vector<std::string> always)
    : suspects_(std::move(crashed_stack)), always_(std::move(always)),
      settings_(crashed_with_settings ? settings_suspect::untested
                                      : settings_suspect::none) {}

culprit_search::culprit_search(std::vector<std::string> suspects,
                               std::vector<std::string> cleared,
                               bool combination, std::size_t launches,
                               settings_suspect settings,
                               std::vector<std::string> always, bool ran_fine)
    : suspects_(std::move(suspects)), always_(std::move(always)),
      combination_(combination), ran_fine_(ran_fine), launches_(launches),
      settings_(settings) {
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
  // Searches saved without "ran_fine": a cleared part ran fine.
  ran_fine_ = ran_fine_ || !cleared_.empty();
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
  if (settings_ == settings_suspect::untested) {
    return suspects_; // the whole crashed stack, without the settings
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
  if (settings_ == settings_suspect::untested) {
    settings_ = crashed ? settings_suspect::cleared : settings_suspect::culprit;
    return;
  }
  const std::vector<std::string> asked = next_test();
  ran_fine_ = ran_fine_ || !crashed;
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
  if (settings_ == settings_suspect::untested) {
    return false;
  }
  if (settings_ == settings_suspect::culprit || combination_ ||
      suspects_.size() <= 1) {
    return true;
  }
  return cleared_.empty() && apart().empty();
}

std::optional<std::string> culprit_search::culprit() const {
  if (done() && settings_ != settings_suspect::culprit && !combination_ &&
      suspects_.size() == 1) {
    return suspects_.front();
  }
  return std::nullopt;
}

bool culprit_search::inseparable() const {
  return done() && settings_ != settings_suspect::culprit && !combination_ &&
         suspects_.size() >= 2;
}

namespace {
constexpr std::pair<settings_suspect, const char *> settings_names[] = {
    {settings_suspect::none, "none"},
    {settings_suspect::untested, "untested"},
    {settings_suspect::cleared, "cleared"},
    {settings_suspect::culprit, "culprit"},
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
  json::set(doc, "settings", json::string_array(value.settings, alloc), alloc);
  json::set(doc, "testing", json::string_array(value.testing, alloc), alloc);
  json::set(doc, "loaded", json::string_array(value.loaded, alloc), alloc);
  json::set(doc, "suspects", json::string_array(value.search.suspects(), alloc),
            alloc);
  json::set(doc, "cleared", json::string_array(value.search.cleared(), alloc),
            alloc);
  json::set(doc, "always", json::string_array(value.search.always(), alloc),
            alloc);
  json::set(doc, "combination", value.search.needs_combination(), alloc);
  json::set(doc, "ran_fine", value.search.ran_fine(), alloc);
  json::set(doc, "launches", static_cast<std::int64_t>(value.search.launches()),
            alloc);
  // "db_pool" is the settings step's name from when larger zone memory was
  // the only setting; searches saved then still read.
  for (const auto &[state, name] : settings_names) {
    if (state == value.search.settings_step()) {
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
  // "settings" is a list of flag names, each one of suspect_settings() once.
  // A development build of flip-model presentation (never released) kept the
  // step there as a string instead; a value of another kind, or a name this
  // version does not know (a newer build's setting, a hand edit), is not a
  // search this version can continue: Home would name and turn off the wrong
  // setting.
  if (const auto *settings = json::member(root, "settings")) {
    if (!settings->IsArray()) {
      return std::nullopt;
    }
    for (const auto &item : settings->GetArray()) {
      if (!item.IsString()) {
        return std::nullopt;
      }
      std::string flag(item.GetString(), item.GetStringLength());
      if (!holds(suspect_settings(), flag) || holds(out.settings, flag)) {
        return std::nullopt;
      }
      out.settings.push_back(std::move(flag));
    }
  }
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
  // The settings step keeps its first name, "db_pool". Searches saved
  // before the larger zone memory was a suspect have no "db_pool"; a value
  // this version does not know ends the search.
  settings_suspect settings = settings_suspect::none;
  if (const auto name = json::get_string(root, "db_pool")) {
    const auto *known =
        std::ranges::find_if(settings_names, [&](const auto &entry) {
          return *name == entry.second;
        });
    if (known == std::ranges::end(settings_names)) {
      return std::nullopt;
    }
    settings = known->first;
  }
  // Saved before "settings" existed (larger zone memory was the only
  // experimental setting then), a settings step was larger zone memory.
  if (out.settings.empty() && settings != settings_suspect::none) {
    out.settings = {std::string(db_pool::flag_name)};
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
  out.search =
      culprit_search(std::move(suspects), std::move(cleared),
                     bool_at(root, "combination"), launches, settings,
                     strings_at(root, "always"), bool_at(root, "ran_fine"));
  return out;
}
} // namespace xerces_core
