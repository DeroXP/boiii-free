// The diagnostics bundle (docs/DESIGN.md 5.10): a readable zip, scrubbed of
// the profile folder, the player name and server addresses.

#include "harness.hpp"

#include <chrono>
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

TEST_CASE("bundle: no server address or favourite name goes in") {
  const fs::path home = fs::temp_directory_path() / "xerces_bundle_servers";
  fs::remove_all(home);
  // A log from a build that wrote addresses, as a player's PC may still have.
  write(
      home / "logs" / "20261105-020000-launcher-3.jsonl",
      R"({"event":"launcher.servers.join","msg":"203.0.113.7:27017 modded"})"
      "\n"
      R"({"event":"launcher.launch.started","msg":"-launch +connect dave.example.org"})"
      "\n"
      R"({"event":"x","msg":"[2001:db8::5]:27017"})"
      "\n");
  write(
      home / "config" / "launcher.json",
      R"({"favorites":[{"name":"Dave's house","address":"203.0.113.7:27017"}],)"
      R"("recent_servers":["203.0.113.7:27017","198.51.100.3:27017"],)"
      R"("mode":"zm"})");

  const fs::path out = home / "bundle.zip";
  REQUIRE(make_bundle(home, out, "C:\\Users\\Someone", "{}").ok);
  std::string bytes;
  {
    std::ifstream in(out, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
  }
  const auto files = unzip(bytes);
  for (const auto &[name, text] : files) {
    for (const char *secret : {"203.0.113", "198.51.100", "2001:db8",
                               "dave.example.org", "Dave's house"}) {
      if (text.find(secret) != std::string::npos) {
        CHECK_EQ(name + " holds " + secret, std::string());
      }
    }
  }
  REQUIRE(files.contains("launcher.json"));
  const auto config = json::parse(files.at("launcher.json"));
  REQUIRE(config.ok);
  CHECK(!config.document.HasMember("favorites"));
  CHECK(!config.document.HasMember("recent_servers"));
  const auto *favorites = json::member(config.document, "favorites_count");
  const auto *recent = json::member(config.document, "recent_servers_count");
  REQUIRE(favorites != nullptr && favorites->IsInt64());
  REQUIRE(recent != nullptr && recent->IsInt64());
  CHECK_EQ(favorites->GetInt64(), std::int64_t{1});
  CHECK_EQ(recent->GetInt64(), std::int64_t{2});
  CHECK_EQ(json::get_string(config.document, "mode").value_or(""),
           std::string("zm"));
  const std::string &log = files.at("logs/20261105-020000-launcher-3.jsonl");
  CHECK(log.find("launcher.servers.join") != std::string::npos);
  CHECK(log.find("<address> modded") != std::string::npos);
  fs::remove_all(home);
}

TEST_CASE("bundle: escaped and port-less addresses go too, lines stay JSON") {
  const fs::path home = fs::temp_directory_path() / "xerces_bundle_escapes";
  fs::remove_all(home);
  // Lines as a launcher writes them: a message with a newline before the
  // address, and the Servers screen naming a saved or recent server as it
  // was typed, without a port.
  write(home / "logs" / "20261105-030000-launcher-4.jsonl",
        R"({"event":"a","msg":"timed out\n203.0.113.9:27017"})"
        "\n"
        R"({"event":"b","msg":"a\nfriend.example.org:27017","data":{"n":1}})"
        "\n"
        R"({"event":"launcher.servers.favorite","msg":"join dave.ddns.example"})"
        "\n"
        R"({"event":"launcher.servers.recent","msg":"remove Lan-Box"})"
        "\n");
  write(home / "config" / "launcher.json",
        R"({"favorites":[{"name":"Dave","address":"dave.ddns.example"}],)"
        R"("recent_servers":["lan-box:27017"]})");

  const fs::path out = home / "bundle.zip";
  REQUIRE(make_bundle(home, out, "C:\\Users\\Someone", "{}").ok);
  std::string bytes;
  {
    std::ifstream in(out, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
  }
  const auto files = unzip(bytes);
  for (const auto &[name, text] : files) {
    for (const char *secret : {"203.0.113", "friend.example", "dave.ddns",
                               "lan-box", "Lan-Box"}) {
      if (text.find(secret) != std::string::npos) {
        CHECK_EQ(name + " holds " + secret, std::string());
      }
    }
  }
  const std::string &log = files.at("logs/20261105-030000-launcher-4.jsonl");
  std::size_t lines = 0;
  for (std::size_t start = 0; start < log.size();) {
    const std::size_t end = log.find('\n', start);
    REQUIRE(end != std::string::npos);
    const auto line = json::parse(log.substr(start, end - start));
    CHECK(line.ok);
    if (line.ok && json::member(line.document, "data") != nullptr) {
      CHECK_EQ(json::get_string(line.document, "msg").value_or(""),
               std::string("a\n<address>"));
    }
    ++lines;
    start = end + 1;
  }
  CHECK_EQ(lines, std::size_t{4});
  CHECK(log.find("join <address>") != std::string::npos);
  CHECK(log.find("remove <address>") != std::string::npos);
  fs::remove_all(home);
}

TEST_CASE("bundle: favourite names in old logs are scrubbed too") {
  const fs::path home = fs::temp_directory_path() / "xerces_bundle_names";
  fs::remove_all(home);
  // Footer notices as launchers before this one logged them: the name the
  // player gave a favourite, and a single-label host the patterns miss.
  write(
      home / "logs" / "20261105-040000-launcher-5.jsonl",
      R"({"event":"launcher.notice","msg":"Added to favourites: Dave's house"})"
      "\n"
      R"({"event":"launcher.notice","msg":"Removed from favourites: dave's HOUSE"})"
      "\n"
      R"({"event":"launcher.notice","msg":"Saved to favourites. Use Edit to name it: davespc:27017"})"
      "\n");
  write(home / "config" / "launcher.json",
        R"({"favorites":[{"name":"Dave's house","address":"davespc:27017"}]})");

  const fs::path out = home / "bundle.zip";
  REQUIRE(make_bundle(home, out, "C:\\Users\\Someone", "{}").ok);
  std::string bytes;
  {
    std::ifstream in(out, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
  }
  const auto files = unzip(bytes);
  const std::string &log = files.at("logs/20261105-040000-launcher-5.jsonl");
  for (const char *secret : {"Dave", "dave", "davespc"}) {
    CHECK(log.find(secret) == std::string::npos);
  }
  CHECK(log.find("Added to favourites: <address>") != std::string::npos);
  CHECK(log.find("Removed from favourites: <address>") != std::string::npos);
  CHECK(log.find("name it: <address>") != std::string::npos);
  fs::remove_all(home);
}

TEST_CASE("bundle: a bundle that cannot be written is not reported ready") {
  const fs::path home = fs::temp_directory_path() / "xerces_bundle_unwritable";
  fs::remove_all(home);
  write(home / "logs" / "20261105-050000-launcher-6.jsonl",
        "{\"msg\":\"ok\"}\n");

  // Where the zip should go is an (empty) folder: refused, and the folder
  // stays; only a bundle this call began is removed. (A disk that fills
  // while the last block is written fails at close, which is checked too,
  // but cannot be staged here.)
  const fs::path folder = home / "crash" / "taken.zip";
  fs::create_directories(folder);
  const bundle_result into_folder = make_bundle(home, folder, "", "{}");
  CHECK(!into_folder.ok);
  CHECK(!into_folder.error.empty());
  CHECK(fs::is_directory(folder));

  // Its folder cannot be made (a file has that name).
  write(home / "blocked", "file");
  const bundle_result blocked =
      make_bundle(home, home / "blocked" / "bundle.zip", "", "{}");
  CHECK(!blocked.ok);
  CHECK(!fs::exists(home / "blocked" / "bundle.zip"));

  // Written: the whole zip is on disk, down to its end record.
  const fs::path out = home / "crash" / "bundle.zip";
  REQUIRE(make_bundle(home, out, "", "{}").ok);
  const std::string bytes = xerces_tests::read_file(out);
  REQUIRE(bytes.size() >= 22);
  CHECK_EQ(get32(bytes, bytes.size() - 22), std::uint32_t{0x06054b50});
  CHECK(unzip(bytes).contains("logs/20261105-050000-launcher-6.jsonl"));
  fs::remove_all(home);
}

TEST_CASE("bundle: the game log stays in after many launcher sessions") {
  const fs::path home = fs::temp_directory_path() / "xerces_bundle_roles";
  fs::remove_all(home);
  // The game crashed (its log is old), then Xerces was opened eight times
  // without playing; a CLI run and a file of no known name are older still.
  const auto now = fs::file_time_type::clock::now();
  const auto at = [&](const std::string &name, int minutes_ago) {
    const fs::path file = home / "logs" / name;
    write(file, "{\"msg\":\"" + name + "\"}\n");
    fs::last_write_time(file, now - std::chrono::minutes(minutes_ago));
  };
  at("20261105-060000-cli-1.jsonl", 100);
  at("odd.jsonl", 95);
  at("20261105-061000-game-2.jsonl", 90);
  for (int i = 0; i < 8; ++i) {
    at("20261105-07000" + std::to_string(i) + "-launcher-" +
           std::to_string(10 + i) + ".jsonl",
       80 - i * 10); // the last one is the newest
  }

  const fs::path out = home / "bundle.zip";
  const bundle_result result = make_bundle(home, out, "", "{}");
  REQUIRE(result.ok);
  const auto files = unzip(xerces_tests::read_file(out));
  std::size_t logs = 0;
  for (const auto &[name, text] : files) {
    logs += name.starts_with("logs/") ? 1 : 0;
  }
  // Six logs: the newest of each role (launcher, game, cli, the odd one),
  // then the newest of the rest, which are launcher logs.
  CHECK_EQ(logs, std::size_t(6));
  CHECK(files.contains("logs/20261105-061000-game-2.jsonl"));
  CHECK(files.contains("logs/20261105-060000-cli-1.jsonl"));
  CHECK(files.contains("logs/odd.jsonl"));
  CHECK(files.contains("logs/20261105-070007-launcher-17.jsonl"));
  CHECK(files.contains("logs/20261105-070006-launcher-16.jsonl"));
  CHECK(files.contains("logs/20261105-070005-launcher-15.jsonl"));
  CHECK(!files.contains("logs/20261105-070004-launcher-14.jsonl"));
  CHECK(!files.contains("logs/20261105-070000-launcher-10.jsonl"));

  // With room for one, the newest log of all.
  const auto one = unzip(xerces_tests::read_file(
      make_bundle(home, home / "one.zip", "", "{}", 1).file));
  CHECK(one.contains("logs/20261105-070007-launcher-17.jsonl"));
  CHECK(!one.contains("logs/20261105-061000-game-2.jsonl"));
  fs::remove_all(home);
}

TEST_CASE("bundle: system information is JSON") {
  const auto parsed = json::parse(system_info_json());
  REQUIRE(parsed.ok);
  CHECK(parsed.document.HasMember("windows"));
  CHECK(parsed.document.HasMember("cpu"));
  CHECK(parsed.document.HasMember("ram_mb"));
}
