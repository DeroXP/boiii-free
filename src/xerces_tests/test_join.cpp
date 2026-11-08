// Joining another player's self-hosted game (docs/DESIGN.md 11.10): the
// content ids a host advertises, the check against this PC, the words the
// game shows, open to direct connect, and the journal after a join.

#include "harness.hpp"

#include <map>
#include <string>
#include <vector>

#include <xerces_core/join_check.hpp>
#include <xerces_core/journal.hpp>
#include <xerces_core/version.hpp>

using namespace xerces_core;

namespace {
const std::string hash_a =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string hash_b =
    "sha256:fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

bool contains(const std::string &text, const std::string &part) {
  return text.find(part) != std::string::npos;
}

// A PC that has some items, with or without a content id.
struct fake_pc {
  std::map<std::string, own_item> items; // by key
  own_lookup lookup() const {
    return [this](const host_item &item) {
      std::vector<std::string> keys{item.key};
      keys.insert(keys.end(), item.other_keys.begin(), item.other_keys.end());
      for (const auto &key : keys) {
        if (const auto it = items.find(key); it != items.end()) {
          return it->second;
        }
      }
      return own_item{};
    };
  }
};
} // namespace

TEST_CASE("join: a content id is the first 16 hex digits of the hash") {
  CHECK_EQ(short_content_id(hash_a), std::string("0123456789abcdef"));
  CHECK_EQ(short_content_id("sha256:abc"), std::string());
  CHECK_EQ(short_content_id(hash_a.substr(7)), std::string()); // no tag
  CHECK_EQ(short_content_id("sha256:" + std::string(64, 'G')), std::string());
  CHECK_EQ(short_content_id(""), std::string());
}

TEST_CASE("join: content ids round trip, and older hosts send none") {
  const std::vector<content_id> ids = {{"1944930083", "0123456789abcdef"},
                                       {"3171632315", "fedcba9876543210"},
                                       {"zm_rust", "00000000000000aa"}};
  const std::string text = format_content_ids(ids);
  CHECK_EQ(text, std::string("1944930083=0123456789abcdef,"
                             "3171632315=fedcba9876543210,"
                             "zm_rust=00000000000000aa"));
  const auto back = parse_content_ids(text);
  REQUIRE_EQ(back.size(), std::size_t(3));
  CHECK_EQ(back[2].key, std::string("zm_rust"));
  CHECK_EQ(*find_content_id(back, "3171632315"),
           std::string("fedcba9876543210"));
  CHECK(!find_content_id(back, "3656779213").has_value());

  // A host before 2026-11-05 (or BOIII) says nothing.
  CHECK(parse_content_ids("").empty());
  // Junk is skipped, upper case is read, the first id of a key wins.
  const auto mixed = parse_content_ids(
      "a=0123456789ABCDEF,broken,b=xyz,=0123456789abcdef,a=fedcba9876543210,"
      " c = 0000000000000001 ,d:e=0123456789abcdef");
  REQUIRE_EQ(mixed.size(), std::size_t(2));
  CHECK_EQ(mixed[0].key, std::string("a"));
  CHECK_EQ(mixed[0].id, std::string("0123456789abcdef"));
  CHECK_EQ(mixed[1].key, std::string("c"));

  // Keys the list or the lobby content cannot carry are left out when
  // written, and the list is capped.
  CHECK_EQ(format_content_ids({{"a,b", "0123456789abcdef"},
                               {"a:b", "0123456789abcdef"},
                               {"a b", "0123456789abcdef"},
                               {"ok", "short"}}),
           std::string());
  std::vector<content_id> many;
  for (int i = 0; i < 100; ++i) {
    many.push_back({"k" + std::to_string(i), "0123456789abcdef"});
  }
  CHECK_EQ(parse_content_ids(format_content_ids(many)).size(), max_content_ids);
  // No ':' or space: the lobby content splits on both.
  const std::string many_text = format_content_ids(many);
  CHECK(many_text.find(':') == std::string::npos);
  CHECK(many_text.find(' ') == std::string::npos);
}

TEST_CASE("join: the installed index round trips and is found by folder") {
  std::vector<installed_content> catalog(3);
  catalog[0].folder = "1944930083";
  catalog[0].info.id = "ws.1944930083";
  catalog[0].info.name = "Solo Easter Eggs";
  catalog[0].content_hash = hash_a;
  catalog[1].folder = "zm_RUST";
  catalog[1].info.id = "local.zm_rust";
  catalog[1].info.kind = content_kind::map;
  catalog[1].content_hash = hash_b;
  catalog[2].folder = "unhashed"; // no content hash: left out
  const auto index = parse_installed_index(installed_index_to_json(catalog));
  REQUIRE_EQ(index.size(), std::size_t(2));
  CHECK_EQ(index[0].id, std::string("ws.1944930083"));
  CHECK_EQ(index[0].name, std::string("Solo Easter Eggs"));
  CHECK_EQ(index[0].kind, std::string("mod"));
  CHECK_EQ(index[1].kind, std::string("map"));
  CHECK_EQ(index[1].content_id, std::string("fedcba9876543210"));
  const installed_entry *map = find_installed(index, "zm_rust");
  REQUIRE(map != nullptr);
  CHECK_EQ(map->folder, std::string("zm_RUST"));
  CHECK(find_installed(index, "zm_rus") == nullptr);
  CHECK(parse_installed_index("not json").empty());
  CHECK(parse_installed_index(R"({"schema":"other","items":[]})").empty());
}

TEST_CASE("join: the host's items in load order") {
  const auto items = host_items("1944930083", "Solo Easter Eggs",
                                "3171632315, 1944930083,3656779213,3171632315",
                                "zm_rust", "3010010010");
  REQUIRE_EQ(items.size(), std::size_t(4));
  CHECK(items[0].kind == host_item_kind::main_mod);
  CHECK_EQ(items[0].name, std::string("Solo Easter Eggs"));
  CHECK_EQ(items[0].workshop_id, std::string("1944930083"));
  CHECK_EQ(items[1].key, std::string("3171632315"));
  CHECK_EQ(items[2].key, std::string("3656779213"));
  CHECK(items[3].kind == host_item_kind::map);
  CHECK_EQ(items[3].workshop_id, std::string("3010010010"));
  REQUIRE_EQ(items[3].other_keys.size(), std::size_t(1));

  // "usermaps" is no mod, a local mod has no Workshop page, no map is none.
  const auto plain = host_items("usermaps", "", "my_mod", "", "");
  REQUIRE_EQ(plain.size(), std::size_t(1));
  CHECK(plain[0].workshop_id.empty());
  CHECK(host_items("", "", "", "", "").empty());
}

TEST_CASE("join: everything missing is reported at once") {
  // The joiner has the main mod only.
  fake_pc pc;
  pc.items["1944930083"] = {true, "0123456789abcdef", "Solo Easter Eggs"};
  const auto items =
      host_items("1944930083", "", "3171632315,3656779213,my_local_mod",
                 "zm_rust", "3010010010");
  const join_check check = check_join(items, {}, pc.lookup());
  CHECK(!check.ok());
  REQUIRE_EQ(check.missing.size(), std::size_t(4));
  CHECK(check.differ.empty());

  const join_notice notice = missing_notice(check);
  CHECK_EQ(notice.title, std::string("Mods and map not installed"));
  CHECK(contains(notice.message, "Mod: Workshop item 3171632315"));
  CHECK(contains(notice.message, "Mod: Workshop item 3656779213"));
  CHECK(contains(notice.message, "Mod: my_local_mod, not a Workshop item"));
  CHECK(contains(notice.message, "Map: zm_rust (Workshop item 3010010010)"));
  CHECK(contains(notice.message, "the one that is not a Workshop item"));
  CHECK(
      contains(notice.message, "Get the 3 Workshop items through Steam now?"));
  CHECK(notice.workshop_ids == notice.workshop_pages);
  CHECK(notice.workshop_pages ==
        std::vector<std::string>({"3171632315", "3656779213", "3010010010"}));

  // One stacked mod missing: singular words.
  pc.items["3171632315"] = {true, "", "DS4C Patched"};
  pc.items["my_local_mod"] = {true, "", "Mine"};
  pc.items["3010010010"] = {true, "", "RUST"}; // found by the map's id
  const join_check one = check_join(items, {}, pc.lookup());
  REQUIRE_EQ(one.missing.size(), std::size_t(1));
  const join_notice single = missing_notice(one);
  CHECK_EQ(single.title, std::string("Mod not installed"));
  CHECK(contains(single.message, "This server uses a mod you don't have"));
  CHECK(contains(single.message, "Get it through Steam now?"));

  // Nothing with a Workshop page: no question, nothing to open.
  const join_check local =
      check_join(host_items("", "", "their_mod", "", ""), {}, pc.lookup());
  const join_notice ask = missing_notice(local);
  CHECK(ask.workshop_pages.empty());
  CHECK(contains(ask.message, "Ask the host where to get it"));
  CHECK(!contains(ask.message, "?"));
}

TEST_CASE("join: a different version is named, unknown ids are not") {
  fake_pc pc;
  pc.items["1944930083"] = {true, "0123456789abcdef", "Solo Easter Eggs"};
  pc.items["3171632315"] = {true, "1111111111111111", "DS4C Patched"};
  pc.items["3656779213"] = {true, "", "Classic Viewmodel FOV"}; // unknown
  const auto items =
      host_items("1944930083", "", "3171632315,3656779213", "", "");

  // An older host sends no ids: nothing to compare, the join goes on.
  CHECK(check_join(items, {}, pc.lookup()).ok());

  const auto host_ids = parse_content_ids("1944930083=0123456789abcdef,"
                                          "3171632315=2222222222222222,"
                                          "3656779213=3333333333333333");
  const join_check check = check_join(items, host_ids, pc.lookup());
  REQUIRE_EQ(check.differ.size(), std::size_t(1));
  CHECK(check.missing.empty());
  CHECK_EQ(check.differ[0].item.key, std::string("3171632315"));
  CHECK_EQ(check.differ[0].host_id, std::string("2222222222222222"));
  CHECK_EQ(check.differ[0].own_id, std::string("1111111111111111"));

  const join_notice notice = differ_notice(check);
  CHECK_EQ(notice.title, std::string("Different versions"));
  CHECK(
      contains(notice.message, "Mod: DS4C Patched (Workshop item 3171632315)"));
  CHECK(contains(notice.message, "Join anyway?"));
  CHECK(notice.workshop_pages.empty()); // "Yes" joins, it opens nothing
  CHECK(missing_notice(check).message.empty());

  // A map's id may be listed under its Workshop id.
  fake_pc maps;
  maps.items["zm_rust"] = {true, "4444444444444444", "RUST"};
  const auto map_items = host_items("", "", "", "zm_rust", "3010010010");
  CHECK_EQ(check_join(map_items,
                      parse_content_ids("3010010010=5555555555555555"),
                      maps.lookup())
               .differ.size(),
           std::size_t(1));
}

TEST_CASE("join: text from the host is shown safely") {
  fake_pc pc;
  const auto items =
      host_items("my^1mod\x01", std::string(200, 'x'), "", "", "");
  const join_notice notice = missing_notice(check_join(items, {}, pc.lookup()));
  CHECK(!contains(notice.message, "^"));
  CHECK(!contains(notice.message, "\x01"));
  CHECK(contains(notice.message, "..."));
}

TEST_CASE("join: the Xerces version warning") {
  const std::string own = xerces_version_text;
  CHECK_EQ(xerces_version_warning(own, own), std::string());
  CHECK_EQ(xerces_version_warning("", own), std::string()); // BOIII, older
  const std::string warning = xerces_version_warning("0.2.0", "0.1.0-dev");
  CHECK(contains(warning, "The host runs Xerces 0.2.0, you run 0.1.0-dev."));
  CHECK(!contains(xerces_version_warning("^1evil\nline", own), "evil"));
}

TEST_CASE("join: the message for each lobby join failure") {
  CHECK_EQ(lobby_failure_message(lobby_failure::full, 0),
           std::string("The host's lobby is full."));
  CHECK(contains(lobby_failure_message(lobby_failure::closed, 0),
                 "DIRECT CONNECT button"));
  CHECK(contains(lobby_failure_message(lobby_failure::version, 0),
                 "same Xerces build"));
  CHECK(contains(lobby_failure_message(lobby_failure::other, 42), "code 42"));
  // No switched-off feature and no BOIII in any of them.
  for (const lobby_failure failure :
       {lobby_failure::full, lobby_failure::busy, lobby_failure::closed,
        lobby_failure::network_mode, lobby_failure::version,
        lobby_failure::in_progress, lobby_failure::refused,
        lobby_failure::other}) {
    const std::string message = lobby_failure_message(failure, 7);
    CHECK(!contains(message, "BOIII"));
    CHECK(!contains(message, "friends list"));
    CHECK(!contains(message, "friend's"));
  }
  for (const std::string &message :
       {unreachable_message(27017), host_in_menus_message()}) {
    CHECK(!contains(message, "BOIII"));
    CHECK(!contains(message, "friends list"));
    CHECK(!contains(message, "enable UPnP"));
    CHECK(contains(message, "DIRECT CONNECT"));
  }
  CHECK(contains(unreachable_message(27018), "UDP port 27018"));
  CHECK(contains(unreachable_message(27017), "boiii.exe"));
}

TEST_CASE("join: a missing map names itself, never a download") {
  const std::string with_page = missing_map_message("zm_rust", "3010010010");
  CHECK(contains(with_page, "zm_rust (Workshop item 3010010010)"));
  CHECK(!contains(with_page, "download it..."));
  CHECK(!contains(with_page, "Reconnecting"));
  const std::string without = missing_map_message("zm_rust", "");
  CHECK(contains(without, "Ask the host for its Workshop page"));
}

TEST_CASE("join: router addresses are told apart") {
  CHECK(classify_ipv4("203.0.113.7") == ipv4_scope::public_net);
  CHECK(classify_ipv4("100.64.0.1") == ipv4_scope::shared);
  CHECK(classify_ipv4("100.127.255.255") == ipv4_scope::shared);
  CHECK(classify_ipv4("100.128.0.1") == ipv4_scope::public_net);
  CHECK(classify_ipv4("10.1.2.3") == ipv4_scope::private_net);
  CHECK(classify_ipv4("172.16.0.1") == ipv4_scope::private_net);
  CHECK(classify_ipv4("172.32.0.1") == ipv4_scope::public_net);
  CHECK(classify_ipv4("192.168.1.1") == ipv4_scope::private_net);
  CHECK(classify_ipv4("127.0.0.1") == ipv4_scope::loopback);
  for (const char *bad : {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "a.b.c.d",
                          "1..2.3", "1.2.3.4 "}) {
    CHECK(classify_ipv4(bad) == ipv4_scope::invalid);
  }
}

TEST_CASE("join: what opening a lobby to direct connect says") {
  const auto pending =
      direct_connect_notice(port_map_result::pending, "", 27017);
  CHECK_EQ(pending.title, std::string("Open to direct connect"));
  CHECK(contains(pending.message, "UDP port 27017"));

  const auto open =
      direct_connect_notice(port_map_result::mapped, "203.0.113.7", 27017);
  CHECK_EQ(open.title, std::string("Router port open"));
  CHECK(contains(open.message, "203.0.113.7:27017"));

  const auto cgnat =
      direct_connect_notice(port_map_result::mapped, "100.70.1.2", 27017);
  CHECK(contains(cgnat.title, "shared"));
  CHECK(contains(cgnat.message, "Let someone else host"));

  const auto double_nat =
      direct_connect_notice(port_map_result::mapped, "192.168.0.2", 27017);
  CHECK(contains(double_nat.title, "another router"));

  CHECK(contains(
      direct_connect_notice(port_map_result::no_router, "", 27018).message,
      "forward UDP port 27018"));
  CHECK(
      contains(direct_connect_notice(port_map_result::refused, "", 27017).title,
               "refused"));
  CHECK(!contains(
      direct_connect_notice(port_map_result::closed, "", 27017).message,
      "router"));
  CHECK(contains(
      direct_connect_notice(port_map_result::closed, "203.0.113.7:27017", 27017)
          .message,
      "no longer forwards"));
}

TEST_CASE("join: the journal records the host's mods") {
  std::vector<installed_entry> index(1);
  index[0].folder = "1944930083";
  index[0].id = "ws.1944930083";
  index[0].content_id = "0123456789abcdef";
  CHECK(joined_stack_ids({"1944930083", "3171632315", ""}, index) ==
        std::vector<std::string>({"ws.1944930083", "3171632315"}));

  launch_journal journal;
  journal.plan_id = "sha256:abc";
  journal.stack = {"ws.1944930083", "ws.3171632315"};
  journal.phase = launch_phase::in_game;
  // Not joined: written as before, so older launchers read it unchanged.
  CHECK(!contains(journal_to_json(journal), "joined"));
  CHECK(decide_safe_mode(journal).safe_mode);

  // Joined a host with other mods, then crashed: this plan stays on.
  journal.joined = true;
  journal.joined_stack = {"ws.1944930083"};
  const auto back = parse_journal(journal_to_json(journal));
  REQUIRE(back.has_value());
  CHECK(back->joined);
  CHECK_EQ(back->joined_stack, journal.joined_stack);
  CHECK_EQ(journal_to_json(*back), journal_to_json(journal));
  const safe_mode_decision after_join = decide_safe_mode(journal);
  CHECK(!after_join.safe_mode);
  CHECK(after_join.crashed_after_join);
  CHECK(!culprit_search_possible(after_join));

  // A vanilla host counts too: none of this plan's mods ran.
  journal.joined_stack.clear();
  CHECK(!decide_safe_mode(journal).safe_mode);

  // The host ran this plan's own stack: the crash is these mods' as before.
  journal.joined_stack = journal.stack;
  CHECK(decide_safe_mode(journal).safe_mode);
  CHECK(!decide_safe_mode(journal).crashed_after_join);

  // A clean exit after a join is no crash at all.
  journal.joined_stack = {"x"};
  journal.phase = launch_phase::clean_exit;
  CHECK(!decide_safe_mode(journal).crashed_after_join);

  // An older journal has no "joined".
  const auto old = parse_journal(
      R"({"schema":1,"plan_id":"p","stack":["a"],"phase":"in_game"})");
  REQUIRE(old.has_value());
  CHECK(!old->joined);
  CHECK(old->joined_stack.empty());
  // "joined_stack" without "joined" is ignored.
  const auto odd = parse_journal(
      R"({"plan_id":"p","stack":["a"],"phase":"in_game","joined_stack":["b"]})");
  REQUIRE(odd.has_value());
  CHECK(odd->joined_stack.empty());
}

TEST_CASE("join: one offer gives one key from getinfo and from the lobby") {
  const std::string ids = "zm_rust=0123456789abcdef";
  // getinfo names a custom map without a mod "usermaps", the lobby "".
  CHECK_EQ(join_offer_key("usermaps", "", "zm_rust", ids),
           join_offer_key("", "", "zm_rust", ids));
  // Spaces, duplicates and the main mod in the stack change nothing.
  CHECK_EQ(
      join_offer_key("1944930083", "3171632315, 1944930083,3171632315", "", ""),
      join_offer_key("1944930083", "3171632315", "", ""));
  // Another mod, another order, another map or other ids are another offer.
  CHECK(join_offer_key("1944930083", "a,b", "", "") !=
        join_offer_key("1944930083", "b,a", "", ""));
  CHECK(join_offer_key("1944930083", "", "", "") !=
        join_offer_key("", "1944930083", "", ""));
  CHECK(join_offer_key("", "", "zm_rust", ids) !=
        join_offer_key("", "", "zm_rust", "zm_rust=fedcba9876543210"));
  CHECK(join_offer_key("", "", "zm_rust", "") !=
        join_offer_key("", "", "zm_other", ""));
}

TEST_CASE("join: a host's long lists are cut short") {
  std::string stack;
  for (int i = 0; i < 200; ++i) {
    stack += (i ? "," : "") + std::to_string(1000000000 + i);
  }
  const auto items = host_items("999", "", stack, "zm_rust", "3010010010");
  // At most max_host_mods mods, the main mod among them, and the map.
  REQUIRE_EQ(items.size(), max_host_mods + 1);
  CHECK(items.front().kind == host_item_kind::main_mod);
  CHECK(items.back().kind == host_item_kind::map);

  fake_pc pc;
  const join_check check = check_join(items, {}, pc.lookup());
  REQUIRE_EQ(check.missing.size(), items.size());
  const join_notice notice = missing_notice(check);
  CHECK_EQ(notice.workshop_pages.size(), max_workshop_pages);
  CHECK(contains(notice.message,
                 "- and " + std::to_string(items.size() - max_listed_items) +
                     " more\n"));
  // Steam gets every one of them; only the pages to open stop at 10
  CHECK(contains(notice.message, "Get the " + std::to_string(items.size()) +
                                     " Workshop items through Steam now?"));
  CHECK_EQ(notice.workshop_ids.size(), items.size());
  // One line per listed item, plus the "and more" line.
  std::size_t lines = 0;
  for (std::size_t at = 0;
       (at = notice.message.find("\n- ", at)) != std::string::npos; ++at) {
    ++lines;
  }
  CHECK_EQ(lines, max_listed_items + 1);

  // Up to the limit nothing is cut.
  const join_check few = check_join(
      host_items("", "", "1,2,3,4,5,6,7,8,9,10", "", ""), {}, pc.lookup());
  const join_notice all = missing_notice(few);
  CHECK_EQ(all.workshop_pages.size(), std::size_t(10));
  CHECK(!contains(all.message, "more\n"));
  CHECK(contains(all.message, "Get the 10 Workshop items through Steam now?"));

  // The "Different versions" list is cut the same way.
  fake_pc has_all;
  std::vector<content_id> host_ids;
  for (const auto &item : items) {
    has_all.items[item.key] = {true, "1111111111111111", ""};
    host_ids.push_back({item.key, "2222222222222222"});
  }
  const join_check differ = check_join(
      items, parse_content_ids(format_content_ids(host_ids)), has_all.lookup());
  // Only the first max_content_ids ids travel; each of them differs.
  CHECK_EQ(differ.differ.size(), max_content_ids);
  CHECK(contains(differ_notice(differ).message, " more\n"));
}

TEST_CASE("join: a forwarded port without an outside address") {
  CHECK(classify_mapped("203.0.113.7") == mapped_reach::public_address);
  CHECK(classify_mapped("100.64.0.1") == mapped_reach::shared);
  CHECK(classify_mapped("192.168.0.2") == mapped_reach::private_address);
  for (const char *none : {"", "0.0.0.0", "127.0.0.1", "garbage"}) {
    CHECK(classify_mapped(none) == mapped_reach::unknown);
    const auto notice =
        direct_connect_notice(port_map_result::mapped, none, 27017);
    // The port is open; nothing blames a second router or says to swap.
    CHECK_EQ(notice.title, std::string("Router port open"));
    CHECK(contains(notice.message, "did not say its internet address"));
    CHECK(contains(notice.message, "port 27017"));
    CHECK(!contains(notice.message, "another router"));
    CHECK(!contains(notice.message, "someone else host"));
    CHECK(!contains(notice.message, "()"));
  }
}

TEST_CASE("join: a culprit test that joined another game counts for nothing") {
  CHECK(culprit_test_result(std::nullopt) == culprit_test_end::crashed);

  launch_journal test;
  test.stack = {"ws.1944930083"}; // the mods under test
  test.phase = launch_phase::in_game;
  CHECK(culprit_test_result(test) == culprit_test_end::crashed);
  test.phase = launch_phase::clean_exit;
  CHECK(culprit_test_result(test) == culprit_test_end::ran_fine);

  // Joined a host with the whole D3 stack: neither a crash nor a clean run
  // says anything about the one mod under test.
  test.joined = true;
  test.joined_stack = {"ws.1944930083", "ws.3171632315", "ws.3656779213"};
  CHECK(journal_ran_host_mods(test));
  CHECK(culprit_test_result(test) == culprit_test_end::not_counted);
  test.phase = launch_phase::loading_map;
  CHECK(culprit_test_result(test) == culprit_test_end::not_counted);

  // A vanilla host: none of the tested mods ran either.
  test.joined_stack.clear();
  CHECK(culprit_test_result(test) == culprit_test_end::not_counted);

  // The host ran exactly the tested mods: the result counts.
  test.joined_stack = test.stack;
  CHECK(!journal_ran_host_mods(test));
  CHECK(culprit_test_result(test) == culprit_test_end::crashed);
  test.phase = launch_phase::clean_exit;
  CHECK(culprit_test_result(test) == culprit_test_end::ran_fine);
}
