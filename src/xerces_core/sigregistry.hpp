#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics.hpp"
#include "sigscan.hpp"

namespace xerces_core {
// The signature registry file, compat/signatures.json (DESIGN.md 4.3):
//
//   {"schema": "xerces.signatures/1",
//    "signatures": [{"id": "db.override_asset", "symbol": "DB_OverrideAsset",
//                    "kind": "function", "section": ".text",
//                    "patterns": ["?? ?? ?? ?? ?? 48 89 6C 24 10 ..."],
//                    "offset": 0, "known": {"0x6531394": "0x1423D20"},
//                    "features": ["multi_mod"]}]}
//
// "kind" is function, site or data_rip; data_rip entries also give
// "disp_offset" and "insn_end". "known" maps a build's PE checksum to the RVA
// the symbol must resolve to on that build, both as "0x" hex strings.
inline constexpr std::string_view signature_registry_schema =
    "xerces.signatures/1";

struct signature_registry {
  std::vector<signature> signatures;
  diagnostics diags;
  bool ok() const { return !diags.has_errors(); }
};

signature_registry parse_signature_registry(std::string_view text);

// A module image laid out by RVA (the mapped game, or a dump of it): its PE
// header checksum and its sections in header order, both .text sections
// included (resolve_signatures only ever scans the first).
struct image_layout {
  std::uint32_t checksum = 0;
  std::uint32_t size_of_image = 0;
  std::vector<section_view> sections;
};

std::optional<image_layout>
read_image_layout(std::span<const std::uint8_t> image,
                  std::string *error = nullptr);
} // namespace xerces_core
