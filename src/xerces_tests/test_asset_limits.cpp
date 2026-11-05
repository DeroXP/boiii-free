// Asset pool limits (xerces_core/asset_limits.hpp, docs/DESIGN.md 5.4 step
// 4): pool files read the way upstream's client reads them, the client's own
// pool sizes and their settings, and the largest limits over many mods.

#include "harness.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <xerces_core/asset_limits.hpp>
#include <xerces_core/catalog.hpp>
#include <xerces_core/game_files.hpp>
#include <xerces_core/planner.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
using limits = std::vector<pool_limit>;

// A header line long enough that upstream's first-characters damage stays in
// it (see parse_pool_limits).
const std::string header = "// pools\n";

fs::path fresh_dir(const std::string &name) {
  const fs::path dir = fs::temp_directory_path() / ("xerces_pools_" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void write_bytes(const fs::path &path, const std::string &bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}

// Upstream's asset_limits::parse_list with utils::string::replace and split
// as they are (src/client/component/asset_limits.cpp,
// src/common/utils/string.cpp), quadratic replace included: what the
// one-pass parser must agree with.
namespace upstream {
std::string replace(std::string str, const std::string &from,
                    const std::string &to) {
  if (from.empty()) {
    return str;
  }
  for (size_t start_pos = 0; start_pos != std::string::npos;
       start_pos = str.find(from, start_pos)) {
    str.replace(start_pos, from.length(), to);
    start_pos += to.length();
  }
  return str;
}

std::vector<std::string> split(const std::string &s, char delim) {
  std::vector<std::string> result;
  size_t start = 0;
  size_t end = s.find(delim);
  while (end != std::string::npos) {
    result.push_back(s.substr(start, end - start));
    start = end + 1;
    end = s.find(delim, start);
  }
  result.push_back(s.substr(start));
  return result;
}

template <typename T> std::optional<T> parse_int(const std::string_view &str) {
  const char *first = str.data();
  const char *last = str.data() + str.size();
  int base = 10;
  if (str.starts_with("0x")) {
    base = 16;
    first = str.data() + 2;
  }
  T result = {};
  auto [ptr, ec] = std::from_chars(first, last, result, base);
  if (ec == std::errc() && ptr == last) {
    return result;
  }
  return std::nullopt;
}

std::vector<pool_limit> parse_list(std::string data) {
  std::vector<pool_limit> result;
  data = replace(replace(replace(data, "\r\n", "\n"), "\r", ""), "\t", " ");
  for (const std::string &line : split(data, '\n')) {
    const std::vector<std::string> parts = split(line, ' ');
    if (parts.size() > 1) {
      const auto type = parse_int<int32_t>(parts[0]);
      if (type && *type >= 0 && *type < asset_type_count) {
        if (const auto size = parse_int<uint32_t>(parts.back())) {
          result.push_back({*type, *size});
        }
      }
    }
  }
  return result;
}
} // namespace upstream
} // namespace

namespace xerces_core {
// For failure messages (found by argument-dependent lookup).
std::ostream &operator<<(std::ostream &out,
                         const std::vector<pool_limit> &value) {
  out << "[";
  for (const auto &limit : value) {
    out << " " << limit.type << ":" << limit.size;
  }
  return out << " ]";
}
} // namespace xerces_core

TEST_CASE("pools: both line forms read type first and size last") {
  // T7Recharged: int_type_value string_type_name default_pool_size
  // extended_pool_size. Minimal: int_type_value extended_pool_size.
  CHECK_EQ(parse_pool_limits(header + "23 weapon 1536 4096\n"
                                      "0x26 fx 0x7D0 0x1F40\n"
                                      "4 11264\n"
                                      "0x6 0x8000\n"),
           (limits{{23, 4096}, {0x26, 0x1F40}, {4, 11264}, {6, 0x8000}}));
  // Whatever sits between the first and last part is not read.
  CHECK_EQ(parse_pool_limits(header + "23 any thing at all 4096\n"),
           (limits{{23, 4096}}));
  // The last line needs no line break; repeated types stay, in order.
  CHECK_EQ(parse_pool_limits(header + "23 100\n23 50"),
           (limits{{23, 100}, {23, 50}}));
}

TEST_CASE("pools: upstream's normalisation always eats the first line") {
  // utils::string::replace replaces at position 0 before it searches: the
  // first two characters go and the third becomes a space, so the first line
  // starts with an empty part. The game reads only the second line here.
  CHECK_EQ(parse_pool_limits("23 weapon 1536 4096\n"
                             "38 fx 2000 8000\n"),
           (limits{{38, 8000}}));
  CHECK_EQ(parse_pool_limits("23 4096"), limits{});
  // Three characters before the first line break keep the damage there.
  CHECK_EQ(parse_pool_limits("abc\n23 4096\n"), (limits{{23, 4096}}));
  CHECK_EQ(parse_pool_limits("abc\r\n23 4096\r\n"), (limits{{23, 4096}}));
  // Shorter first lines (a blank line too) take the next line with them.
  CHECK_EQ(parse_pool_limits("ab\n23 4096\n38 8000\n"), (limits{{38, 8000}}));
  CHECK_EQ(parse_pool_limits("\n23 4096\n38 8000\n"), (limits{{38, 8000}}));
  CHECK_EQ(parse_pool_limits("\r\n23 4096\r\n38 8000\r\n"),
           (limits{{38, 8000}}));
  CHECK_EQ(parse_pool_limits("\n\n23 4096\n38 8000\n"), (limits{{38, 8000}}));
  CHECK_EQ(parse_pool_limits(""), limits{});
  CHECK_EQ(parse_pool_limits("a"), limits{});
}

TEST_CASE("pools: CRLF, lone CR and tabs normalise as upstream's do") {
  CHECK_EQ(parse_pool_limits(header + "23 weapon 1536 4096\r\n"
                                      "38\tfx\t2000\t8000\r\n"),
           (limits{{23, 4096}, {38, 8000}}));
  // A lone CR is dropped, not a line break: "40\r96" reads as 4096.
  CHECK_EQ(parse_pool_limits(header + "23 40\r96\n"), (limits{{23, 4096}}));
  // ... so two lines joined by a lone CR are one line: type 23, size 8000.
  CHECK_EQ(parse_pool_limits(header + "23 4096\r38 8000\n"),
           (limits{{23, 8000}}));
  // A tab is one space, so a tab next to a space makes an empty part.
  CHECK_EQ(parse_pool_limits(header + "23\t 4096\n"), (limits{{23, 4096}}));
  CHECK_EQ(parse_pool_limits(header + "23 4096\t\n"), limits{});
}

TEST_CASE("pools: the one-pass parser reads what upstream's loop reads") {
  // Random texts compared with upstream's code as it is: a short start of
  // any of the characters the rules turn on, then lines of numbers and
  // near-numbers between spaces and tabs, ended by any kind of line break.
  const std::vector<std::string> starts = {"/", "\r", "\n", " ", "\t", "1"};
  const std::vector<std::string> fields = {
      "23", "0x17", "4096", "0x1F40", "0",  "1", "0X1",
      "-1", "a2",   "0x67", "x",      "0x", ""};
  const std::vector<std::string> gaps = {" ",  " ",   " ", "  ",
                                         "\t", " \t", "\r"};
  const std::vector<std::string> ends = {"\n", "\n", "\r\n", "\r", "\n\r", ""};
  std::mt19937 random(5);
  const auto any = [&](const std::vector<std::string> &from) {
    return from[std::uniform_int_distribution<std::size_t>(0, from.size() -
                                                                  1)(random)];
  };
  const auto count = [&](std::size_t most) {
    return std::uniform_int_distribution<std::size_t>(0, most)(random);
  };
  std::size_t found = 0;
  for (int round = 0; round < 50000; ++round) {
    std::string text;
    for (std::size_t n = count(4); n > 0; --n) {
      text += any(starts);
    }
    for (std::size_t line = count(4); line > 0; --line) {
      text += any(fields);
      for (std::size_t n = count(3); n > 0; --n) {
        text += any(gaps) + any(fields);
      }
      text += any(ends);
    }
    const limits expected = upstream::parse_list(text);
    found += expected.size();
    const limits got = parse_pool_limits(text);
    CHECK_EQ(got, expected);
    if (got != expected) {
      break; // one failure is enough to read
    }
  }
  CHECK(found > 1000); // the texts did make limits
}

TEST_CASE("pools: a large file of line breaks parses in linear time") {
  // Upstream's replace shifts the rest of the text at every match that
  // shrinks it; copied as it was, a file just under max_pool_file_bytes of
  // "\r" took over a minute.
  for (const std::string fill : {"\r", "\r\n", "\t"}) {
    std::string text;
    while (text.size() + fill.size() < max_pool_file_bytes) {
      text += fill;
    }
    text += "\n// pools\n23 4096\n";
    const auto started = std::chrono::steady_clock::now();
    CHECK_EQ(parse_pool_limits(text), (limits{{23, 4096}}));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
  }
}

TEST_CASE("pools: parts are split at every single space") {
  // Two spaces make an empty middle part, which is ignored.
  CHECK_EQ(parse_pool_limits(header + "23  4096\n"), (limits{{23, 4096}}));
  CHECK_EQ(parse_pool_limits(header + "23 weapon  1536   4096\n"),
           (limits{{23, 4096}}));
  // An empty first or last part does not read as a number.
  CHECK_EQ(parse_pool_limits(header + " 23 4096\n"), limits{});
  CHECK_EQ(parse_pool_limits(header + "23 4096 \n"), limits{});
  // Fewer than two parts: skipped.
  CHECK_EQ(parse_pool_limits(header + "23\n4096\n\n"), limits{});
}

TEST_CASE("pools: numbers are whole parts, hex only after a lower-case 0x") {
  CHECK_EQ(parse_pool_limits(header + "0x17 0x1000\n"), (limits{{23, 4096}}));
  CHECK_EQ(parse_pool_limits(header + "0x17 0xfFfF\n"), (limits{{23, 65535}}));
  CHECK_EQ(parse_pool_limits(header + "0X17 4096\n"), limits{});
  CHECK_EQ(parse_pool_limits(header + "23 0X1000\n"), limits{});
  CHECK_EQ(parse_pool_limits(header + "23 0x\n"), limits{});
  CHECK_EQ(parse_pool_limits(header + "23 4096k\n"), limits{});
  CHECK_EQ(parse_pool_limits(header + "23 +4096\n"), limits{});
  CHECK_EQ(parse_pool_limits(header + "23 -1\n"), limits{});
  CHECK_EQ(parse_pool_limits(header + "23 4294967295\n"),
           (limits{{23, 4294967295u}}));
  CHECK_EQ(parse_pool_limits(header + "23 4294967296\n"), limits{});
  CHECK_EQ(parse_pool_limits(header + "23 0\n"), (limits{{23, 0}}));
  // A leading zero is still decimal.
  CHECK_EQ(parse_pool_limits(header + "017 10\n"), (limits{{17, 10}}));
}

TEST_CASE("pools: the type must be a pool the client knows") {
  // valid_xassettype: 0 <= type < XAssetType::COUNT (0x67).
  CHECK_EQ(parse_pool_limits(header + "0 10\n0x66 20\n"),
           (limits{{0, 10}, {0x66, 20}}));
  CHECK_EQ(parse_pool_limits(header + "0x67 10\n103 10\n-1 10\n"), limits{});
  CHECK_EQ(parse_pool_limits(header + "-0 10\n"), (limits{{0, 10}}));
  CHECK_EQ(parse_pool_limits(header + "0x80000000 10\n"), limits{});
  CHECK_EQ(parse_pool_limits(header + "weapon 10\n"), limits{});
}

TEST_CASE("pools: the table is upstream's DEFAULT_POOL_CONFIGS") {
  const auto pools = asset_pools();
  REQUIRE_EQ(pools.size(), std::size_t{0x67});
  for (std::size_t i = 0; i < pools.size(); ++i) {
    CHECK_EQ(pools[i].type, static_cast<std::int32_t>(i));
    CHECK(pools[i].setting_key.starts_with("ap_"));
    CHECK(!pools[i].label.empty());
  }
  CHECK_EQ(std::string(pools[0].setting_key), std::string("ap_physpreset"));
  CHECK_EQ(pools[0].default_size, 0x113u);
  const asset_pool_info *weapon = find_asset_pool(0x17);
  REQUIRE(weapon != nullptr);
  CHECK_EQ(std::string(weapon->name), std::string("WEAPON"));
  CHECK_EQ(std::string(weapon->label), std::string("Weapon"));
  CHECK_EQ(std::string(weapon->setting_key), std::string("ap_weapon"));
  CHECK_EQ(weapon->default_size, 0x600u);
  CHECK(!weapon->fixed);
  CHECK_EQ(std::string(find_asset_pool(0x28)->setting_key),
           std::string("ap_klf"));
  CHECK_EQ(find_asset_pool(0x66)->default_size, 0x32u);
  // reallocate_asset_pool never grows these: is_fixed_pool's five, which the
  // engine sizes itself, and UI_MAP and WEAPON_FULL, which have no entry size
  // (DB_GetXAssetTypeSize_Impl gives -1).
  for (const std::int32_t type : {0x5, 0x9, 0xD, 0xE, 0x10, 0x13, 0x1A}) {
    CHECK(find_asset_pool(type)->fixed);
  }
  CHECK_EQ(std::count_if(pools.begin(), pools.end(),
                         [](const asset_pool_info &p) { return p.fixed; }),
           std::ptrdiff_t{7});
  CHECK(find_asset_pool(-1) == nullptr);
  CHECK(find_asset_pool(0x67) == nullptr);
}

TEST_CASE("pools: launcher_settings.json read as the client reads it") {
  // is_enabled: missing, empty or "1" is on, unless disable_asset_pools "1".
  CHECK(parse_pool_settings("").enabled);
  CHECK(parse_pool_settings("{}").enabled);
  CHECK(parse_pool_settings("not json").enabled);
  CHECK(parse_pool_settings("[1]").enabled);
  CHECK(parse_pool_settings(R"({"asset_limits_enabled":""})").enabled);
  CHECK(parse_pool_settings(R"({"asset_limits_enabled":1})").enabled);
  CHECK(!parse_pool_settings(R"({"asset_limits_enabled":"0"})").enabled);
  CHECK(!parse_pool_settings(R"({"asset_limits_enabled":"true"})").enabled);
  CHECK(!parse_pool_settings(R"({"disable_asset_pools":"1"})").enabled);
  CHECK(parse_pool_settings(R"({"disable_asset_pools":1})").enabled);
  CHECK(!parse_pool_settings(
             R"({"asset_limits_enabled":"1","disable_asset_pools":"1"})")
             .enabled);
  // The first of two members with one name counts, as FindMember finds it.
  CHECK(!parse_pool_settings(
             R"({"asset_limits_enabled":"0","asset_limits_enabled":"1"})")
             .enabled);
  // game_files' switch reads the same rule.
  CHECK(!asset_pools_enabled(R"({"disable_asset_pools":"1"})"));
  CHECK(asset_pools_enabled("not json"));
}

TEST_CASE("pools: get_pool_size rules for the ap_* keys") {
  const auto size = [](const std::string &value) {
    return pool_size_setting(
        parse_pool_settings(R"({"ap_weapon":")" + value + R"("})"), 0x17);
  };
  CHECK_EQ(pool_size_setting(parse_pool_settings("{}"), 0x17), 0x600u);
  CHECK_EQ(size("4096"), 4096u);
  // std::stoul: leading spaces, a sign and trailing text are taken.
  CHECK_EQ(size(" 4096"), 4096u);
  CHECK_EQ(size("+4096"), 4096u);
  CHECK_EQ(size("4096 entries"), 4096u);
  CHECK_EQ(size("12.5"), 12u);
  // Base 10 only: "0x1000" reads as 0.
  CHECK_EQ(size("0x1000"), 0u);
  CHECK_EQ(size("0"), 0u);
  // Above INT32_MAX, or not a number, is the default.
  CHECK_EQ(size("2147483647"), 2147483647u);
  CHECK_EQ(size("2147483648"), 0x600u);
  CHECK_EQ(size("-1"), 0x600u);
  CHECK_EQ(size("99999999999"), 0x600u);
  CHECK_EQ(size("lots"), 0x600u);
  CHECK_EQ(size(""), 0x600u);
  // Only strings count, and only the first member of a name, string or not.
  CHECK_EQ(
      pool_size_setting(parse_pool_settings(R"({"ap_weapon":4096})"), 0x17),
      0x600u);
  CHECK_EQ(
      pool_size_setting(
          parse_pool_settings(R"({"ap_weapon":0,"ap_weapon":"4096"})"), 0x17),
      0x600u);
  CHECK_EQ(pool_size_setting(parse_pool_settings("{}"), 0x67), 0u);
}

TEST_CASE("pools: the default a mod's size is compared with") {
  const pool_settings stock = parse_pool_settings("");
  CHECK_EQ(default_pool_size(stock, 0x17).value_or(0), 0x600u);
  // Fixed pools and pools whose size is 0 keep the game's own size.
  CHECK(!default_pool_size(stock, 0x9).has_value());
  CHECK(!default_pool_size(stock, 0x13).has_value()); // UI_MAP, 0x00
  CHECK(!default_pool_size(stock, 0x18).has_value()); // WEAPONDEF, 0x00
  // UI_MAP never grows, so a setting changes nothing either.
  CHECK(!default_pool_size(parse_pool_settings(R"({"ap_ui_map":"64"})"), 0x13)
             .has_value());
  CHECK(!default_pool_size(parse_pool_settings(R"({"ap_weapon":"0"})"), 0x17)
             .has_value());
  CHECK_EQ(
      default_pool_size(parse_pool_settings(R"({"ap_weapon":"9000"})"), 0x17)
          .value_or(0),
      9000u);
  // The client's pools off: every pool keeps the game's own size.
  CHECK(!default_pool_size(
             parse_pool_settings(R"({"asset_limits_enabled":"0"})"), 0x17)
             .has_value());
}

TEST_CASE("pools: largest limits over many mods") {
  const limits a = {{0x17, 4096}, {0x26, 1000}, {0x9, 0x10000}};
  const limits b = {{0x17, 8192}, {0x26, 4000}, {0x26, 3000}, {0x4, 100}};
  const limits c = {{0x17, 8192}, {0x6, 0x5800}};
  const std::vector<mod_pool_limits> mods = {
      {"a.mod", a}, {"b.mod", b}, {"c.mod", c}, {"none", {}}};
  const std::vector<pool_need> needs = largest_limits(mods);
  REQUIRE_EQ(needs.size(), std::size_t{5});
  // By type.
  CHECK_EQ(needs[0].type, 0x4);
  CHECK_EQ(needs[0].size, 100u);
  CHECK_EQ(needs[0].mod, std::string("b.mod"));
  CHECK_EQ(needs[0].default_size.value_or(0), 0x2C00u);
  CHECK(!needs[0].raises); // below the default
  CHECK_EQ(needs[1].type, 0x6);
  CHECK(!needs[1].raises); // equal to the default
  CHECK_EQ(needs[2].type, 0x9);
  CHECK(needs[2].fixed);
  CHECK(!needs[2].raises); // IMAGE never grows
  CHECK(!needs[2].default_size.has_value());
  CHECK_EQ(needs[3].type, 0x17);
  CHECK_EQ(needs[3].size, 8192u);
  CHECK_EQ(needs[3].mod, std::string("b.mod")); // tie: the first given
  CHECK(needs[3].raises);
  CHECK_EQ(needs[4].type, 0x26);
  CHECK_EQ(needs[4].size, 4000u);
  CHECK_EQ(needs[4].mod, std::string("b.mod"));
  CHECK(needs[4].raises);

  // With the client's pools off, every growable pool a mod asks for grows.
  const auto off = largest_limits(
      mods, parse_pool_settings(R"({"asset_limits_enabled":"0"})"));
  CHECK(off[0].raises);
  CHECK(!off[2].raises);
  // A setting above the mods' sizes means they raise nothing there.
  const auto big = largest_limits(
      mods, parse_pool_settings(R"({"ap_weapon":"10000","ap_fx":"4000"})"));
  CHECK(!big[3].raises);
  CHECK(!big[4].raises);
  CHECK(largest_limits({}).empty());
}

TEST_CASE("pools: sizes and pools the client never grows raise nothing") {
  // A T7Recharged-style list with sizes of 0, and pools with no entry size.
  // reallocate_asset_pool grows nothing to 0 (itemCount < 0 is never true)
  // and returns at once for UI_MAP and WEAPON_FULL.
  const limits t7 = parse_pool_limits("// T7Recharged pools\r\n"
                                      "19 uimap 0 64\r\n"
                                      "26 weaponfull 0 64\r\n"
                                      "24 weapondef 0 0\r\n"
                                      "68 typeinfo 0 0\r\n"
                                      "23 weapon 1536 0\r\n");
  REQUIRE_EQ(t7.size(), std::size_t{5});
  const std::vector<mod_pool_limits> mods = {{"t7.mod", t7}};
  for (const auto &settings :
       {parse_pool_settings(""),
        parse_pool_settings(R"({"asset_limits_enabled":"0"})")}) {
    const std::vector<pool_need> needs = largest_limits(mods, settings);
    REQUIRE_EQ(needs.size(), std::size_t{5});
    for (const pool_need &need : needs) {
      CHECK(!need.raises);
    }
    CHECK(needs[0].fixed); // UI_MAP
    CHECK(!needs[1].fixed);
    CHECK(!needs[2].fixed);
    CHECK(needs[3].fixed); // WEAPON_FULL
  }
  // 1 is the smallest size that grows a pool whose default is unknown.
  const limits one = {{0x18, 1}};
  CHECK(
      largest_limits(std::vector<mod_pool_limits>{{"one.mod", one}})[0].raises);
}

TEST_CASE("pools: sizes above INT32_MAX are no one's largest") {
  // reallocate_asset_pool grows only while itemCount <
  // static_cast<int32_t>(new_size), which is negative above INT32_MAX: the
  // client ignores those lines, and the next mod's size is what it applies.
  const limits huge = parse_pool_limits(header + "4 4294967295\n"
                                                 "23 0x80000000\n");
  REQUIRE_EQ(huge, (limits{{4, 4294967295u}, {23, 0x80000000u}}));
  const limits fine = {{4, 20000}, {23, 0x7FFFFFFF}};
  const std::vector<mod_pool_limits> mods = {{"ws.111", huge},
                                             {"ws.222", fine}};
  const std::vector<pool_need> needs = largest_limits(mods);
  REQUIRE_EQ(needs.size(), std::size_t{2});
  CHECK_EQ(needs[0].type, 4);
  CHECK_EQ(needs[0].size, 20000u);
  CHECK_EQ(needs[0].mod, std::string("ws.222"));
  CHECK(needs[0].raises);
  CHECK_EQ(needs[1].size, max_pool_size);
  CHECK_EQ(needs[1].mod, std::string("ws.222"));
  CHECK(needs[1].raises);
  // Alone, they ask for nothing the client applies.
  CHECK(largest_limits(std::vector<mod_pool_limits>{{"ws.111", huge}}).empty());
}

TEST_CASE("pools: the file is found where the client looks") {
  const fs::path dir = fresh_dir("find");
  // A game mods\ folder: <name>\zone when it exists.
  write_bytes(dir / "local" / "assetlimits.txt", header + "23 100\n");
  write_bytes(dir / "local" / "zone" / "assetpools.txt", header + "23 200\n");
  CHECK_EQ(pool_search_root(dir / "local", content_layout::game_folder),
           dir / "local" / "zone");
  CHECK_EQ(find_pool_file(
               pool_search_root(dir / "local", content_layout::game_folder))
               .value_or(fs::path()),
           dir / "local" / "zone" / "assetpools.txt");
  // A Workshop item: the item folder itself, walked in directory order.
  CHECK_EQ(pool_search_root(dir / "local", content_layout::workshop),
           dir / "local");
  CHECK_EQ(find_pool_file(dir / "local").value_or(fs::path()),
           dir / "local" / "assetlimits.txt");
  // No zone folder: the folder.
  write_bytes(dir / "flat" / "core_mod.ff", "x");
  CHECK_EQ(pool_search_root(dir / "flat", content_layout::game_folder),
           dir / "flat");
  CHECK(!find_pool_file(dir / "flat").has_value());
  CHECK(!find_pool_file(dir / "missing").has_value());
  // Names match exactly: case matters, as in the client's path compare.
  write_bytes(dir / "case" / "AssetLimits.txt", header + "23 100\n");
  CHECK(!find_pool_file(dir / "case").has_value());
  // A folder with the name is not the file.
  fs::create_directories(dir / "folder" / "assetlimits.txt");
  CHECK(!find_pool_file(dir / "folder").has_value());
  // NTFS lists names in order and the walk enters a folder where it is
  // listed, so a\assetpools.txt comes before assetlimits.txt.
  write_bytes(dir / "order" / "a" / "assetpools.txt", header + "23 1\n");
  write_bytes(dir / "order" / "assetlimits.txt", header + "23 2\n");
  write_bytes(dir / "order" / "b" / "assetlimits.txt", header + "23 3\n");
  CHECK_EQ(find_pool_file(dir / "order").value_or(fs::path()),
           dir / "order" / "a" / "assetpools.txt");
  fs::remove_all(dir);
}

TEST_CASE("pools: the catalog reads each mod's pool file, maps' never") {
  const fs::path dir = fresh_dir("catalog");
  const fs::path mods = dir / "mods";
  const std::string file = "// T7Recharged pools\r\n"
                           "23 weapon 1536 4096\r\n"
                           "38 fx 2000 8000\r\n";
  write_bytes(mods / "big" / "zone" / "core_mod.ff", "ff");
  write_bytes(mods / "big" / "zone" / "workshop.json",
              R"({"Title":"Big","PublisherID":"111","Type":"mod"})");
  write_bytes(mods / "big" / "zone" / "assetlimits.txt", file);
  // Outside zone\: the client never sees it for a game mods\ folder.
  write_bytes(mods / "big" / "assetpools.txt", header + "23 99999\n");
  write_bytes(mods / "plain" / "zone" / "core_mod.ff", "ff");
  write_bytes(mods / "plain" / "zone" / "workshop.json",
              R"({"Title":"Plain","PublisherID":"222","Type":"mod"})");
  write_bytes(mods / "map" / "zone" / "workshop.json",
              R"({"Title":"Map","PublisherID":"333","Type":"map"})");
  write_bytes(mods / "map" / "zone" / "assetlimits.txt", file);

  const catalog_scan scan = scan_mods_directory(mods);
  REQUIRE_EQ(scan.catalog.size(), std::size_t{3});
  const auto find = [&](const std::string &id) -> const installed_content & {
    for (const auto &item : scan.catalog) {
      if (item.info.id == id) {
        return item;
      }
    }
    throw std::runtime_error("missing " + id);
  };
  const installed_content &big = find("ws.111");
  CHECK_EQ(big.pool_file, std::string("zone/assetlimits.txt"));
  CHECK_EQ(big.pool_limits, (limits{{23, 4096}, {38, 8000}}));
  // The pool file is hashed like any other file, and only once.
  CHECK_EQ(big.content_hash, content_hash_of(read_files(big.path)));
  CHECK(find("ws.222").pool_file.empty());
  CHECK(find("ws.222").pool_limits.empty());
  // The client reads pool files only when it loads a mod.
  CHECK(find("ws.333").pool_file.empty());
  CHECK(find("ws.333").pool_limits.empty());

  // A Workshop item: the client walks the item folder itself, and the
  // root's assetpools.txt is listed before the zone folder.
  scan_options workshop;
  workshop.layout = content_layout::workshop;
  const catalog_scan items = scan_mods_directory(mods, workshop);
  for (const auto &item : items.catalog) {
    if (item.info.id == "ws.111") {
      CHECK_EQ(item.pool_file, std::string("assetpools.txt"));
      CHECK_EQ(item.pool_limits, (limits{{23, 99999}}));
    }
  }
  CHECK(layout_of_directory(fs::path("D:/Steam/steamapps/workshop/content/"
                                     "311210")) == content_layout::workshop);
  CHECK(layout_of_directory(fs::path("D:/Steam/steamapps/workshop/content/"
                                     "311210/")) == content_layout::workshop);
  CHECK(layout_of_directory(fs::path("D:/Games/BO3/mods")) ==
        content_layout::game_folder);

  // An oversized file is left unread, with a warning.
  write_bytes(mods / "plain" / "zone" / "assetpools.txt",
              header + std::string(max_pool_file_bytes, ' ') + "\n23 1\n");
  const catalog_scan large = scan_mods_directory(mods);
  bool warned = false;
  for (const auto &item : large.diags.items()) {
    warned = warned || item.code == "catalog.pools.too_big";
  }
  CHECK(warned);
  for (const auto &item : large.catalog) {
    if (item.info.id == "ws.222") {
      CHECK_EQ(item.pool_file, std::string("zone/assetpools.txt"));
      CHECK(item.pool_limits.empty());
    }
  }
  fs::remove_all(dir);
}

namespace {
installed_content pool_mod(const std::string &id, const std::string &ver,
                           limits pools = {}) {
  installed_content content;
  content.info = parse_manifest(R"({"schema":1,"id":")" + id + R"(","name":")" +
                                id + R"(","version":")" + ver + R"("})")
                     .value;
  content.folder = id;
  content.files = {"zone/core_mod.ff"};
  content.content_hash = "sha256:" + std::string(64, 'a');
  content.pool_limits = std::move(pools);
  return content;
}

plan_request pool_request(std::vector<std::string> mods) {
  plan_request request;
  request.mode = "zm";
  request.mods = std::move(mods);
  request.game_build = "24784313";
  request.xerces_version = *version::parse("0.1.0");
  return request;
}
} // namespace

TEST_CASE("pools: the plan reports its stack's pools and every mod's") {
  installed_content map = pool_mod("some.map", "1.0.0", {{23, 1 << 20}});
  map.info.kind = content_kind::map;
  const std::vector<installed_content> catalog = {
      pool_mod("a.mod", "1.0.0", {{0x17, 4096}, {0x26, 1000}}),
      pool_mod("b.mod", "2.1.0", {{0x17, 8192}, {0x4, 100}}),
      pool_mod("c.mod", "1.0.0", {{0x17, 16384}, {0x6, 0x8000}}),
      pool_mod("d.mod", "1.0.0", {{0x17, 16384}}), map};
  plan_request request = pool_request({"b.mod", "a.mod"});
  request.map = "some.map";
  const plan p = resolve_plan(catalog, request);
  REQUIRE(p.ok());
  CHECK(p.pools.enabled);
  // The stack: a.mod and b.mod only; the map's file is never read.
  REQUIRE_EQ(p.pools.stack.size(), std::size_t{3});
  CHECK_EQ(p.pools.stack[0].type, 0x4);
  CHECK(!p.pools.stack[0].raises);
  CHECK_EQ(p.pools.stack[1].type, 0x17);
  CHECK_EQ(p.pools.stack[1].size, 8192u);
  CHECK_EQ(p.pools.stack[1].mod, std::string("b.mod"));
  CHECK_EQ(p.pools.stack[1].default_size.value_or(0), 0x600u);
  CHECK(p.pools.stack[1].raises);
  CHECK_EQ(p.pools.stack[2].type, 0x26);
  CHECK(!p.pools.stack[2].raises); // 1000 is below FX's 0x7D0
  // Every installed mod, the map left out; a tie names the lowest id.
  REQUIRE_EQ(p.pools.installed.size(), std::size_t{4});
  CHECK_EQ(p.pools.installed[2].type, 0x17);
  CHECK_EQ(p.pools.installed[2].size, 16384u);
  CHECK_EQ(p.pools.installed[2].mod, std::string("c.mod"));
  CHECK_EQ(p.pools.installed[1].type, 0x6);
  CHECK(p.pools.installed[1].raises);

  const std::string text = plan_to_json(p);
  CHECK(text.find(R"("asset_pools":{"enabled":true,"installed":[)") !=
        std::string::npos);
  CHECK(text.find(R"({"default":1536,"fixed":false,"label":"Weapon",)"
                  R"("mod":"b.mod","name":"WEAPON","raises":true,)"
                  R"("setting_key":"ap_weapon","size":8192,"type":23})") !=
        std::string::npos);

  // launcher_settings.json changes the defaults, never the ids.
  plan_request off = request;
  off.launcher_settings = R"({"asset_limits_enabled":"0"})";
  const plan q = resolve_plan(catalog, off);
  CHECK(!q.pools.enabled);
  CHECK(!q.pools.stack[0].default_size.has_value());
  CHECK(q.pools.stack[0].raises);
  CHECK(plan_to_json(q).find(R"("default":null)") != std::string::npos);
  CHECK_EQ(q.plan_id, p.plan_id);
  CHECK_EQ(q.content_id, p.content_id);
  CHECK_EQ(q.canonical, p.canonical);
}

TEST_CASE("pools: pool limits leave plan_id and content_id alone") {
  // The literal ids come from the planner before pool limits existed: the
  // same two mods, now with pool files, plus an installed mod outside the
  // plan and a launcher_settings.json, must give the same ones.
  const char *plan_id = "sha256:540c19dd5688eaf8e68a8492ddaea0bb"
                        "c864682c1f0cfd1d7c9a13baea519dd3";
  const char *content_id = "sha256:d32ba51e7348069db500fc921052a660"
                           "261dc8df31dfd1af3558626210c057e5";
  const std::vector<installed_content> catalog = {
      pool_mod("a.mod", "1.0.0", {{0x17, 4096}}),
      pool_mod("b.mod", "2.1.0", {{0x26, 9000}}),
      pool_mod("other.mod", "1.0.0", {{0x4, 99999}})};
  plan_request request = pool_request({"b.mod", "a.mod"});
  request.launcher_settings = R"({"ap_weapon":"8000"})";
  const plan p = resolve_plan(catalog, request);
  CHECK_EQ(p.plan_id, std::string(plan_id));
  CHECK_EQ(p.content_id, std::string(content_id));
  CHECK_EQ(p.pools.installed.size(), std::size_t{3});
  // And without any pool data: the same.
  const plan bare =
      resolve_plan({pool_mod("a.mod", "1.0.0"), pool_mod("b.mod", "2.1.0")},
                   pool_request({"b.mod", "a.mod"}));
  CHECK_EQ(bare.plan_id, std::string(plan_id));
  CHECK_EQ(bare.content_id, std::string(content_id));
}
