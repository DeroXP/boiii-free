#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diagnostics.hpp"
#include "planner.hpp"
#include "semver.hpp"

namespace xerces_core {
// A pack (DESIGN.md 8.1): a named, version-pinned plan template. Packs
// reference mods and never contain them: no files, paths or user names.
struct pack_mod {
  std::string id;
  version ver;
  std::optional<std::uint64_t> workshop_id;
  std::string content_hash;
  std::map<std::string, std::string> settings; // key -> canonical JSON
};

struct pack_map {
  std::string id;
  std::string content_hash;
};

// Choices the player made once and the pack remembers (6.3).
struct pack_decisions {
  std::set<std::pair<std::string, std::string>> allow_conflicts; // sorted pairs
  std::set<std::string> allow_overrides; // mods allowed undeclared overrides
  std::set<std::string> keep_order;      // "type:name" decided by order
  bool map_scripts_first = true;
};

struct pack {
  std::int64_t schema = 1;
  std::string id; // UUID
  std::string name;
  std::string mode = "zm";
  std::optional<pack_map> map;
  std::vector<pack_mod> mods;
  std::vector<std::string> order; // highest priority first
  std::string requires_xerces;
  std::string game_build;
  pack_decisions decisions;
};

struct pack_result {
  pack value;
  diagnostics diags;
  bool ok() const { return !diags.has_errors(); }
};

pack_result parse_pack(std::string_view text);
std::string pack_to_json(const pack &value); // canonical

inline constexpr std::string_view share_code_prefix = "XRC1.";
inline constexpr std::size_t share_code_max_json = 256 * 1024;

// "XRC1." + base64url of the zstd-compressed canonical JSON (8.2).
std::string encode_share_code(const pack &value);
pack_result decode_share_code(std::string_view code);

// A pack becomes a plan request with pinned versions and saved decisions.
plan_request pack_to_request(const pack &value, const std::string &game_build,
                             const version &xerces_version);
// Applying a pack checks the installed content is what the pack pinned (8.3).
diagnostics verify_pack_content(const pack &value, const plan &resolved);
// Exports a resolved plan as a pack.
pack pack_from_plan(const plan &resolved, const std::string &name,
                    const std::string &id,
                    const std::map<std::string, std::uint64_t> &workshop_ids);

std::string new_pack_id(); // random UUID v4
bool valid_uuid(std::string_view text);

// Base64url without padding (RFC 4648 section 5).
std::string base64url_encode(std::string_view bytes);
std::optional<std::string> base64url_decode(std::string_view text);
} // namespace xerces_core
