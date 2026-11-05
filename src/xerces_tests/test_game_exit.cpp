// The console tail the game keeps for its log, and when the game's own exit
// counts as clean (found in the custom-map runs of 2026-11-05).

#include "harness.hpp"

#include <string>

#include <xerces_core/journal.hpp>
#include <xerces_core/text_tail.hpp>

using namespace xerces_core;

TEST_CASE("console tail: whole and partial lines, oldest first") {
  text_tail tail;
  CHECK(tail.last_lines(5).empty());
  tail.append("first line\nsecond ");
  tail.append("line\r\nthird\rfourth");
  const auto lines = tail.last_lines(10);
  REQUIRE(lines.size() == 4u);
  CHECK(lines[0] == "first line");
  CHECK(lines[1] == "second line");
  CHECK(lines[2] == "third");
  CHECK(lines[3] == "fourth"); // still waiting for its end
  const auto two = tail.last_lines(2);
  REQUIRE(two.size() == 2u);
  CHECK(two[0] == "third");
  CHECK(two[1] == "fourth");
}

TEST_CASE("console tail: a \\r\\n split across two appends is one line end") {
  text_tail tail;
  tail.append("a\r");
  tail.append("\nb\n\n\nc\n");
  const auto lines = tail.last_lines(10);
  REQUIRE(lines.size() == 3u); // empty lines are dropped
  CHECK(lines[0] == "a");
  CHECK(lines[1] == "b");
  CHECK(lines[2] == "c");
}

TEST_CASE(
    "console tail: when full, keeps the newest text and drops a cut line") {
  text_tail tail;
  // 1000 numbered lines of 30 characters: far more than the buffer holds.
  for (int i = 0; i < 1000; ++i) {
    std::string line = "line " + std::to_string(i);
    line.resize(29, '.');
    tail.append(line + "\n");
  }
  const auto lines = tail.last_lines(1000);
  REQUIRE(!lines.empty());
  CHECK(lines.size() <= text_tail::capacity / 30);
  CHECK(lines.size() >= text_tail::capacity / 30 - 1);
  CHECK(lines.back().rfind("line 999", 0) == 0);
  for (const auto &line : lines) {
    CHECK(line.size() == 29u); // no line that lost its start
    CHECK(line.rfind("line ", 0) == 0);
  }
  tail.clear();
  CHECK(tail.last_lines(5).empty());
  tail.append("again\n");
  REQUIRE(tail.last_lines(5).size() == 1u);
  CHECK(tail.last_lines(5)[0] == "again");
}

TEST_CASE(
    "console tail: one line longer than the buffer leaves nothing whole") {
  text_tail tail;
  tail.append(std::string(text_tail::capacity + 100, 'x'));
  CHECK(tail.last_lines(5).empty());
  tail.append("\nafter\n");
  const auto lines = tail.last_lines(5);
  REQUIRE(lines.size() == 1u);
  CHECK(lines[0] == "after");
}

TEST_CASE("journal: the game's own exit is clean only after a quit or with 0") {
  CHECK(game_exit_is_clean(true, 0xFFFFFFFFu)); // the player quit
  CHECK(game_exit_is_clean(true, 1u));          // quit after a match: code 1
  CHECK(game_exit_is_clean(false, 0u));
  CHECK(game_exit_is_clean(false, std::nullopt)); // ended another way
  CHECK(!game_exit_is_clean(false, 0xFFFFFFFFu)); // mid-load on two maps
  CHECK(!game_exit_is_clean(false, 1u));
}
