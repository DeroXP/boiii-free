#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics.hpp"
#include "semver.hpp"

namespace xerces_core {
enum class content_kind { mod, map };

const char *to_string(content_kind kind);

// An asset as the engine names it: type (scriptparsetree, stringtable, rawfile,
// localize, weapon, ...) and a normalized name (lower case, forward slashes).
struct asset_ref {
  std::string type;
  std::string name;
  std::string reason;
  std::vector<int> key; // stringtable key columns, for "extends"
};

struct dependency_ref {
  std::string id;
  std::string range_text = "*";
  version_range range = version_range::any();
  bool optional = false;
};

struct conflict_ref {
  std::string id;
  std::string reason;
};

// "after" means load after (rank above) the listed mods, "before" the reverse.
struct order_hints {
  std::vector<std::string> after;
  std::vector<std::string> before;
  std::int64_t priority = 0;
};

enum class hook_kind { before, after, wrap, replace };

const char *to_string(hook_kind kind);

struct hook_decl {
  std::string target;  // "scripts/zm/_zm_perks.gsc::give_perk"
  std::string handler; // "scripts/zm/vela_perks.gsc::on_give_perk"
  hook_kind kind = hook_kind::wrap;
};

struct capabilities {
  bool native_code = false;
  bool lua_unsafe = false;
  std::vector<std::string> lua_net;
  std::vector<std::string> lua_files;
  bool gsc_exec_command = false;
  bool gsc_files = false;
  bool gsc_replacefunc = false;
};

struct setting_decl {
  std::string key;
  std::string type; // bool, int, float, string, enum
  std::string default_json;
  std::optional<double> min;
  std::optional<double> max;
  std::vector<std::string> options;
  std::string label;
  std::string dvar;
  std::string scope = "server"; // server settings go into the content id
};

struct manifest {
  std::int64_t schema = 1;
  std::string id;
  content_kind kind = content_kind::mod;
  std::string name;
  version ver;
  std::vector<std::string> authors;
  std::string license;
  std::string requires_xerces_text;
  std::optional<version_range> requires_xerces;
  std::vector<std::string> modes; // empty: every mode
  std::optional<std::uint64_t> workshop_id;
  std::vector<dependency_ref> dependencies;
  std::vector<conflict_ref> conflicts;
  order_hints order;
  std::vector<asset_ref> provides;
  std::vector<asset_ref> overrides;
  std::vector<asset_ref> extends;
  std::vector<std::string> entry_scripts;
  std::vector<hook_decl> hooks;
  capabilities caps;
  std::vector<setting_decl> settings;
  std::map<std::string, std::string> integrity; // file -> "sha256:<hex>"
  bool legacy = false;
  std::vector<std::string> unknown_fields; // JSON pointers, kept, warned

  bool supports_mode(std::string_view mode) const;
  bool declares_override(std::string_view type, std::string_view name) const;
};

struct manifest_result {
  manifest value;
  diagnostics diags;
  bool ok() const { return !diags.has_errors(); }
};

inline constexpr std::string_view manifest_file_name = "xerces.mod.json";
inline constexpr std::size_t mod_stack_name_limit = 37;

bool valid_mod_id(std::string_view id);
bool valid_mode(std::string_view mode);
std::string normalize_asset_name(std::string_view name);

manifest_result parse_manifest(std::string_view text);

// Canonical JSON for a manifest (synthesized ones, templates, tests).
std::string manifest_to_json(const manifest &value);

// Mods without a manifest (every Workshop mod today) get one, marked legacy.
struct legacy_input {
  std::string folder;                        // folder name on disk
  std::optional<std::string> workshop_json;  // contents, if present
  std::vector<std::string> files;            // relative paths, '/' separated
  std::string content_hash;                  // "sha256:..." over the files
  std::optional<std::uint64_t> time_updated; // from Steam, when known
};

manifest synthesize_legacy(const legacy_input &input,
                           const std::set<std::string> &taken_ids,
                           diagnostics *diags = nullptr);

// A title without the game's colour codes ("^1" to "^9", "^0"), which
// Workshop titles often carry: "^6DS4C ^5Patched" -> "DS4C Patched". For
// display only; ids and hashes never change.
std::string strip_color_codes(std::string_view text);

// "Type" from workshop.json, when it is a map.
bool workshop_json_is_map(std::string_view workshop_json);
} // namespace xerces_core
