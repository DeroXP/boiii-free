// The Servers screen's bookkeeping (docs/DESIGN.md 7.3, 11.1): recent
// servers, favourites and the reasons a typed address is refused.

#include "harness.hpp"

#include <string>
#include <vector>

#include <xerces_core/launch.hpp>
#include <xerces_core/servers_book.hpp>

using namespace xerces_core;

TEST_CASE(
    "servers: a problem is reported exactly when the address is refused") {
  const std::vector<std::string> good = {"192.168.1.20:27017",
                                         "10.0.0.1",
                                         "host.example:28960",
                                         "[::1]:27017",
                                         "[fe80::1]",
                                         "my-server.lan:1",
                                         "a:65535"};
  for (const auto &address : good) {
    CHECK(valid_server_address(address));
    CHECK_EQ(server_address_problem(address), std::string());
  }
  const std::vector<std::string> bad = {"a b:1",
                                        "x:0",
                                        "x:70000",
                                        "x;quit",
                                        "+connect",
                                        "-flag",
                                        ".lan",
                                        "host:",
                                        ":27017",
                                        "1.2.3.4:abc",
                                        "::1",
                                        "[::1",
                                        "[::1]x",
                                        "[zz]:1",
                                        "[a]",
                                        "[::1]:",
                                        "x:123456",
                                        "a\"b:1",
                                        std::string(300, 'a')};
  for (const auto &address : bad) {
    CHECK(!valid_server_address(address));
    CHECK(!server_address_problem(address).empty());
  }
  CHECK_EQ(server_address_problem(""), std::string());
}

TEST_CASE("servers: the reasons say what to fix") {
  CHECK_EQ(server_address_problem("1.2.3.4:"),
           std::string("Add the port after the colon, for example :27017."));
  CHECK_EQ(server_address_problem("1.2.3.4:99999"),
           std::string("The port is a number from 1 to 65535."));
  CHECK_EQ(server_address_problem("1.2.3.4 :1"),
           std::string("Remove the spaces from the address."));
  // The game connects over IPv4 only, so no hint shows an IPv6 example.
  CHECK_EQ(server_address_problem("fe80::1"), std::string(ipv4_only_problem));
  CHECK_EQ(server_address_problem("[zz]:1"), std::string(ipv4_only_problem));
  for (const char *typed : {"[::1", "[::1]x"}) {
    const std::string problem = server_address_problem(typed);
    CHECK(problem.find("IPv4 only") != std::string::npos);
    CHECK(problem.find("[::1]") == std::string::npos);
  }
  CHECK_EQ(
      server_address_problem("srv_1:27017"),
      std::string("Use only letters, digits, dots and dashes in the host."));
  CHECK_EQ(server_address_problem("-x"),
           std::string("The host cannot start with a dot or a dash."));
}

TEST_CASE("servers: an IPv6 address can be saved but not joined") {
  // Valid for the command line and for favourites, but the game's sockets
  // are IPv4 only.
  CHECK(valid_server_address("[::1]:27017"));
  CHECK_EQ(server_address_problem("[::1]:27017"), std::string());
  CHECK_EQ(server_join_problem("[::1]:27017"), std::string(ipv4_only_problem));
  CHECK_EQ(server_join_problem("[fe80::1]"), std::string(ipv4_only_problem));
  CHECK_EQ(server_join_problem("192.168.1.20:27017"), std::string());
  CHECK_EQ(server_join_problem("my-server.lan"), std::string());
  CHECK_EQ(server_join_problem("x:0"), server_address_problem("x:0"));
  CHECK_EQ(server_join_problem(""), std::string());
}

TEST_CASE("servers: addresses are trimmed and compared without case") {
  CHECK_EQ(trim_server_address("  10.0.0.1:27017 \t\r\n"),
           std::string("10.0.0.1:27017"));
  CHECK_EQ(trim_server_address("   "), std::string());
  CHECK(same_server("Host.Example:1", "host.example:1"));
  CHECK(same_server("[FE80::1]:5", "[fe80::1]:5"));
  CHECK(!same_server("host:1", "host:2"));
  CHECK(!same_server("host", "host:1"));
}

TEST_CASE("servers: recent servers are newest first, unique and capped") {
  std::vector<std::string> recent;
  CHECK(remember_recent_server(recent, "a:1"));
  CHECK(remember_recent_server(recent, "b:1"));
  CHECK(remember_recent_server(recent, " A:1 "));
  CHECK_EQ(recent.size(), std::size_t(2));
  CHECK_EQ(recent[0], std::string("A:1"));
  CHECK_EQ(recent[1], std::string("b:1"));

  CHECK(!remember_recent_server(recent, "not an address"));
  CHECK(!remember_recent_server(recent, ""));
  CHECK_EQ(recent.size(), std::size_t(2));

  for (int i = 0; i < 15; ++i) {
    CHECK(remember_recent_server(recent, "h" + std::to_string(i) + ":27017"));
  }
  CHECK_EQ(recent.size(), max_recent_servers);
  CHECK_EQ(recent.front(), std::string("h14:27017"));
  CHECK_EQ(recent.back(), std::string("h5:27017"));

  CHECK(forget_recent_server(recent, "H14:27017"));
  CHECK(!forget_recent_server(recent, "h14:27017"));
  CHECK_EQ(recent.size(), max_recent_servers - 1);
  CHECK_EQ(recent.front(), std::string("h13:27017"));
}

TEST_CASE("servers: favourite names are cleaned and fall back to the address") {
  CHECK_EQ(clean_server_name("  Friday zombies \n", "a:1"),
           std::string("Friday zombies"));
  CHECK_EQ(clean_server_name("", "a:1"), std::string("a:1"));
  CHECK_EQ(clean_server_name("\t\x01 ", "a:1"), std::string("a:1"));
  CHECK_EQ(clean_server_name("Tab\there", "a:1"), std::string("Tabhere"));
  CHECK_EQ(clean_server_name(std::string(80, 'n'), "a:1").size(),
           max_server_name);
  // 63 ASCII bytes and then a two-byte character: cut before it, not inside.
  const std::string name = std::string(63, 'n') + "\xC3\xA9" + "tail";
  CHECK_EQ(clean_server_name(name, "a:1"), std::string(63, 'n'));
}

TEST_CASE("servers: favourites are added, renamed and kept unique") {
  std::vector<favorite_server> favorites;
  favorite_edit edit =
      save_favorite(favorites, "Home LAN", " 192.168.1.20:27017 ");
  CHECK(edit.ok());
  CHECK_EQ(edit.index, std::size_t(0));
  REQUIRE(favorites.size() == 1);
  CHECK_EQ(favorites[0].address, std::string("192.168.1.20:27017"));
  CHECK_EQ(favorites[0].name, std::string("Home LAN"));

  edit = save_favorite(favorites, "", "srv.example:28960");
  CHECK(edit.ok());
  CHECK_EQ(favorites[1].name, std::string("srv.example:28960"));

  edit = save_favorite(favorites, "Again", "SRV.example:28960");
  CHECK((edit.status == favorite_status::duplicate));
  CHECK_EQ(edit.index, std::size_t(1));

  edit = save_favorite(favorites, "Bad", "bad address");
  CHECK((edit.status == favorite_status::bad_address));
  CHECK_EQ(favorites.size(), std::size_t(2));

  // A rename keeps its own address without calling it a duplicate.
  edit = save_favorite(favorites, "Friday", "srv.example:28960", 1);
  CHECK(edit.ok());
  CHECK_EQ(favorites[1].name, std::string("Friday"));
  // Moving one favourite onto another's address is refused.
  edit = save_favorite(favorites, "Clash", "192.168.1.20:27017", 1);
  CHECK((edit.status == favorite_status::duplicate));
  CHECK_EQ(edit.index, std::size_t(0));
  edit = save_favorite(favorites, "Gone", "a:1", 7);
  CHECK((edit.status == favorite_status::missing));

  CHECK_EQ(find_favorite(favorites, "srv.EXAMPLE:28960").value_or(99),
           std::size_t(1));
  CHECK(!find_favorite(favorites, "nowhere:1").has_value());

  std::vector<favorite_server> many;
  for (std::size_t i = 0; i < max_favorite_servers; ++i) {
    CHECK(save_favorite(many, "", "h" + std::to_string(i) + ":1").ok());
  }
  CHECK(
      (save_favorite(many, "", "one-more:1").status == favorite_status::full));
}
