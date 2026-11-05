#include "harness.hpp"

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
