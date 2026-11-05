#pragma once

// Asset pool limits, headless (docs/DESIGN.md 5.4 step 4): what a mod's or
// map's assetlimits.txt or assetpools.txt asks for, the client's own pool
// sizes and their launcher_settings.json overrides, and the largest size each
// pool needs for a set of mods and maps. The rules are upstream's client's,
// so the numbers are the ones the game uses today:
//
// - src/client/component/asset_limits.cpp: parse_list, DEFAULT_POOL_CONFIGS,
//   is_enabled and get_pool_size;
// - src/client/game/impl/ugc/ugc.cpp: UGC_LoadItem_PrepareAssetPool, which
//   finds the file when a mod loads (UGC_LoadModByPublisherId_Impl, for the
//   main mod and each stacked one) and when a custom map loads
//   (UGC_LoadUsermapByPublisherId_Impl);
// - src/client/game/impl/db/db.cpp: reallocate_asset_pool only grows a pool,
//   never past INT32_MAX entries, and never one of the fixed pools.
//
// Sizing the pools in the game and blocking growth while assets are live is
// not here; this makes the numbers known.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
// The XAssetType values the client takes from a pool file: valid_xassettype
// in src/client/game/structs/db/xasset/core.hpp accepts PHYSPRESET (0) up to,
// not including, XAssetType::COUNT (0x67).
inline constexpr std::int32_t asset_type_count = 0x67;

// One line of a pool file: grow pool `type` to `size` entries.
struct pool_limit {
  std::int32_t type = 0;  // XAssetType
  std::uint32_t size = 0; // entries
  bool operator==(const pool_limit &) const = default;
};

// A pool file's text read as the client's asset_limits::parse_list reads it,
// lines in file order (a type may repeat; the client applies each line, and a
// pool only grows):
//
// - Line breaks: "\r\n" becomes "\n", every other "\r" is dropped (it does
//   not end a line), tabs become spaces. Lines are split at '\n'.
// - Parts: a line is split at every single space, so two spaces in a row make
//   an empty part between them and a space at either end makes an empty first
//   or last part. A line with fewer than two parts is skipped.
// - The first part is the type, the last part the size; parts between them
//   are ignored. That reads both the T7Recharged form "int_type_value
//   string_type_name default_pool_size extended_pool_size" and the minimal
//   "int_type_value extended_pool_size".
// - Numbers are whole parts: decimal, or hex after a lower-case "0x" ("0X" is
//   not hex). The type is a signed 32-bit number below asset_type_count and
//   at least 0; the size an unsigned 32-bit number with no sign. A line whose
//   type or size does not read is skipped.
//
// Upstream's utils::string::replace, which does the line-break and tab
// normalisation, always replaces at the start of the text before it searches
// (its loop runs the body once with position 0). So the first two characters
// of every file are dropped and the third becomes a space, whatever they are,
// line breaks included. The text then starts with a space, which empties the
// type part of whatever line comes first: a file's first line never counts,
// and when it is shorter than three characters the damage reaches into the
// line after it. This mirrors that, so the numbers match the game; the tests
// pin it.
std::vector<pool_limit> parse_pool_limits(std::string data);

// The names the client looks for. The first file found with either name is
// the mod's or map's pool file, whatever is in it.
inline constexpr std::string_view pool_file_names[] = {"assetlimits.txt",
                                                       "assetpools.txt"};

// Where a mod's or map's folder came from, which decides where the client
// looks.
enum class content_layout {
  // <game>\mods\<name> or <game>\usermaps\<name>: the client's entry points
  // at <name>\zone when that is a folder, else at <name> (ugc.cpp,
  // va_mods_path and va_user_content_path in workshop.cpp; the disk
  // fallback's populate_workshop_paths uses zone too).
  game_folder,
  // A Workshop item (steamapps\workshop\content\311210\<id>), mod or map:
  // the entry points at the item folder itself (workshop.cpp,
  // supplement_ugc_from_workshop).
  workshop,
};

// The folder the client searches for a mod's or map's pool file.
std::filesystem::path pool_search_root(const std::filesystem::path &folder,
                                       content_layout layout);

// The file UGC_LoadItem_PrepareAssetPool reads: the first entry under root
// that is not a folder and is named exactly (case matters) one of
// pool_file_names, in std::filesystem::recursive_directory_iterator order,
// which is what upstream's utils::io::list_files walks. That order is the
// file system's: NTFS lists a folder's names in its upper-case collation
// order and the iterator enters a folder where it lists it, so
// "a\assetlimits.txt" comes before "assetpools.txt"; FAT and network drives
// may list in another order. Nothing when root is not a folder or holds no
// such file. Upstream's walk throws on an error midway; this one stops.
std::optional<std::filesystem::path>
find_pool_file(const std::filesystem::path &root);

// Pool files larger than this are not read (a real one is a few kilobytes).
// The client reads any size, so the catalog marks such a file unread
// (installed_content::pool_file_unread) rather than as asking for nothing.
inline constexpr std::uintmax_t max_pool_file_bytes = std::uintmax_t{1} << 20;

// The largest size the client grows a pool to: reallocate_asset_pool
// (db.cpp) grows only while the pool's count is below
// static_cast<int32_t>(new_size), which is negative for anything larger.
inline constexpr std::uint32_t max_pool_size = 0x7FFFFFFF;

// One pool the client sizes at DB_AssetPoolInit (DEFAULT_POOL_CONFIGS).
struct asset_pool_info {
  std::int32_t type;
  std::string_view name;        // the XAssetType name: "XMODEL"
  std::string_view label;       // upstream launcher's label: "XModel"
  std::string_view setting_key; // launcher_settings.json key: "ap_xmodel"
  std::uint32_t default_size;   // upstream's default
  // The client never grows it, from settings or pool files:
  // reallocate_asset_pool (db.cpp) returns first for is_fixed_pool's types,
  // which the engine sizes itself (IMAGE, XMODELMESH and the three world
  // pools), and for the two DB_GetXAssetTypeSize_Impl has no entry size for
  // (it gives -1 for UI_MAP and WEAPON_FULL).
  bool fixed;
};

// Every pool in DEFAULT_POOL_CONFIGS, in type order (one per type below
// asset_type_count).
std::span<const asset_pool_info> asset_pools();
// The pool of a type; nullptr outside 0 to asset_type_count - 1.
const asset_pool_info *find_asset_pool(std::int32_t type);

// launcher_settings.json (<game>\boiii_players\user) as the client's
// asset_limits reads it: parsed whole (a broken file or one that is not an
// object reads as {}), the first member of each name, strings only.
struct pool_settings {
  // is_enabled: asset_limits_enabled missing, empty or "1", and
  // disable_asset_pools not "1". When off the client sizes no pool at
  // DB_AssetPoolInit; mods' pool files still grow pools when they load.
  bool enabled = true;
  // Each name's first member as get_setting sees it: a string up to its
  // first NUL, any other value "" (the same as a missing one).
  std::map<std::string, std::string, std::less<>> values;
};
pool_settings parse_pool_settings(std::string_view launcher_settings_json);

// get_pool_size: the setting_key's value read by std::stoul (leading spaces,
// a sign and trailing text allowed, base 10), used when it is at most
// INT32_MAX, else the pool's default. 0 for a type with no pool.
std::uint32_t pool_size_setting(const pool_settings &settings,
                                std::int32_t type);

// The size a pool has before any mod asks for more, as far as Xerces can
// tell: pool_size_setting when the client's pools are on. Nothing when the
// game keeps its own size, which Xerces does not know: the pools are off,
// the pool is fixed, or the setting is 0 (growing to 0 changes nothing).
std::optional<std::uint32_t> default_pool_size(const pool_settings &settings,
                                               std::int32_t type);

// One mod's or map's pool limits, for largest_limits.
struct mod_pool_limits {
  std::string id;
  std::span<const pool_limit> limits;
};

// One pool a set of mods (and maps) needs.
struct pool_need {
  std::int32_t type = 0;
  std::uint32_t size = 0; // the largest size any of them asks for
  std::string mod; // the mod or map asking for it: the first given, on a tie
  std::optional<std::uint32_t> default_size; // default_pool_size
  bool fixed = false;                        // asset_pool_info::fixed
  // The client grows the pool for it: not fixed, not 0, and larger than the
  // default (or the default is the game's own size, which Xerces does not
  // know).
  bool raises = false;
};

// Per type, the largest size any of them asks for, by type. Every type one
// asks for is listed, also when the default is larger already, but a size
// above max_pool_size is left out: the client never applies it, so it is no
// one's largest.
std::vector<pool_need> largest_limits(std::span<const mod_pool_limits> mods,
                                      const pool_settings &settings = {});
} // namespace xerces_core
