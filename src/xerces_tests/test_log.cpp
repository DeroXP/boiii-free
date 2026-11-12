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
  // A file's name and its line are no address: a script or Lua error is
  // found by them.
  for (const char *text :
       {"lua/shared/luautils.lua:57: function expected instead of nil",
        "scripts/zm/zm.gsc:412", "at client.cpp:7", "see Config.CFG:3",
        "exe_common.inl:288"}) {
    CHECK_EQ(redact_addresses(text), std::string(text));
  }
  // A host that only starts like one still is.
  CHECK_EQ(redact_addresses("join lua.example.org:27017"), "join " + a);
  CHECK_EQ(redact_addresses("join my.luas:27017"), "join " + a);
  // A host name without a port is only caught after +connect.
  CHECK_EQ(redact_addresses("-launch +connect dave.example.org"),
           "-launch +connect " + a);
  CHECK_EQ(redact_addresses("+CONNECT myserver:27017 +set x 1"),
           "+CONNECT " + a + " +set x 1");
  CHECK_EQ(redact_addresses("+connect \"host\" -x"),
           "+connect \"" + a + "\" -x");
  CHECK_EQ(redact_addresses_in_json(R"({"cmd":"+connect \"host\""})"),
           R"({"cmd":"+connect \")" + a + R"(\""})");
  CHECK_EQ(redact_addresses("+connected"), "+connected");
  // The launcher's join since 0.1.0-beta.6.
  CHECK_EQ(redact_addresses("-launch +set xerces_join dave.example.org"),
           "-launch +set xerces_join " + a);
  CHECK_EQ(redact_addresses("+set XERCES_JOIN \"myserver:27017\""),
           "+set XERCES_JOIN \"" + a + "\"");
  CHECK_EQ(redact_addresses_in_json(R"({"cmd":"+set xerces_join host:1"})"),
           R"({"cmd":"+set xerces_join )" + a + R"("})");
  // An invite's ticket is a secret too.
  CHECK_EQ(redact_addresses("+set xerces_join_ticket "
                            "0123456789abcdef0123456789abcdef -x"),
           "+set xerces_join_ticket " + a + " -x");
  CHECK_EQ(redact_addresses("my_xerces_join home"), "my_xerces_join home");
  CHECK_EQ(redact_addresses("xerces_joined home"), "xerces_joined home");

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

TEST_CASE("logs: an address right after a JSON escape is still found") {
  const std::string a(redacted_address);
  // The escape's letter is not part of the address, and the escape survives.
  CHECK_EQ(redact_addresses_in_json(R"({"m":"line one\n203.0.113.7:27017"})"),
           R"({"m":"line one\n)" + a + R"("})");
  CHECK_EQ(redact_addresses_in_json(R"({"m":"name\t203.0.113.7"})"),
           R"({"m":"name\t)" + a + R"("})");
  CHECK_EQ(redact_addresses_in_json(R"({"m":"a\nfriend.example.org:27017"})"),
           R"({"m":"a\n)" + a + R"("})");
  CHECK_EQ(redact_addresses_in_json(R"({"m":"x 203.0.113.7"})"),
           R"({"m":"x )" + a + R"("})");
  CHECK_EQ(redact_addresses_in_json(R"({"203.0.113.7:1":true})"),
           R"({")" + a + R"(":true})");
  // Each result is valid JSON again.
  for (const char *text :
       {R"({"m":"😀 203.0.113.7","n":[1.5,null]})",
        R"({"m":"\ud83d 203.0.113.7"})", R"({"m":"\\203.0.113.7"})",
        R"({"m":"a\"b [::1]:27017 \/ c"})"}) {
    const std::string scrubbed = redact_addresses_in_json(text);
    CHECK(scrubbed.find("203.0.113") == std::string::npos);
    CHECK(scrubbed.find("::1") == std::string::npos);
    CHECK(json::parse(scrubbed).ok);
  }
  // A string with nothing to scrub keeps its escapes byte for byte.
  const std::string untouched =
      R"({"m":"café\n\"q\"","n":1.5,"t":"12:34:56"})";
  CHECK_EQ(redact_addresses_in_json(untouched), untouched);
  // JSON Lines, the last one cut short by a crash.
  CHECK_EQ(redact_addresses_in_json("{\"m\":\"a\\n10.1.2.3\"}\n{\"m\":\"b\"}\n"
                                    "{\"m\":\"cut 10.1.2.4"),
           "{\"m\":\"a\\n" + a + "\"}\n{\"m\":\"b\"}\n{\"m\":\"cut " + a);
}

TEST_CASE("logs: a data object with an escaped address keeps its data") {
  json_log log("launcher", "s4");
  log.set_profile_path("");
  for (const char *error :
       {R"(timed out\n203.0.113.7:27017)", R"(timed out\n203.0.113.7)",
        R"(timed out\nfriend.example.org:27017)"}) {
    log_event event;
    event.event = "launcher.servers.ping";
    event.message = "ping";
    event.data_json = std::string(R"({"pid":7,"error":")") + error + R"("})";
    const std::string line = log.format(event, "t");
    CHECK(line.find("203.0.113") == std::string::npos);
    CHECK(line.find("example.org") == std::string::npos);
    const auto parsed = json::parse(line);
    REQUIRE(parsed.ok);
    const auto *data = json::member(parsed.document, "data");
    REQUIRE(data != nullptr);
    CHECK_EQ(json::get_string(*data, "error").value_or(""),
             "timed out\n" + std::string(redacted_address));
  }
}

TEST_CASE("logs: known server addresses are found without a port") {
  const std::string a(redacted_address);
  const std::vector<std::string> known = {"dave.example.org:27017", "LANBOX",
                                          "pc", "[2001:db8::9]:27017"};
  CHECK_EQ(redact_addresses("join dave.example.org vanilla", known),
           "join " + a + " vanilla");
  CHECK_EQ(redact_addresses("recent remove DAVE.example.org:27017.", known),
           "recent remove " + a + ".");
  CHECK_EQ(redact_addresses("favorite join lanbox", known),
           "favorite join " + a);
  CHECK_EQ(redact_addresses("www.dave.example.org", known), "www." + a);
  // Only whole words: longer names stay, and so do names too short to say
  // anything about anyone.
  for (const char *text : {"dave.example.organic", "xdave.example.org",
                           "dave.example.org-2", "lanbox2", "pc on"}) {
    CHECK_EQ(redact_addresses(text, known), std::string(text));
  }
  // In JSON only strings are searched: a server called "null" leaves the
  // document intact.
  const std::vector<std::string> odd = {"null"};
  CHECK_EQ(redact_addresses_in_json(R"({"m":"join null","v":null})", odd),
           R"({"m":"join )" + a + R"(","v":null})");
  CHECK_EQ(redact_addresses_in_json(R"({"m":"x\ndave.example.org"})", known),
           R"({"m":"x\n)" + a + R"("})");
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
