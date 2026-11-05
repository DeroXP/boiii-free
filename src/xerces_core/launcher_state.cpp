#include "launcher_state.hpp"

#include <chrono>

#include "catalog.hpp"
#include "detect.hpp"
#include "json.hpp"
#include "launcher_home.hpp"
#include "manifest.hpp"

namespace xerces_core {
namespace fs = std::filesystem;

game_state detect_game_state(const std::optional<std::string> &saved_dir,
                             const fs::path &compat_dir, bool preflight) {
  game_state next;
  next.searched = true;
  std::optional<fs::path> saved;
  if (saved_dir) {
    saved = path_from_utf8(*saved_dir);
  }
  const detection found =
      detect_game(saved, steam_roots_from_registry(), real_filesystem());
  next.tried = found.tried;
  next.dir = found.game_dir;
  if (!next.dir) {
    return next;
  }
  const fs::path dir = *next.dir;
  const fs::path steamapps = dir.parent_path().parent_path();
  next.workshop_dir =
      steamapps / "workshop" / "content" / std::string(bo3_app_id);
  next.mods_dir = dir / "mods";
  next.usermaps_dir = dir / "usermaps";
  if (const auto acf = read_text_file(
          steamapps / ("appmanifest_" + std::string(bo3_app_id) + ".acf"))) {
    next.steam_build = appmanifest_buildid(*acf);
  }
  if (!preflight) {
    return next;
  }
  std::string error;
  next.exe = read_exe_facts(dir / std::string(bo3_exe), &error);
  if (!next.exe) {
    next.error = "BlackOps3.exe could not be read: " + error;
  } else {
    const auto registry_text = read_text_file(compat_dir / "builds.json");
    const build_registry registry =
        parse_build_registry(registry_text.value_or(""));
    if (!registry_text || !registry.ok()) {
      next.error = "compat/builds.json is missing or broken.";
    }
    next.preflight = preflight_check(*next.exe, next.steam_build, registry);
  }
  return next;
}

const installed_content *catalog_state::find(std::string_view id) const {
  const installed_content *best = nullptr;
  for (const auto &item : items) {
    if (item.info.id == id && (!best || best->info.ver < item.info.ver)) {
      best = &item;
    }
  }
  return best;
}

catalog_state scan_catalog(const game_state &game, const fs::path &cache_file,
                           const std::atomic<bool> *cancel,
                           const catalog_progress &progress) {
  struct source {
    std::string name;
    fs::path dir;
  };
  std::vector<source> sources;
  if (game.dir) {
    sources = {{"workshop", game.workshop_dir},
               {"mods", game.mods_dir},
               {"usermaps", game.usermaps_dir}};
  }
  const auto started = std::chrono::steady_clock::now();
  catalog_state result;
  hash_cache cache;
  cache.load(cache_file);
  for (const source &from : sources) {
    std::error_code ec;
    if (!fs::is_directory(from.dir, ec)) {
      continue;
    }
    scan_options options;
    options.cache = &cache;
    options.cancel = cancel;
    if (progress) {
      options.progress = [&progress, &from](std::size_t index,
                                            std::size_t total,
                                            const std::string &name) {
        progress(from.name, index, total, name);
      };
    }
    catalog_scan scan = scan_mods_directory(from.dir, options);
    for (auto &item : scan.catalog) {
      // Workshop titles carry the game's colour codes; the launcher shows
      // plain text (ids and hashes are untouched, so plans do not change).
      item.info.name = strip_color_codes(item.info.name);
      result.items.push_back(std::move(item));
      result.sources.push_back(from.name);
    }
    result.diags.append(scan.diags);
    if (scan.cancelled) {
      break;
    }
  }
  const bool cancelled = cancel && cancel->load();
  cache.save(cache_file, !cancelled);
  result.cache_hits = cache.hits();
  result.cache_misses = cache.misses();
  result.bytes_hashed = cache.bytes_hashed();
  result.seconds = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - started)
                       .count();
  result.scanned = !cancelled;
  return result;
}

signature_state parse_signature_report(std::string_view text) {
  signature_state next;
  const auto parsed = json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return next;
  }
  const auto &root = parsed.document;
  next.known = true;
  next.game_build = json::get_string(root, "game_build").value_or("");
  next.checked = json::get_string(root, "checked").value_or("");
  next.registry = json::get_string(root, "registry").value_or("");
  next.problem = json::get_string(root, "problem").value_or("");
  if (const auto *cached = json::member(root, "from_cache");
      cached && cached->IsBool()) {
    next.from_cache = cached->GetBool();
  }
  if (const auto *disabled = json::member(root, "disabled");
      disabled && disabled->IsObject()) {
    for (auto it = disabled->MemberBegin(); it != disabled->MemberEnd(); ++it) {
      next.disabled.emplace_back(it->name.GetString(),
                                 it->name.GetStringLength());
      std::vector<std::string> ids;
      if (it->value.IsArray()) {
        for (const auto &id : it->value.GetArray()) {
          if (id.IsString()) {
            ids.emplace_back(id.GetString(), id.GetStringLength());
          }
        }
      }
      next.disabled_ids.push_back(std::move(ids));
    }
  }
  if (const auto *entries = json::member(root, "entries");
      entries && entries->IsArray()) {
    for (const auto &entry : entries->GetArray()) {
      if (!entry.IsObject()) {
        continue;
      }
      ++next.total;
      signature_entry item;
      item.id = json::get_string(entry, "id").value_or("");
      item.name = json::get_string(entry, "name").value_or("?");
      const auto *resolved = json::member(entry, "resolved");
      item.resolved = resolved && resolved->IsBool() && resolved->GetBool();
      item.rva = json::get_string(entry, "rva").value_or("");
      item.error = json::get_string(entry, "error").value_or("");
      if (item.resolved) {
        ++next.matched;
      } else {
        next.missing.push_back(item.name);
      }
      next.entries.push_back(std::move(item));
    }
  }
  return next;
}
} // namespace xerces_core
