#pragma once

// Signature rules the game client applies (src/client/xerces/signatures.cpp
// and src/client/component/mod_stack.cpp), kept here so they can be tested
// without the game (docs/DESIGN.md 4.3, 4.4).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sigscan.hpp"

namespace xerces_core {
// The registry entries mod_stack hooks on the client. compat/signatures.json
// is a data file that can be swapped (XERCES_COMPAT_PATH, an older checkout),
// so the client requires every one of them in its report (require_signatures
// below) and mod_stack looks each up again before it patches anything; either
// way multi-mod stays off if one is missing. A new mod_stack hook is added
// here as well, so the registry test checks it is in the shipped registry.
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

// The engine's out-of-memory handler, which the client's xerces_out_of_memory
// component replaces (feature "oom_report", DESIGN.md 5.6). A miss turns only
// that feature off: the game then ends as the engine ends it, without a
// message. Required the same way as the multi-mod ids.
namespace oom_report_signatures {
inline constexpr std::string_view out_of_mem_error =
    "sys.out_of_mem_error_internal";

inline constexpr std::array all = {out_of_mem_error};
} // namespace oom_report_signatures

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

// Turns `feature` off in a finished report unless every id in `ids` resolved
// (DESIGN.md 4.4: each feature declares the symbols it needs). The registry's
// "features" tags only turn a feature off when an entry tagged with it
// misses; an entry the feature uses that the registry lacks, or tags for
// another feature, would leave it on while its hooks are not installed. After
// this the report says it is off, so everything that reads it agrees: the
// client's feature_enabled, the pipe's status, the loader's plan check and
// the launcher's "Multi-mod was off" warning (it reads the report's
// "disabled" map). An id the report has no entry for gets a failed one with
// the error `absent`, so the log and the launcher name it. Nothing is added
// when every id resolved, not even an empty list: the launcher treats any
// "disabled" key as off. Returns the ids added to report.disabled[feature].
std::vector<std::string>
require_signatures(signature_report &report, std::string_view feature,
                   std::span<const std::string_view> ids,
                   std::string_view absent);
} // namespace xerces_core
