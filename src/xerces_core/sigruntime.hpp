#pragma once

// Signature rules the game client applies (src/client/xerces/signatures.cpp
// and src/client/component/mod_stack.cpp), kept here so they can be tested
// without the game (docs/DESIGN.md 4.3, 4.4).

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sigscan.hpp"

namespace xerces_core {
// The registry entries mod_stack hooks on the client. compat/signatures.json
// is a data file that can be swapped (XERCES_COMPAT_PATH, an older checkout),
// so mod_stack looks every one of them up before it patches anything and
// keeps multi-mod off if one is missing. A new mod_stack hook is added here
// as well, so the registry test checks it is in the shipped registry.
namespace multi_mod_signatures {
inline constexpr std::string_view expand_zone_list_call =
    "db.load_xassets.expand_zone_list_call";
inline constexpr std::string_view load_xzone_call =
    "db.load_xassets.load_xzone_call";
inline constexpr std::string_view build_os_path_for_thread =
    "fs.build_os_path_for_thread";
inline constexpr std::string_view link_xasset_entry = "db.link_xasset_entry";
inline constexpr std::string_view override_asset = "db.override_asset";
inline constexpr std::string_view key_value_pairs_get_next_value =
    "com.key_value_pairs_get_next_value";
inline constexpr std::string_view start_header_read = "snd.start_header_read";
inline constexpr std::string_view render_loaded_mod_label_call =
    "ui.render_loaded_mod.label_call";
inline constexpr std::string_view mod_change_apply_queued =
    "ugc.mod_change_apply_queued";

inline constexpr std::array all = {
    expand_zone_list_call,    load_xzone_call,
    build_os_path_for_thread, link_xasset_entry,
    override_asset,           key_value_pairs_get_next_value,
    start_header_read,        render_loaded_mod_label_call,
    mod_change_apply_queued,
};
} // namespace multi_mod_signatures

// The client's cache file for a game build and the registry text it was made
// from: "signatures-0x6531394-<first 16 hex digits of the text's
// SHA-256>.json". The game build alone is not enough: a registry that fixes a
// pattern or adds a known address on the same build must scan again instead
// of reusing addresses the old registry found, and two checkouts with
// different registries that share one <home> each keep their own cache.
std::string signature_cache_name(std::uint32_t build_checksum,
                                 std::string_view registry_text);

// Whether a report read from the cache still agrees with the registry: every
// registry entry is cached, every cached id is a registry entry, and no cached
// address differs from the entry's known address for this build (the rule of
// DESIGN.md 4.3 holds on a cache hit, not only after a scan). On false, *why
// names the first disagreement and the caller scans again.
bool cached_signatures_agree(const signature_report &cached,
                             const std::vector<signature> &signatures,
                             std::uint32_t build_checksum,
                             std::string *why = nullptr);
} // namespace xerces_core
