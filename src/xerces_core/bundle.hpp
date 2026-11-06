#pragma once

// The diagnostics bundle (docs/DESIGN.md 5.10): one zip with what a bug report
// needs, built from <home> only. Every text file in it has the user's profile
// folder replaced by %USERPROFILE% (redact_profile) and server addresses
// replaced (redact_addresses_in_json, which also looks for the config's
// favourite and recent server addresses, and its favourites' names, word for
// word), the config loses the player name and keeps only counts of its
// favourite and recent servers, and FILES.txt lists what is inside, so the
// player can see it before sharing. The bundle is never uploaded; crash
// reports are a separate, smaller thing (crash_report.hpp, D21).
//
// Inside: the newest logs (max_logs of them, with the newest of each role,
// the launcher's and the game's among them), the launch journal, the plan
// the journal names, the signature report, the launcher config, and
// system.json (Windows build, CPU, RAM, GPUs and displays, Wine or Proton,
// Xerces version). Minidumps are not included yet.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
// A zip archive with stored (uncompressed) entries: no compression library,
// readable by every zip tool.
class zip_writer {
public:
  void add(std::string_view name, std::string_view bytes);
  // The finished archive.
  std::string finish() const;
  std::size_t entries() const { return entries_.size(); }

private:
  struct entry {
    std::string name;
    std::uint32_t crc = 0;
    std::uint32_t size = 0;
    std::uint32_t offset = 0;
  };
  std::vector<entry> entries_;
  std::string data_;
};

std::uint32_t crc32(std::string_view bytes);

// What this PC is (Windows build, CPU, RAM, GPUs, displays, Wine), as JSON.
std::string system_info_json();

struct bundle_result {
  bool ok = false;
  std::filesystem::path file;
  std::vector<std::string> contents; // names inside the zip
  std::string error;
};

// Writes the bundle to out (a .zip). profile is the folder to redact
// (%USERPROFILE%); system_json is system_info_json() (passed in for tests).
bundle_result make_bundle(const std::filesystem::path &home,
                          const std::filesystem::path &out,
                          std::string_view profile,
                          std::string_view system_json,
                          std::size_t max_logs = 6);
} // namespace xerces_core
