#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
// A byte pattern with wildcards: "48 8B 05 ?? ?? ?? ?? 48 85 C0" ("?" works
// too).
struct byte_pattern {
  std::vector<std::uint8_t> bytes;
  std::vector<bool> fixed; // false where the pattern has a wildcard
};

std::optional<byte_pattern> parse_pattern(std::string_view text,
                                          std::string *error = nullptr);

// Offsets of matches, at most `limit` of them (2 is enough to know a match is
// not unique).
std::vector<std::size_t> find_matches(std::span<const std::uint8_t> haystack,
                                      const byte_pattern &pattern,
                                      std::size_t limit = 2);

enum class symbol_kind {
  function, // the match is the function (plus offset)
  data_rip, // a RIP-relative operand in the match points at the data
  site,     // a mid-function location, such as a call site
};

// One entry of the signature registry (DESIGN.md 4.3).
struct signature {
  std::string id;     // "db.override_asset"
  std::string symbol; // "DB_OverrideAsset"
  symbol_kind kind = symbol_kind::function;
  std::vector<std::string> patterns; // tried in order
  std::string section = ".text";
  std::int64_t offset = 0;      // added to the match
  std::int64_t disp_offset = 0; // data_rip: where the 32-bit displacement is
  std::int64_t insn_end = 0;    // data_rip: where the instruction ends
  std::map<std::uint32_t, std::uint64_t> known; // build checksum -> RVA
  std::vector<std::string> features;            // features that need it
};

// A section of the mapped image. Only the first ".text" is ever scanned; the
// second one is never read (DESIGN.md 4.3).
struct section_view {
  std::string name;
  std::uint64_t rva = 0;
  std::span<const std::uint8_t> bytes;
};

struct signature_result {
  std::string id;
  std::string symbol;
  bool ok = false;
  std::uint64_t rva = 0;
  std::size_t matches = 0; // of the last pattern tried
  std::string pattern;     // the pattern that matched
  std::string error;
};

struct signature_report {
  std::vector<signature_result> results;
  // feature -> signature ids it is missing; a feature with any is turned off
  std::map<std::string, std::vector<std::string>> disabled;

  const signature_result *find(std::string_view id) const;
  bool feature_enabled(std::string_view feature) const;
  // The launcher's wording (DESIGN.md 4.4).
  std::string message(std::string_view feature, std::string_view feature_label,
                      std::string_view still_works) const;
};

signature_report resolve_signatures(const std::vector<signature> &signatures,
                                    const std::vector<section_view> &sections,
                                    std::uint32_t build_checksum);

// The cache keeps each symbol's RVA and the bytes it found there; a start
// re-validates by comparing those bytes instead of scanning (DESIGN.md 4.3).
std::string signature_cache_to_json(const signature_report &report,
                                    std::uint32_t build_checksum,
                                    const std::vector<section_view> &sections);
// Nothing unless every signature is cached for this build and its bytes still
// match; then the caller scans.
std::optional<signature_report>
load_signature_cache(std::string_view json, std::uint32_t build_checksum,
                     const std::vector<section_view> &sections,
                     const std::vector<signature> &signatures);
} // namespace xerces_core
