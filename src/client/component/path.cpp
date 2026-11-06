#include <std_include.hpp>

#include "path.hpp"

#include <atomic>

#include <utils/string.hpp>
#include <xerces/runtime.hpp>
#include <xerces_core/json.hpp>

namespace path {

namespace {
// Xerces: the game log's line for a path Windows would not resolve. Once per
// game: the game asks about the same path again and again.
void note_unresolved(const std::filesystem::path &path,
                     const std::error_code &ec) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    xerces_runtime::log_warn(
        "path.unresolved",
        "Windows did not resolve a path the game asked for; it is passed on "
        "as it is",
        "{\"path\":" +
            xerces_core::json::quote(utils::string::convert(path.native())) +
            ",\"error\":" + std::to_string(ec.value()) + "}");
  }
}

// Xerces: the path with its links, "." and ".." resolved, or the path as it
// came when Windows will not say where it leads. The throwing overload ended
// the game: on 2026-11-06 Windows answered "Access is denied" for boiii.start
// next to boiii.exe in a OneDrive folder, and every launch crashed 8 s in
// (docs/TESTING.md, "OneDrive crash"). The game's own call then gets
// Windows' answer, as it would without these hooks.
std::filesystem::path canonical_or_self(const std::filesystem::path &path) {
  std::error_code ec;
  std::filesystem::path out = std::filesystem::weakly_canonical(path, ec);
  if (!ec) {
    return out;
  }
  note_unresolved(path, ec);
  return path;
}
} // namespace

const std::filesystem::path &cwd() {
  static const std::filesystem::path path = std::filesystem::current_path();
  return path;
}

const std::filesystem::path &root() {
  static const std::filesystem::path path = cwd().root_path();
  return path;
}

const std::filesystem::path &players_directory() {
  static const std::filesystem::path path =
      canonical_or_self(cwd() / "players");
  return path;
}

const std::filesystem::path &boiii_players_directory() {
  static const std::filesystem::path path =
      canonical_or_self(cwd() / "boiii_players");
  return path;
}

const std::filesystem::path &steam_workshop_content_directory() {
  static const std::filesystem::path path = canonical_or_self(
      cwd() / "../../workshop/content" / game::APP_ID_STR);
  return path;
}

bool have_steam_workshop_content_directory() {
  static const bool result = [] {
    std::error_code ec;
    return std::filesystem::exists(steam_workshop_content_directory(), ec);
  }();
  return result;
}

const std::filesystem::path &usermaps_directory() {
  static const std::filesystem::path path =
      canonical_or_self(cwd() / "usermaps");
  return path;
}

const std::filesystem::path &mods_directory() {
  static const std::filesystem::path path = canonical_or_self(cwd() / "mods");
  return path;
}

const std::filesystem::path &lpc_directory() {
  static const std::filesystem::path path = canonical_or_self(cwd() / "LPC");
  return path;
}

bool reparent(std::filesystem::path &path, const std::filesystem::path &current,
              const std::filesystem::path &replacement) {
  if (path == current) {
    path = replacement;
    return true;
  }

  // Xerces: the error_code overloads here and below; a path Windows will not
  // resolve is not under current.
  std::error_code ec;
  const std::filesystem::path rel = std::filesystem::relative(path, current, ec);
  if (!ec && !rel.empty() && rel.native()[0] != '.') {
    path = replacement / std::move(rel);
    return true;
  }
  return false;
}

std::filesystem::path strip_parts(const std::filesystem::path &p,
                                  size_t count) {
  if (p.empty() || p.begin() == p.end()) {
    return {};
  }

  std::filesystem::path::iterator it = p.begin();
  std::advance(it, count);

  std::filesystem::path result;
  for (; it != p.end(); ++it) {
    result /= *it; // Append each subsequent part
  }

  return result;
}

bool try_replace_workshop_root(std::filesystem::path &path) {
  if (!have_steam_workshop_content_directory()) {

    // Calculate relative path once to avoid duplicate reparent overhead
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(
        path, steam_workshop_content_directory(), ec);
    // Check if path is a child of the Steam workshop content directory
    if (!ec && !rel.empty() && rel.native()[0] != '.') {
      const std::filesystem::path rel_zone_parented =
          part_count(rel) == 1 ? std::move(rel) / "zone"
                               : part(rel, 0) / "zone" / strip_parts(rel, 1);
      const std::filesystem::path in_usermaps_tree =
          usermaps_directory() / rel_zone_parented;

      if (std::filesystem::exists(in_usermaps_tree, ec)) {
        path = in_usermaps_tree;
        return true;
      }

      const std::filesystem::path in_mods_tree =
          mods_directory() / std::move(rel_zone_parented);
      path = in_mods_tree;
      return true;
    }
  }
  return false;
}

std::filesystem::path normalize(const std::filesystem::path &path) {
  static const ParentMatch parent_replacements[] = {
      {players_directory(), boiii_players_directory()},
      {root() / "LPC", lpc_directory()},
      {"\\LPC", lpc_directory()}};

  std::error_code ec;
  std::filesystem::path canonicalized =
      std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    // Nothing to reparent: the parents below would ask Windows about this
    // path again.
    note_unresolved(path, ec);
    return path;
  }

  if (!try_replace_workshop_root(canonicalized)) {
    for (size_t i = 0; i < std::size(parent_replacements) &&
                       !reparent(canonicalized, parent_replacements[i]);
         ++i) {
    }
  }

#ifndef NDEBUG
  const std::string path_str = utils::string::convert(path.native());
  const std::string canonicalized_str =
      utils::string::convert(canonicalized.native());
  game::trace("[Path] Normalized \"{}\" -> \"{}\"", path_str,
              canonicalized_str);
#endif

  return canonicalized;
}

} // namespace path
