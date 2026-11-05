// The commands that work on <home> as Xerces.exe does (DESIGN.md 15.2): mods
// info and hand edits of the load order, plan apply (Home's Launch), packs
// apply (the Packs screen's Apply), logs tail and the game's signature
// report. The plan, the launch and the pack go through the same xerces_core
// code as the launcher (launcher_launch, pack_apply), so the two cannot
// disagree.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <xerces_core/journal.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/launcher_config.hpp>
#include <xerces_core/launcher_home.hpp>
#include <xerces_core/launcher_launch.hpp>
#include <xerces_core/launcher_state.hpp>
#include <xerces_core/log.hpp>
#include <xerces_core/log_tail.hpp>
#include <xerces_core/manifest.hpp>
#include <xerces_core/mods_edit.hpp>
#include <xerces_core/mods_info.hpp>
#include <xerces_core/pack.hpp>
#include <xerces_core/pack_apply.hpp>
#include <xerces_core/packs_store.hpp>

#include "cli.hpp"

namespace xerces_cli {
namespace {
using namespace xerces_core;

// ------------------------------------------------------------------- log
// The CLI's log (role cli, events "cli.<event>"). One file per day, appended
// to by every run, so scripts that call xerces_cli often do not push the
// launcher's and the game's sessions out of the last 20.
class cli_log {
public:
  explicit cli_log(fs::path logs)
      : logs_(std::move(logs)),
        log_("cli", std::to_string(GetCurrentProcessId())) {
    wchar_t profile[MAX_PATH]{};
    const DWORD size =
        GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (size > 0 && size < MAX_PATH) {
      log_.set_profile_path(path_utf8(fs::path(std::wstring(profile, size))));
    }
  }

  void write(log_level level, std::string_view event, std::string_view message,
             std::string_view data_json) {
    log_event item;
    item.level = level;
    item.component = "cli";
    item.event = "cli." + std::string(event);
    item.message = std::string(message);
    item.data_json = std::string(data_json);
    if (!chosen_) {
      chosen_ = true;
      std::string today = json_log::now_utc().substr(0, 10); // 2026-11-05
      std::erase(today, '-');
      append_to_ = todays_cli_log(logs_, today);
      if (!append_to_ && !log_.open(logs_)) {
        return;
      }
    }
    if (append_to_) {
      std::ofstream out(*append_to_, std::ios::app | std::ios::binary);
      out << log_.format(item, json_log::now_utc()) << '\n';
    } else {
      log_.write(item);
    }
  }

  event_log events() {
    return [this](log_level level, std::string_view event,
                  std::string_view message, std::string_view data_json) {
      write(level, event, message, data_json);
    };
  }

private:
  fs::path logs_;
  json_log log_;
  bool chosen_ = false;
  std::optional<fs::path> append_to_;
};

// ------------------------------------------------------------------ home
std::optional<fs::path> home_override(const arguments &args) {
  if (const auto given = args.get("--home")) {
    return fs::path(*given);
  }
  return std::nullopt;
}

struct home_view {
  launcher_paths paths;
  launcher_config config;
  bool config_found = false;
  std::string config_error; // launcher.json did not parse
  game_state game;
  catalog_state catalog;
};

// launcher.json as it is on disk now.
void read_config(home_view &out) {
  out.config = launcher_config{};
  out.config_found = false;
  out.config_error.clear();
  if (const auto text = read_text_file(out.paths.config_file())) {
    out.config_found = true;
    auto parsed = parse_launcher_config(*text);
    if (!parsed.ok()) {
      out.config_error = parsed.diags.items().empty()
                             ? "launcher.json could not be read"
                             : parsed.diags.items().front().message;
    }
    out.config = std::move(parsed.value);
  }
}

// <home> as Xerces.exe reads it: launcher.json, then (when asked) the game,
// its pre-flight check and the catalog with the hash cache.
home_view open_home(const arguments &args, bool detect, bool preflight,
                    bool scan) {
  home_view out;
  out.paths = find_paths(home_override(args));
  read_config(out);
  if (detect || scan) {
    out.game =
        detect_game_state(out.config.game_dir, out.paths.compat, preflight);
  }
  if (scan) {
    const fs::path cache = out.paths.cache / "hashes.txt";
    std::error_code ec;
    if (out.game.dir && !fs::exists(cache, ec)) {
      std::cerr << "There is no hash cache in " << utf8(out.paths.cache)
                << " yet: this first scan reads every file of the Workshop "
                   "folder (minutes for a large one).\n";
    }
    out.catalog = scan_catalog(out.game, cache);
  }
  return out;
}

std::string mode_of(const arguments &args, const launcher_config &config) {
  const std::string mode = args.get("--mode").value_or(config.mode);
  if (!valid_mode(mode)) {
    throw std::invalid_argument("Modes are zm, mp and cp, not \"" + mode +
                                "\".");
  }
  return mode;
}

const char *mode_name(const std::string &mode) {
  return mode == "mp" ? "Multiplayer" : mode == "cp" ? "Campaign" : "Zombies";
}

std::string two_digits(std::size_t value) {
  return (value < 10 ? "0" : "") + std::to_string(value);
}

void say_launcher_runs(const fs::path &home) {
  std::cerr << "Xerces.exe is open on " << utf8(home)
            << " (or another xerces_cli is changing its config). Xerces.exe "
               "keeps the config in memory and would overwrite this change "
               "the next time it saves. Close it first, or make the change "
               "in Xerces.exe.\n";
}

// Xerces.exe keeps the config in memory and writes all of it when it saves,
// so a change made here while it runs would be lost. A quick look before any
// slow work; the change itself is made under launcher_home_lock.
bool refuse_while_launcher_runs(const arguments &args) {
  const fs::path home = find_home(home_override(args));
  if (!launcher_running(home)) {
    return false;
  }
  say_launcher_runs(home);
  return true;
}

bool refuse_broken_config(const home_view &home) {
  if (home.config_error.empty()) {
    return false;
  }
  std::cerr << utf8(home.paths.config_file())
            << " cannot be read: " << home.config_error
            << ". Xerces.exe keeps it as launcher.json.broken and starts from "
               "defaults; fix or remove it before changing it here.\n";
  return true;
}

bool save_config(const home_view &home) {
  if (write_text_file(home.paths.config_file(),
                      launcher_config_to_json(home.config))) {
    return true;
  }
  std::cerr << "Could not write " << utf8(home.paths.config_file()) << ".\n";
  return false;
}

const mode_profile *profile_of(const launcher_config &config,
                               const std::string &mode) {
  const auto it = config.profiles.find(mode);
  return it == config.profiles.end() ? nullptr : &it->second;
}

// An id as given, or "ws.<n>" for a bare Workshop number that is installed or
// enabled under that id.
std::string resolve_id(const std::string &given, const catalog_state &catalog,
                       const mode_profile *profile) {
  const auto known = [&](const std::string &id) {
    return catalog.find(id) != nullptr ||
           (profile &&
            (std::ranges::find(profile->mods, id) != profile->mods.end() ||
             (profile->map && *profile->map == id)));
  };
  if (known(given)) {
    return given;
  }
  const bool digits = !given.empty() && std::ranges::all_of(given, [](char c) {
    return c >= '0' && c <= '9';
  });
  if (digits && known("ws." + given)) {
    return "ws." + given;
  }
  return given;
}

std::string name_or_id(const catalog_state &catalog, const std::string &id) {
  const installed_content *item = catalog.find(id);
  return item && !item->info.name.empty() ? item->info.name : id;
}

void print_problems(const plan &resolved) {
  if (resolved.diags.items().empty()) {
    return;
  }
  std::cout << "The " << mode_name(resolved.mode) << " plan now:\n";
  print_diagnostics(resolved.diags);
}

std::size_t parse_count(const std::string &option, const std::string &text) {
  std::size_t value = 0;
  const char *end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end) {
    throw std::invalid_argument(option + " takes a number, not \"" + text +
                                "\".");
  }
  return value;
}
} // namespace

// ------------------------------------------------------------- mods list
int cmd_mods_home_list(const arguments &args) {
  if (!accept_only(args, 2, {"--home", "--mode"}, {"--json"})) {
    return exit_error;
  }
  const home_view home = open_home(args, true, false, true);
  const std::string mode = mode_of(args, home.config);
  const mode_profile *profile = profile_of(home.config, mode);
  if (!home.game.dir) {
    std::cerr << "Black Ops III was not found, so nothing is installed as far "
                 "as Xerces knows. Give --mods-dir to list a folder.\n";
  }
  const auto place = [&](const std::string &id) -> std::size_t {
    if (!profile) {
      return 0;
    }
    const auto it = std::ranges::find(profile->mods, id);
    return it == profile->mods.end()
               ? 0
               : std::size_t(it - profile->mods.begin()) + 1;
  };
  if (args.has("--json")) {
    rapidjson::Document doc(rapidjson::kArrayType);
    auto &alloc = doc.GetAllocator();
    for (std::size_t i = 0; i < home.catalog.items.size(); ++i) {
      const auto &item = home.catalog.items[i];
      rapidjson::Value object(rapidjson::kObjectType);
      json::set(object, "id", item.info.id, alloc);
      json::set(object, "name", item.info.name, alloc);
      json::set(object, "version", item.info.ver.to_string(), alloc);
      json::set(object, "kind", to_string(item.info.kind), alloc);
      json::set(object, "source", home.catalog.sources[i], alloc);
      json::set(object, "folder", item.folder, alloc);
      json::set(object, "legacy", item.info.legacy, alloc);
      json::set(object, "content_hash", item.content_hash, alloc);
      json::set(object, "supports_mode", item.info.supports_mode(mode), alloc);
      json::set(object, "position",
                static_cast<std::int64_t>(place(item.info.id)), alloc);
      json::set(object, "map",
                profile && profile->map && *profile->map == item.info.id,
                alloc);
      doc.PushBack(std::move(object), alloc);
    }
    std::cout << json::pretty(doc) << "\n";
    return exit_ok;
  }
  std::cout << mode_name(mode) << ": " << (profile ? profile->mods.size() : 0)
            << " enabled";
  if (profile && !profile->pack_name.empty()) {
    std::cout << " (pack \"" << profile->pack_name << "\")";
  }
  std::cout << ", " << home.catalog.items.size() << " installed\n";
  for (std::size_t i = 0; i < home.catalog.items.size(); ++i) {
    const auto &item = home.catalog.items[i];
    const std::size_t at = place(item.info.id);
    const bool map = item.info.kind == content_kind::map;
    const bool picked =
        profile && profile->map && *profile->map == item.info.id;
    std::cout << "  "
              << (at       ? two_digits(at)
                  : picked ? std::string("map")
                           : std::string("  -"))
              << (at ? " " : "") << "  " << item.info.id << "  "
              << (item.info.legacy ? std::string("legacy")
                                   : item.info.ver.to_string())
              << "  "
              << (item.info.name.empty() ? item.info.id : item.info.name)
              << "  (" << home.catalog.sources[i] << (map ? ", map" : "")
              << (!map && !item.info.supports_mode(mode) ? ", not for this mode"
                                                         : "")
              << ")\n";
  }
  if (profile) {
    for (const auto &id : profile->mods) {
      if (!home.catalog.find(id)) {
        std::cout << "  " << two_digits(place(id)) << "   " << id
                  << "  (enabled, not installed)\n";
      }
    }
  }
  return exit_ok;
}

// ------------------------------------------------------------- mods info
int cmd_mods_info(const arguments &args) {
  if (!accept_only(args, 3, {"--home", "--mode"}, {"--json"})) {
    return exit_error;
  }
  const std::string given = args.word(2);
  if (given.empty()) {
    std::cerr << "mods info needs an id (as mods list shows it, or a "
                 "Workshop number).\n";
    return exit_error;
  }
  const home_view home = open_home(args, true, false, true);
  const std::string mode = mode_of(args, home.config);
  const mode_profile *profile = profile_of(home.config, mode);
  const plan resolved = resolve_profile(home.catalog.items, home.config, mode,
                                        home.game.steam_build);
  const std::string id = resolve_id(given, home.catalog, profile);
  const auto info = describe_mod(home.catalog, profile, resolved, mode, id);
  if (!info) {
    std::cerr << "\"" << given << "\" is not installed"
              << (home.game.dir ? "" : " (Black Ops III was not found)")
              << " and not enabled for " << mode_name(mode) << ".\n";
    return exit_error;
  }
  if (args.has("--json")) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto &alloc = doc.GetAllocator();
    json::set(doc, "id", info->id, alloc);
    json::set(doc, "mode", mode, alloc);
    json::set(doc, "installed", info->installed, alloc);
    json::set(doc, "kind", info->is_map ? "map" : "mod", alloc);
    json::set(doc, "name", info->name, alloc);
    json::set(doc, "version", info->version, alloc);
    json::set(doc, "legacy", info->legacy, alloc);
    json::set(doc, "source", info->source, alloc);
    json::set(doc, "folder", info->folder, alloc);
    json::set(doc, "folder_name", info->folder_name, alloc);
    json::set(doc, "workshop_id", info->workshop_id, alloc);
    json::set(doc, "files", static_cast<std::int64_t>(info->files), alloc);
    json::set(doc, "zones", static_cast<std::int64_t>(info->zones), alloc);
    json::set(doc, "content_hash", info->content_hash, alloc);
    json::set(doc, "modes", json::string_array(info->modes, alloc), alloc);
    json::set(doc, "supports_mode", info->supports_mode, alloc);
    json::set(doc, "position", static_cast<std::int64_t>(info->position),
              alloc);
    json::set(doc, "enabled", static_cast<std::int64_t>(info->enabled), alloc);
    json::set(doc, "loads_at", static_cast<std::int64_t>(info->loads_at),
              alloc);
    json::set(doc, "dependency", info->dependency, alloc);
    json::set(doc, "map_picked", info->map_picked, alloc);
    json::set(doc, "pinned", info->pinned, alloc);
    json::set(doc, "authors", json::string_array(info->authors, alloc), alloc);
    json::set(doc, "license", info->license, alloc);
    rapidjson::Value touches(rapidjson::kObjectType);
    json::set(touches, "provides", static_cast<std::int64_t>(info->provides),
              alloc);
    json::set(touches, "overrides", static_cast<std::int64_t>(info->overrides),
              alloc);
    json::set(touches, "extends", static_cast<std::int64_t>(info->extends),
              alloc);
    json::set(touches, "hooks", static_cast<std::int64_t>(info->hooks), alloc);
    json::set(doc, "touches", std::move(touches), alloc);
    json::set(doc, "needs", json::string_array(info->needs, alloc), alloc);
    json::set(doc, "conflicts", json::string_array(info->conflicts, alloc),
              alloc);
    rapidjson::Value caps(rapidjson::kArrayType);
    for (const auto &cap : info->capabilities) {
      rapidjson::Value item(rapidjson::kObjectType);
      json::set(item, "label", cap.label, alloc);
      json::set(item, "detail", cap.detail, alloc);
      caps.PushBack(std::move(item), alloc);
    }
    json::set(doc, "capabilities", std::move(caps), alloc);
    rapidjson::Value settings(rapidjson::kArrayType);
    for (const auto &setting : info->settings) {
      rapidjson::Value item(rapidjson::kObjectType);
      json::set(item, "key", setting.key, alloc);
      json::set(item, "label", setting.label, alloc);
      json::set(item, "type", setting.type, alloc);
      json::set(item, "default", setting.default_json, alloc);
      json::set(item, "dvar", setting.dvar, alloc);
      settings.PushBack(std::move(item), alloc);
    }
    json::set(doc, "settings", std::move(settings), alloc);
    rapidjson::Value problems(rapidjson::kArrayType);
    for (const auto &note : info->problems) {
      rapidjson::Value item(rapidjson::kObjectType);
      json::set(item, "level", to_string(note.level), alloc);
      json::set(item, "code", note.code, alloc);
      json::set(item, "message", note.message, alloc);
      problems.PushBack(std::move(item), alloc);
    }
    json::set(doc, "problems", std::move(problems), alloc);
    rapidjson::Value fixes(rapidjson::kArrayType);
    for (const auto &fix : info->fixes) {
      rapidjson::Value item(rapidjson::kObjectType);
      json::set(item, "kind", fix.kind, alloc);
      json::set(item, "id", fix.id, alloc);
      json::set(item, "detail", fix.detail, alloc);
      fixes.PushBack(std::move(item), alloc);
    }
    json::set(doc, "fixes", std::move(fixes), alloc);
    json::set(doc, "why", json::string_array(info->why, alloc), alloc);
    std::cout << json::pretty(doc) << "\n";
    return exit_ok;
  }

  const auto field = [](const char *label, const std::string &value) {
    if (!value.empty()) {
      std::string padded = label;
      padded.resize(std::max<std::size_t>(padded.size(), 15), ' ');
      std::cout << "  " << padded << value << "\n";
    }
  };
  const auto join = [](const std::vector<std::string> &items) {
    std::string out;
    for (const auto &item : items) {
      out += (out.empty() ? "" : ", ") + item;
    }
    return out;
  };
  std::cout << info->name << " (" << info->id << ")\n";
  std::string kind = info->is_map ? "Map" : "Mod";
  if (!info->source.empty()) {
    kind += ", " + info->source;
  }
  if (info->legacy) {
    kind += ", legacy";
  }
  if (!info->installed) {
    kind += ", not installed";
  }
  field("Kind", kind);
  field("Version", info->version);
  if (info->is_map) {
    field("Map", info->map_picked ? "used by this mode" : "not used");
  } else if (info->position) {
    field("Load order", two_digits(info->position) + " of " +
                            std::to_string(info->enabled) +
                            (info->position == 1 ? ", main mod" : ""));
  } else if (info->loads_at) {
    field("Load order",
          "loads at " + two_digits(info->loads_at) + ", added as a dependency");
  } else {
    field("Load order", "off");
  }
  field("Pinned", info->pinned);
  field("Authors", join(info->authors));
  field("License", info->license);
  if (info->installed) {
    std::vector<std::string> modes;
    for (const auto &m : info->modes) {
      modes.push_back(mode_name(m));
    }
    field("Modes", modes.empty() ? std::string("every mode") : join(modes));
    if (!info->supports_mode) {
      field("", std::string("does not support ") + mode_name(mode));
    }
  }
  field("Workshop id", info->workshop_id);
  field("Folder", info->folder);
  if (info->installed) {
    field("Files", std::to_string(info->files));
    field("Zones", std::to_string(info->zones));
    field("Content hash", info->content_hash);
  }
  if (info->installed && !info->legacy) {
    field("Touches", std::to_string(info->provides) + " provided, " +
                         std::to_string(info->overrides) + " overridden, " +
                         std::to_string(info->extends) + " extended, " +
                         std::to_string(info->hooks) + " hooks");
    field("Needs", join(info->needs));
    field("Conflicts with", join(info->conflicts));
  }
  const std::string unknown = !info->installed ? "unknown: it is not installed"
                              : info->legacy
                                  ? "unknown: legacy mods declare nothing"
                                  : "none declared";
  std::cout << "Capabilities:"
            << (info->capabilities.empty() ? " " + unknown : "") << "\n";
  for (const auto &cap : info->capabilities) {
    std::cout << "  " << cap.label
              << (cap.detail.empty() ? "" : ": " + cap.detail) << "\n";
  }
  std::cout << "Settings:" << (info->settings.empty() ? " " + unknown : "")
            << "\n";
  for (const auto &setting : info->settings) {
    std::cout << "  " << (setting.label.empty() ? setting.key : setting.label)
              << ": " << setting.type << ", default " << setting.default_json
              << (setting.dvar.empty() ? "" : ", " + setting.dvar) << "\n";
  }
  std::cout << "Problems:" << (info->problems.empty() ? " none" : "") << "\n";
  for (const auto &note : info->problems) {
    std::cout << "  " << to_string(note.level) << ": " << note.message << " ("
              << note.code << ")\n";
  }
  for (const auto &fix : info->fixes) {
    std::cout << "  fix: " << fix.kind << " " << fix.id
              << (fix.detail.empty() ? "" : " " + fix.detail) << "\n";
  }
  if (!info->why.empty()) {
    std::cout << "Why this order:\n";
    for (const auto &why : info->why) {
      std::cout << "  - " << why << "\n";
    }
  }
  return exit_ok;
}

// ------------------------------------------- mods enable / disable / order
// The Mods screen's hand edits (mods_edit): a mod switched on goes to the
// bottom, a map becomes the mode's map, and any edit detaches the profile
// from an applied pack (its pins go) and moves the saved positions with it.
int cmd_mods_edit(const arguments &args, const std::string &action) {
  const bool order = action == "order";
  if (!(order ? accept_only(args, 3, {"--home", "--mode", "--to"},
                            {"--up", "--down"})
              : accept_only(args, 3, {"--home", "--mode"}, {}))) {
    return exit_error;
  }
  const std::string given = args.word(2);
  if (given.empty()) {
    std::cerr << "mods " << action << " needs an id.\n";
    return exit_error;
  }
  const bool up = args.has("--up"), down = args.has("--down");
  const auto to = args.get("--to");
  if (order && (int(up) + int(down) + int(to.has_value())) != 1) {
    std::cerr << "mods order needs one of --up, --down or --to <n>.\n";
    return exit_error;
  }
  if (refuse_while_launcher_runs(args)) {
    return exit_error;
  }
  // Switching on checks the mod is installed and made for the mode, as the
  // Mods screen only offers those; that needs the catalog, and its first scan
  // can take minutes.
  const bool scan = action == "enable";
  home_view home = open_home(args, scan, false, scan);
  // From the read to the write, the launcher's mutex: a Xerces.exe opened
  // during the scan holds it and the edit is refused; one opened from now on
  // waits until launcher.json is written, then reads the new one.
  const launcher_home_lock lock(home.paths.home);
  if (!lock.held()) {
    say_launcher_runs(home.paths.home);
    return exit_error;
  }
  read_config(home); // as it is now, not as it was before the scan
  if (refuse_broken_config(home)) {
    return exit_error;
  }
  const std::string mode = mode_of(args, home.config);
  const std::string id =
      resolve_id(given, home.catalog, profile_of(home.config, mode));
  launcher_config &config = home.config;
  const mods_snapshot before = mods_take(config, mode);
  mode_profile working = before.profile;
  const auto place = [&]() -> std::size_t {
    const auto it = std::ranges::find(working.mods, id);
    return it == working.mods.end()
               ? 0
               : std::size_t(it - working.mods.begin()) + 1;
  };
  const std::string name = name_or_id(home.catalog, id);
  bool changed = false;
  std::string done;
  if (action == "enable") {
    const installed_content *item = home.catalog.find(id);
    if (!item) {
      std::cerr << "\"" << given << "\" is not installed: "
                << (home.game.dir
                        ? "no folder with this id in the Workshop folder, "
                          "mods\\ or usermaps\\."
                        : "Black Ops III was not found.")
                << "\n";
      return exit_error;
    }
    if (item->info.kind == content_kind::map) {
      changed = mods_set_map(working, id);
      done = name + " is the " + mode_name(mode) + " map";
    } else if (!item->info.supports_mode(mode)) {
      std::cerr << name << " is not made for " << mode_name(mode)
                << " (its manifest lists other modes).\n";
      return exit_error;
    } else {
      changed = mods_set_enabled(working, id, true);
      const std::size_t at = place();
      done = name + " is on at " + two_digits(at) +
             (at == 1 ? " (main mod)" : " (the bottom)");
      if (!changed) {
        done = name + " is already on at " + two_digits(at);
      }
    }
  } else if (action == "disable") {
    if (working.map && *working.map == id) {
      changed = mods_set_map(working, std::nullopt);
      done =
          name + " is no longer the " + std::string(mode_name(mode)) + " map";
    } else {
      changed = mods_set_enabled(working, id, false);
      done = changed ? name + " is off"
                     : name + " is not enabled for " + mode_name(mode);
    }
  } else {
    const std::size_t from = place();
    if (from == 0) {
      std::cerr << name << " is not enabled for " << mode_name(mode)
                << ": switch it on first, only enabled mods have a place in "
                   "the load order.\n";
      return exit_error;
    }
    if (to) {
      const std::size_t target = parse_count("--to", *to);
      if (target < 1 || target > working.mods.size()) {
        std::cerr << "--to takes a place from 1 to " << working.mods.size()
                  << ".\n";
        return exit_error;
      }
      changed = mods_move(working, from - 1, target - 1);
    } else {
      changed = mods_move_by(working, id, up ? -1 : 1);
    }
    const std::size_t at = place();
    done = changed ? name + " moved to " + two_digits(at) +
                         (at == 1 ? " (main mod)" : "")
                   : name + " is already at " + two_digits(at);
  }
  if (!changed) {
    std::cout << done << ".\n";
    return exit_ok;
  }
  profile_for(config, mode) = working;
  const std::string detached = mods_hand_edit(config, mode);
  if (!save_config(home)) {
    return exit_error;
  }
  cli_log log(home.paths.logs);
  log.write(log_level::info, "mods.edit", done,
            "{\"mode\":" + json::quote(mode) + "}");
  std::cout << done << ".";
  if (!detached.empty()) {
    std::cout << " Detached from the pack \"" << detached << "\".";
  }
  std::cout << "\n";
  if (scan) {
    print_problems(resolve_profile(home.catalog.items, config, mode,
                                   home.game.steam_build));
  }
  return exit_ok;
}

// ------------------------------------------------------------ plan apply
// Home's Launch: the mode's profile resolved as Xerces.exe resolves it, and
// the game started the same way (xerces_core::launch_game).
int cmd_plan_apply(const arguments &args) {
  // Anything this command does not know stops it here: a misspelled
  // --dry-launch must not leave a real launch.
  if (!accept_only(args, 2, {"--home", "--mode"},
                   {"--vanilla", "--mods-off", "--dry-launch", "--json"})) {
    return exit_error;
  }
  const bool vanilla = args.has("--vanilla");
  const bool mods_off = args.has("--mods-off");
  if (vanilla && mods_off) {
    std::cerr << "Give --vanilla or --mods-off, not both.\n";
    return exit_error;
  }
  // XERCES_DRY_LAUNCH=1 makes Xerces.exe's launches dry; it does the same
  // here (xerces_core reads it for both).
  const bool dry_by_environment = dry_launch_from_environment();
  const bool dry = args.has("--dry-launch") || dry_by_environment;
  if (dry_by_environment && !args.has("--dry-launch")) {
    std::cerr << "note: XERCES_DRY_LAUNCH=1, so this is a dry launch.\n";
  }
  // Vanilla and safe-mode launches load no mods, so need no catalog.
  const bool modded = !vanilla && !mods_off;
  const home_view home = open_home(args, true, true, modded);
  if (!home.config_error.empty()) {
    std::cerr << "warning: " << utf8(home.paths.config_file())
              << " cannot be read (" << home.config_error
              << "); using the defaults, as Xerces.exe does.\n";
  }
  const std::string mode = mode_of(args, home.config);

  // What Home asks about before a modded launch; here a note, since the
  // command already says how to launch.
  if (modded) {
    std::optional<launch_journal> last;
    if (const auto text = read_text_file(home.paths.journal_file())) {
      last = parse_journal(*text);
    }
    const safe_mode_decision decision = decide_safe_mode(last);
    if (decision.safe_mode && !game_in_run_folder(home.paths.home)) {
      std::cerr << "note: the last modded launch crashed. Launching with mods "
                   "as asked; --mods-off launches with mods off.\n";
    }
    const auto report =
        read_text_file(home.paths.cache / "signatures-report.json");
    if (report && parse_signature_report(*report).multi_mod_off()) {
      std::cerr << "note: at the last launch a signature Xerces needs for "
                   "extra mods was missing, so only the main mod loaded "
                   "(xerces_cli sig check).\n";
    }
  }

  launch_inputs in;
  in.paths = &home.paths;
  in.config = &home.config;
  in.game = &home.game;
  in.catalog = &home.catalog.items;
  in.mode = mode;
  in.modded = !vanilla;
  in.mods_off = mods_off;
  in.dry_launch = dry;
  cli_log log(home.paths.logs);
  // The run folder shows a game once it reaches post_unpack; a real launch
  // also looks for its process, there from the start, so a second plan apply
  // (or one right after Xerces.exe's Launch) does not start another game.
  const auto running = [&] {
    return game_in_run_folder(home.paths.home) ||
           (!dry && game_process_running(home.paths.client));
  };
  const launch_outcome out = launch_game(in, running, log.events());

  if (args.has("--json")) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto &alloc = doc.GetAllocator();
    static const char *const refusals[] = {
        "", "plan", "command", "game", "running", "client", "start"};
    json::set(doc, "mode", mode, alloc);
    json::set(doc, "started", out.started, alloc);
    json::set(doc, "dry_run", out.dry_run, alloc);
    json::set(doc, "refused", refusals[static_cast<int>(out.refusal)], alloc);
    json::set(doc, "message", out.message, alloc);
    json::set(doc, "command_line", out.command_line, alloc);
    json::set(doc, "plan_id", out.plan_id, alloc);
    json::set(doc, "plan_file",
              out.plan_file.empty() ? std::string() : utf8(out.plan_file),
              alloc);
    json::set(doc, "pid", static_cast<std::int64_t>(out.pid), alloc);
    const auto diags = json::parse(diagnostics_json(out.diags));
    json::set(doc, "diagnostics", json::copy(diags.document, alloc), alloc);
    if (out.resolved) {
      const auto parsed = json::parse(plan_to_json(*out.resolved));
      json::set(doc, "plan", json::copy(parsed.document, alloc), alloc);
    }
    std::cout << json::pretty(doc) << "\n";
  } else {
    if (out.resolved && out.refusal == launch_refusal::plan) {
      print_plan(*out.resolved, false);
    } else {
      if (out.resolved) {
        const plan &p = *out.resolved;
        std::cout << "Plan for " << p.mode << ": " << p.stack.size() << " mod"
                  << (p.stack.size() == 1 ? "" : "s") << " (" << p.plan_id
                  << ")\n";
        for (std::size_t i = 0; i < p.stack.size(); ++i) {
          std::cout << "  " << (i + 1) << ". " << p.stack[i].name << " ("
                    << p.stack[i].id << ")" << (i == 0 ? "  [main mod]" : "")
                    << (p.stack[i].requested ? "" : "  [dependency]") << "\n";
        }
        if (p.map) {
          std::cout << "  map: " << p.map->name << " (" << p.map->id << ")\n";
        }
      } else {
        std::cout << (mods_off ? "Mods off (safe mode)" : "Vanilla") << ", "
                  << mode_name(mode) << "\n";
      }
      print_diagnostics(out.diags);
    }
    // Only for a launch that went ahead: after a refusal it would read as if
    // the game had started with it (--json still has it).
    if (!out.command_line.empty() && (out.started || out.dry_run)) {
      std::cout << "command: " << out.command_line << "\n";
    }
    if (!out.plan_file.empty()) {
      std::cout << "plan file: " << utf8(out.plan_file) << "\n";
    }
    if (out.started) {
      std::cout << out.message << " (pid " << out.pid << ")\n";
    } else {
      (out.dry_run ? std::cout : std::cerr) << out.message << "\n";
    }
  }
  if (out.started || out.dry_run) {
    return exit_ok;
  }
  return out.refusal == launch_refusal::plan ||
                 out.refusal == launch_refusal::command
             ? exit_failed
             : exit_error;
}

// ----------------------------------------------------------- packs apply
namespace {
// A share code, a pack file, or a pack in the library (<home>\config\packs)
// by its id or the start of it; from_library says which.
struct found_pack {
  pack_result result;
  bool from_library = false;
};

found_pack find_pack(const std::string &source, const launcher_paths &paths) {
  const std::size_t start = source.find_first_not_of(" \t\r\n");
  if (start != std::string::npos &&
      std::string_view(source).substr(start).starts_with(share_code_prefix)) {
    return {decode_share_code(source), false};
  }
  std::error_code ec;
  if (fs::is_regular_file(fs::path(source), ec)) {
    return {read_pack_file(fs::path(source)), false};
  }
  const fs::path folder = paths.config / "packs";
  const pack_library library = read_pack_library(folder);
  found_pack out;
  out.from_library = true;
  if (const library_entry *exact = library.find(source)) {
    out.result.value = exact->value;
    return out;
  }
  const library_entry *match = nullptr;
  std::size_t matches = 0;
  if (source.size() >= 4) {
    for (const auto &entry : library.packs) {
      if (entry.value.id.starts_with(source)) {
        match = &entry;
        ++matches;
      }
    }
  }
  if (matches == 1) {
    out.result.value = match->value;
    return out;
  }
  out.result.diags.error(
      "pack.source", "",
      matches > 1 ? "\"" + source + "\" starts more than one pack id in " +
                        utf8(folder) + "; give more of it."
                  : "\"" + source +
                        "\" is not a pack file, a share code (XRC1.) or a "
                        "pack id in " +
                        utf8(folder) + ".");
  return out;
}
} // namespace

// The Packs screen's Apply (8.3): the config backed up, the profile replaced
// by the pack (shared code), launcher.json written, then the check of what is
// installed against the pack.
int cmd_packs_apply(const arguments &args) {
  if (!accept_only(args, 3, {"--home", "--mode"}, {"--json"})) {
    return exit_error;
  }
  const std::string source = args.word(2);
  if (source.empty()) {
    std::cerr << "packs apply needs a pack file, a share code or a pack id.\n";
    return exit_error;
  }
  if (refuse_while_launcher_runs(args)) {
    return exit_error;
  }
  home_view home = open_home(args, false, false, false);
  if (refuse_broken_config(home)) {
    return exit_error;
  }
  const found_pack loaded = find_pack(source, home.paths);
  if (!loaded.result.ok()) {
    print_diagnostics(loaded.result.diags);
    return exit_failed;
  }
  pack p = loaded.result.value;
  const std::string mode = args.get("--mode").value_or(p.mode);
  if (!valid_mode(mode)) {
    throw std::invalid_argument("Modes are zm, mp and cp, not \"" + mode +
                                "\".");
  }
  if (mode != p.mode) {
    std::cerr << "note: \"" << p.name << "\" is a " << mode_name(p.mode)
              << " pack; it goes to the " << mode_name(mode)
              << " profile as asked.\n";
  }
  // The catalog for the check afterwards; its first scan can take minutes.
  home.game = detect_game_state(home.config.game_dir, home.paths.compat, false);
  home.catalog = scan_catalog(home.game, home.paths.cache / "hashes.txt");

  // From the read to the write, the launcher's mutex (see cmd_mods_edit).
  const launcher_home_lock lock(home.paths.home);
  if (!lock.held()) {
    say_launcher_runs(home.paths.home);
    return exit_error;
  }
  read_config(home); // as it is now, not as it was before the scan
  if (refuse_broken_config(home)) {
    return exit_error;
  }
  // A file or a share code is placed against the library as the Packs
  // screen's import places it (place_import): when its id already names a
  // different pack of the player's, it is applied under a fresh id, so the
  // profile's pack_id never points at a library pack it does not hold.
  std::string renumbered_from;
  if (!loaded.from_library) {
    const fs::path folder = home.paths.config / "packs";
    import_placement placed = place_import(read_pack_library(folder), p);
    if (placed.kind == import_kind::renumbered) {
      renumbered_from = p.id;
      std::cerr << "note: a different pack in " << utf8(folder)
                << " has this pack's id (" << p.id << "), so it is applied as "
                << placed.value.id << ", as importing it would.\n";
    }
    p = std::move(placed.value);
  }

  cli_log log(home.paths.logs);
  const auto backup =
      apply_pack(home.paths, home.config, p, log.events(), mode);
  // The Packs screen switches to the pack's mode, which saves it as the
  // current mode.
  home.config.mode = mode;
  if (!save_config(home)) {
    return exit_error;
  }

  // The check the Packs screen shows after applying (fill_result).
  const plan resolved = resolve_profile(home.catalog.items, home.config, mode,
                                        home.game.steam_build);
  const diagnostics content = verify_pack_content(p, resolved);
  const pack_check check = check_pack(p, home.catalog.items);
  const bool ok = resolved.ok() && !content.has_errors() && check.ready();
  std::size_t problems = check.problems();
  for (const auto &d : resolved.diags.items()) {
    problems += d.level == severity::error ? 1 : 0;
  }
  log.write(log_level::info, "packs.applied", p.id,
            "{\"ok\":" + std::string(ok ? "true" : "false") +
                ",\"problems\":" + std::to_string(problems) +
                ",\"mode\":" + json::quote(mode) + "}");

  if (args.has("--json")) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto &alloc = doc.GetAllocator();
    json::set(doc, "pack", p.id, alloc);
    json::set(doc, "renumbered_from", renumbered_from, alloc);
    json::set(doc, "name", p.name, alloc);
    json::set(doc, "mode", mode, alloc);
    json::set(doc, "ok", ok, alloc);
    json::set(doc, "backup", backup ? utf8(*backup) : std::string(), alloc);
    rapidjson::Value items(rapidjson::kArrayType);
    for (const auto &item : check.items) {
      rapidjson::Value object(rapidjson::kObjectType);
      json::set(object, "id", item.id, alloc);
      json::set(object, "name", item.name, alloc);
      json::set(object, "map", item.is_map, alloc);
      json::set(object, "state", to_string(item.state), alloc);
      json::set(object, "pinned", item.pinned, alloc);
      json::set(object, "installed", item.installed, alloc);
      items.PushBack(std::move(object), alloc);
    }
    json::set(doc, "check", std::move(items), alloc);
    const auto plan_doc = json::parse(plan_to_json(resolved));
    json::set(doc, "plan", json::copy(plan_doc.document, alloc), alloc);
    const auto content_doc = json::parse(diagnostics_json(content));
    json::set(doc, "content", json::copy(content_doc.document, alloc), alloc);
    std::cout << json::pretty(doc) << "\n";
  } else {
    std::cout << "Applied \"" << p.name << "\" to " << mode_name(mode) << ": "
              << p.order.size() << " mod" << (p.order.size() == 1 ? "" : "s")
              << " pinned" << (p.map ? ", and its map" : "") << ".\n";
    if (backup) {
      std::cout << "Config backup: " << utf8(*backup) << "\n";
    } else {
      std::cerr << "The config backup failed (the log says why); the pack was "
                   "applied anyway.\n";
    }
    for (const auto &item : check.items) {
      if (item.state != pack_item_state::installed) {
        std::cout << "  " << to_string(item.state) << ": " << item.name << " ("
                  << item.id << ")"
                  << (item.pinned.empty() ? "" : " pinned " + item.pinned)
                  << (item.installed.empty() ? ""
                                             : ", installed " + item.installed)
                  << (item.workshop_id
                          ? ", Workshop " + std::to_string(*item.workshop_id)
                          : "")
                  << "\n";
      }
    }
    print_plan(resolved, false);
    print_diagnostics(content);
    std::cout << (ok ? "Ready to launch.\n"
                     : "Not ready: fix the problems above (Mods, or Steam for "
                       "what is missing), then check again.\n");
  }
  return ok ? exit_ok : exit_failed;
}

// -------------------------------------------------------------- logs tail
namespace {
// The bytes of complete lines from offset on, and how far they reach;
// nullopt when the file cannot be opened (it was removed).
std::optional<std::pair<std::string, std::uint64_t>>
read_new_lines(const fs::path &file, std::uint64_t offset) {
  using result = std::pair<std::string, std::uint64_t>;
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff end = in.tellg();
  if (end < 0) {
    return std::nullopt;
  }
  const auto size = static_cast<std::uint64_t>(end);
  if (size <= offset) {
    // Shorter than before: it was written again, so from its start.
    return result{{}, size < offset ? 0 : offset};
  }
  in.seekg(static_cast<std::streamoff>(offset));
  std::string text(static_cast<std::size_t>(size - offset), '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<std::size_t>(in.gcount()));
  const std::size_t last = text.rfind('\n');
  if (last == std::string::npos) {
    return result{{}, offset};
  }
  text.resize(last + 1);
  return result{text, offset + text.size()};
}
} // namespace

int cmd_logs_tail(const arguments &args) {
  if (!accept_only(args, 2, {"--home", "--role", "--lines"}, {"--follow"})) {
    return exit_error;
  }
  const launcher_paths paths = find_paths(home_override(args), false);
  const std::string role = args.get("--role").value_or("");
  if (!role.empty() && role != "launcher" && role != "game" && role != "cli" &&
      role != "server") {
    std::cerr << "Roles are launcher, game, cli and server.\n";
    return exit_error;
  }
  const std::size_t count =
      parse_count("--lines", args.get("--lines").value_or("20"));
  auto file = newest_log(paths.logs, role);
  if (!file) {
    std::cerr << "There is no " << (role.empty() ? "" : role + " ") << "log in "
              << utf8(paths.logs) << " yet.\n";
    return exit_error;
  }
  std::cerr << "== " << utf8(*file) << "\n";
  // Read back from the end: a long session's log is not read whole for the
  // last few lines.
  const auto tail = tail_file(*file, count);
  if (!tail) {
    std::cerr << utf8(*file) << " cannot be read.\n";
    return exit_error;
  }
  for (const auto &line : tail->lines) {
    std::cout << format_log_line(line) << "\n";
  }
  std::cout.flush();
  if (!args.has("--follow")) {
    return exit_ok;
  }
  // Until Ctrl+C: new lines as they are written, and a newer session of the
  // role (the launcher or game started again) as soon as it appears.
  std::uint64_t offset = tail->end;
  bool gone = false;
  for (int tick = 0;; ++tick) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (const auto more = read_new_lines(*file, offset)) {
      offset = more->second;
      for (const auto &line : last_lines(more->first, 0)) {
        std::cout << format_log_line(line) << "\n";
      }
      std::cout.flush();
      gone = false;
    } else if (!gone) {
      gone = true;
      std::cerr << utf8(file->filename())
                << " was removed; waiting for a newer log.\n";
    }
    if (tick % 4 == 3) {
      // Only a session that started later: when the followed file goes, the
      // newest one left is an older session, and its events are not new.
      const auto newest = newest_log(paths.logs, role);
      if (newest && *newest != *file && log_start(*newest) > log_start(*file)) {
        file = newest;
        offset = 0;
        gone = false;
        std::cerr << "== " << utf8(*file) << "\n";
      }
    }
  }
}

// --------------------------------------------------------------- sig check
// The game's own report from its last start (<home>\cache\
// signatures-report.json, DESIGN.md 4.3), without a dump.
int cmd_sig_report(const arguments &args) {
  if (!accept_only(args, 2, {"--home"}, {"--json"})) {
    return exit_error;
  }
  const launcher_paths paths = find_paths(home_override(args), false);
  const fs::path file = paths.cache / "signatures-report.json";
  const auto text = read_text_file(file);
  if (!text) {
    std::cerr << "There is no signature report yet: the game writes "
              << utf8(file)
              << " each time it starts with this home (from Xerces.exe, or "
                 "with XERCES_HOME set). Start it once, or check a memory "
                 "dump with --dump.\n";
    return exit_error;
  }
  const signature_state report = parse_signature_report(*text);
  if (!report.known) {
    std::cerr << utf8(file)
              << " is not a signature report (it does not "
                 "parse as JSON).\n";
    return exit_error;
  }
  if (args.has("--json")) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto &alloc = doc.GetAllocator();
    json::set(doc, "report", utf8(file), alloc);
    json::set(doc, "build", report.game_build, alloc);
    json::set(doc, "checked", report.checked, alloc);
    json::set(doc, "registry", report.registry, alloc);
    json::set(doc, "from_cache", report.from_cache, alloc);
    json::set(doc, "ok", !report.has_miss(), alloc);
    json::set(doc, "matched", static_cast<std::int64_t>(report.matched), alloc);
    json::set(doc, "total", static_cast<std::int64_t>(report.total), alloc);
    json::set(doc, "problem", report.problem, alloc);
    rapidjson::Value results(rapidjson::kArrayType);
    for (const auto &entry : report.entries) {
      rapidjson::Value item(rapidjson::kObjectType);
      json::set(item, "id", entry.id, alloc);
      json::set(item, "symbol", entry.name, alloc);
      json::set(item, "ok", entry.resolved, alloc);
      json::set(item, "rva", entry.rva, alloc);
      json::set(item, "error", entry.error, alloc);
      results.PushBack(std::move(item), alloc);
    }
    json::set(doc, "results", std::move(results), alloc);
    rapidjson::Value off(rapidjson::kObjectType);
    for (std::size_t i = 0; i < report.disabled.size(); ++i) {
      json::set(off, report.disabled[i],
                json::string_array(report.disabled_ids[i], alloc), alloc);
    }
    json::set(doc, "features_off", std::move(off), alloc);
    std::cout << json::pretty(doc) << "\n";
  } else {
    std::cout << "Game build " << report.game_build << ", checked "
              << report.checked
              << (report.from_cache ? " (from the cache)" : "") << ": "
              << report.matched << " of " << report.total
              << " signatures resolved\n";
    if (!report.registry.empty()) {
      std::cout << "Registry: " << report.registry << "\n";
    }
    for (const auto &entry : report.entries) {
      if (entry.resolved) {
        std::cout << "  ok    " << entry.id << "  " << entry.name << "  "
                  << entry.rva << "\n";
      } else {
        std::cout << "  MISS  " << entry.id << "  " << entry.name << ": "
                  << entry.error << "\n";
      }
    }
    if (!report.problem.empty()) {
      std::cout << "Problem: " << report.problem << "\n";
    }
    for (std::size_t i = 0; i < report.disabled.size(); ++i) {
      std::cout << "Off in the game: " << report.disabled[i] << " (";
      for (std::size_t k = 0; k < report.disabled_ids[i].size(); ++k) {
        std::cout << (k ? ", " : "") << report.disabled_ids[i][k];
      }
      std::cout << ")\n";
    }
  }
  return report.has_miss() ? exit_signatures : exit_ok;
}
} // namespace xerces_cli
