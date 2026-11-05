// The first-run screen's BOIII import (docs/DESIGN.md 3.5, 7.3): what it
// finds in a game folder's boiii_players, that it never reads the identity
// folder, that it never changes a BOIII file, and how it merges into the
// launcher config.

#include "harness.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <xerces_core/first_run_import.hpp>
#include <xerces_core/launch.hpp>

#define NOMINMAX
#include <Windows.h>

using namespace xerces_core;
using xerces_tests::make_junction;
namespace fs = std::filesystem;

namespace {
struct temp_dir {
  fs::path path;
  explicit temp_dir(const char *name)
      : path(fs::temp_directory_path() / name) {
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void write(const fs::path &file, const std::string &text) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << text;
}
} // namespace

TEST_CASE("first run: player name and server lists from boiii_players") {
  temp_dir root("xerces_first_run_found");
  const fs::path game = root.path / "Call of Duty Black Ops III";
  const fs::path appdata = root.path / "boiii";
  write(appdata / "cache" / "x.bin", "cache");
  // Upstream's reader accepts a BOM; the name keeps colour codes and loses
  // anything that is not printable ASCII.
  write(game / "boiii_players" / "properties.json",
        "\xEF\xBB\xBF{\"playerName\":\"  ^1Ghost\\u00e9 \",\"launchOptions\":\"-x\"}");
  write(game / "boiii_players" / "user" / "favorite_servers.txt",
        "192.168.1.20:27017\r\n10.0.0.5:27018\n\n192.168.1.20:27017\n"
        "0.0.0.0:0\nnot an address\nplay.example.org:27017\n");
  write(game / "boiii_players" / "user" / "recent_servers.txt",
        "10.0.0.9:27017\n10.0.0.5:27018\n");

  const boiii_import found = find_boiii_import({appdata}, game);
  REQUIRE(found.found_anything());
  REQUIRE(found.player_name.has_value());
  CHECK_EQ(*found.player_name, std::string("^1Ghost"));
  CHECK(found.player_name_file == game / "boiii_players" / "properties.json");
  REQUIRE_EQ(found.favorites.size(), std::size_t(3));
  CHECK_EQ(found.favorites[0], std::string("192.168.1.20:27017"));
  CHECK_EQ(found.favorites[1], std::string("10.0.0.5:27018"));
  CHECK_EQ(found.favorites[2], std::string("play.example.org:27017"));
  REQUIRE_EQ(found.recent_servers.size(), std::size_t(2));
  CHECK_EQ(found.recent_servers[0], std::string("10.0.0.9:27017"));
  CHECK_EQ(found.skipped_lines, std::size_t(2));
  REQUIRE_EQ(found.places.size(), std::size_t(2));
  CHECK_EQ(found.places[0].kind, std::string("appdata"));
  CHECK(found.places[0].exists);
  CHECK_EQ(found.places[1].kind, std::string("players"));
  CHECK(found.places[1].exists);
  CHECK(!found.diags.has_errors());
}

TEST_CASE("first run: nothing to import") {
  temp_dir root("xerces_first_run_empty");
  const fs::path game = root.path / "game";
  fs::create_directories(game);

  const boiii_import none = find_boiii_import({root.path / "boiii"}, game);
  CHECK(!none.found_anything());
  REQUIRE_EQ(none.places.size(), std::size_t(2));
  CHECK(!none.places[0].exists);
  CHECK(!none.places[1].exists);

  // No game folder known: only %LOCALAPPDATA%\boiii is reported.
  const boiii_import no_game = find_boiii_import({root.path / "boiii"}, {});
  CHECK(!no_game.found_anything());
  CHECK_EQ(no_game.places.size(), std::size_t(1));
  CHECK(find_boiii_import({}, {}).places.empty());

  // A properties.json without a name, and broken JSON, give no name.
  write(game / "boiii_players" / "properties.json", "{\"launchOptions\":\"\"}");
  CHECK(
      !find_boiii_import({root.path / "boiii"}, game).player_name.has_value());
  write(game / "boiii_players" / "properties.json", "{\"playerName\":");
  CHECK(
      !find_boiii_import({root.path / "boiii"}, game).player_name.has_value());
  write(game / "boiii_players" / "properties.json",
        "{\"playerName\":\"\\u00e9\\u00e9\"}");
  CHECK(
      !find_boiii_import({root.path / "boiii"}, game).player_name.has_value());
}

TEST_CASE("first run: the identity folder is never read") {
  temp_dir root("xerces_first_run_identity");
  const fs::path appdata = root.path / "boiii";
  write(appdata / "user" / "properties.json", "{\"playerName\":\"Legacy\"}");

  CHECK(is_boiii_identity_path(appdata / "user", appdata));
  CHECK(is_boiii_identity_path(appdata / "user" / "properties.json", appdata));
  CHECK(is_boiii_identity_path(appdata / "USER" / "x.key", appdata));
  CHECK(is_boiii_identity_path(appdata / "cache" / ".." / "user" / "a", appdata));
  CHECK(!is_boiii_identity_path(appdata / "users" / "a", appdata));
  CHECK(!is_boiii_identity_path(appdata / "cache", appdata));
  CHECK(!is_boiii_identity_path(root.path / "game" / "boiii_players" / "user",
                                appdata));
  CHECK(!is_boiii_identity_path(appdata / "user", fs::path()));

  // The legacy properties.json upstream once kept in user\ is not looked at,
  // even when a "game folder" sits inside the identity folder.
  const fs::path inside = appdata / "user" / "game";
  write(inside / "boiii_players" / "properties.json",
        "{\"playerName\":\"Inside\"}");
  const boiii_import found = find_boiii_import({appdata}, inside);
  CHECK(!found.player_name.has_value());
  CHECK(found.diags.has("first_run.import.identity"));
  REQUIRE_EQ(found.places.size(), std::size_t(2));
  CHECK(!found.places[1].exists);

  const boiii_import top = find_boiii_import({appdata}, {});
  CHECK(!top.found_anything());
}

TEST_CASE("first run: links into the identity folder are seen from outside it") {
  using enum boiii_identity_check;
  temp_dir root("xerces_first_run_links");
  const fs::path appdata = root.path / "boiii";
  const fs::path identity = appdata / "user";
  write(identity / "game" / "boiii_players" / "properties.json",
        "{\"playerName\":\"Inside\"}");
  write(appdata / "cache" / "x.bin", "cache");

  // boiii_players is a junction into the identity folder: nothing is read.
  const fs::path game = root.path / "game";
  REQUIRE(make_junction(game / "boiii_players",
                        identity / "game" / "boiii_players"));
  CHECK((check_boiii_identity_path(game / "boiii_players" / "properties.json",
                                   appdata) == inside));
  const boiii_import through = find_boiii_import({appdata}, game);
  CHECK(!through.player_name.has_value());
  CHECK(through.diags.has("first_run.import.identity"));
  REQUIRE_EQ(through.places.size(), std::size_t(2));
  CHECK(!through.places[1].exists);

  // Into the identity folder where the target does not exist; through the
  // app data folder and then "user"; and a junction whose shown name points
  // somewhere harmless while Windows follows it into the identity folder.
  REQUIRE(make_junction(root.path / "ghost", identity / "nothing"));
  CHECK((check_boiii_identity_path(root.path / "ghost" / "x", appdata) == inside));
  REQUIRE(make_junction(root.path / "up", appdata));
  CHECK((check_boiii_identity_path(root.path / "up" / "USER" / "k", appdata) ==
         inside));
  CHECK((check_boiii_identity_path(root.path / "up" / "cache" / "x.bin",
                                   appdata) == outside));
  REQUIRE(make_junction(root.path / "liar", identity, root.path / "elsewhere"));
  CHECK((check_boiii_identity_path(root.path / "liar" / "x", appdata) == inside));

  // A junction elsewhere is followed and read through.
  const fs::path other = root.path / "other";
  write(root.path / "elsewhere" / "boiii_players" / "properties.json",
        "{\"playerName\":\"Elsewhere\"}");
  REQUIRE(make_junction(other / "boiii_players",
                        root.path / "elsewhere" / "boiii_players"));
  CHECK((check_boiii_identity_path(other / "boiii_players" / "properties.json",
                                   appdata) == outside));
  const boiii_import linked = find_boiii_import({appdata}, other);
  REQUIRE(linked.player_name.has_value());
  CHECK_EQ(*linked.player_name, std::string("Elsewhere"));

  // A loop of links, and names Windows does not allow, cannot be checked:
  // they are not read either, with their own note.
  REQUIRE(make_junction(root.path / "loop_a", root.path / "loop_b"));
  REQUIRE(make_junction(root.path / "loop_b", root.path / "loop_a"));
  CHECK((check_boiii_identity_path(root.path / "loop_a" / "x", appdata) ==
         unknown));
  CHECK(is_boiii_identity_path(root.path / "loop_a" / "x", appdata));
  CHECK((check_boiii_identity_path(root.path / "ga*e" / "x", appdata) == unknown));
  CHECK((check_boiii_identity_path(root.path / "game:s" / "x", appdata) ==
         unknown));
  const boiii_import looped =
      find_boiii_import({appdata}, root.path / "loop_a");
  CHECK(looped.diags.has("first_run.import.unchecked"));
  CHECK(!looped.diags.has("first_run.import.identity"));

  // Short (8.3) names reach the same folder; this needs them on the volume.
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetShortPathNameW(root.path.c_str(), buffer.data(),
                                         DWORD(buffer.size()));
  const fs::path short_root(buffer.substr(0, length));
  if (length > 0 && short_root.native() != root.path.native()) {
    CHECK((check_boiii_identity_path(short_root / "boiii" / "user" / "k",
                                     appdata) == inside));
    CHECK((check_boiii_identity_path(short_root / "boiii" / "cache", appdata) ==
           outside));
  }

  // Symbolic links need Developer Mode or elevation; checked when allowed.
  std::error_code ec;
  fs::create_directory_symlink(identity, root.path / "sym", ec);
  if (!ec) {
    CHECK((check_boiii_identity_path(root.path / "sym" / "x", appdata) == inside));
  }
  fs::create_directory_symlink("boiii\\user", root.path / "relsym", ec);
  if (!ec) {
    CHECK((check_boiii_identity_path(root.path / "relsym" / "x", appdata) ==
           inside));
  }
}

TEST_CASE("first run: BOIII files are only read") {
  temp_dir root("xerces_first_run_readonly");
  const fs::path game = root.path / "game";
  const fs::path properties = game / "boiii_players" / "properties.json";
  const fs::path favorites =
      game / "boiii_players" / "user" / "favorite_servers.txt";
  const std::string properties_text = "{\"playerName\":\"Reader\"}";
  const std::string favorites_text = "1.2.3.4:27017\n";
  write(properties, properties_text);
  write(favorites, favorites_text);
  const auto properties_time = fs::last_write_time(properties);
  const auto favorites_time = fs::last_write_time(favorites);

  launcher_config config;
  const boiii_import found = find_boiii_import({root.path / "boiii"}, game);
  const boiii_import_result applied =
      apply_boiii_import(config, found, boiii_import_choice{});
  CHECK(applied.changed());

  CHECK_EQ(xerces_tests::read_file(properties), properties_text);
  CHECK_EQ(xerces_tests::read_file(favorites), favorites_text);
  CHECK((fs::last_write_time(properties) == properties_time));
  CHECK((fs::last_write_time(favorites) == favorites_time));
  // Nothing new appears next to them either.
  std::size_t files = 0;
  for (const auto &entry : fs::recursive_directory_iterator(game)) {
    files += entry.is_regular_file() ? 1 : 0;
  }
  CHECK_EQ(files, std::size_t(2));
}

TEST_CASE("first run: server addresses") {
  CHECK(is_server_address("192.168.1.20:27017"));
  CHECK(is_server_address("play.example.org:1"));
  CHECK(is_server_address("host-1:65535"));
  CHECK(!is_server_address("192.168.1.20"));
  CHECK(!is_server_address(":27017"));
  CHECK(!is_server_address("host:"));
  CHECK(!is_server_address("host:0"));
  CHECK(!is_server_address("host:65536"));
  CHECK(!is_server_address("host:12a"));
  CHECK(!is_server_address("0.0.0.0:27017"));
  CHECK(!is_server_address("bad host:27017"));
  CHECK(!is_server_address("evil\"<b>:27017"));
  // launch refuses these (a leading '-' or '.' is not a host), so the import
  // does too: there is one rule for a server address.
  CHECK(!is_server_address("-x:1"));
  CHECK(!is_server_address(".x:1"));
  CHECK(is_server_address("[::1]:27017"));
  CHECK(!is_server_address("[::1]"));
  CHECK(!is_server_address("::1:27017"));
  for (const char *address :
       {"192.168.1.20:27017", "play.example.org:1", "host-1:65535",
        "[::1]:27017", "-x:1", ".x:1", "host:0", "host:"}) {
    if (is_server_address(address)) {
      CHECK(valid_server_address(address));
    }
  }

  std::size_t skipped = 0;
  const auto list = parse_boiii_server_list(
      "A.example:1\na.EXAMPLE:1\nb.example:2\nc.example:3\n", 2, &skipped);
  REQUIRE_EQ(list.size(), std::size_t(2));
  CHECK_EQ(list[0], std::string("A.example:1"));
  CHECK_EQ(list[1], std::string("b.example:2"));
  CHECK_EQ(skipped, std::size_t(0));
  CHECK(parse_boiii_server_list("", 10).empty());
  CHECK(parse_boiii_server_list("\r\n\r\n", 10).empty());
}

TEST_CASE("first run: names") {
  CHECK_EQ(sanitize_boiii_player_name("  Ghost  "), std::string("Ghost"));
  CHECK_EQ(sanitize_boiii_player_name("Gh\x01ost\x7F"), std::string("Ghost"));
  CHECK_EQ(sanitize_boiii_player_name("\t\r\n"), std::string(""));
  CHECK_EQ(sanitize_boiii_player_name(std::string(100, 'x')).size(),
           std::size_t(64));
  CHECK((boiii_player_name_from_properties("[1,2]") == std::nullopt));
  CHECK((boiii_player_name_from_properties("{\"playerName\":5}") == std::nullopt));
  CHECK((boiii_player_name_from_properties("{\"playerName\":\"Ok\"}") ==
         std::optional<std::string>("Ok")));
}

TEST_CASE("first run: applying the import to the config") {
  boiii_import found;
  found.player_name = "Ghost";
  found.favorites = {"1.1.1.1:27017", "2.2.2.2:27017"};
  found.recent_servers = {"3.3.3.3:27017", "4.4.4.4:27017"};

  launcher_config config;
  config.player_name = "Old";
  config.favorites.push_back({"Home server", "1.1.1.1:27017"});
  config.recent_servers = {"4.4.4.4:27017", "9.9.9.9:27017"};
  CHECK_EQ(new_boiii_favorites(config, found), std::size_t(1));
  CHECK_EQ(new_boiii_recent_servers(config, found), std::size_t(1));

  // Choices are respected.
  boiii_import_choice only_name;
  only_name.favorites = false;
  only_name.recent_servers = false;
  launcher_config named = config;
  const auto first = apply_boiii_import(named, found, only_name);
  CHECK(first.player_name);
  CHECK_EQ(first.favorites, std::size_t(0));
  CHECK_EQ(named.player_name, std::string("Ghost"));
  CHECK_EQ(named.favorites.size(), std::size_t(1));

  const auto all = apply_boiii_import(config, found, boiii_import_choice{});
  CHECK(all.player_name);
  CHECK_EQ(all.favorites, std::size_t(1));
  CHECK_EQ(all.recent_servers, std::size_t(1));
  REQUIRE_EQ(config.favorites.size(), std::size_t(2));
  CHECK_EQ(config.favorites[0].name, std::string("Home server"));
  CHECK_EQ(config.favorites[1].address, std::string("2.2.2.2:27017"));
  CHECK_EQ(config.favorites[1].name, std::string(""));
  // Xerces' own recent servers are newer, so they stay first.
  REQUIRE_EQ(config.recent_servers.size(), std::size_t(3));
  CHECK_EQ(config.recent_servers[0], std::string("4.4.4.4:27017"));
  CHECK_EQ(config.recent_servers[2], std::string("3.3.3.3:27017"));

  // A second import adds nothing.
  const auto again = apply_boiii_import(config, found, boiii_import_choice{});
  CHECK(!again.changed());
  CHECK_EQ(new_boiii_favorites(config, found), std::size_t(0));

  // The recent list stops at the Servers screen's limit.
  launcher_config full;
  for (std::size_t i = 0; i < max_recent_servers; ++i) {
    full.recent_servers.push_back("10.0.0." + std::to_string(i) + ":1");
  }
  // A full list has nothing new to offer: the screen must not show servers
  // the import cannot add.
  std::size_t no_room = 0;
  CHECK_EQ(new_boiii_recent_servers(full, found, &no_room), std::size_t(0));
  CHECK_EQ(no_room, std::size_t(2));
  CHECK_EQ(apply_boiii_import(full, found, boiii_import_choice{}).recent_servers,
           std::size_t(0));
  CHECK_EQ(full.recent_servers.size(), max_recent_servers);

  // One place left: one is new, one does not fit, and after the import
  // nothing is new.
  full.recent_servers.pop_back();
  CHECK_EQ(new_boiii_recent_servers(full, found, &no_room), std::size_t(1));
  CHECK_EQ(no_room, std::size_t(1));
  CHECK_EQ(apply_boiii_import(full, found, boiii_import_choice{}).recent_servers,
           std::size_t(1));
  CHECK_EQ(new_boiii_recent_servers(full, found, &no_room), std::size_t(0));
  CHECK_EQ(no_room, std::size_t(1));
}

TEST_CASE("first run: the import fills Xerces' lists only as far as they go") {
  // BOIII keeps 50 recent servers; the Servers screen keeps 10 and the next
  // join cuts the list back to that. 30 of them on an empty list: 10 arrive,
  // the screen says 20 do not fit, and a join loses none of the 10.
  boiii_import found;
  for (int i = 0; i < 30; ++i) {
    found.recent_servers.push_back("10.1.0." + std::to_string(i) + ":27017");
  }
  launcher_config config;
  std::size_t no_room = 0;
  CHECK_EQ(new_boiii_recent_servers(config, found, &no_room),
           max_recent_servers);
  CHECK_EQ(no_room, std::size_t(30) - max_recent_servers);
  boiii_import_choice recent_only;
  recent_only.player_name = false;
  recent_only.favorites = false;
  CHECK_EQ(apply_boiii_import(config, found, recent_only).recent_servers,
           max_recent_servers);
  REQUIRE_EQ(config.recent_servers.size(), max_recent_servers);
  CHECK_EQ(config.recent_servers.front(), std::string("10.1.0.0:27017"));
  // A join keeps every imported server but the oldest, as with any join.
  const std::vector<std::string> imported = config.recent_servers;
  CHECK(remember_recent_server(config.recent_servers, "192.0.2.1:27017"));
  REQUIRE_EQ(config.recent_servers.size(), max_recent_servers);
  CHECK(std::equal(imported.begin(), imported.end() - 1,
                   config.recent_servers.begin() + 1));

  // Favourites: the Servers screen refuses every add past 100. With 95
  // saved, 5 of BOIII's 8 new ones fit (the first 5, in its order), and an
  // address already saved is not counted against the room.
  launcher_config saved;
  for (std::size_t i = 0; i < max_favorite_servers - 5; ++i) {
    saved.favorites.push_back({"", "10.2.0." + std::to_string(i) + ":1"});
  }
  boiii_import offered;
  offered.favorites.push_back("10.2.0.0:1"); // already saved
  for (int i = 0; i < 8; ++i) {
    offered.favorites.push_back("10.3.0." + std::to_string(i) + ":1");
  }
  CHECK_EQ(new_boiii_favorites(saved, offered, &no_room), std::size_t(5));
  CHECK_EQ(no_room, std::size_t(3));
  boiii_import_choice favorites_only;
  favorites_only.player_name = false;
  favorites_only.recent_servers = false;
  CHECK_EQ(apply_boiii_import(saved, offered, favorites_only).favorites,
           std::size_t(5));
  REQUIRE_EQ(saved.favorites.size(), max_favorite_servers);
  CHECK_EQ(saved.favorites.back().address, std::string("10.3.0.4:1"));
  CHECK_EQ(new_boiii_favorites(saved, offered, &no_room), std::size_t(0));
  CHECK_EQ(no_room, std::size_t(3));
  // The Servers screen agrees: the list is full, not over full.
  CHECK((save_favorite(saved.favorites, "", "10.9.9.9:1").status ==
         favorite_status::full));

  // A list already over the limit (an older import, a hand-edited config)
  // has no room at all.
  for (int i = 0; i < 5; ++i) {
    saved.favorites.push_back({"", "10.4.0." + std::to_string(i) + ":1"});
  }
  CHECK_EQ(new_boiii_favorites(saved, offered, &no_room), std::size_t(0));
  CHECK_EQ(apply_boiii_import(saved, offered, favorites_only).favorites,
           std::size_t(0));
}

namespace {
// Removes a tree that may be deeper than MAX_PATH ("\\?\" names); links are
// removed as links, never followed.
void remove_deep(const std::wstring &dir) {
  WIN32_FIND_DATAW found{};
  const HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &found);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = found.cFileName;
      if (name == L"." || name == L"..") {
        continue;
      }
      const std::wstring child = dir + L"\\" + name;
      const bool folder =
          (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        folder ? RemoveDirectoryW(child.c_str()) : DeleteFileW(child.c_str());
      } else if (folder) {
        remove_deep(child);
      } else {
        DeleteFileW(child.c_str());
      }
    } while (FindNextFileW(find, &found));
    FindClose(find);
  }
  RemoveDirectoryW(dir.c_str());
}

// A folder deeper than MAX_PATH, removed with everything in it when the test
// ends (before temp_dir, whose remove_all cannot reach that deep).
struct deep_tree {
  std::wstring top; // "\\?\C:\..."
  ~deep_tree() { remove_deep(top); }
};

bool make_dir(const std::wstring &dir) {
  return CreateDirectoryW(dir.c_str(), nullptr) ||
         GetLastError() == ERROR_ALREADY_EXISTS;
}
} // namespace

TEST_CASE("first run: shares and network names are never outside") {
  using enum boiii_identity_check;
  temp_dir root("xerces_first_run_shares");
  const fs::path appdata = root.path / "boiii";
  write(appdata / "user" / "k.txt", "SECRET");
  write(appdata / "user" / "game" / "boiii_players" / "properties.json",
        "{\"playerName\":\"Inside\"}");
  write(appdata / "cache" / "x.bin", "cache");

  // The fake identity folder through this PC's own admin share: the same
  // folder under another name. Shares are judged by their name alone, which
  // cannot say which folder of which PC they are, so nothing is opened.
  const std::wstring local = fs::absolute(root.path).native();
  REQUIRE(local.size() > 3 && local[1] == L':');
  const std::wstring below = local.substr(2); // "\Users\...\xerces_..."
  const std::wstring admin = std::wstring(1, local[0]) + L"$";
  for (const std::wstring &share :
       {L"\\\\localhost\\" + admin, L"\\\\127.0.0.1\\" + admin,
        L"\\\\?\\UNC\\localhost\\" + admin, L"\\\\.\\UNC\\localhost\\" + admin,
        L"//localhost/" + admin}) {
    const fs::path through(share + below);
    CHECK((check_boiii_identity_path(through / "boiii" / "user" / "k.txt",
                                     appdata) == unknown));
    CHECK((check_boiii_identity_path(through / "boiii" / "cache" / "x.bin",
                                     appdata) == unknown));
    const boiii_import found =
        find_boiii_import({appdata}, through / "boiii" / "user" / "game");
    CHECK(!found.player_name.has_value());
    CHECK(found.diags.has("first_run.import.unchecked"));
    REQUIRE_EQ(found.places.size(), std::size_t(2));
    CHECK(!found.places[1].exists);
  }
  CHECK(is_boiii_identity_path(fs::path(L"\\\\server\\share\\game"), appdata));
  // Device paths other than a drive are not checked either.
  CHECK(is_boiii_identity_path(
      fs::path(L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\x"), appdata));
}

TEST_CASE("first run: names past MAX_PATH and Windows' own name rules") {
  using enum boiii_identity_check;
  temp_dir root("xerces_first_run_long");
  const fs::path appdata = root.path / "boiii";
  const fs::path identity = appdata / "user";
  write(identity / "k.txt", "SECRET");
  write(identity / "game" / "boiii_players" / "properties.json",
        "{\"playerName\":\"Inside\"}");
  write(appdata / "cache" / "x.bin", "cache");

  // What Windows itself drops or applies before it looks a name up: a
  // trailing dot, a trailing space at the end, "." and "..".
  CHECK((check_boiii_identity_path(appdata / "user." / "k.txt", appdata) ==
         inside));
  CHECK((check_boiii_identity_path(appdata / "user ", appdata) == inside));
  CHECK((check_boiii_identity_path(appdata / "cache" / ".." / "user" / "k.txt",
                                   appdata) == inside));
  // A "\\?\" name is taken as written, as Windows takes it.
  const std::wstring prefixed = L"\\\\?\\" + fs::absolute(appdata).native();
  CHECK((check_boiii_identity_path(fs::path(prefixed + L"\\user\\k.txt"),
                                   appdata) == inside));
  CHECK((check_boiii_identity_path(fs::path(prefixed + L"\\USER"), appdata) ==
         inside));
  CHECK((check_boiii_identity_path(fs::path(prefixed + L"\\cache\\x.bin"),
                                   appdata) == outside));

  // A folder deeper than MAX_PATH (made through "\\?\": the tests are not
  // long-path aware either) with a junction into the identity folder.
  const deep_tree tree{L"\\\\?\\" + fs::absolute(root.path).native() + L"\\" +
                       std::wstring(30, L'a')};
  std::wstring deep = tree.top;
  REQUIRE(make_dir(deep));
  for (int i = 1; i < 12; ++i) {
    deep += L"\\" + std::wstring(30, wchar_t(L'a' + i));
    REQUIRE(make_dir(deep));
  }
  REQUIRE(deep.size() > MAX_PATH + 4);
  REQUIRE(make_dir(deep + L"\\lnk"));
  REQUIRE(make_junction(fs::path(deep + L"\\lnk"), identity));
  CHECK((check_boiii_identity_path(fs::path(deep + L"\\lnk\\k.txt"), appdata) ==
         inside));
  CHECK((check_boiii_identity_path(fs::path(deep + L"\\lnk"), appdata) ==
         inside));
  // Without the prefix, as a config file could name it.
  const std::wstring deep_plain = deep.substr(4);
  CHECK((check_boiii_identity_path(fs::path(deep_plain + L"\\lnk\\k.txt"),
                                   appdata) == inside));

  // A short game folder whose boiii_players leads into the deep folder,
  // where another junction leads on into the identity folder. The game
  // folder is an ordinary one: no "\\?\" anywhere in what the player picked.
  REQUIRE(make_dir(deep + L"\\bp"));
  REQUIRE(make_junction(fs::path(deep + L"\\bp"),
                        identity / "game" / "boiii_players"));
  const fs::path game = root.path / "game";
  REQUIRE(
      make_junction(game / "boiii_players", fs::path(deep_plain + L"\\bp")));
  CHECK((check_boiii_identity_path(game / "boiii_players" / "properties.json",
                                   appdata) == inside));
  const boiii_import through = find_boiii_import({appdata}, game);
  CHECK(!through.player_name.has_value());
  CHECK(through.diags.has("first_run.import.identity"));

  // The deep folder itself is outside, and so is a game folder leading
  // there through a junction (nothing to read in it).
  CHECK((check_boiii_identity_path(fs::path(deep_plain + L"\\elsewhere"),
                                   appdata) == outside));
  const fs::path other = root.path / "other";
  REQUIRE(make_junction(other / "boiii_players", fs::path(deep_plain)));
  CHECK((check_boiii_identity_path(other / "boiii_players" / "x.json",
                                   appdata) == outside));
}

TEST_CASE(
    "first run: the identity folder is looked for where upstream keeps it") {
  using enum boiii_identity_check;
  const fs::path known = "C:\\Users\\Someone\\AppData\\Local";
  // The variable names the same folder (any case, a trailing '\'): one.
  std::vector<fs::path> dirs = boiii_appdata_dirs(
      known, fs::path("c:\\users\\someone\\APPDATA\\local\\"));
  REQUIRE_EQ(dirs.size(), std::size_t(1));
  CHECK(dirs[0] == known / "boiii");
  CHECK_EQ(boiii_appdata_dirs(known, std::nullopt).size(), std::size_t(1));
  // Pointed elsewhere (a portable launcher, a script): both, the known
  // folder (where the fenced code reads the key) first.
  dirs = boiii_appdata_dirs(known, fs::path("D:\\Portable\\AppData"));
  REQUIRE_EQ(dirs.size(), std::size_t(2));
  CHECK(dirs[0] == known / "boiii");
  CHECK(dirs[1] == fs::path("D:\\Portable\\AppData") / "boiii");
  // Without the known folder the identity folder cannot be told, whatever
  // the variable says.
  CHECK(boiii_appdata_dirs(std::nullopt, known).empty());
  CHECK(boiii_appdata_dirs(fs::path(), known).empty());

  temp_dir root("xerces_first_run_appdata");
  const fs::path real = root.path / "Local";     // the known folder
  const fs::path moved = root.path / "Portable"; // LOCALAPPDATA, redirected
  write(real / "boiii" / "user" / "game" / "boiii_players" / "properties.json",
        "{\"playerName\":\"Known\"}");
  write(moved / "boiii" / "user" / "game" / "boiii_players" / "properties.json",
        "{\"playerName\":\"Variable\"}");
  const std::vector<fs::path> both = boiii_appdata_dirs(real, moved);
  REQUIRE_EQ(both.size(), std::size_t(2));

  // A game folder whose boiii_players leads into either identity folder is
  // not read.
  for (const fs::path &local : {real, moved}) {
    const fs::path game = root.path / ("game-" + local.filename().string());
    REQUIRE(make_junction(game / "boiii_players",
                          local / "boiii" / "user" / "game" / "boiii_players"));
    const boiii_import found = find_boiii_import(both, game);
    CHECK(!found.player_name.has_value());
    CHECK(found.diags.has("first_run.import.identity"));
  }
  // Checking only the variable's folder would have read the known one.
  CHECK((check_boiii_identity_path(root.path / "game-Local" / "boiii_players",
                                   moved / "boiii") == outside));

  // Nothing known: a plain game folder is not read either.
  const fs::path plain = root.path / "plain";
  write(plain / "boiii_players" / "properties.json",
        "{\"playerName\":\"Plain\"}");
  const boiii_import none = find_boiii_import({}, plain);
  CHECK(!none.player_name.has_value());
  CHECK(none.diags.has("first_run.import.no_appdata"));
  REQUIRE_EQ(none.places.size(), std::size_t(1));
  CHECK(!none.places[0].exists);
  const boiii_import read = find_boiii_import(both, plain);
  CHECK_EQ(read.player_name.value_or(""), std::string("Plain"));
}
