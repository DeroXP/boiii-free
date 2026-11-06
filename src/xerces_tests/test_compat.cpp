// The launcher's pre-flight check (docs/DESIGN.md 4.2 and 4.6) and Steam's
// build number, on synthetic data.

#include "harness.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <xerces_core/catalog.hpp>
#include <xerces_core/compat.hpp>
#include <xerces_core/detect.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
const std::string current_sha(64, 'a');

const char *registry_json = R"({
  "schema": "xerces.builds/1",
  "builds": [
    {"name": "Current", "steam_build": 24784313, "checksum": "0x6531394",
     "size": 1000, "link_time": "0x6A7B6355",
     "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
     "status": "supported"},
    {"name": "Legacy", "checksum": "0x888C368", "status": "upstream-only"},
    {"name": "Broken", "steam_build": 24000000, "checksum": "0x1111",
     "status": "unsupported"},
    {"name": "Server", "exe": "BlackOps3_UnrankedDedicatedServer.exe",
     "checksum": "0x14C28B4", "status": "upstream-only"}
  ]
})";

exe_facts current_facts() { return {1000, 0x6531394, 0x6A7B6355, current_sha}; }

void put32(std::vector<char> &bytes, std::size_t at, std::uint32_t value) {
  std::memcpy(bytes.data() + at, &value, 4);
}
} // namespace

TEST_CASE("pre-flight: builds.json parses and reports mistakes") {
  const build_registry registry = parse_build_registry(registry_json);
  REQUIRE(registry.ok());
  REQUIRE_EQ(registry.builds.size(), 4u);
  CHECK_EQ(registry.builds[0].checksum, 0x6531394u);
  CHECK_EQ(*registry.builds[0].steam_build, 24784313u);
  CHECK_EQ(*registry.builds[0].link_time, 0x6A7B6355u);
  CHECK(registry.builds[1].status == build_status::upstream_only);
  CHECK(!registry.builds[1].sha256.has_value());
  CHECK_EQ(registry.builds[3].exe,
           std::string("BlackOps3_UnrankedDedicatedServer.exe"));

  const build_registry bad = parse_build_registry(R"({
    "schema": "xerces.builds/1",
    "builds": [
      {"checksum": "0x1", "status": "supported"},
      {"name": "a", "checksum": "106107796", "status": "supported"},
      {"name": "b", "checksum": "0x1", "sha256": "ABC", "status": "supported"},
      {"name": "c", "checksum": "0x1", "status": "maybe"},
      {"name": "d", "checksum": "0x1", "size": -4, "status": "supported"},
      {"name": "e", "checksum": "0x1", "status": "supported", "colour": 1}
    ]})");
  CHECK(!bad.ok());
  CHECK(bad.diags.has("build.name"));
  CHECK(bad.diags.has("build.checksum"));
  CHECK(bad.diags.has("build.sha256"));
  CHECK(bad.diags.has("build.status"));
  CHECK(bad.diags.has("build.number"));
  CHECK(bad.diags.has("build.key.unknown"));
  CHECK(parse_build_registry(R"({"schema":"x","builds":[]})")
            .diags.has("build.schema"));
}

TEST_CASE("pre-flight: the supported build launches") {
  const build_registry registry = parse_build_registry(registry_json);
  const preflight_result result =
      preflight_check(current_facts(), 24784313, registry);
  CHECK(result.verdict == preflight_verdict::supported);
  CHECK(result.can_launch());
  CHECK(result.message.empty());
  REQUIRE(result.build.has_value());
  CHECK_EQ(result.build->name, std::string("Current"));
}

TEST_CASE("pre-flight: a changed exe is unknown and asks for a Steam verify") {
  const build_registry registry = parse_build_registry(registry_json);
  exe_facts changed = current_facts();
  changed.sha256 = std::string(64, 'b'); // same checksum, other bytes
  const preflight_result result = preflight_check(changed, 24784313, registry);
  CHECK(result.verdict == preflight_verdict::unknown);
  CHECK(!result.can_launch());
  CHECK(result.message.find("Verify the game files in Steam") !=
        std::string::npos);
  CHECK(result.message.find("Xerces supports build 24784313.") !=
        std::string::npos);
}

TEST_CASE("pre-flight: a Steam update past every listed build says so") {
  const build_registry registry = parse_build_registry(registry_json);
  exe_facts updated{2000, 0x7777777, 0x6B000000, std::string(64, 'c')};
  const preflight_result result = preflight_check(updated, 24901234, registry);
  CHECK(result.verdict == preflight_verdict::newer);
  CHECK_EQ(result.message,
           std::string("Steam updated Black Ops III (build 24901234). Xerces "
                       "supports build 24784313. Play through Steam until a "
                       "Xerces update supports the new build. Xerces does not "
                       "check for updates yet: the tester channel announces "
                       "the next zip."));
  // Without Steam's build number an unknown exe cannot be called newer.
  CHECK(preflight_check(updated, std::nullopt, registry).verdict ==
        preflight_verdict::unknown);
}

TEST_CASE("pre-flight: builds known by checksum only, and per executable") {
  const build_registry registry = parse_build_registry(registry_json);
  const exe_facts legacy{900, 0x888C368, 0x5C000000, std::string(64, 'd')};
  const preflight_result old = preflight_check(legacy, 1234567, registry);
  CHECK(old.verdict == preflight_verdict::upstream_only);
  CHECK(old.can_launch());
  CHECK(!old.message.empty());

  const exe_facts broken{1, 0x1111, 0, std::string(64, 'e')};
  CHECK(preflight_check(broken, std::nullopt, registry).verdict ==
        preflight_verdict::unsupported);

  // The server's checksum means nothing for BlackOps3.exe.
  const exe_facts server{1, 0x14C28B4, 0, std::string(64, 'f')};
  CHECK(preflight_check(server, std::nullopt, registry).verdict ==
        preflight_verdict::unknown);
  CHECK(preflight_check(server, std::nullopt, registry,
                        "BlackOps3_UnrankedDedicatedServer.exe")
            .verdict == preflight_verdict::upstream_only);
}

TEST_CASE("pre-flight: facts come from the PE header and the file hash") {
  const fs::path dir = fs::temp_directory_path() / "xerces_compat_exe";
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::vector<char> bytes(0x400, 0);
  bytes[0] = 'M';
  bytes[1] = 'Z';
  put32(bytes, 0x3C, 0x80);
  put32(bytes, 0x80, 0x00004550);
  put32(bytes, 0x80 + 8, 0x6A7B6355);      // TimeDateStamp
  put32(bytes, 0x80 + 24 + 64, 0x6531394); // CheckSum
  const fs::path exe = dir / "BlackOps3.exe";
  std::ofstream(exe, std::ios::binary).write(bytes.data(), bytes.size());

  std::string error;
  const auto facts = read_exe_facts(exe, &error);
  REQUIRE(facts.has_value());
  CHECK_EQ(facts->size, 0x400u);
  CHECK_EQ(facts->checksum, 0x6531394u);
  CHECK_EQ(facts->link_time, 0x6A7B6355u);
  CHECK_EQ(facts->sha256, file_sha256_hex(exe));

  std::ofstream(dir / "text.exe", std::ios::binary) << "not a program";
  CHECK(!read_exe_facts(dir / "text.exe", &error).has_value());
  CHECK(!read_exe_facts(dir / "missing.exe", &error).has_value());
  fs::remove_all(dir);
}

TEST_CASE("pre-flight: Steam's build number from the appmanifest") {
  CHECK_EQ(*appmanifest_buildid(R"("AppState"
{
  "appid"   "311210"
  "buildid" "24784313"
  "installdir" "Call of Duty Black Ops III"
})"),
           24784313u);
  CHECK(!appmanifest_buildid(R"("AppState" { "appid" "311210" })"));
  CHECK(!appmanifest_buildid(R"("AppState" { "buildid" "24x" })"));
  CHECK(!appmanifest_buildid("not vdf {"));
}
