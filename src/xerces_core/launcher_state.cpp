#include "launcher_state.hpp"

#include <algorithm>
#include <chrono>

#include "catalog.hpp"
#include "detect.hpp"
#include "file_hash.hpp"
#include "json.hpp"
#include "launcher_home.hpp"
#include "manifest.hpp"

namespace xerces_core {
namespace fs = std::filesystem;

game_state detect_game_state(const std::optional<std::string> &saved_dir,
                             const fs::path &compat_dir, bool preflight,
                             hash_cache *cache) {
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
  // The zone memory figures (5.3 step 5): by Steam's build number now, by
  // the exe's checksum once the pre-flight has read it.
  match_zone_memory(next, compat_dir, std::nullopt);
  if (preflight) {
    preflight_game_state(next, compat_dir, cache);
  }
  return next;
}

void match_zone_memory(game_state &game, const fs::path &compat_dir,
                       std::optional<std::uint32_t> checksum) {
  const zone_memory_table zone_table = parse_zone_memory(
      read_text_file(compat_dir / "zone_memory.json").value_or(""));
  if (const zone_memory_build *build =
          find_zone_memory_build(zone_table, game.steam_build, checksum)) {
    game.zone_memory = *build;
  }
}

void preflight_game_state(game_state &game, const fs::path &compat_dir,
                          hash_cache *cache) {
  if (!game.dir) {
    return;
  }
  std::string error;
  game.exe = read_exe_facts(*game.dir / std::string(bo3_exe), &error, cache);
  if (!game.exe) {
    game.error = "BlackOps3.exe could not be read: " + error;
    return;
  }
  const auto registry_text = read_text_file(compat_dir / "builds.json");
  const build_registry registry =
      parse_build_registry(registry_text.value_or(""));
  if (!registry_text || !registry.ok()) {
    game.error = "compat/builds.json is missing or broken.";
  }
  game.preflight = preflight_check(*game.exe, game.steam_build, registry);
  if (!game.zone_memory) {
    match_zone_memory(game, compat_dir, game.exe->checksum);
  }
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

std::string read_launcher_settings(const game_state &game) {
  if (!game.dir) {
    return {};
  }
  return read_text_file(*game.dir / "boiii_players" / "user" /
                        "launcher_settings.json")
      .value_or("");
}

catalog_state scan_catalog(const game_state &game, hash_cache &cache,
                           const std::atomic<bool> *cancel,
                           const catalog_progress &progress,
                           unsigned hash_threads) {
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
  // This scan's own lookups: the launcher's pre-flight hashes BlackOps3.exe
  // through the same cache at the same time (and the CLI's just before), so
  // the cache's totals would mix the exe into the scan's figures, by
  // whichever finished first.
  hash_cache::stats stats;
  for (const source &from : sources) {
    std::error_code ec;
    if (!fs::is_directory(from.dir, ec)) {
      continue;
    }
    scan_options options;
    options.cache = &cache;
    options.stats = &stats;
    options.cancel = cancel;
    // Where the client looks for each mod's asset pool file.
    options.layout = from.name == "workshop" ? content_layout::workshop
                                             : content_layout::game_folder;
    options.hash_threads =
        hash_threads != 0 ? hash_threads : default_hash_threads(from.dir);
    result.hash_threads = std::max(result.hash_threads, options.hash_threads);
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
      // The same id from another folder: a Workshop item also kept as an old
      // manual copy in the game's mods or usermaps folder (both carry its
      // PublisherID), or mods\foo and usermaps\foo. Which copy loads is then
      // not settled (the planner takes one, the game the other), so both rows
      // in Mods say so (review, 2026-11-13).
      for (std::size_t i = 0; i < result.items.size(); ++i) {
        const installed_content &other = result.items[i];
        if (other.info.id != item.info.id || result.sources[i] == from.name) {
          continue;
        }
        const std::string message =
            "Two copies of " + item.info.id + " are installed: " +
            path_utf8(other.path) + " and " + path_utf8(item.path) +
            ". Which one loads is not settled: delete the one you do not "
            "want (the copy in the game's mods or usermaps folder is "
            "usually an old manual one) and scan again.";
        result.diags.add(severity::warning, "catalog.duplicate", other.folder,
                         message);
        if (item.folder != other.folder) {
          result.diags.add(severity::warning, "catalog.duplicate",
                           item.folder, message);
        }
        break;
      }
      result.items.push_back(std::move(item));
      result.sources.push_back(from.name);
    }
    result.diags.append(scan.diags);
    if (scan.cancelled) {
      break;
    }
    if (!scan.listed) {
      // The folder could not be listed (catalog.read): none of its files
      // was seen, which says nothing about whether they are gone, so its
      // entries stay for the next scan that can read it.
      continue;
    }
    std::error_code absolute_ec;
    const fs::path root = fs::absolute(from.dir, absolute_ec);
    result.scanned_roots.push_back(absolute_ec ? from.dir : root);
  }
  const bool cancelled = cancel && cancel->load();
  result.cache_hits = stats.hits;
  result.cache_misses = stats.misses;
  result.bytes_hashed = stats.bytes_hashed;
  result.seed_hits = stats.seed_hits;
  result.hash_seconds = stats.hash_seconds;
  result.slowest_file = stats.slowest_file;
  result.slowest_bytes = stats.slowest_bytes;
  result.slowest_seconds = stats.slowest_seconds;
  result.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  result.scanned = !cancelled;
  if (cancelled) {
    result.scanned_roots.clear();
  }
  return result;
}

catalog_state scan_catalog(const game_state &game, const fs::path &cache_file,
                           const std::atomic<bool> *cancel,
                           const catalog_progress &progress) {
  hash_cache cache;
  open_hash_cache(cache, cache_file);
  catalog_state result = scan_catalog(game, cache, cancel, progress);
  save_hash_cache(cache, cache_file, result);
  return result;
}

hash_cache_opened open_hash_cache(hash_cache &cache,
                                  const fs::path &cache_file) {
  hash_cache_opened opened;
  cache.load(cache_file);
  opened.version = cache.loaded_version();
  opened.entries = cache.size();
  opened.seed = hash_cache_seed_file(cache_file);
  if (opened.seed && cache.load_seed(*opened.seed)) {
    opened.seed_entries = cache.seed_size();
  } else {
    opened.seed.reset();
  }
  return opened;
}

bool save_hash_cache(const hash_cache &cache, const fs::path &cache_file,
                     const catalog_state &scanned) {
  // A cancelled scan has no scanned_roots: nothing is pruned.
  return cache.save(cache_file, scanned.scanned_roots);
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
