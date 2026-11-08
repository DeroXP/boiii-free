#include <std_include.hpp>

#include "mod_stack.hpp"
#include "scheduler.hpp"
#include "workshop/workshop.hpp"

#include <component/lua/lua_state.hpp>
#include <game/game.hpp>
#include <game/impl/ugc/ugc.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include <utils/hook.hpp>
#include <utils/string.hpp>

#include <xerces/runtime.hpp>
#include <xerces/signatures.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/sigruntime.hpp>
#include <xerces_core/zone_names.hpp>

namespace mod_stack {
namespace {
using namespace game::lua;
using namespace game::lua::hks;
using game::bg::cache::BGCacheInfoDef;
using game::db::xasset::XAssetEntry;
using game::db::xasset::XAssetType;
using game::db::xzone::XZoneInfo;
using game::db::xzone::XZoneName;
using game::db::xzone::XZoneState;

constexpr std::string_view ALIAS_PREFIX = "stack_";
constexpr int32_t CORE_MOD_FLAGS = 0x40;
constexpr int32_t MODE_MOD_FLAGS = 0x100;
constexpr std::array MODES{"mp", "zm", "cp"};

utils::hook::detour zone_path_hook;
utils::hook::detour link_asset_hook;
utils::hook::detour zone_dependency_hook;
utils::hook::detour open_sound_bank_hook;
utils::hook::detour asset_priority_hook;
utils::hook::detour apply_queued_hook;

struct provider {
  std::string id;
  std::filesystem::path folder;
};

struct route {
  std::filesystem::path folder;
  std::string id;
  std::string name;
};

struct precache_source {
  game::bg::cache::BGCacheInfo *cache;
  std::vector<BGCacheInfoDef> defs;
};

std::recursive_mutex mutex;
std::map<std::string, std::filesystem::path, std::greater<>> folders;
std::vector<std::string> loaded;
std::vector<std::string> priorities;
size_t ui_owner = SIZE_MAX;
std::unordered_map<const void *, std::string> sound_owners;
std::map<std::string, std::map<std::string, precache_source>> precaches;
std::deque<std::vector<BGCacheInfoDef>> merged_precaches;
std::deque<std::string> stream_names;
std::unordered_map<const char *, const char *> stream_originals;

struct script_copy {
  std::string zone;
  std::string name;
  std::string data;
};
std::vector<script_copy> script_copies_;
// Best mod priority that shipped either half (.gsc/.csc) of each script
std::unordered_map<std::string, size_t> script_owners;

std::string startup_list;
std::atomic_bool reload_pending;

// xerces_map_scripts_first: a custom map keeps its own scripts against mods
// (DESIGN.md 6.3 rule 6). On unless turned off.
game::EngineDependentDvarMut map_scripts_first_dvar;
// xerces_map_ui_first: the same for its HUD and other UI Lua. Off unless on.
game::EngineDependentDvarMut map_ui_first_dvar;
// The host's choice while this process is a client of another host: the
// server's .gsc and every client's .csc must come from the same copies, or
// clientfields disagree and the client is dropped. -1: decide locally (alone,
// hosting, a dedicated server); 0 or 1: what the host announced (0 also for a
// host that announced nothing, which uses the engine's order).
std::atomic<int> host_rule{-1};

bool own_map_scripts_first() {
  return map_scripts_first_dvar ? map_scripts_first_dvar.get_bool() : true;
}

bool map_scripts_first_enabled() {
  const int rule = host_rule.load();
  const bool remote_client = !game::is_server() && !game::server_running();
  if (remote_client && rule >= 0) {
    return rule == 1;
  }
  return own_map_scripts_first();
}

// Halves (.gsc/.csc, xerces_core::script_half) of each script the mods'
// zones ship, by name without extension; filled as their zones link and
// cleared at every mod load.
std::unordered_map<std::string, std::uint8_t> mod_halves;

// What the custom map being loaded ships, so map-first never splits a
// script's halves (xerces_core::decide_map_half). The map's zones link after
// the mods', one script at a time, so whether the map ships a script's other
// half is only known for sure from an earlier load: the halves seen are kept
// in <home>\cache\map-scripts\<map>.txt, trusted while the map's zone file
// keeps its size and time.
struct map_scripts_state {
  std::string usermap;
  std::filesystem::path cache_file;
  std::string stamp; // "<size>:<time>" of the map's zone file
  xerces_core::map_script_halves seen;
  std::optional<xerces_core::map_script_halves> known;
  std::map<std::string, std::uint8_t> tentative; // taken before knowing
  std::unordered_set<std::string> logged;        // one log line per script
  bool written = false;
};
map_scripts_state map_scripts;

// The custom map being loaded or played: its internal name, which is the name
// of its zones; "" for stock maps. The engine's own active usermap, set when
// a usermap starts loading (before its zones are found) and cleared for a
// stock map (workshop.cpp). A level set-up hook would come too late: on a
// local game the server loads the map's zones before the client sets up.
std::string_view current_usermap() {
  const char *name = game::ugc::active_usermap->internalName;
  return {name, strnlen(name, sizeof(game::ugc::active_usermap->internalName))};
}

thread_local std::vector<XZoneInfo> prepared;
thread_local std::vector<std::array<char, 64>> prepared_names;
thread_local std::string sound_owner;

std::string command_line_list() {
  int32_t count = 0;
  LPWSTR *const argv = CommandLineToArgvW(GetCommandLineW(), &count);
  std::string result;
  for (int32_t i = 0; argv && i + 2 < count; ++i) {
    if (_wcsicmp(argv[i], L"+set") == 0 &&
        _wcsicmp(argv[i + 1], L"fs_extramods") == 0) {
      result = utils::string::convert(argv[i + 2]);
    }
  }
  LocalFree(argv);
  return result;
}

std::string main_mod() {
  const std::string id = game::ugc::UGC_ActiveMod_PublisherId();
  return id == "usermaps" ? std::string{} : id;
}

bool is_mod_zone(const std::string_view name) {
  return name == "core_mod" || name == "mp_mod" || name == "zm_mod" ||
         name == "cp_mod";
}

std::string alias(const std::string &id, const std::string_view name) {
  return std::format("{}{}_{}", ALIAS_PREFIX, id, name);
}

bool has_file(const std::filesystem::path &folder, const std::string &file) {
  std::error_code ec;
  return std::filesystem::is_regular_file(folder / file, ec);
}

std::optional<route> find_route(const std::string_view name) {
  std::string_view rest = name;
  std::string_view language;
  if (rest.size() > 3 && rest[2] == '_' && !rest.starts_with(ALIAS_PREFIX)) {
    language = rest.substr(0, 3);
    rest.remove_prefix(3);
  }
  if (!rest.starts_with(ALIAS_PREFIX)) {
    return std::nullopt;
  }
  rest.remove_prefix(ALIAS_PREFIX.size());

  std::scoped_lock lock(mutex);
  for (const auto &[id, folder] : folders) {
    if (rest.size() > id.size() && rest.starts_with(id) &&
        rest[id.size()] == '_') {
      return route{folder, id,
                   std::string(language) +
                       std::string(rest.substr(id.size() + 1))};
    }
  }
  return std::nullopt;
}

std::string zone_name(const XAssetEntry *entry) {
  if (!entry->zoneIndex) {
    return {};
  }
  return reinterpret_cast<XZoneName *>(
             game::db::xzone::g_zoneNames.get())[entry->zoneIndex]
      .name;
}

std::string loading_zone() {
  const char *name = game::db::load::g_load->filename;
  return name ? name : "";
}

bool zone_alive(const std::string &name) {
  // The zone count can trail the highest used slot, so scan the whole table
  const auto *zones =
      reinterpret_cast<XZoneName *>(game::db::xzone::g_zoneNames.get());
  for (uint32_t i = 0; i <= game::db::xzone::MAX_ZONE_COUNT; ++i) {
    const XZoneState state = zones[i].state;
    if (name == zones[i].name && (state == XZoneState::XZONE_LOADING ||
                                  state == XZoneState::XZONE_LOADED ||
                                  state == XZoneState::XZONE_COMPLETE)) {
      return true;
    }
  }
  return false;
}

std::vector<provider> resolve_providers(const std::vector<std::string> &ids) {
  std::vector<provider> result;
  for (const std::string &id : ids) {
    const game::ugc::UGCItem *mod = game::ugc::UGC_UnwrapOptionalUGCItem(
        game::ugc::UGC_GetModByPublisherId(id.c_str()));
    if (mod && mod->absolutePathZoneFiles[0] &&
        alias(id, "en_zm_mod_patch").size() < 60) {
      result.push_back({id, mod->absolutePathZoneFiles});
    }
  }
  return result;
}

int32_t prepare_zones_stub(XZoneInfo *zones, const int32_t count, char *,
                           XZoneInfo *, const int32_t capacity) {
  const std::span<XZoneInfo> input(zones, count);
  const bool mod_load = std::ranges::any_of(input, [](const XZoneInfo &zone) {
    return zone.allocFlags == CORE_MOD_FLAGS;
  });
  std::vector<std::string> ids;
  {
    std::scoped_lock lock(mutex);
    ids = mod_load ? get() : loaded;
  }
  const std::vector<provider> extras = resolve_providers(ids);
  std::deque<std::string> names;
  std::vector<XZoneInfo> plan;

  const auto add_extras = [&](const std::string &zone, const int32_t flags) {
    for (const provider &mod : extras) {
      if (has_file(mod.folder, zone + ".ff")) {
        names.push_back(alias(mod.id, zone));
        plan.push_back({names.back().c_str(), flags, 0, 0, 0, {}});
      }
    }
  };
  const game::ugc::UGCItem *main = game::ugc::UGC_UnwrapOptionalUGCItem(
      game::ugc::UGC_GetModByPublisherId(main_mod().c_str()));
  const auto main_has = [&](const std::string &zone) {
    return !main || has_file(main->absolutePathZoneFiles, zone + ".ff");
  };
  const auto contains = [&](const std::string &name) {
    return std::ranges::any_of(input, [&](const XZoneInfo &zone) {
      return zone.name && name == zone.name;
    });
  };

  for (const XZoneInfo &zone : input) {
    plan.push_back(zone);
    if (zone.name && is_mod_zone(zone.name)) {
      add_extras(zone.name, zone.allocFlags);
    }
    if (!zone.name && zone.allocFlags == CORE_MOD_FLAGS &&
        !main_has("core_mod")) {
      add_extras("core_mod", CORE_MOD_FLAGS);
    }
  }
  for (const char *mode : MODES) {
    if (contains(std::format("{}_common", mode)) &&
        !main_has(std::format("{}_mod", mode))) {
      add_extras(std::format("{}_mod", mode), MODE_MOD_FLAGS);
    }
  }

  {
    std::scoped_lock lock(mutex);
    for (auto &[cache, sources] : precaches) {
      std::erase_if(sources, [&](const auto &source) {
        return std::ranges::any_of(plan, [&](const XZoneInfo &zone) {
          return zone.name && (source.first == zone.name ||
                               (source.first.size() > 3 &&
                                source.first.substr(3) == zone.name));
        });
      });
    }
  }
  if (mod_load) {
    std::scoped_lock lock(mutex);
    loaded = ids;
    priorities.clear();
    script_owners.clear();
    mod_halves.clear();
    for (const provider &mod : extras) {
      folders[mod.id] = mod.folder;
      priorities.push_back(mod.id);
    }
    ui_owner = SIZE_MAX;
  }
  if (mod_load && main_mod().empty()) {
    scheduler::once([] { set({}); }, scheduler::main);
  }

  const size_t size = std::max<size_t>(capacity, plan.size() * 4);
  prepared.resize(size);
  prepared_names.resize(size);
  return game::db::load::DB_ExpandZoneList(
      plan.data(), static_cast<int32_t>(plan.size()),
      reinterpret_cast<char (*)[64]>(prepared_names.data()), prepared.data(),
      static_cast<int32_t>(size));
}

void schedule_zones_stub(XZoneInfo *, const int32_t count) {
  game::db::load::DB_LoadXZone(prepared.data(), count);
}

int32_t zone_path_stub(const char *name, const char *extension,
                       const int32_t size, char *buf, const game::ZoneType type,
                       const char *publisher) {
  if (name && extension) {
    if (const std::optional<route> target = find_route(name)) {
      return std::snprintf(buf, size, "%s/%s%s",
                           target->folder.generic_string().c_str(),
                           target->name.c_str(), extension);
    }
    if (!sound_owner.empty() && type == game::ZoneType::MOD) {
      std::scoped_lock lock(mutex);
      if (const auto folder = folders.find(sound_owner);
          folder != folders.end()) {
        return std::snprintf(buf, size, "%s/%s%s",
                             folder->second.generic_string().c_str(), name,
                             extension);
      }
    }
  }
  return zone_path_hook.invoke<int32_t>(name, extension, size, buf, type,
                                        publisher);
}

bool zone_dependency_stub(const char *key, const char *zone,
                          const char **cursor) {
  const std::optional<route> target =
      key && zone && cursor && std::string_view(key) == "xpak_read"
          ? find_route(zone)
          : std::nullopt;
  if (!target) {
    return zone_dependency_hook.invoke<bool>(key, zone, cursor);
  }

  {
    std::scoped_lock lock(mutex);
    if (const auto original = stream_originals.find(*cursor);
        original != stream_originals.end()) {
      *cursor = original->second;
    }
  }
  if (!zone_dependency_hook.invoke<bool>(key, target->name.c_str(), cursor)) {
    return false;
  }

  const char *original = *cursor;
  if (!original || !has_file(target->folder, std::string(original) + ".xpak")) {
    return true;
  }

  const std::string name = alias(target->id, original);
  std::scoped_lock lock(mutex);
  const auto existing = std::ranges::find(stream_names, name);
  const std::string &stored = existing != stream_names.end()
                                  ? *existing
                                  : stream_names.emplace_back(name);
  stream_originals[stored.c_str()] = original;
  *cursor = stored.c_str();
  return true;
}

bool open_sound_bank_stub(const bool streamed, void *asset_bank, void *load) {
  {
    std::scoped_lock lock(mutex);
    const auto owner = load ? sound_owners.find(*static_cast<void **>(load))
                            : sound_owners.end();
    sound_owner = owner != sound_owners.end() ? owner->second : std::string{};
  }
  const bool result =
      open_sound_bank_hook.invoke<bool>(streamed, asset_bank, load);
  sound_owner.clear();
  return result;
}

// 0 for the main mod's zones, 1.. for each extra in stack order, nullopt for
// everything else (xerces_core/zone_names.hpp has the naming rules; the main
// mod's mode tier "zm_mod" used to fall through as unranked).
std::optional<size_t> zone_priority(const std::string &zone) {
  std::scoped_lock lock(mutex);
  const xerces_core::zone_class found =
      xerces_core::classify_zone(zone, current_usermap(), priorities);
  switch (found.kind) {
  case xerces_core::zone_kind::main_tier:
    return 0;
  case xerces_core::zone_kind::extra:
    return found.extra_index + 1;
  default:
    return std::nullopt;
  }
}

std::optional<size_t> priority_of(const XAssetEntry *entry) {
  return zone_priority(zone_name(entry));
}

void merge_precache(const XAssetEntry *entry) {
  game::bg::cache::BGCacheInfo *cache = entry->asset.header.bgCache;
  if (!cache || !cache->name || !is_mod_zone(cache->name) ||
      cache->defCount < 0) {
    return;
  }

  // A zone reloaded under the same name frees its old caches, so only keep
  // caches the asset pool still holds
  std::unordered_set<const void *> live{cache};
  for (const auto &item : game::db::xasset::pool::g_assetEntryPool->pool) {
    if (item.entry.inuse && item.entry.asset.type == XAssetType::BG_CACHE) {
      live.insert(item.entry.asset.header.data);
    }
  }

  std::scoped_lock lock(mutex);
  std::map<std::string, precache_source> &sources = precaches[cache->name];
  sources[loading_zone()] = {cache, {cache->def, cache->def + cache->defCount}};
  std::erase_if(sources, [&](const auto &source) {
    return !zone_alive(source.first) || !live.contains(source.second.cache);
  });

  size_t top = ui_owner;
  for (const auto &[zone, source] : sources) {
    top = std::min(top, zone_priority(zone).value_or(SIZE_MAX));
  }
  if (sources.size() < 2 &&
      zone_priority(sources.begin()->first).value_or(SIZE_MAX) == top) {
    return;
  }
  std::map<std::pair<game::bg::cache::BGCacheTypes, std::string>,
           BGCacheInfoDef>
      unique;
  // Owner first so its menus win name clashes; other mods' menus still load.
  for (const bool owner_pass : {true, false}) {
    for (const auto &[zone, source] : sources) {
      if ((zone_priority(zone).value_or(SIZE_MAX) == top) != owner_pass) {
        continue;
      }
      for (const BGCacheInfoDef &def : source.defs) {
        unique.try_emplace({def.type, def.name}, def);
      }
    }
  }
  std::vector<BGCacheInfoDef> &merged = merged_precaches.emplace_back();
  for (const auto &[key, def] : unique) {
    merged.push_back(def);
  }
  for (const auto &[zone, source] : sources) {
    source.cache->def = merged.data();
    source.cache->defCount = static_cast<int32_t>(merged.size());
  }
}

bool is_ui_file(const XAssetEntry *entry) {
  const char *name = entry->asset.type == XAssetType::RAWFILE
                         ? *static_cast<const char **>(entry->asset.header.data)
                         : nullptr;
  return name && (std::string_view(name).starts_with("ui/") ||
                  std::string_view(name).ends_with(".lua"));
}

void track_ui_owner() {
  if (const std::optional<size_t> priority = zone_priority(loading_zone())) {
    std::scoped_lock lock(mutex);
    ui_owner = std::min(ui_owner, *priority);
  }
}

constexpr std::string_view ENTRY_SCRIPTS[] = {
    "scripts/zm/gametypes/_clientids.gsc",
    "scripts/mp/gametypes/_clientids.gsc",
    "scripts/cp/gametypes/_clientids.gsc",
};

bool is_entry_script(const XAssetEntry *entry) {
  const game::scr::ScriptParseTree *tree = entry->asset.header.scriptParseTree;
  return tree && tree->name &&
         std::ranges::find(ENTRY_SCRIPTS, std::string_view(tree->name)) !=
             std::end(ENTRY_SCRIPTS);
}

std::string script_base(const XAssetEntry *entry) {
  const game::scr::ScriptParseTree *tree = entry->asset.header.scriptParseTree;
  const std::string_view name = tree && tree->name ? tree->name : "";
  if (!name.ends_with(".gsc") && !name.ends_with(".csc")) {
    return {};
  }
  return std::string(name.substr(0, name.size() - 4));
}

// With the mod stack's mutex held: the map-scripts state follows the custom
// map being loaded, reading its cache when the map changes.
void sync_map_scripts(const std::string_view usermap) {
  if (map_scripts.usermap == usermap) {
    return;
  }
  map_scripts = {};
  map_scripts.usermap = std::string(usermap);
  if (usermap.empty()) {
    return;
  }
  std::string file_name(usermap);
  std::ranges::replace_if(
      file_name,
      [](const char c) {
        return !std::isalnum(static_cast<unsigned char>(c)) && c != '_' &&
               c != '-';
      },
      '_');
  map_scripts.cache_file =
      xerces_runtime::home() / "cache" / "map-scripts" / (file_name + ".txt");
  std::error_code ec;
  const char *zone_dir = game::ugc::active_usermap->absolutePathZoneFiles;
  const std::filesystem::path zone =
      std::filesystem::path(std::string(
          zone_dir,
          strnlen(zone_dir,
                  sizeof(game::ugc::active_usermap->absolutePathZoneFiles)))) /
      (std::string(usermap) + ".ff");
  const auto size = std::filesystem::file_size(zone, ec);
  if (ec) {
    return; // no stamp: no cache, every pair is decided as on a first load
  }
  const auto time = std::filesystem::last_write_time(zone, ec);
  if (ec) {
    return;
  }
  map_scripts.stamp = std::to_string(size) + ":" +
                      std::to_string(time.time_since_epoch().count());
  map_scripts.known = xerces_core::parse_map_scripts(
      xerces_runtime::read_file(map_scripts.cache_file), map_scripts.stamp);
}

// Which halves of each script the mods and the custom map ship, as their
// zones link.
void track_script_half(const XAssetEntry *entry) {
  const game::scr::ScriptParseTree *tree = entry->asset.header.scriptParseTree;
  std::string base;
  const std::uint8_t half =
      xerces_core::script_half_of(tree && tree->name ? tree->name : "", &base);
  if (!half) {
    return;
  }
  const std::string zone = loading_zone();
  const std::string_view usermap = current_usermap();
  std::scoped_lock lock(mutex);
  switch (xerces_core::classify_zone(zone, usermap, priorities).kind) {
  case xerces_core::zone_kind::main_tier:
  case xerces_core::zone_kind::extra:
    mod_halves[base] |= half;
    break;
  case xerces_core::zone_kind::usermap:
    sync_map_scripts(usermap);
    map_scripts.seen[base] |= half;
    break;
  default:
    break;
  }
}

// Once the player is in the custom map (every zone loaded): the cache for
// its next load, and the mixed pairs a first load could not avoid. Back in
// the menus the state is dropped, so the next load starts fresh.
void write_map_scripts() {
  if (!game::is_client()) {
    return;
  }
  if (!game::com::Com_IsInGame()) {
    std::scoped_lock lock(mutex);
    if (!map_scripts.usermap.empty()) {
      map_scripts = {};
    }
    return;
  }
  if (game::cg::clientUIActives->actives[0].connectionState !=
      game::connstate_t::ACTIVE) {
    return;
  }
  std::string cache_text;
  std::filesystem::path cache_file;
  std::vector<std::string> mixed;
  {
    std::scoped_lock lock(mutex);
    if (map_scripts.usermap.empty() || map_scripts.written) {
      return;
    }
    map_scripts.written = true;
    for (const auto &[base, taken] : map_scripts.tentative) {
      const auto seen = map_scripts.seen.find(base);
      if (seen == map_scripts.seen.end() ||
          (seen->second & (xerces_core::half_gsc | xerces_core::half_csc)) !=
              (xerces_core::half_gsc | xerces_core::half_csc)) {
        mixed.push_back(base);
      }
    }
    if (!map_scripts.stamp.empty() && !map_scripts.seen.empty()) {
      cache_text =
          xerces_core::format_map_scripts(map_scripts.stamp, map_scripts.seen);
      cache_file = map_scripts.cache_file;
    }
  }
  for (const std::string &base : mixed) {
    xerces_runtime::log_warn(
        "loader.script.mixed_halves",
        base + ": the map ships one half and a mod both; this first load "
               "mixed them, the next load keeps the mod's pair",
        "{\"script\":" + xerces_core::json::quote(base) + "}");
  }
  if (!cache_text.empty()) {
    xerces_runtime::write_file(cache_file, cache_text);
  }
}

// DESIGN.md 6.3 rule 6 on a custom map: the map keeps its own scripts. The
// engine ranks every mod tier above a usermap's zones, so a mod that ships
// its own version of a script the map also ships (DS4C's perk scripts) used
// to replace the map's copy, and the map's other scripts then failed to link
// ("GSC LINK ERROR ... Unresolved external", SANCTUM OBITUS, 2026-11-05).
// This applies with one mod too. Entry scripts keep the stack's own rule.
// The same holds for the map's HUD and other UI Lua (ui/ rawfiles; the user
// chose it on 2026-11-05: DS4C's t7hud_zm_custom.lua replaced RUST V5's and
// errored), behind its own switch, xerces_map_ui_first. UI runs on each
// client alone, so unlike scripts it needs no agreement with the host.
std::optional<int32_t> map_script_choice(const XAssetEntry *candidate,
                                         const XAssetEntry *existing) {
  const bool is_script = candidate->asset.type == XAssetType::SCRIPTPARSETREE;
  const bool is_ui = !is_script && is_ui_file(candidate);
  if (is_script ? (is_entry_script(candidate) || !map_scripts_first_enabled())
                : (!is_ui || !(map_ui_first_dvar ? map_ui_first_dvar.get_bool()
                                                 : true))) {
    return std::nullopt;
  }
  const std::string incoming_zone = zone_name(candidate);
  const std::string existing_zone = zone_name(existing);
  const std::string_view usermap = current_usermap();
  if (usermap.empty()) {
    return std::nullopt;
  }
  xerces_core::script_choice choice;
  {
    std::scoped_lock lock(mutex);
    choice = xerces_core::decide_script_override(
        xerces_core::classify_zone(incoming_zone, usermap, priorities).kind,
        xerces_core::classify_zone(existing_zone, usermap, priorities).kind,
        true);
  }
  if (choice == xerces_core::script_choice::engine) {
    return std::nullopt;
  }
  std::string script;
  if (is_script) {
    const game::scr::ScriptParseTree *tree =
        candidate->asset.header.scriptParseTree;
    script = tree && tree->name ? tree->name : "";
  } else if (const char *name =
                 *static_cast<const char **>(candidate->asset.header.data)) {
    script = name;
  }
  const bool map_wins = choice == xerces_core::script_choice::incoming_wins;
  std::string base;
  const std::uint8_t half =
      is_script ? xerces_core::script_half_of(script, &base) : 0;
  {
    std::scoped_lock lock(mutex);
    sync_map_scripts(usermap);
    // Never split a script's halves between the map and a mod.
    if (half) {
      const auto mods = mod_halves.find(base);
      const auto seen = map_scripts.seen.find(base);
      std::optional<std::uint8_t> known;
      if (map_scripts.known) {
        const auto found = map_scripts.known->find(base);
        known = found == map_scripts.known->end() ? 0 : found->second;
      }
      const xerces_core::half_choice halves = xerces_core::decide_map_half(
          half, mods == mod_halves.end() ? 0 : mods->second,
          seen == map_scripts.seen.end() ? 0 : seen->second, known);
      if (halves == xerces_core::half_choice::skip) {
        if (map_scripts.logged.insert(script).second) {
          xerces_runtime::log_info(
              "loader.script.map_first_skipped",
              script + ": the map ships only this half and a mod both; the "
                       "mod keeps its pair",
              "{\"script\":" + xerces_core::json::quote(script) + "}");
        }
        return std::nullopt;
      }
      if (halves == xerces_core::half_choice::take_tentative) {
        map_scripts.tentative[base] |= half;
      }
    }
    // The engine asks once per copy it meets: log each script once per map.
    if (!map_scripts.logged.insert(script).second) {
      return map_wins ? 1 : 0;
    }
  }
  xerces_runtime::log_info(
      is_script ? "loader.script.map_first" : "loader.ui.map_first",
      script + ": the map's copy " + (map_wins ? "replaces" : "stays over") +
          " the mod's",
      "{\"script\":" + xerces_core::json::quote(script) + ",\"map_zone\":" +
          xerces_core::json::quote(map_wins ? incoming_zone : existing_zone) +
          ",\"mod_zone\":" +
          xerces_core::json::quote(map_wins ? existing_zone : incoming_zone) +
          "}");
  return map_wins ? 1 : 0;
}

int32_t asset_priority_stub(const XAssetEntry *candidate,
                            const XAssetEntry *existing) {
  const int32_t original =
      asset_priority_hook.invoke<int32_t>(candidate, existing);
  if (candidate->asset.type != existing->asset.type) {
    return original;
  }
  if (const std::optional<int32_t> choice =
          map_script_choice(candidate, existing)) {
    return *choice;
  }
  {
    std::scoped_lock lock(mutex);
    if (priorities.empty()) {
      return original;
    }
  }

  const std::optional<size_t> incoming = priority_of(candidate);
  const std::optional<size_t> active = priority_of(existing);
  if (candidate->asset.type == XAssetType::SCRIPTPARSETREE &&
      is_entry_script(candidate)) {
    // Mods' entry scripts all run as renamed copies (see script_copies), so
    // the game's own copy keeps the original slot
    if (incoming) {
      return 0;
    }
    if (active) {
      return 1;
    }
  }
  if (incoming && candidate->asset.type == XAssetType::SCRIPTPARSETREE) {
    // A script's server and client halves must come from the same mod, or
    // their clientfield registrations disagree and the match disconnects
    std::scoped_lock lock(mutex);
    const auto owner = script_owners.find(script_base(candidate));
    if (owner != script_owners.end() && owner->second < *incoming) {
      return 0;
    }
  }
  if (incoming && active && *incoming != *active) {
    return *incoming < *active;
  }
  return original;
}

void track_script_owner(const XAssetEntry *entry) {
  const std::optional<size_t> priority = zone_priority(loading_zone());
  if (!priority) {
    return;
  }
  std::string base = script_base(entry);
  if (base.empty()) {
    return;
  }
  std::scoped_lock lock(mutex);
  const auto [owner, added] =
      script_owners.try_emplace(std::move(base), *priority);
  if (!added) {
    owner->second = std::min(owner->second, *priority);
  }
}

void capture_script_copy(const XAssetEntry *entry) {
  const game::scr::ScriptParseTree *tree = entry->asset.header.scriptParseTree;
  if (!tree || !tree->name || !tree->buffer || !tree->len ||
      std::ranges::find(ENTRY_SCRIPTS, std::string_view(tree->name)) ==
          std::end(ENTRY_SCRIPTS)) {
    return;
  }
  const std::string zone = loading_zone();
  if (!is_mod_zone(zone) && !find_route(zone)) {
    return;
  }
  std::string data(reinterpret_cast<const char *>(tree->buffer), tree->len);
  std::scoped_lock lock(mutex);
  std::erase_if(script_copies_, [&](const script_copy &copy) {
    return copy.zone == zone && copy.name == tree->name;
  });
  script_copies_.push_back({zone, tree->name, std::move(data)});
}

template <typename T> T *at(std::string &buf, const size_t offset) {
  return offset + sizeof(T) <= buf.size()
             ? reinterpret_cast<T *>(buf.data() + offset)
             : nullptr;
}

uint32_t append_string(std::string &buf, const std::string &text) {
  const auto offset = static_cast<uint32_t>(buf.size());
  buf.append(text);
  buf.push_back('\0');
  return offset;
}

// Moves a compiled script into its own namespace, file name and system name so
// the game treats it as a separate script from the copy it already loaded.
std::optional<std::string> rename_script(std::string buf,
                                         const std::string &new_name,
                                         const std::string &tag) {
  using game::scr::GSC_EXPORT_ITEM;
  using game::scr::GSC_IMPORT_ITEM;
  using game::scr::GSC_OBJ;
  using game::scr::GSC_STRINGTABLE_ITEM;
  using game::scr::ScrVarCanonicalName_t;

  const GSC_OBJ *header = at<GSC_OBJ>(buf, 0);
  if (!header || !header->hasMagic(GSC_OBJ::T7_MAGIC)) {
    return std::nullopt;
  }
  const GSC_OBJ obj = *header;

  std::map<ScrVarCanonicalName_t, ScrVarCanonicalName_t> spaces;
  std::set<std::pair<ScrVarCanonicalName_t, ScrVarCanonicalName_t>> own;
  for (uint16_t i = 0; i < obj.exports_count; ++i) {
    auto *item = at<GSC_EXPORT_ITEM>(
        buf, obj.exports_offset + size_t{i} * sizeof(GSC_EXPORT_ITEM));
    if (!item) {
      return std::nullopt;
    }
    own.emplace(item->name, item->name_space);
    auto [space, added] = spaces.try_emplace(item->name_space);
    if (added) {
      space->second = game::scr::builtin::fnv1a(
          std::format("{:x}_{}", item->name_space, tag).c_str());
    }
    item->name_space = space->second;
  }

  size_t offset = obj.imports_offset;
  for (uint16_t i = 0; i < obj.imports_count; ++i) {
    auto *item = at<GSC_IMPORT_ITEM>(buf, offset);
    if (!item) {
      return std::nullopt;
    }
    if (own.contains({item->name, item->name_space})) {
      item->name_space = spaces[item->name_space];
    }
    offset += sizeof(GSC_IMPORT_ITEM) + size_t{item->num_address} * 4;
  }

  const std::string system = "clientids";
  std::optional<uint32_t> renamed_system;
  offset = obj.stringtablefixup_offset;
  for (uint16_t i = 0; i < obj.stringtablefixup_count; ++i) {
    auto *item = at<GSC_STRINGTABLE_ITEM>(buf, offset);
    if (!item || item->string >= buf.size()) {
      return std::nullopt;
    }
    const size_t next =
        offset + sizeof(GSC_STRINGTABLE_ITEM) + size_t{item->num_address} * 4;
    if (std::string_view(buf.data() + item->string) == system) {
      if (!renamed_system) {
        renamed_system = append_string(buf, system + "_" + tag);
      }
      at<GSC_STRINGTABLE_ITEM>(buf, offset)->string = *renamed_system;
    }
    offset = next;
  }

  const uint32_t name =
      append_string(buf, new_name.substr(0, new_name.size() - 4));
  at<GSC_OBJ>(buf, 0)->name = name;
  return buf;
}

// Xerces: every link, so asset_owner's index knows it is out of date.
std::atomic<uint64_t> link_count{0};

XAssetEntry *link_asset_stub(XAssetEntry *entry, const game::qboolean allow) {
  link_count.fetch_add(1, std::memory_order_relaxed);
  if (entry->asset.type == XAssetType::BG_CACHE) {
    merge_precache(entry);
  }
  if (is_ui_file(entry)) {
    track_ui_owner();
  }
  if (entry->asset.type == XAssetType::SCRIPTPARSETREE) {
    capture_script_copy(entry);
    track_script_owner(entry);
    track_script_half(entry);
  }
  XAssetEntry *result = link_asset_hook.invoke<XAssetEntry *>(entry, allow);
  if (game::is_client() && entry->asset.type == XAssetType::SOUND) {
    for (const XAssetEntry *linked : {entry, result}) {
      const std::optional<route> owner =
          find_route(linked == entry ? loading_zone() : zone_name(linked));
      std::scoped_lock lock(mutex);
      if (owner) {
        sound_owners[linked->asset.header.data] = owner->id;
      } else {
        sound_owners.erase(linked->asset.header.data);
      }
    }
  }
  return result;
}

void apply_queued_stub(const game::LocalClientNum_t client) {
  apply_queued_hook.invoke<void>(client);
  const game::ugc::ModLoadState state = game::ugc::active_mod->loadState;
  if (client != game::LOCAL_CLIENT_0 || !reload_pending ||
      state == game::ugc::ModLoadState::LOADING ||
      (state == game::ugc::ModLoadState::COMPLETE &&
       game::ugc::UGC_ModLoad_TryComplete(client)) ||
      game::ugc::UGC_ModLoad_XZoneLoading()) {
    return;
  }
  reload_pending = false;
  const std::string main = main_mod();
  if (!main.empty() && !is_loaded()) {
    game::ugc::UGC_LoadModByPublisherId_Impl(client, main.c_str(), true);
  }
}

const char *mod_label_stub(const char *, const char *loaded, const char *,
                           const char *) {
  const std::string main = main_mod();
  const game::ugc::UGCItem *mod = game::ugc::UGC_UnwrapOptionalUGCItem(
      game::ugc::UGC_GetModByPublisherId(main.c_str()));
  const std::string title =
      mod && mod->title[0] ? mod->title : game::ugc::active_mod->internalName;
  const size_t extras = get().size();
  if (!extras) {
    return utils::string::va("%s%s", loaded, title.c_str());
  }
  return utils::string::va("%s%s (and %zu more)", loaded, title.c_str(),
                           extras);
}

// asset_owner's index: the live entry of each loaded asset of a type, by its
// header, built with one walk of the entry pool the first time a type is
// asked for after the last link. A menu labels a hundred camos at once.
struct owner_index {
  uint64_t at_links = UINT64_MAX;
  std::unordered_map<int32_t, std::unordered_map<const void *, XAssetEntry *>>
      by_type;
};
std::mutex owner_mutex;
owner_index owners;

XAssetEntry *live_entry(const XAssetType type, const void *data) {
  std::scoped_lock lock(owner_mutex);
  const uint64_t now = link_count.load(std::memory_order_relaxed);
  if (owners.at_links != now) {
    owners.by_type.clear();
    owners.at_links = now;
  }
  auto &index = owners.by_type[static_cast<int32_t>(type)];
  const auto still = [&](const XAssetEntry *entry) {
    return entry && entry->inuse && entry->asset.type == type &&
           entry->asset.header.data == data;
  };
  if (const auto found = index.find(data);
      found != index.end() && still(found->second)) {
    return found->second;
  }
  // Built again when an asset is missing (a zone unloaded or loaded since):
  // the header is the live entry's, as game.ismodrawfile finds it.
  index.clear();
  for (auto &item : game::db::xasset::pool::g_assetEntryPool->pool) {
    XAssetEntry &entry = item.entry;
    if (entry.inuse && entry.asset.type == type) {
      index[entry.asset.header.data] = &entry;
    }
  }
  const auto found = index.find(data);
  return found != index.end() ? found->second : nullptr;
}

luaReturnCount_e lua_get(lua_State *luaVM) {
  const char *main = lua_tostring(luaVM, 1);
  lua_pushstring(luaVM,
                 main ? utils::string::join(get(main), ",") : serialize());
  return luaReturnCount_e::ONE;
}

luaReturnCount_e lua_set(lua_State *luaVM) {
  const char *list = lua_tostring(luaVM, 1);
  set(list ? list : "");
  return luaReturnCount_e::NONE;
}

luaReturnCount_e lua_reload(lua_State *) {
  reload_pending = true;
  return luaReturnCount_e::NONE;
}

// The stacked mods a list gives beside the main mod: none without a main
// mod, and neither the main mod nor duplicates.
std::vector<std::string> extras_of(const std::string &list,
                                   const std::string &main) {
  std::vector<std::string> result;
  if (main.empty() || main == "usermaps") {
    return result;
  }
  for (std::string id : utils::string::split(list, ',')) {
    utils::string::trim(id);
    if (!id.empty() && id != main &&
        std::ranges::find(result, id) == result.end()) {
      result.push_back(std::move(id));
    }
  }
  return result;
}
} // namespace

std::vector<std::string> get(const std::string &main) {
  const std::optional<std::string_view> list =
      game::get_dvar_string("fs_extramods");
  return extras_of(list ? std::string(*list) : startup_list, main);
}

std::vector<std::string> get() { return get(main_mod()); }

std::string serialize() { return utils::string::join(get(), ","); }

std::vector<std::pair<std::string, std::string>> script_copies() {
  std::vector<script_copy> copies;
  {
    std::scoped_lock lock(mutex);
    std::erase_if(script_copies_, [](const script_copy &copy) {
      return !zone_alive(copy.zone);
    });
    copies = script_copies_;
  }

  std::vector<std::pair<std::string, std::string>> result;
  std::vector<std::string_view> seen;
  for (const script_copy &copy : copies) {
    const game::scr::ScriptParseTree *active =
        game::db::xasset::DB_FindXAssetHeader(XAssetType::SCRIPTPARSETREE,
                                              copy.name.c_str(), false, 0)
            .scriptParseTree;
    if ((active && active->buffer &&
         std::string_view(reinterpret_cast<const char *>(active->buffer),
                          active->len) == copy.data) ||
        std::ranges::find(seen, copy.data) != seen.end()) {
      continue;
    }
    seen.push_back(copy.data);

    const std::optional<route> owner = find_route(copy.zone);
    const std::string tag = owner ? owner->id : "main";
    const std::string name =
        copy.name.substr(0, copy.name.size() - 4) + "_" + tag + ".gsc";
    if (std::optional<std::string> data = rename_script(copy.data, name, tag)) {
      result.emplace_back(name, std::move(*data));
    } else {
      printf("[mod_stack] could not load %s from %s\n", copy.name.c_str(),
             copy.zone.c_str());
    }
  }
  return result;
}

void set(const std::string &list) {
  if (game::EngineDependentDvarMut dvar = game::get_dvar("fs_extramods")) {
    dvar.set(list.c_str());
  }
}

bool is_loaded() {
  const std::vector<std::string> extras = get();
  std::scoped_lock lock(mutex);
  return loaded == extras;
}

bool is_loaded(const std::string &list) {
  const std::vector<std::string> extras = extras_of(list, main_mod());
  std::scoped_lock lock(mutex);
  return loaded == extras;
}

// Xerces' command pipe (src/client/xerces/ipc.cpp) asks for the same reload
// as ModStack.Reload in the mods menu.
void request_reload() { reload_pending = true; }

std::optional<std::string> asset_owner(const int32_t type,
                                       const std::string &name) {
  const auto asset_type = static_cast<XAssetType>(type);
  const void *data = game::db::xasset::DB_FindXAssetHeader(
                         asset_type, name.c_str(), false, 0)
                         .data;
  // A name the type has no asset for gets a stand-in (the default image,
  // material or model), whose owner says nothing about the name: on
  // 2026-11-06 names that were no image, like a camo's "Topaz", came back as
  // Hyperdimension Neptunia's. DB_IsXAssetDefault (db.cpp uses it too) tells
  // a stand-in from the real asset.
  if (!data || game::db::xasset::DB_IsXAssetDefault(asset_type, name.c_str())) {
    return std::nullopt;
  }
  const XAssetEntry *entry = live_entry(asset_type, data);
  if (!entry) {
    return std::nullopt;
  }
  const std::string zone = zone_name(entry);
  std::scoped_lock lock(mutex);
  const xerces_core::zone_class found =
      xerces_core::classify_zone(zone, current_usermap(), priorities);
  switch (found.kind) {
  case xerces_core::zone_kind::main_tier: {
    std::string id = main_mod();
    return id.empty() ? std::nullopt : std::optional(std::move(id));
  }
  case xerces_core::zone_kind::extra:
    if (found.extra_index < priorities.size()) {
      return priorities[found.extra_index];
    }
    return std::nullopt;
  case xerces_core::zone_kind::usermap: {
    const char *id = game::ugc::active_usermap->publisherId;
    std::string text(id, strnlen(id, sizeof(game::ugc::active_usermap->publisherId)));
    return text.empty() ? std::nullopt : std::optional(std::move(text));
  }
  default:
    return std::nullopt;
  }
}

const void *game_copy(const int32_t type, const std::string &name) {
  const auto asset_type = static_cast<XAssetType>(type);
  const void *data = game::db::xasset::DB_FindXAssetHeader(
                         asset_type, name.c_str(), false, 0)
                         .data;
  if (!data || game::db::xasset::DB_IsXAssetDefault(asset_type, name.c_str())) {
    return nullptr;
  }
  const XAssetEntry *entry = live_entry(asset_type, data);
  if (!entry) {
    return nullptr;
  }
  std::scoped_lock lock(mutex);
  const auto from_game = [&](const XAssetEntry *candidate) {
    return xerces_core::classify_zone(zone_name(candidate), current_usermap(),
                                      priorities)
               .kind == xerces_core::zone_kind::stock;
  };
  if (from_game(entry)) {
    return nullptr;
  }
  // The copies that lost stay linked behind the live entry through
  // nextOverride, in priority order (DB_OverrideAsset inserts each copy that
  // loses there; seen in its disassembly on 2026-11-06). The engine marks them
  // not in use (inuse 0), but their data stays in their zone: on 2026-11-06
  // the game's own lua/shared/luareadonlytables.lua (core_patch, 659 bytes)
  // sat right behind Ascendance's. So a copy counts while its zone is loaded.
  // The bound only guards against a broken chain.
  auto &pool = game::db::xasset::pool::g_assetEntryPool->pool;
  uint32_t index = entry->nextOverride;
  for (int steps = 0; index != 0 &&
                      index < game::db::xasset::pool::XASSET_ENTRY_POOL_LENGTH &&
                      steps < 256;
       ++steps) {
    const XAssetEntry *candidate = &pool[index].entry;
    if (candidate->asset.type == asset_type && candidate->asset.header.data &&
        from_game(candidate) && zone_alive(zone_name(candidate))) {
      return candidate->asset.header.data;
    }
    index = candidate->nextOverride;
  }
  return nullptr;
}

std::string workshop_title(const std::string &id) {
  for (const game::ugc::UGCItem *item :
       {game::ugc::UGC_UnwrapOptionalUGCItem(
            game::ugc::UGC_GetModByPublisherId(id.c_str())),
        game::ugc::UGC_UnwrapOptionalUGCItem(
            game::ugc::UGC_GetUsermapByPublisherId(id.c_str()))}) {
    if (item && item->title[0]) {
      return std::string(item->title);
    }
  }
  return {};
}

std::string active_usermap() { return std::string(current_usermap()); }

std::string map_scripts_first_info() {
  return own_map_scripts_first() ? "1" : "0";
}

void follow_host_map_scripts_first(const std::string_view announced) {
  host_rule = announced == "1" ? 1 : 0;
}

void use_own_map_scripts_first() { host_rule = -1; }

// Where each hook goes. The last four are hooked on the client only.
struct hook_targets {
  std::uintptr_t expand_zone_list_call = 0;
  std::uintptr_t load_xzone_call = 0;
  std::uintptr_t build_os_path_for_thread = 0;
  std::uintptr_t link_xasset_entry = 0;
  std::uintptr_t override_asset = 0;
  std::uintptr_t key_value_pairs_get_next_value = 0;
  std::uintptr_t start_header_read = 0;
  std::uintptr_t render_loaded_mod_label_call = 0;
  std::uintptr_t mod_change_apply_queued = 0;
};

// The dedicated server keeps upstream's tables until it has its own
// signature set (D22).
hook_targets server_targets() {
  using game::db::load::DB_LoadXAssets;
  hook_targets targets;
  targets.expand_zone_list_call =
      DB_LoadXAssets.offset(game::select_value(0x347, 0x0, 0x32D));
  targets.load_xzone_call =
      DB_LoadXAssets.offset(game::select_value(0x353, 0x0, 0x339));
  targets.build_os_path_for_thread =
      reinterpret_cast<std::uintptr_t>(game::fs::FS_BuildOSPathForThread.get());
  targets.link_xasset_entry = reinterpret_cast<std::uintptr_t>(
      game::db::load::DB_LinkXAssetEntry.get());
  targets.override_asset =
      reinterpret_cast<std::uintptr_t>(game::db::load::DB_OverrideAsset.get());
  return targets;
}

// On the client every target comes from the signature registry. The client
// already turns "multi_mod" off when one of these ids is missing or did not
// resolve, whatever the registry tags it with (require_signatures in
// signatures.cpp, so the launcher and the pipe say so too). This is the last
// check before patching: every address is looked up first, and if one is
// still missing nothing is hooked, so an address is never read from an empty
// optional even if that rule or the id list changes.
std::optional<hook_targets> client_targets() {
  namespace ids = xerces_core::multi_mod_signatures;
  hook_targets targets;
  std::vector<std::string> missing;
  const auto take = [&](std::string_view id, std::uintptr_t &out) {
    if (const std::optional<std::uintptr_t> address =
            xerces_signatures::address(id)) {
      out = *address;
    } else {
      missing.emplace_back(id);
    }
  };
  take(ids::expand_zone_list_call, targets.expand_zone_list_call);
  take(ids::load_xzone_call, targets.load_xzone_call);
  take(ids::build_os_path_for_thread, targets.build_os_path_for_thread);
  take(ids::link_xasset_entry, targets.link_xasset_entry);
  take(ids::override_asset, targets.override_asset);
  take(ids::key_value_pairs_get_next_value,
       targets.key_value_pairs_get_next_value);
  take(ids::start_header_read, targets.start_header_read);
  take(ids::render_loaded_mod_label_call, targets.render_loaded_mod_label_call);
  take(ids::mod_change_apply_queued, targets.mod_change_apply_queued);
  if (missing.empty()) {
    return targets;
  }

  std::string names;
  std::string list;
  for (const std::string &id : missing) {
    names += (names.empty() ? "" : ", ") + id;
    list += (list.empty() ? "\"" : ",\"") + id + "\"";
  }
  xerces_runtime::log_error(
      "xerces.feature.off",
      "Multi-mod is off: the signature registry has no resolved address for " +
          names + ". The game and a single mod (fs_game) still work.",
      R"({"feature":"multi_mod","missing":[)" + list + "]}");
  return std::nullopt;
}

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("mod_stack");

  static void install_hooks(const hook_targets &targets) {
    utils::hook::call(targets.expand_zone_list_call, prepare_zones_stub);
    utils::hook::call(targets.load_xzone_call, schedule_zones_stub);
    zone_path_hook.create(targets.build_os_path_for_thread, zone_path_stub);
    link_asset_hook.create(targets.link_xasset_entry, link_asset_stub);
    asset_priority_hook.create(targets.override_asset, asset_priority_stub);

    if (game::is_client()) {
      zone_dependency_hook.create(targets.key_value_pairs_get_next_value,
                                  zone_dependency_stub);
      open_sound_bank_hook.create(targets.start_header_read,
                                  open_sound_bank_stub);
      utils::hook::call(targets.render_loaded_mod_label_call, mod_label_stub);
      apply_queued_hook.create(targets.mod_change_apply_queued,
                               apply_queued_stub);
      xerces_runtime::log_info("xerces.feature.on",
                               "Multi-mod hooks installed from signatures",
                               R"({"feature":"multi_mod"})");
    }
  }

public:
  void post_unpack() override {
    if (!game::is_server() && !game::is_new_client()) {
      return;
    }

    startup_list = command_line_list();
    scheduler::once(
        [] {
          game::register_dvar_string(
              "fs_extramods", startup_list.c_str(), game::dvarFlags_e::NONE,
              "Extra mods loaded together with fs_game, comma separated, "
              "highest priority first. fs_game always has the highest "
              "priority.");
          map_scripts_first_dvar = game::register_dvar_bool(
              "xerces_map_scripts_first", true, game::dvarFlags_e::NONE,
              "On a custom map, the map keeps its own scripts when a mod "
              "ships another version of them (applies at the next map "
              "load).");
          map_ui_first_dvar = game::register_dvar_bool(
              "xerces_map_ui_first", false, game::dvarFlags_e::NONE,
              "On a custom map, the map keeps its own HUD and other UI "
              "Lua when a mod ships another version of them (applies at "
              "the next map load). Off by default since 2026-11-08: it "
              "ended Big Bang Tower with DS4C and Ascendance, and RUST "
              "loads without it.");
        },
        scheduler::main);

    // Xerces (DESIGN.md 4.3, D18): on the client every hook target comes from
    // the signature registry, resolved against the running game; if any is
    // missing, multi-mod stays off for this run instead of patching guessed
    // addresses (extra mods are then not loaded; fs_game still works). The
    // dedicated server keeps upstream's tables until it has its own signature
    // set (D22).
    if (!game::is_client()) {
      install_hooks(server_targets());
    } else if (!xerces_signatures::feature_enabled("multi_mod")) {
      xerces_runtime::log_error(
          "xerces.feature.off",
          xerces_signatures::feature_message(
              "multi_mod", "Multi-mod",
              "The game and a single mod (fs_game) still work."),
          R"({"feature":"multi_mod"})");
    } else if (const std::optional<hook_targets> targets = client_targets()) {
      install_hooks(*targets);
    }

    if (game::is_client()) {
      // The map-scripts cache, once the player is in a custom map.
      scheduler::loop(write_map_scripts, scheduler::main, 1s);

      static constexpr const luaL_Reg ModStackLibrary[] = {
          lua_state::luaL_LoggedReg<"ModStack", "Get", lua_get>(),
          lua_state::luaL_LoggedReg<"ModStack", "Set", lua_set>(),
          lua_state::luaL_LoggedReg<"ModStack", "Reload", lua_reload>(),
          {nullptr, nullptr},
      };
      lua_state::register_library("ModStack", ModStackLibrary);
    }
  }
};
} // namespace mod_stack

REGISTER_COMPONENT(mod_stack::component)
