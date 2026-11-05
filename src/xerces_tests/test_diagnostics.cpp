#include "harness.hpp"

#include <chrono>
#include <fstream>
#include <thread>

#include <xerces_core/diagnostics_report.hpp>

using namespace xerces_core;

TEST_CASE("diagnostics: sizes, counts, hex and times read the same everywhere") {
  CHECK_EQ(format_bytes(0), "0 bytes");
  CHECK_EQ(format_bytes(1), "1 byte");
  CHECK_EQ(format_bytes(1023), "1023 bytes");
  CHECK_EQ(format_bytes(1536), "1.5 KB");
  CHECK_EQ(format_bytes(106059256), "101.1 MB");
  CHECK_EQ(format_bytes(std::uint64_t{59} * 1024 * 1024 * 1024), "59.0 GB");
  CHECK_EQ(format_count(0), "0");
  CHECK_EQ(format_count(999), "999");
  CHECK_EQ(format_count(1000), "1,000");
  CHECK_EQ(format_count(106059256), "106,059,256");
  // The spelling compat/builds.json uses, so the value can be searched for.
  CHECK_EQ(format_hex32(0x6531394), "0x6531394");
  CHECK_EQ(format_hex32(0x6A7B6355), "0x6A7B6355");
  CHECK_EQ(format_unix_time(0), "1970-01-01 00:00:00 UTC");
  CHECK_EQ(format_unix_time(0x6A7B6355), "2026-08-11 18:00:53 UTC");
  CHECK_EQ(short_hash("sha256:51ca63bbc660e0826943"), "51ca63bbc660");
  CHECK_EQ(short_hash("51ca63bbc660e0826943", 8), "51ca63bb");
  CHECK_EQ(short_hash("abc"), "abc");
}

TEST_CASE("diagnostics: the report aligns fields and hides the profile") {
  report_section build;
  build.title = "Game";
  build.fields = {{"Folder", "C:\\Users\\Ginld\\Games\\BO3"},
                  {"Steam build", "24784313"},
                  {"Error", ""}};
  report_section sigs;
  sigs.title = "Signatures";
  sigs.fields = {{"Matched", "11/12"}};
  sigs.lines = {"Missing: db.override_asset"};
  const std::string text = format_report({build, sigs}, "C:\\Users\\Ginld");
  CHECK_EQ(text,
           "Xerces diagnostics report\r\n"
           "=========================\r\n"
           "\r\n"
           "Game\r\n"
           "----\r\n"
           "Folder       %USERPROFILE%\\Games\\BO3\r\n"
           "Steam build  24784313\r\n"
           "Error        -\r\n"
           "\r\n"
           "Signatures\r\n"
           "----------\r\n"
           "Matched  11/12\r\n"
           "  Missing: db.override_asset\r\n");
  // Forward slashes and other letter case are the same folder.
  const std::string mixed =
      format_report({{"Logs", {{"File", "c:/users/GINLD/x/l.jsonl"}}, {}}},
                    "C:\\Users\\Ginld");
  CHECK(mixed.find("Ginld") == std::string::npos);
  CHECK(mixed.find("GINLD") == std::string::npos);
  CHECK(mixed.find("%USERPROFILE%/x/l.jsonl") != std::string::npos);
  // No profile known: nothing is replaced.
  const std::string plain =
      format_report({{"A", {{"B", "C:\\Users\\x"}}, {}}}, "");
  CHECK(plain.find("C:\\Users\\x") != std::string::npos);
}

TEST_CASE("diagnostics: crash bundles are listed newest first") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "xerces_tests_crash";
  fs::remove_all(dir);
  CHECK(list_crash_entries(dir).empty()); // missing folder: no crashes
  fs::create_directories(dir / "bundle-b");
  std::ofstream(dir / "older.dmp", std::ios::binary) << std::string(2048, 'x');
  std::ofstream(dir / "bundle-b" / "minidump.dmp", std::ios::binary)
      << std::string(100, 'y');
  std::ofstream(dir / "bundle-b" / "log.jsonl", std::ios::binary)
      << std::string(28, 'z');
  std::ofstream(dir / "newest.txt", std::ios::binary) << "crash";
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(dir / "older.dmp", now - std::chrono::hours(48));
  fs::last_write_time(dir / "bundle-b", now - std::chrono::hours(1));
  fs::last_write_time(dir / "newest.txt", now);

  const auto entries = list_crash_entries(dir);
  REQUIRE_EQ(entries.size(), std::size_t{3});
  CHECK_EQ(entries[0].name, "newest.txt");
  CHECK_EQ(entries[0].size, std::uint64_t{5});
  CHECK(!entries[0].folder);
  CHECK_EQ(entries[1].name, "bundle-b");
  CHECK(entries[1].folder);
  CHECK_EQ(entries[1].size, std::uint64_t{128});
  CHECK_EQ(entries[2].name, "older.dmp");
  CHECK_EQ(entries[2].size, std::uint64_t{2048});
  // Times are real seconds since 1970, about two days apart here.
  const std::int64_t span = entries[0].modified - entries[2].modified;
  CHECK(span > 47 * 3600 && span < 49 * 3600);
  const std::int64_t wall = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
  CHECK(entries[0].modified > wall - 600 && entries[0].modified < wall + 600);
  fs::remove_all(dir);
}
