#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics.hpp"

namespace xerces_core {
// What the pre-flight check compares (DESIGN.md 4.2).
struct exe_facts {
  std::uint64_t size = 0;
  std::uint32_t checksum = 0;  // optional header CheckSum
  std::uint32_t link_time = 0; // file header TimeDateStamp
  std::string sha256;          // lower-case hex
};

// Reads the PE header and hashes the file in pieces.
std::optional<exe_facts> read_exe_facts(const std::filesystem::path &exe,
                                        std::string *error = nullptr);

enum class build_status { supported, upstream_only, unsupported };

// One entry of compat/builds.json (DESIGN.md 4.6). A field left out matches
// any value, so a build known only by its checksum can still be listed.
struct known_build {
  std::string name;
  std::string exe = "BlackOps3.exe";
  std::optional<std::uint64_t> steam_build;
  std::uint32_t checksum = 0;
  std::optional<std::uint64_t> size;
  std::optional<std::uint32_t> link_time;
  std::optional<std::string> sha256;
  build_status status = build_status::unsupported;
};

// compat/builds.json:
//   {"schema": "xerces.builds/1",
//    "builds": [{"name": "September 2026 Steam client", "exe": "BlackOps3.exe",
//                "steam_build": 24784313, "checksum": "0x6531394",
//                "size": 106059256, "link_time": "0x6A7B6355",
//                "sha256": "51ca...", "status": "supported"}]}
// status is supported, upstream-only or unsupported.
inline constexpr std::string_view build_registry_schema = "xerces.builds/1";

struct build_registry {
  std::vector<known_build> builds;
  diagnostics diags;
  bool ok() const { return !diags.has_errors(); }
};

build_registry parse_build_registry(std::string_view text);

enum class preflight_verdict {
  supported,     // launch
  upstream_only, // launch; some Xerces features may be off (4.4)
  unsupported,   // a build listed as unsupported
  newer,         // Steam updated the game past every listed build
  unknown,       // not a listed build, or its files changed
};

const char *to_string(build_status status);
const char *to_string(preflight_verdict verdict);

struct preflight_result {
  preflight_verdict verdict = preflight_verdict::unknown;
  std::optional<known_build> build; // the entry that matched
  std::string message; // what the launcher says; empty if supported
  bool can_launch() const {
    return verdict == preflight_verdict::supported ||
           verdict == preflight_verdict::upstream_only;
  }
};

// steam_build is the installed game's "buildid" from its appmanifest, when
// known. The most specific matching entry wins (most fields given).
preflight_result preflight_check(const exe_facts &facts,
                                 std::optional<std::uint64_t> steam_build,
                                 const build_registry &registry,
                                 std::string_view exe_name = "BlackOps3.exe");
} // namespace xerces_core
