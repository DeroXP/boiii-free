#pragma once

// The Diagnostics screen's report (DESIGN.md 5.10, 7.3): the plain text a
// player pastes into a bug report, and the small formatting helpers the screen
// and the report share so both say the same thing. Headless: no UI, no game
// headers.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
struct report_field {
  std::string name;
  std::string value;
};

// One block of the report: aligned "name  value" lines, then free lines
// (lists such as missing signatures or catalog diagnostics).
struct report_section {
  std::string title;
  std::vector<report_field> fields;
  std::vector<std::string> lines;
};

// The whole report as text with Windows line endings (it goes to the
// clipboard and into Windows text boxes). The profile folder is replaced with
// %USERPROFILE% everywhere, the same rule as the logs (15.1), so the report
// can be shared as it is.
std::string format_report(const std::vector<report_section> &sections,
                          std::string_view profile);

// 512 bytes, 1.5 KB, 101.1 MB, 2.3 GB (1024-based, one decimal).
std::string format_bytes(std::uint64_t bytes);
// 106,059,256: exact sizes stay readable.
std::string format_count(std::uint64_t value);
// 0x6531394, the way compat/builds.json writes checksums and link times, so a
// value can be searched for in that file.
std::string format_hex32(std::uint32_t value);
// Seconds since 1970 as "2026-08-04 14:03:01 UTC" (PE link times).
std::string format_unix_time(std::int64_t seconds);
// The first length hex digits, without a "sha256:" prefix.
std::string short_hash(std::string_view hash, std::size_t length = 12);

// A file or folder in <home>\crash.
struct crash_entry {
  std::string name;
  std::uint64_t size = 0; // a folder: the files inside it
  std::int64_t modified = 0; // seconds since 1970, UTC
  bool folder = false;
};

// Everything in the crash folder, newest first (then by name). A missing or
// unreadable folder is simply empty: no crash has been recorded.
std::vector<crash_entry> list_crash_entries(const std::filesystem::path &dir);

// %USERPROFILE% as UTF-8, empty when it is not set.
std::string profile_folder();
} // namespace xerces_core
