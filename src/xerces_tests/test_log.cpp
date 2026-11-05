#include "harness.hpp"

#include <string>
#include <vector>

#include <xerces_core/json.hpp>
#include <xerces_core/log.hpp>

using namespace xerces_core;

TEST_CASE("logs: lines carry the documented fields") {
  json_log log("cli", "s1");
  log.set_profile_path("");
  log_event event;
  event.level = log_level::warn;
  event.component = "planner";
  event.event = "plan.resolved";
  event.message = "two mods";
  event.plan_id = "sha256:abc";
  event.data_json = R"({"mods":2})";
  const std::string line = log.format(event, "2026-11-04T21:58:55.392Z");
  CHECK_EQ(
      line,
      R"({"component":"planner","data":{"mods":2},"event":"plan.resolved",)"
      R"("level":"warn","msg":"two mods","plan_id":"sha256:abc","role":"cli",)"
      R"("session":"s1","ts":"2026-11-04T21:58:55.392Z"})");
  CHECK(json::parse(line).ok);
}

TEST_CASE("logs: the profile folder never appears") {
  const std::string profile = "C:\\Users\\Ginld";
  CHECK_EQ(redact_profile("C:\\Users\\Ginld\\XercesHome\\logs", profile),
           "%USERPROFILE%\\XercesHome\\logs");
  CHECK_EQ(redact_profile("c:/users/ginld/x", profile), "%USERPROFILE%/x");
  CHECK_EQ(redact_profile(R"({"p":"C:\\Users\\Ginld\\a"})", profile),
           R"({"p":"%USERPROFILE%\\a"})");
  json_log log("game", "s2");
  log.set_profile_path(profile);
  log_event event;
  event.message = "reading C:\\Users\\Ginld\\mods";
  event.data_json = R"({"path":"C:\\Users\\Ginld\\mods"})";
  const std::string line = log.format(event, "t");
  CHECK(line.find("Ginld") == std::string::npos);
  CHECK(line.find("%USERPROFILE%") != std::string::npos);
}

TEST_CASE("logs: server addresses are replaced") {
  const std::string a(redacted_address);
  CHECK_EQ(redact_addresses("join 203.0.113.7:27017 modded"),
           "join " + a + " modded");
  CHECK_EQ(redact_addresses("from 10.0.0.2."), "from " + a + ".");
  CHECK_EQ(redact_addresses("x=192.168.1.20,y"), "x=" + a + ",y");
  CHECK_EQ(redact_addresses("[2001:db8::7]:27017 and [::1]"), a + " and " + a);
  CHECK_EQ(redact_addresses("[fe80::1%eth0]:1"), a);
  CHECK_EQ(redact_addresses("v6 fe80::1 and ::ffff:192.0.2.1 here"),
           "v6 " + a + " and " + a + " here");
  CHECK_EQ(redact_addresses("full 2001:db8:0:0:1:0:0:1"), "full " + a);
  CHECK_EQ(redact_addresses("favorite add daves-house.example.org:27017"),
           "favorite add " + a);
  // A host name without a port is only caught after +connect.
  CHECK_EQ(redact_addresses("-launch +connect dave.example.org"),
           "-launch +connect " + a);
  CHECK_EQ(redact_addresses("+CONNECT myserver:27017 +set x 1"),
           "+CONNECT " + a + " +set x 1");
  CHECK_EQ(redact_addresses(R"({"cmd":"+connect \"host\""})"),
           R"({"cmd":"+connect \")" + a + R"(\""})");
  CHECK_EQ(redact_addresses("+connected"), "+connected");

  // Things that only look a little like addresses stay.
  const std::vector<std::string> kept = {
      "2026-11-04T21:58:55.392Z",
      "Windows 10.0.26300",
      "build 10.0.26300.1742",
      "version 1.2.3.4.5",
      "v1.2.3.4",
      "sha256:" + std::string(64, 'a'),
      R"({"a":1,"b.c":2,"d":"e"})",
      "plan.json",
      "launcher.servers.join",
      "C:\\Users\\x\\y.ff",
      "std::string",
      "12:34:56",
      "999.1.1.1",
      "mods 1:2",
      "x.y:999999",
      "\"ram_mb\":32768",
  };
  for (const std::string &text : kept) {
    CHECK_EQ(redact_addresses(text), text);
  }
}

TEST_CASE("logs: lines never carry a server address") {
  json_log log("launcher", "s3");
  log.set_profile_path("");
  log_event event;
  event.event = "launcher.launch.started";
  event.message = "-launch -nointro +connect 203.0.113.7:27017";
  event.data_json =
      R"({"pid":12,"server":"[2001:db8::1]:27017","host":"x.example.net:1"})";
  const std::string line = log.format(event, "t");
  CHECK(line.find("203.0.113") == std::string::npos);
  CHECK(line.find("2001:db8") == std::string::npos);
  CHECK(line.find("example.net") == std::string::npos);
  const auto parsed = json::parse(line);
  REQUIRE(parsed.ok);
  // The data object survives the scrubbing as JSON.
  const auto *data = json::member(parsed.document, "data");
  REQUIRE(data != nullptr);
  CHECK_EQ(json::get_string(*data, "server").value_or(""),
           std::string(redacted_address));
  CHECK(json::member(*data, "pid") != nullptr);
}

TEST_CASE("logs: only the newest sessions are kept") {
  const auto dir = std::filesystem::temp_directory_path() / "xerces_tests_logs";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  for (int i = 0; i < 25; ++i) {
    std::ofstream(dir / ("2000010" + std::to_string(i % 10) + "-00000" +
                         std::to_string(i / 10) + "-cli-old" +
                         std::to_string(i) + ".jsonl"))
        << "{}\n";
  }
  {
    json_log log("cli", "now");
    REQUIRE(log.open(dir, 20));
    log.write({log_level::info, "test", "test.event", "hello", "", ""});
    std::size_t count = 0;
    bool current_kept = false;
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
      ++count;
      current_kept = current_kept || entry.path() == log.path();
    }
    CHECK_EQ(count, 20u);
    CHECK(current_kept);
  } // closes the file before the folder goes
  std::filesystem::remove_all(dir);
}
