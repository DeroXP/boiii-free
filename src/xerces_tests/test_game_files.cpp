// binds.cfg and launcher_settings.json as the client and upstream's launcher
// read them (xerces_core/game_files.hpp).

#include "harness.hpp"

#include <string>

#include <xerces_core/game_files.hpp>
#include <xerces_core/json.hpp>

using namespace xerces_core;

TEST_CASE("game files: binds read and written like upstream") {
  const auto binds = parse_binds("bind F \"say hi\"\n"
                                 "  BIND g give all  \r\n"
                                 "// comment\n"
                                 "bind nocommand\n"
                                 "bind f \"say again\"\n");
  REQUIRE_EQ(binds.size(), std::size_t{2});
  CHECK_EQ(binds[0].key, std::string("f"));
  CHECK_EQ(binds[0].command, std::string("say again"));
  CHECK_EQ(binds[1].key, std::string("g"));
  CHECK_EQ(binds[1].command, std::string("give all"));
  CHECK_EQ(binds_to_text(binds),
           std::string("bind f \"say again\"\nbind g \"give all\"\n"));
  CHECK(parse_binds(binds_to_text(binds)).size() == binds.size());
}

TEST_CASE("game files: a bind cannot break out of its line") {
  CHECK(valid_bind_key("mouse4"));
  CHECK(!valid_bind_key("a b"));
  CHECK(!valid_bind_key("a;b"));
  CHECK(!valid_bind_key(std::string(33, 'k')));
  CHECK(valid_bind_command("toggle cg_drawFPS; say on"));
  CHECK(!valid_bind_command("say \"x\""));
  CHECK(!valid_bind_command("a\nbind f quit"));
  CHECK(!valid_bind_command(""));
  const std::string text =
      binds_to_text({{"f", "ok"}, {"g", "bad\"quote"}, {"h h", "x"}});
  CHECK_EQ(text, std::string("bind f \"ok\"\n"));
}

TEST_CASE("game files: the asset pool switch keeps the other settings") {
  CHECK(asset_pools_enabled(""));
  CHECK(asset_pools_enabled("{}"));
  CHECK(asset_pools_enabled(R"({"asset_limits_enabled":"1"})"));
  CHECK(!asset_pools_enabled(R"({"asset_limits_enabled":"0"})"));
  CHECK(!asset_pools_enabled(R"({"disable_asset_pools":"1"})"));

  const std::string off = set_asset_pools_enabled(
      R"({"ap_image":"9000","friends_only":"1","disable_asset_pools":"1"})",
      false);
  CHECK(!asset_pools_enabled(off));
  const auto parsed = json::parse(off);
  REQUIRE(parsed.ok);
  CHECK_EQ(json::get_string(parsed.document, "ap_image").value_or(""),
           std::string("9000"));
  CHECK_EQ(json::get_string(parsed.document, "friends_only").value_or(""),
           std::string("1"));
  CHECK(!parsed.document.HasMember("disable_asset_pools"));
  CHECK(asset_pools_enabled(set_asset_pools_enabled(off, true)));
  CHECK(asset_pools_enabled(set_asset_pools_enabled("not json", true)));
}
