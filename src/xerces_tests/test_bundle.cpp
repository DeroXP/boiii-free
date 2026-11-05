// The diagnostics bundle (docs/DESIGN.md 5.10): a readable zip, scrubbed of
// the profile folder and the player name.

#include "harness.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <xerces_core/bundle.hpp>
#include <xerces_core/json.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
std::uint32_t get32(const std::string &bytes, std::size_t at) {
  return std::uint32_t(std::uint8_t(bytes[at])) |
         (std::uint32_t(std::uint8_t(bytes[at + 1])) << 8) |
         (std::uint32_t(std::uint8_t(bytes[at + 2])) << 16) |
         (std::uint32_t(std::uint8_t(bytes[at + 3])) << 24);
}

std::uint16_t get16(const std::string &bytes, std::size_t at) {
  return std::uint16_t(std::uint8_t(bytes[at]) |
                       (std::uint8_t(bytes[at + 1]) << 8));
}

// Reads a stored zip by its central directory, checking every CRC.
std::map<std::string, std::string> unzip(const std::string &bytes) {
  std::map<std::string, std::string> out;
  REQUIRE(bytes.size() >= 22);
  const std::size_t end = bytes.size() - 22;
  REQUIRE_EQ(get32(bytes, end), std::uint32_t{0x06054b50});
  const std::uint16_t count = get16(bytes, end + 10);
  std::size_t at = get32(bytes, end + 16);
  for (std::uint16_t i = 0; i < count; ++i) {
    REQUIRE_EQ(get32(bytes, at), std::uint32_t{0x02014b50});
    const std::uint32_t crc = get32(bytes, at + 16);
    const std::uint32_t size = get32(bytes, at + 24);
    const std::uint16_t name_size = get16(bytes, at + 28);
    const std::uint32_t local = get32(bytes, at + 42);
    const std::string name = bytes.substr(at + 46, name_size);
    REQUIRE_EQ(get32(bytes, local), std::uint32_t{0x04034b50});
    const std::size_t data = local + 30 + get16(bytes, local + 26);
    const std::string contents = bytes.substr(data, size);
    CHECK_EQ(crc32(contents), crc);
    out[name] = contents;
    at += 46 + name_size;
  }
  return out;
}

void write(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}
} // namespace

TEST_CASE("bundle: a stored zip reads back with matching CRCs") {
  CHECK_EQ(crc32("123456789"), std::uint32_t{0xCBF43926});
  zip_writer zip;
  zip.add("a.txt", "alpha");
  zip.add("dir/b.json", "{}");
  zip.add("empty", "");
  const auto files = unzip(zip.finish());
  REQUIRE_EQ(files.size(), std::size_t{3});
  CHECK_EQ(files.at("a.txt"), std::string("alpha"));
  CHECK_EQ(files.at("dir/b.json"), std::string("{}"));
  CHECK_EQ(files.at("empty"), std::string());
}

TEST_CASE("bundle: logs, journal, plan and report, scrubbed") {
  const fs::path home = fs::temp_directory_path() / "xerces_bundle_home";
  fs::remove_all(home);
  const std::string profile = "C:\\Users\\Someone";
  write(home / "logs" / "20261105-010000-launcher-1.jsonl",
        R"({"msg":"C:\\Users\\Someone\\XercesHome"})" "\n");
  write(home / "logs" / "20261105-010100-game-2.jsonl", "{\"msg\":\"ok\"}\n");
  const std::string id(64, 'a');
  write(home / "logs" / "journal.json",
        "{\"phase\":\"in_game\",\"plan_id\":\"sha256:" + id +
            "\",\"safe_mode\":false,\"schema\":1,\"stack\":[\"ws.1\"],"
            "\"started\":\"\",\"updated\":\"\"}");
  write(home / "config" / "plans" / (id + ".json"), R"({"stack":[]})");
  write(home / "cache" / "signatures-report.json", R"({"entries":[]})");
  write(home / "config" / "launcher.json",
        R"({"game_dir":"C:\\Users\\Someone\\Games\\BO3","player_name":"Secret"})");

  const fs::path out = home / "crash" / "bundle.zip";
  const bundle_result result =
      make_bundle(home, out, profile, R"({"windows":"10.0.26300"})");
  REQUIRE(result.ok);
  std::string bytes;
  {
    std::ifstream in(out, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
  }
  const auto files = unzip(bytes);
  CHECK(files.contains("logs/20261105-010000-launcher-1.jsonl"));
  CHECK(files.contains("logs/20261105-010100-game-2.jsonl"));
  CHECK(files.contains("journal.json"));
  CHECK(files.contains("plan.json"));
  CHECK(files.contains("signatures-report.json"));
  CHECK(files.contains("system.json"));
  CHECK(files.contains("FILES.txt"));
  REQUIRE(files.contains("launcher.json"));
  CHECK(files.at("launcher.json").find("Secret") == std::string::npos);
  for (const auto &[name, text] : files) {
    CHECK(text.find("Someone") == std::string::npos);
  }
  CHECK(files.at("logs/20261105-010000-launcher-1.jsonl").find("%USERPROFILE%") !=
        std::string::npos);
  fs::remove_all(home);
}

TEST_CASE("bundle: system information is JSON") {
  const auto parsed = json::parse(system_info_json());
  REQUIRE(parsed.ok);
  CHECK(parsed.document.HasMember("windows"));
  CHECK(parsed.document.HasMember("cpu"));
  CHECK(parsed.document.HasMember("ram_mb"));
}
