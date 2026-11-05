// The console tail the game keeps for its log, and when the game's own exit
// counts as clean (found in the custom-map runs of 2026-11-05).

#include "harness.hpp"

#include <string>

#include <xerces_core/journal.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/log.hpp>
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

TEST_CASE("utf-8: bad bytes become U+FFFD and cuts never split a character") {
  const std::string fffd = "\xEF\xBF\xBD";
  CHECK(json::valid_utf8("plain", 100) == "plain");
  // A CP-1252 apostrophe (0x92) from a mod's script print.
  CHECK(json::valid_utf8("it\x92s", 100) == "it" + fffd + "s");
  // "é" is 2 bytes, "€" 3: a cut at 4 bytes keeps "aé" whole and drops "€".
  CHECK(json::valid_utf8("a\xC3\xA9\xE2\x82\xAC", 4) == "a\xC3\xA9");
  CHECK(json::valid_utf8("a\xC3\xA9\xE2\x82\xAC", 6) ==
        "a\xC3\xA9\xE2\x82\xAC");
  // A character cut off by the end of the text, an overlong form, a surrogate.
  CHECK(json::valid_utf8("x\xE2\x82", 100) == "x" + fffd + fffd);
  CHECK(json::valid_utf8("\xC0\xAF", 100) == fffd + fffd);
  CHECK(json::valid_utf8("\xED\xA0\x80", 100) == fffd + fffd + fffd);
  CHECK(json::valid_utf8("\xF0\x9F\x98\x80", 100) == "\xF0\x9F\x98\x80");
  CHECK(json::valid_utf8("", 10).empty());
  CHECK(json::valid_utf8("abc", 0).empty());
}

TEST_CASE("utf-8: a log event built from bad text keeps its data") {
  json_log log("game", "1");
  log_event event;
  event.event = "loader.engine.error";
  event.message = json::valid_utf8("bad \x92 byte", 300);
  event.data_json = "{\"console\":[" +
                    json::quote(json::valid_utf8("line \x92 here", 600)) + "]}";
  const std::string line = log.format(event, "2026-11-05T00:00:00.000Z");
  CHECK(line.find("\"data\"") != std::string::npos);
  CHECK(line.find("line ") != std::string::npos);
}

TEST_CASE("journal: the game's own exit is clean only after a quit or with 0") {
  CHECK(game_exit_is_clean(true, 0xFFFFFFFFu)); // the player quit
  CHECK(game_exit_is_clean(true, 1u));          // quit after a match: code 1
  CHECK(game_exit_is_clean(false, 0u));
  CHECK(game_exit_is_clean(false, std::nullopt)); // ended another way
  CHECK(!game_exit_is_clean(false, 0xFFFFFFFFu)); // mid-load on two maps
  CHECK(!game_exit_is_clean(false, 1u));
}
