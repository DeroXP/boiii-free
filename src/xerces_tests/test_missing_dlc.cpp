// The notice for a server on one of the game's own maps that the player's game
// lacks (docs/DESIGN.md D9): it names the DLC, points at Steam and offers no
// download.

#include "harness.hpp"

#include <string>
#include <string_view>

#include <xerces_core/missing_dlc.hpp>

using namespace xerces_core;

namespace {
bool contains(const std::string &text, std::string_view part) {
  return text.find(part) != std::string::npos;
}
} // namespace

TEST_CASE("missing dlc: the notice names the map and asks to open the store") {
  const dlc_notice notice = missing_dlc_notice("zm_castle");
  CHECK_EQ(notice.title, std::string("DLC needed"));
  CHECK(contains(notice.message,
                 "This server uses DLC you don't own or haven't installed"));
  CHECK(contains(notice.message, "(the map zm_castle)"));
  CHECK(contains(notice.message, "Properties, DLC"));
  CHECK(notice.message.ends_with("Open the store page in Steam now?"));
}

TEST_CASE("missing dlc: no download offer and no web link") {
  for (const std::string_view map : {"zm_castle", "mp_rise", ""}) {
    const dlc_notice notice = missing_dlc_notice(map);
    CHECK(!contains(notice.message, "http"));
    CHECK(!contains(notice.message, "://"));
    CHECK(!contains(notice.message, "ownload"));
  }
}

TEST_CASE("missing dlc: a name that is not a plain map name is left out") {
  using namespace std::string_view_literals;
  for (const std::string_view map :
       {""sv, "zm_castle\n^1Click here"sv, "ZM_CASTLE"sv, "zm castle"sv,
        "zm-castle"sv, "zm_\xC3\xA9"sv, "zm\0x"sv}) {
    const dlc_notice notice = missing_dlc_notice(map);
    CHECK(!contains(notice.message, "(the map"));
    CHECK(contains(notice.message, "haven't installed.\n\n"));
  }
  const std::string longest(64, 'a');
  CHECK(contains(missing_dlc_notice(longest).message, longest));
  const std::string too_long(65, 'a');
  CHECK(!contains(missing_dlc_notice(too_long).message, "(the map"));
}

TEST_CASE("missing dlc: the store page is Black Ops III's, opened in Steam") {
  CHECK_EQ(std::string(bo3_store_page), std::string("steam://store/311210"));
}
