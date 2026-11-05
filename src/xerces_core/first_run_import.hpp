#pragma once

// The read-only BOIII import of the first-run screen (docs/DESIGN.md 3.5,
// 7.3). Upstream BOIII keeps the player name in <game>\boiii_players\
// properties.json and the server lists in <game>\boiii_players\user\
// (favorite_servers.txt, recent_servers.txt, one "a.b.c.d:port" per line).
// Only what maps onto the launcher config is offered: the player name, the
// favourite servers and the recently joined servers.
//
// Nothing here writes, moves or deletes a BOIII file. %LOCALAPPDATA%\boiii\user
// holds the player's identity key (read by the fenced auth code), so every
// read refuses any path inside it, even through a link, and checking a path
// never opens anything inside it either (first_run_identity.cpp).

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics.hpp"
#include "launcher_config.hpp"

namespace xerces_core {
// Upstream keeps at most this many recent servers; the import does the same.
inline constexpr std::size_t boiii_recent_limit = 50;
inline constexpr std::size_t boiii_favorite_limit = 500;

// One place the import looked at, for the screen.
struct boiii_place {
  std::string kind; // "appdata" (%LOCALAPPDATA%\boiii) or "players" (boiii_players)
  std::filesystem::path path;
  bool exists = false;
};

struct boiii_import {
  std::vector<boiii_place> places;

  std::optional<std::string> player_name; // printable ASCII, trimmed
  std::filesystem::path player_name_file;

  std::vector<std::string> favorites; // host:port, file order, no duplicates
  std::filesystem::path favorites_file;

  std::vector<std::string> recent_servers; // host:port, newest first
  std::filesystem::path recent_file;

  std::size_t skipped_lines = 0; // server lines that were not host:port
  diagnostics diags; // unreadable or broken files, refused paths (warnings)

  bool found_anything() const {
    return player_name.has_value() || !favorites.empty() ||
           !recent_servers.empty();
  }
};

// Looks in boiii_appdata (%LOCALAPPDATA%\boiii) and game_dir\boiii_players.
// Either may be missing. Reads a few small files; never writes.
boiii_import find_boiii_import(
    const std::optional<std::filesystem::path> &boiii_appdata,
    const std::optional<std::filesystem::path> &game_dir);

// Where a path really leads, as far as the identity folder is concerned.
enum class boiii_identity_check {
  outside,
  inside,  // boiii_appdata\user or inside it
  unknown, // could not be checked without opening it: treated as inside
};

// Follows path one name at a time, the way Windows would (long and short
// names, any case, junctions, symbolic links, subst drives, volume mount
// points), without opening anything inside boiii_appdata\user: each name is
// looked up in its parent folder's listing and each link is read as a link.
// unknown when a step cannot be checked that way (a folder that cannot be
// listed, a link that cannot be read or points somewhere Windows-internal,
// a loop of links, a name Windows does not allow).
boiii_identity_check
check_boiii_identity_path(const std::filesystem::path &path,
                          const std::filesystem::path &boiii_appdata);

// True unless check_boiii_identity_path says outside. Such paths are never
// read. An empty boiii_appdata means there is no identity folder: false.
bool is_boiii_identity_path(const std::filesystem::path &path,
                            const std::filesystem::path &boiii_appdata);

// Upstream's name rule (printable ASCII only), trimmed; empty when nothing is
// left.
std::string sanitize_boiii_player_name(std::string_view name);

// The player name from upstream's properties.json text (a UTF-8 BOM is
// allowed, as upstream's reader allows it).
std::optional<std::string> boiii_player_name_from_properties(std::string_view text);

// "a.b.c.d:port" or "host:port" lines; blank lines are ignored, anything else
// that is not host:port is counted in *skipped. Duplicates (ignoring case) are
// dropped; at most `limit` addresses are kept.
std::vector<std::string> parse_boiii_server_list(std::string_view text,
                                                 std::size_t limit,
                                                 std::size_t *skipped = nullptr);
// An address Xerces can later join: launch's valid_server_address (launch.hpp),
// with the port required, and without 0.0.0.0 (what upstream writes for an
// address it could not parse).
bool is_server_address(std::string_view address);

struct boiii_import_choice {
  bool player_name = true;
  bool favorites = true;
  bool recent_servers = true;
};

struct boiii_import_result {
  bool player_name = false;
  std::size_t favorites = 0;      // added
  std::size_t recent_servers = 0; // added
  bool changed() const {
    return player_name || favorites > 0 || recent_servers > 0;
  }
};

// What applying would add to config: favourites and recent servers it does
// not already have (addresses compared ignoring case). Recent servers stop at
// boiii_recent_limit, as apply_boiii_import does; *no_room (when given)
// receives how many more are new but do not fit.
std::size_t new_boiii_favorites(const launcher_config &config,
                                const boiii_import &found);
std::size_t new_boiii_recent_servers(const launcher_config &config,
                                     const boiii_import &found,
                                     std::size_t *no_room = nullptr);

// Copies the chosen values into config (the caller saves it). Favourites are
// appended without duplicates; recent servers go after the ones Xerces
// already has (those are newer), up to boiii_recent_limit.
boiii_import_result apply_boiii_import(launcher_config &config,
                                       const boiii_import &found,
                                       const boiii_import_choice &choice);
} // namespace xerces_core
