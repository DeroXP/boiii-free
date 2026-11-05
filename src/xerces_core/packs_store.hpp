#pragma once

// The launcher's pack library (DESIGN.md 8.1 to 8.3): one canonical JSON file
// per pack in <home>\config\packs\<pack id>.json, plus the checks the Packs
// screen shows (is every pinned mod installed as pinned?) and the rules for
// importing and naming. Headless: no UI, no game headers, no network.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics.hpp"
#include "pack.hpp"
#include "planner.hpp"

namespace xerces_core {
// Pack files people import are small; anything bigger is not a pack. Matches
// the share-code limit so a file and a code accept the same packs.
inline constexpr std::uintmax_t pack_file_max_bytes = share_code_max_json;
inline constexpr std::size_t pack_name_max_chars = 80;

struct library_entry {
  pack value;
  std::filesystem::path file;
};

struct pack_library {
  // By name (case-insensitive), then id, so the list does not jump around.
  std::vector<library_entry> packs;
  // Files that could not be read or are not valid packs, by file name. They
  // are left on disk untouched.
  diagnostics diags;
  const library_entry *find(std::string_view id) const;
};

// <folder>\<id>.json.
std::filesystem::path pack_library_file(const std::filesystem::path &folder,
                                        std::string_view id);

// Every <uuid>.json in folder. A folder that does not exist is an empty
// library, not an error.
pack_library read_pack_library(const std::filesystem::path &folder);

// Checks the pack the way an importer would (parse_pack over its canonical
// JSON) and writes it as <folder>\<id>.json through a temporary file. False,
// with the reasons in diags, when it is not a valid pack or the write failed.
bool write_pack(const std::filesystem::path &folder, const pack &value,
                diagnostics &diags);

// Removes <folder>\<id>.json and every other .json file in folder that holds
// a pack with this id (copies would bring it back on the next read). False if
// none was there or none could go.
bool remove_pack(const std::filesystem::path &folder, std::string_view id);

// A file someone gave the player: pack JSON or a share code saved as text.
pack_result read_pack_text(std::string_view text);
// What the player pasted into the share-code field: pack JSON when it starts
// with '{', else a share code, so a mistyped or unknown code gets the
// share-code reasons rather than a JSON parse error.
pack_result read_pasted_pack(std::string_view text);
// The same for a file on disk, refusing missing, unreadable and oversized
// files with a plain message.
pack_result read_pack_file(const std::filesystem::path &file);

// Where an imported pack goes. Its own id when that is free or already holds
// the very same pack; a fresh id when the id holds a different pack, so an
// import never overwrites a pack the player has.
enum class import_kind { added, duplicate, renumbered };
struct import_placement {
  pack value;
  import_kind kind = import_kind::added;
};
import_placement place_import(const pack_library &library, pack incoming,
                              const std::function<std::string()> &make_id =
                                  new_pack_id);

// Trims spaces at both ends.
std::string trim_pack_name(std::string_view name);
// Empty when the (trimmed) name is fine, else a sentence for the player.
std::string pack_name_problem(std::string_view name);
// A pack with this name (ignoring case and outer spaces), if any.
const library_entry *find_by_name(const pack_library &library,
                                  std::string_view name);

// Black Ops III colour codes (^0 to ^9) in Workshop titles, removed for
// display: "^3PZM^7 Pegasus" is "PZM Pegasus".
std::string strip_colour_codes(std::string_view text);
// A file name for exporting a pack: letters, digits, space, '-', '_' and '.'
// kept, anything else (path separators, ':' and the like) replaced; never
// empty. Without the extension.
std::string pack_file_stem(std::string_view name);
// The file an export writes, from the name typed in the save picker: kept
// when it already ends in ".json" (any case), else given ".xpack.json"
// (".json" after a typed ".xpack"). Judged on the end of the name, because
// std::filesystem sees ".2" as the extension of "Zombies v1.2".
std::filesystem::path with_pack_extension(const std::filesystem::path &file);

// How each pinned mod (and the map) compares with what is installed.
enum class pack_item_state {
  installed,     // the pinned version with the pinned files
  missing,       // no version of it is installed
  other_version, // installed, but not the pinned version
  changed,       // the pinned version, but its files differ (an update)
};
const char *to_string(pack_item_state state);

struct pack_item_check {
  std::string id;
  std::string name;      // the installed title, else the id
  bool is_map = false;
  std::string pinned;    // the pinned version ("" for the map)
  std::string installed; // the installed version, when it differs
  pack_item_state state = pack_item_state::missing;
  // Where the player can get it: the pack's Workshop id, else one read from
  // a "ws.<number>" id or the installed manifest.
  std::optional<std::uint64_t> workshop_id;
};

struct pack_check {
  std::vector<pack_item_check> items; // the mods in load order, then the map
  std::size_t missing = 0, other_version = 0, changed = 0;
  bool ready() const { return missing + other_version + changed == 0; }
  std::size_t problems() const { return missing + other_version + changed; }
};

pack_check check_pack(const pack &value,
                      const std::vector<installed_content> &catalog);
} // namespace xerces_core
