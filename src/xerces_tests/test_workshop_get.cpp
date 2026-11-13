#include "harness.hpp"

#include <atomic>
#include <filesystem>

#include <string>
#include <vector>

#include <xerces_core/launcher_home.hpp>
#include <xerces_core/workshop_get.hpp>

using namespace xerces_core::workshop_get;

TEST_CASE("workshop get: a progress line round trips") {
  const progress line{2804432292ull, "downloading", 1024, 4096, ""};
  const std::string text = to_json(line);
  CHECK_EQ(text, std::string(R"({"id":"2804432292","state":"downloading",)"
                             R"("done":1024,"total":4096})"));
  const auto back = parse_progress(text);
  REQUIRE(back.has_value());
  CHECK(*back == line);

  const progress failed{3171632315ull, "failed", 0, 0, "Steam stopped."};
  CHECK(parse_progress(to_json(failed)) == failed);
  CHECK(!parse_progress("not json").has_value());
  CHECK(!parse_progress(R"({"id":"1"})").has_value());
  // Steam's own lines on stdout, if any, are skipped
  CHECK(
      !parse_progress("Setting breakpad minidump AppID = 311210").has_value());
}

TEST_CASE("workshop get: Steam's item state flags") {
  // EItemState: 1 subscribed, 4 installed, 8 needs update, 16 downloading,
  // 32 download pending
  CHECK(phase_of(0, false) == item_phase::none);
  CHECK(phase_of(0, true) == item_phase::subscribing);
  CHECK(phase_of(1, true) == item_phase::queued);
  CHECK(phase_of(1 | 32, true) == item_phase::queued);
  CHECK(phase_of(1 | 8, true) == item_phase::queued);
  CHECK(phase_of(1 | 16, true) == item_phase::downloading);
  CHECK(phase_of(1 | 4 | 8 | 16, true) == item_phase::downloading);
  CHECK(phase_of(1 | 4, false) == item_phase::installed);
  // Installed but out of date is not done
  CHECK(phase_of(1 | 4 | 8, true) == item_phase::queued);
  CHECK_EQ(std::string(to_string(item_phase::installed)),
           std::string("installed"));
}

TEST_CASE("workshop get: ids") {
  using ids = std::vector<std::uint64_t>;
  CHECK(parse_ids("2804432292") == ids{2804432292ull});
  const ids three{1, 2, 3};
  CHECK(parse_ids("1,2 3,,2") == three);
  CHECK(!parse_ids("").has_value());
  CHECK(!parse_ids("0").has_value());
  CHECK(!parse_ids("12a").has_value());
  CHECK(!parse_ids("-5").has_value());
  CHECK(!parse_ids("1;quit").has_value());
}

TEST_CASE(
    "workshop get: the helper starts with two handles and is waited for") {
  // where.exe stands in for xerces_cli.exe: it takes the same command line,
  // refuses it and ends with a code of its own. What is tested is the start
  // (the handle list, the job, the suspended start resumed) and the wait.
  const std::string root = xerces_core::environment_utf8(L"SystemRoot");
  REQUIRE(!root.empty());
  const std::filesystem::path where =
      xerces_core::path_from_utf8(root) / "System32" / "where.exe";
  REQUIRE(std::filesystem::exists(where));
  std::atomic<void *> process{nullptr};
  std::string error;
  int lines = 0;
  const int code = xerces_core::workshop_get::run_helper(
      where, std::filesystem::temp_directory_path() / "xerces_no_game",
      {311210}, [&](const xerces_core::workshop_get::progress &) { ++lines; },
      &process, &error);
  CHECK(code > 0);
  CHECK(error.empty());
  CHECK(lines == 0);
  CHECK(process.load() == nullptr);
}
