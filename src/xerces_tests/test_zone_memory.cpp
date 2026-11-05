// Zone memory (docs/DESIGN.md 5.3 step 5): fastfile headers, the figures in
// compat/zone_memory.json, what a mod's or map's zones take from the DB
// stack's main pool, and the planner's check of every installed map against
// a plan's mods. Every .ff here is a synthetic header written to a temp
// folder; no game file is read.

#include "harness.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <xerces_core/catalog.hpp>
#include <xerces_core/fastfile_header.hpp>
#include <xerces_core/launcher_home.hpp>
#include <xerces_core/launcher_launch.hpp>
#include <xerces_core/launcher_state.hpp>
#include <xerces_core/mods_edit.hpp>
#include <xerces_core/planner.hpp>
#include <xerces_core/zone_memory.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
constexpr std::uint64_t MiB = 1024 * 1024;

void put32(std::string &out, std::size_t at, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    out[at + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
  }
}

void put64(std::string &out, std::size_t at, std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) {
    out[at + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
  }
}

// A fastfile header as the game writes one, cut to `size` bytes. The other
// blocks hold something too, so a test sees that only 5 and 6 count.
std::string ff(std::uint64_t block5, std::uint64_t block6,
               std::string_view magic = fastfile_magic,
               std::uint32_t version = fastfile_version,
               std::size_t size = fastfile_header_bytes) {
  std::string out(fastfile_header_bytes + 64, '\0');
  std::memcpy(out.data(), magic.data(), magic.size());
  put32(out, 8, version);
  put64(out, fastfile_blocks_offset, 0x2000);
  put64(out, fastfile_blocks_offset + 8 * 1, 3 * MiB);
  put64(out, fastfile_blocks_offset + 8 * 5, block5);
  put64(out, fastfile_blocks_offset + 8 * 6, block6);
  put64(out, fastfile_blocks_offset + 8 * 8, 4000 * MiB);
  out.resize(size);
  return out;
}

fs::path fresh(const std::string &name) {
  const fs::path dir = fs::temp_directory_path() / ("xerces_zonemem_" + name);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

void write(const fs::path &file, const std::string &bytes) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << bytes;
}

using file_list = std::vector<std::pair<std::string, std::string>>;

// A Workshop item: <root>\<publisher>\workshop.json and its files.
void workshop_item(const fs::path &root, const std::string &publisher,
                   const std::string &type, const std::string &title,
                   const file_list &files) {
  write(root / publisher / "workshop.json",
        R"({"PublisherID":")" + publisher + R"(","Type":")" + type +
            R"(","Title":")" + title + R"("})");
  for (const auto &[name, bytes] : files) {
    write(root / publisher / name, bytes);
  }
}

// Two mods and six maps, sizes in whole MiB of blocks 5 + 6:
// - ws.100 "Big Mod": core_mod 300, zm_mod 150, mp_mod 999 (not loaded in
//   Zombies), language copies 1 and 2 of core_mod (the larger counts): 452;
// - ws.101 "Small Mod": core_mod 1;
// - maps ws.200 "Map A" 800 + a language copy of 1, ws.201 "Map B" 1160,
//   ws.202 "Map C" 600, ws.203 "Map D" (Multiplayer) 1500, ws.204 "Map E"
//   1250 (too big even with no mods), ws.205 "Map F" 801 (ties Map A).
fs::path write_catalog(const std::string &name, bool with_map_e = true) {
  const fs::path root = fresh(name);
  workshop_item(root, "100", "mod", "Big Mod",
                {{"core_mod.ff", ff(100 * MiB, 200 * MiB)},
                 {"core_mod.xpak", "not a fastfile"},
                 {"zm_mod.ff", ff(50 * MiB, 100 * MiB)},
                 {"mp_mod.ff", ff(999 * MiB, 0)},
                 {"en_core_mod.ff", ff(1 * MiB, 0)},
                 {"fr_core_mod.ff", ff(2 * MiB, 0)},
                 {"en_zm_mod.ff", ff(0, 0)}});
  workshop_item(root, "101", "mod", "Small Mod",
                {{"core_mod.ff", ff(1 * MiB, 0)}});
  workshop_item(
      root, "200", "map", "Map A",
      {{"zm_a.ff", ff(400 * MiB, 400 * MiB)}, {"en_zm_a.ff", ff(1 * MiB, 0)}});
  workshop_item(root, "201", "map", "Map B",
                {{"zm_b.ff", ff(360 * MiB, 800 * MiB)}});
  workshop_item(root, "202", "map", "Map C", {{"zm_c.ff", ff(0, 600 * MiB)}});
  workshop_item(root, "203", "map", "Map D",
                {{"mp_d.ff", ff(750 * MiB, 750 * MiB)}});
  if (with_map_e) {
    workshop_item(root, "204", "map", "Map E",
                  {{"zm_e.ff", ff(250 * MiB, 1000 * MiB)}});
  }
  workshop_item(root, "205", "map", "Map F",
                {{"zm_f.ff", ff(1 * MiB, 800 * MiB)}});
  return root;
}

plan_request zombies(std::vector<std::string> mods,
                     std::optional<std::string> map = std::nullopt,
                     bool budget = true) {
  plan_request request;
  request.mode = "zm";
  request.mods = std::move(mods);
  request.map = std::move(map);
  request.game_build = "24784313";
  if (budget) {
    request.zone_budget = zone_budget{2048, 849};
  }
  return request;
}

std::vector<std::string> over_ids(const plan &p) {
  std::vector<std::string> out;
  for (const auto &map : p.zone_memory.maps_over) {
    out.push_back(map.id);
  }
  return out;
}

const diagnostic *find_diag(const plan &p, const std::string &code) {
  for (const auto &item : p.diags.items()) {
    if (item.code == code) {
      return &item;
    }
  }
  return nullptr;
}

bool contains(const std::string &text, const std::string &part) {
  return text.find(part) != std::string::npos;
}
} // namespace

// ----------------------------------------------------------- the header
TEST_CASE("zone memory: a fastfile header gives its version and blocks") {
  const auto header = parse_fastfile_header(ff(180 * MiB, 632 * MiB));
  REQUIRE(header.has_value());
  CHECK_EQ(header->version, fastfile_version);
  CHECK_EQ(header->blocks[0], std::uint64_t(0x2000));
  CHECK_EQ(header->blocks[1], 3 * MiB);
  CHECK_EQ(header->blocks[5], 180 * MiB);
  CHECK_EQ(header->blocks[6], 632 * MiB);
  CHECK_EQ(header->blocks[8], 4000 * MiB);
  CHECK_EQ(header->blocks[9], std::uint64_t(0));
  CHECK_EQ(header->db_main_pool_bytes(), 812 * MiB);

  // More than the header is fine; it is the start of a zone.
  CHECK(parse_fastfile_header(ff(1, 2, fastfile_magic, fastfile_version,
                                 fastfile_header_bytes + 64))
            .has_value());
  // Sizes that would wrap stop at the largest value instead.
  const auto huge = parse_fastfile_header(ff(~std::uint64_t(0) - 5, 100));
  REQUIRE(huge.has_value());
  CHECK_EQ(huge->db_main_pool_bytes(), ~std::uint64_t(0));
}

TEST_CASE("zone memory: another magic or version, or a short file, is none") {
  CHECK(!parse_fastfile_header(ff(1, 2, "TAff0001")).has_value());
  CHECK(!parse_fastfile_header(ff(1, 2, "IWff0100")).has_value());
  CHECK(!parse_fastfile_header(ff(1, 2, fastfile_magic, 0x250)).has_value());
  CHECK(!parse_fastfile_header(ff(1, 2, fastfile_magic, 0x252)).has_value());
  CHECK(!parse_fastfile_header(ff(1, 2, fastfile_magic, fastfile_version,
                                  fastfile_header_bytes - 1))
             .has_value());
  CHECK(!parse_fastfile_header("").has_value());
  CHECK(!parse_fastfile_header("TAff0000").has_value());

  const fs::path dir = fresh("headers");
  write(dir / "good.ff", ff(5 * MiB, 6 * MiB, fastfile_magic, fastfile_version,
                            fastfile_header_bytes + 1000));
  write(dir / "short.ff", ff(5 * MiB, 6 * MiB).substr(0, 0x80));
  write(dir / "empty.ff", "");
  write(dir / "other.ff", ff(5 * MiB, 6 * MiB, "XXff0000"));
  const auto good = read_fastfile_header(dir / "good.ff");
  REQUIRE(good.has_value());
  CHECK_EQ(good->db_main_pool_bytes(), 11 * MiB);
  CHECK(!read_fastfile_header(dir / "short.ff").has_value());
  CHECK(!read_fastfile_header(dir / "empty.ff").has_value());
  CHECK(!read_fastfile_header(dir / "other.ff").has_value());
  CHECK(!read_fastfile_header(dir / "missing.ff").has_value());
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ------------------------------------------------------------ the figures
TEST_CASE("zone memory: compat/zone_memory.json parses and is looked up") {
  const zone_memory_table table = parse_zone_memory(R"({
    "schema": "xerces.zone_memory/1",
    "_comment": "comments are fine",
    "builds": [
      {"name": "Steam", "steam_build": 24784313, "checksum": "0x6531394",
       "db_main_pool_mib": 2048,
       "baseline_mib": {"zm": 849, "mp": null}},
      {"name": "By checksum only", "checksum": "0xABC",
       "db_main_pool_mib": 1024.5, "baseline_mib": {"cp": 100.25},
       "_why": "ignored"}
    ]})");
  REQUIRE(table.ok());
  CHECK(table.diags.items().empty());
  REQUIRE_EQ(table.builds.size(), std::size_t(2));
  CHECK_EQ(table.builds[0].steam_build, std::optional<std::uint64_t>(24784313));
  CHECK_EQ(table.builds[0].checksum, std::optional<std::uint32_t>(0x6531394));
  CHECK(!table.builds[0].baseline_mib.contains("mp")); // null: not measured
  CHECK(!table.builds[1].steam_build.has_value());

  // Steam's build number, as the launcher writes game_build, or a checksum,
  // as `plan resolve --game-build` takes it.
  const auto zm = find_zone_budget(table, "24784313", "zm");
  REQUIRE(zm.has_value());
  CHECK_EQ(zm->pool_mib, 2048.0);
  CHECK_EQ(zm->baseline_mib, 849.0);
  const auto by_checksum = find_zone_budget(table, "0x6531394", "zm");
  REQUIRE(by_checksum.has_value());
  CHECK_EQ(by_checksum->baseline_mib, 849.0);
  const auto cp = find_zone_budget(table, "0xabc", "cp");
  REQUIRE(cp.has_value());
  CHECK_EQ(cp->pool_mib, 1024.5);
  CHECK_EQ(cp->baseline_mib, 100.25);

  // No figures: an unmeasured mode, another build, no build at all.
  CHECK(!find_zone_budget(table, "24784313", "mp").has_value());
  CHECK(!find_zone_budget(table, "24784313", "cp").has_value());
  CHECK(!find_zone_budget(table, "24784314", "zm").has_value());
  CHECK(!find_zone_budget(table, "0x6531395", "zm").has_value());
  CHECK(!find_zone_budget(table, "unknown", "zm").has_value());
  CHECK(!find_zone_budget(table, "", "zm").has_value());
  CHECK(!find_zone_budget(zone_memory_table{}, "24784313", "zm").has_value());

  // The Steam build wins over a checksum that names another entry.
  CHECK_EQ(find_zone_memory_build(table, 24784313, 0xABC), &table.builds[0]);
  CHECK_EQ(find_zone_memory_build(table, 1, 0xABC), &table.builds[1]);
  CHECK(find_zone_memory_build(table, std::nullopt, std::nullopt) == nullptr);
}

TEST_CASE("zone memory: mistakes in zone_memory.json are named") {
  const auto codes = [](std::string_view text) {
    std::vector<std::string> out;
    const zone_memory_table table = parse_zone_memory(text);
    for (const auto &item : table.diags.items()) {
      out.push_back(item.code);
    }
    return out;
  };
  const auto has = [](const std::vector<std::string> &list,
                      const std::string &code) {
    return std::ranges::find(list, code) != list.end();
  };
  CHECK(has(codes("not json"), "zone_memory.json"));
  CHECK(has(codes(R"({"schema":"xerces.builds/1","builds":[]})"),
            "zone_memory.schema"));
  CHECK(has(codes(R"({"schema":"xerces.zone_memory/1"})"), "zone_memory.list"));
  const std::string head = R"({"schema":"xerces.zone_memory/1","builds":[)";
  CHECK(has(codes(head + R"({"name":"x","db_main_pool_mib":2048}]})"),
            "zone_memory.build"));
  CHECK(has(codes(head + R"({"name":"x","steam_build":1}]})"),
            "zone_memory.pool"));
  CHECK(has(
      codes(head + R"({"name":"x","steam_build":1,"db_main_pool_mib":0}]})"),
      "zone_memory.pool"));
  CHECK(has(codes(head + R"({"name":"x","checksum":"6531394",)"
                         R"("db_main_pool_mib":1}]})"),
            "zone_memory.checksum"));
  CHECK(has(codes(head + R"({"steam_build":1,"db_main_pool_mib":1}]})"),
            "zone_memory.name"));
  CHECK(has(codes(head + R"({"name":"x","steam_build":1,"db_main_pool_mib":1,)"
                         R"("baseline_mib":{"zz":1}}]})"),
            "zone_memory.mode"));
  CHECK(has(codes(head + R"({"name":"x","steam_build":1,"db_main_pool_mib":1,)"
                         R"("baseline_mib":{"zm":"849"}}]})"),
            "zone_memory.baseline"));
  const auto unknown =
      parse_zone_memory(head + R"({"name":"x","steam_build":1,)"
                               R"("db_main_pool_mib":1,"pool":2}]})");
  CHECK(unknown.ok()); // a warning
  REQUIRE_EQ(unknown.diags.items().size(), std::size_t(1));
  CHECK_EQ(unknown.diags.items()[0].code,
           std::string("zone_memory.key.unknown"));
}

TEST_CASE("zone memory: the repository's figures match a listed build") {
  const fs::path compat =
      xerces_tests::fixtures_dir().parent_path().parent_path() / "compat";
  const zone_memory_table table =
      parse_zone_memory(xerces_tests::read_file(compat / "zone_memory.json"));
  REQUIRE(table.ok());
  CHECK(table.diags.items().empty());
  const build_registry builds =
      parse_build_registry(xerces_tests::read_file(compat / "builds.json"));
  REQUIRE(builds.ok());
  // Every build with figures is one builds.json lists.
  for (const auto &entry : table.builds) {
    CHECK(std::ranges::any_of(builds.builds, [&](const known_build &known) {
      return (!entry.checksum || *entry.checksum == known.checksum) &&
             (!entry.steam_build || entry.steam_build == known.steam_build);
    }));
  }
  // Zombies is measured on the September 2026 build; the others are not.
  const auto zm = find_zone_budget(table, "24784313", "zm");
  REQUIRE(zm.has_value());
  CHECK_EQ(zm->pool_mib, 2048.0);
  CHECK_EQ(zm->baseline_mib, 849.0);
  CHECK(!find_zone_budget(table, "24784313", "mp").has_value());
  CHECK(!find_zone_budget(table, "24784313", "cp").has_value());
}

// -------------------------------------------------- a mod's or map's zones
TEST_CASE("zone memory: zone files are .ff at the root or in zone/") {
  CHECK(is_zone_file("zm_x.ff"));
  CHECK(is_zone_file("zone/zm_x.ff"));
  CHECK(is_zone_file("Zone/ZM_X.FF"));
  CHECK(!is_zone_file("sub/zm_x.ff"));
  CHECK(!is_zone_file("zone/sub/zm_x.ff"));
  CHECK(!is_zone_file("zm_x.xpak"));
  CHECK(!is_zone_file(".ff"));
}

TEST_CASE("zone memory: a mod's tiers in the mode, one language copy each") {
  const zone_header_map mod = {
      {"core_mod.ff", 300 * MiB},     {"zm_mod.ff", 150 * MiB},
      {"zm_mod_patch.ff", 7 * MiB},   {"mp_mod.ff", 999 * MiB},
      {"mp_mod_patch.ff", 5 * MiB},   {"en_core_mod.ff", 1 * MiB},
      {"fr_core_mod.ff", 2 * MiB},    {"ja_core_mod.ff", std::nullopt},
      {"en_zm_mod.ff", std::nullopt}, {"extra_zone.ff", 50 * MiB}};
  // Zombies: core_mod + the larger language copy, zm_mod and its patch; the
  // only copy of zm_mod is unread. mp_mod and other zones do not load.
  const zone_memory_use zm = zone_memory_of(mod, "zm", true);
  CHECK_EQ(zm.bytes, (300 + 2 + 150 + 7) * MiB);
  REQUIRE_EQ(zm.unread.size(), std::size_t(1));
  CHECK_EQ(zm.unread[0], std::string("en_zm_mod.ff"));
  const zone_memory_use mp = zone_memory_of(mod, "mp", true);
  CHECK_EQ(mp.bytes, (300 + 2 + 999 + 5) * MiB);
  CHECK(mp.unread.empty());
  // Campaign: no cp_mod here, so only core_mod.
  CHECK_EQ(zone_memory_of(mod, "cp", true).bytes, (300 + 2) * MiB);

  // Mod Tools layout, and a language copy with no zone of its own.
  const zone_header_map tools = {{"zone/core_mod.ff", std::nullopt},
                                 {"zone/en_zm_mod.ff", 4 * MiB}};
  const zone_memory_use t = zone_memory_of(tools, "zm", true);
  CHECK_EQ(t.bytes, 4 * MiB);
  REQUIRE_EQ(t.unread.size(), std::size_t(1));
  CHECK_EQ(t.unread[0], std::string("zone/core_mod.ff"));
}

TEST_CASE("zone memory: a map's zones, and the mode its names say") {
  const zone_header_map map = {{"zone/zm_x.ff", 800 * MiB},
                               {"zone/en_zm_x.ff", 1 * MiB},
                               {"zone/ja_zm_x.ff", 3 * MiB},
                               {"zone/zm_x_patch.ff", 4 * MiB},
                               // no "extra" zone: "ui" is part of the name
                               {"zone/ui_extra.ff", 10 * MiB}};
  const zone_memory_use use = zone_memory_of(map, "zm", false);
  CHECK_EQ(use.bytes, (800 + 3 + 4 + 10) * MiB);
  CHECK(use.unread.empty());
  CHECK(map_zone_modes(map) == std::set<std::string>{"zm"});
  CHECK(map_zone_modes({{"mp_y.ff", 1}, {"en_mp_y.ff", 1}}) ==
        std::set<std::string>{"mp"});
  CHECK(map_zone_modes({{"cp_z.ff", 1}}) == std::set<std::string>{"cp"});
  CHECK(map_zone_modes({{"custom.ff", 1}}).empty());
  CHECK(map_zone_modes({{"zm_mod.ff", 1}, {"core_mod.ff", 1}}).empty());
}

TEST_CASE("zone memory: the catalog reads every zone header, nothing else") {
  const fs::path root = fresh("catalog");
  workshop_item(root, "300", "map", "Layout",
                {{"zm_l.ff", ff(10 * MiB, 20 * MiB)},
                 {"zm_l.xpak", "data"},
                 {"en_zm_l.ff", ff(1, 2)},
                 {"short.ff", "TAff0000"},
                 {"other.ff", ff(1, 2, "XXff0000")},
                 {"deep/zm_deep.ff", ff(1, 2)}});
  write(root / "tools" / "zone" / "core_mod.ff", ff(0, 7 * MiB));
  write(root / "tools" / "zone" / "en_core_mod.ff", ff(1 * MiB, 0));
  const catalog_scan scan = scan_mods_directory(root);
  REQUIRE_EQ(scan.catalog.size(), std::size_t(2));
  const installed_content &map = scan.catalog[0];
  CHECK_EQ(map.info.id, std::string("ws.300"));
  REQUIRE_EQ(map.zone_headers.size(), std::size_t(4));
  CHECK_EQ(map.zone_headers.at("zm_l.ff"), std::optional(30 * MiB));
  CHECK_EQ(map.zone_headers.at("en_zm_l.ff"), std::optional(std::uint64_t(3)));
  CHECK(!map.zone_headers.at("short.ff").has_value());
  CHECK(!map.zone_headers.at("other.ff").has_value());
  CHECK(!map.zone_headers.contains("deep/zm_deep.ff"));
  const installed_content &tools = scan.catalog[1];
  CHECK_EQ(tools.zone_headers.at("zone/core_mod.ff"), std::optional(7 * MiB));
  CHECK_EQ(zone_memory_of(tools.zone_headers, "zm", true).bytes, 8 * MiB);
  // A header is no reason for a scan note.
  CHECK(scan.diags.items().empty());
  std::error_code ec;
  fs::remove_all(root, ec);
}

// ------------------------------------------------------------ the planner
TEST_CASE(
    "zone memory: a plan without a map names the maps over, worst first") {
  const fs::path root = write_catalog("nomap");
  const catalog_scan scan = scan_mods_directory(root);
  const plan p = resolve_plan(scan.catalog, zombies({"ws.100", "ws.101"}));
  REQUIRE(p.ok());
  REQUIRE_EQ(p.stack.size(), std::size_t(2));
  CHECK_EQ(p.stack[0].zone_memory, 452 * MiB);
  CHECK_EQ(p.stack[1].zone_memory, 1 * MiB);
  const zone_memory_report &memory = p.zone_memory;
  REQUIRE(memory.budget.has_value());
  CHECK_EQ(memory.stack_bytes, 453 * MiB);
  CHECK(!memory.map_bytes.has_value());
  CHECK(memory.unread.empty());
  // E 2552 (over even alone), B 2462, then A and F tie at 2103: by id. C
  // fits; D is a Multiplayer map.
  CHECK(over_ids(p) ==
        (std::vector<std::string>{"ws.204", "ws.201", "ws.200", "ws.205"}));
  REQUIRE_EQ(memory.maps_over.size(), std::size_t(4));
  CHECK_EQ(memory.maps_over[0].name, std::string("Map E"));
  CHECK_EQ(memory.maps_over[0].need_mib, 2552.0);
  CHECK_EQ(memory.maps_over[0].over_by_mib, 504.0);
  CHECK(memory.maps_over[0].over_without_mods);
  CHECK_EQ(memory.maps_over[1].over_by_mib, 414.0);
  CHECK(!memory.maps_over[1].over_without_mods);
  CHECK_EQ(memory.maps_over[2].need_mib, 2103.0);
  CHECK_EQ(memory.maps_over[3].need_mib, 2103.0);

  // Removing a mod helps B, A and F; E needs another map, so it gets its own
  // warning without that advice.
  const diagnostic *warning = find_diag(p, "plan.memory.maps_over");
  REQUIRE(warning != nullptr);
  CHECK(warning->level == severity::warning);
  CHECK(contains(warning->message, "3 maps do not fit in the game's memory "
                                   "with these mods: Map B (414 MiB over), "
                                   "Map A (55 MiB over) and Map F (55 MiB "
                                   "over)."));
  CHECK(!contains(warning->message, "Map E"));
  CHECK(contains(warning->message, "closes while loading them"));
  CHECK(contains(warning->message, "Remove a mod"));
  CHECK(contains(warning->message,
                 "The mod that takes the most is Big Mod (452 MiB)."));
  const diagnostic *too_big = find_diag(p, "plan.memory.maps_too_big");
  REQUIRE(too_big != nullptr);
  CHECK(too_big->level == severity::warning);
  CHECK_EQ(too_big->message,
           std::string("The map Map E (504 MiB over) does not fit in the "
                       "game's memory even with no mods: the game closes "
                       "while loading it. Play another map."));
  CHECK(find_diag(p, "plan.memory.over") == nullptr);
  CHECK(find_diag(p, "plan.memory.unknown") == nullptr);
  CHECK(find_diag(p, "plan.memory.maps_unread") == nullptr);

  // The plan JSON carries the numbers; MiB to one decimal.
  const std::string text = plan_to_json(p);
  CHECK(contains(text, R"("zone_memory":{"baseline_mib":849,"checked":true,)"
                       R"("larger_pool":false,"larger_pool_mib":null,)"
                       R"("map_mib":null,"maps_over":[{"fits_larger_pool":)"
                       R"(false,"id":"ws.204",)"
                       R"("name":"Map E","need_mib":2552,"over_by_mib":504,)"
                       R"("over_without_mods":true},)"));
  CHECK(contains(text, R"("pool_mib":2048,"stack_mib":453,"total_mib":1302,)"
                       R"("unread":[]})"));
  CHECK(contains(text, R"("zone_mib":452)"));

  // One mod left: only the map too big on its own is over, and removing the
  // mod would not help.
  const plan small = resolve_plan(scan.catalog, zombies({"ws.101"}));
  CHECK(over_ids(small) == std::vector<std::string>{"ws.204"});
  CHECK(find_diag(small, "plan.memory.maps_over") == nullptr);
  const diagnostic *one = find_diag(small, "plan.memory.maps_too_big");
  REQUIRE(one != nullptr);
  CHECK(contains(one->message, "The map Map E (52 MiB over)"));
  CHECK(contains(one->message, "closes while loading it"));
  CHECK(!contains(one->message, "Remove a mod"));

  // No mods at all: nothing says "with these mods" or "Remove a mod".
  const plan none = resolve_plan(scan.catalog, zombies({}));
  REQUIRE(none.ok());
  CHECK(none.stack.empty());
  CHECK(over_ids(none) == std::vector<std::string>{"ws.204"});
  CHECK(find_diag(none, "plan.memory.maps_over") == nullptr);
  REQUIRE(find_diag(none, "plan.memory.maps_too_big") != nullptr);
  for (const auto &item : none.diags.items()) {
    CHECK(!contains(item.message, "with these mods"));
    CHECK(!contains(item.message, "Remove a mod"));
    CHECK(!contains(item.message, "takes the most"));
  }
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("zone memory: the plan's own map over the pool is an error") {
  const fs::path root = write_catalog("withmap");
  const catalog_scan scan = scan_mods_directory(root);
  const plan p =
      resolve_plan(scan.catalog, zombies({"ws.100", "ws.101"}, "ws.200"));
  CHECK(!p.ok());
  REQUIRE(p.map.has_value());
  CHECK_EQ(p.zone_memory.map_bytes, std::optional(801 * MiB));
  const diagnostic *error = find_diag(p, "plan.memory.over");
  REQUIRE(error != nullptr);
  CHECK(error->level == severity::error);
  CHECK_EQ(error->path, std::string("/map"));
  CHECK(contains(error->message, "Map A does not fit in the game's memory "
                                 "with these mods (55 MiB over)"));
  CHECK(contains(error->message, "close while loading it"));
  // The others are still named, without the plan's map.
  const diagnostic *warning = find_diag(p, "plan.memory.maps_over");
  REQUIRE(warning != nullptr);
  CHECK(contains(warning->message, "2 maps do not fit"));
  CHECK(contains(warning->message, "Map B (414 MiB over) and Map F"));
  CHECK(!contains(warning->message, "Map A"));
  const diagnostic *too_big_other = find_diag(p, "plan.memory.maps_too_big");
  REQUIRE(too_big_other != nullptr);
  CHECK(contains(too_big_other->message, "The map Map E (504 MiB over)"));
  CHECK(over_ids(p) ==
        (std::vector<std::string>{"ws.204", "ws.201", "ws.200", "ws.205"}));
  CHECK(contains(plan_to_json(p), R"("map_mib":801,)"));
  CHECK(contains(plan_to_json(p), R"("total_mib":2103,)"));

  // A map too big even alone: removing mods does not help.
  const plan alone = resolve_plan(scan.catalog, zombies({"ws.101"}, "ws.204"));
  const diagnostic *too_big = find_diag(alone, "plan.memory.over");
  REQUIRE(too_big != nullptr);
  CHECK(contains(too_big->message, "even with no mods"));
  CHECK(contains(too_big->message, "Pick another map."));
  CHECK(!contains(too_big->message, "Remove a mod"));

  // A map that fits: no error.
  const plan fits =
      resolve_plan(scan.catalog, zombies({"ws.100", "ws.101"}, "ws.202"));
  CHECK(fits.ok());
  CHECK(find_diag(fits, "plan.memory.over") == nullptr);
  CHECK_EQ(fits.zone_memory.map_bytes, std::optional(600 * MiB));
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("zone memory: the larger zone memory's budget") {
  zone_memory_build build;
  build.name = "test";
  build.db_main_pool_mib = 2048;
  build.baseline_mib = {{"zm", 849}};
  const auto stock = zone_budget_for(build, "zm");
  REQUIRE(stock.has_value());
  CHECK_EQ(stock->pool_mib, 2048.0);
  CHECK(!stock->larger_pool);
  CHECK_EQ(stock->larger_pool_mib, 4096.0);
  const auto larger = zone_budget_for(build, "zm", true);
  REQUIRE(larger.has_value());
  CHECK_EQ(larger->pool_mib, 4096.0);
  CHECK(larger->larger_pool);
  CHECK_EQ(larger->baseline_mib, 849.0); // the game's own zones are the same
  // The client raises only the stock 2 GiB pool: another build's pool stays.
  build.db_main_pool_mib = 3072;
  const auto other = zone_budget_for(build, "zm", true);
  REQUIRE(other.has_value());
  CHECK_EQ(other->pool_mib, 3072.0);
  CHECK(!other->larger_pool);
  CHECK_EQ(other->larger_pool_mib, 0.0);
}

TEST_CASE("zone memory: maps the larger zone memory makes room for") {
  const fs::path root = write_catalog("larger");
  workshop_item(
      root, "102", "mod", "Broken Mod",
      {{"core_mod.ff", "TAff0000 too short"}, {"zm_mod.ff", ff(0, 10 * MiB)}});
  const catalog_scan scan = scan_mods_directory(root);
  // The stock pool, on a build the larger zone memory applies to.
  const auto stock = [](std::vector<std::string> mods,
                        std::optional<std::string> map = std::nullopt) {
    plan_request request = zombies(std::move(mods), std::move(map));
    request.zone_budget = zone_budget{2048, 849, false, 4096};
    return request;
  };
  const plan p =
      resolve_plan(scan.catalog, stock({"ws.100", "ws.101"}, "ws.200"));
  CHECK(!p.ok());
  for (const auto &map : p.zone_memory.maps_over) {
    CHECK(map.fits_larger_pool); // 2552 MiB at most
  }
  const diagnostic *error = find_diag(p, "plan.memory.over");
  REQUIRE(error != nullptr);
  CHECK(contains(error->message,
                 "Map A does not fit in the game's memory with these mods (55 "
                 "MiB over): the game would close while loading it. Turn on "
                 "Larger zone memory (experimental) in Settings, remove a mod, "
                 "or pick another map."));
  const diagnostic *warning = find_diag(p, "plan.memory.maps_over");
  REQUIRE(warning != nullptr);
  CHECK(contains(warning->message, "Turn on Larger zone memory (experimental) "
                                   "in Settings to make room for them."));
  const diagnostic *too_big = find_diag(p, "plan.memory.maps_too_big");
  REQUIRE(too_big != nullptr);
  CHECK(contains(too_big->message, "Play another map. Turn on Larger zone "
                                   "memory (experimental) in Settings to make "
                                   "room for it."));
  CHECK(contains(plan_to_json(p), R"("larger_pool":false,"larger_pool_mib":)"
                                  R"(4096,)"));

  // Too big even alone, but not for the larger pool.
  const plan alone = resolve_plan(scan.catalog, stock({"ws.101"}, "ws.204"));
  const diagnostic *own = find_diag(alone, "plan.memory.over");
  REQUIRE(own != nullptr);
  CHECK(contains(own->message, "even with no mods (52 MiB over): the game "
                               "would close while loading it. Turn on Larger "
                               "zone memory (experimental) in Settings to "
                               "play it, or pick another map."));

  // A mod with a zone not read: nothing is promised.
  const plan unread = resolve_plan(scan.catalog, stock({"ws.102"}, "ws.204"));
  REQUIRE(!unread.zone_memory.maps_over.empty());
  for (const auto &map : unread.zone_memory.maps_over) {
    CHECK(!map.fits_larger_pool);
  }
  for (const auto &item : unread.diags.items()) {
    CHECK(!contains(item.message, "Larger zone memory"));
  }

  // The launch has the larger zone memory: what is still over is over that,
  // and the setting is not offered (a small pool here, to have maps over).
  plan_request on = zombies({"ws.100", "ws.101"}, "ws.204");
  on.zone_budget = zone_budget{2400, 849, true, 2400};
  const plan larger = resolve_plan(scan.catalog, on);
  CHECK(over_ids(larger) == (std::vector<std::string>{"ws.204", "ws.201"}));
  const diagnostic *over = find_diag(larger, "plan.memory.over");
  REQUIRE(over != nullptr);
  CHECK(contains(over->message, "Map E does not fit in the larger zone memory "
                                "with these mods (152 MiB over)"));
  CHECK(!contains(over->message, "Turn on"));
  CHECK(contains(plan_to_json(larger), R"("larger_pool":true,)"));
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("zone memory: without figures the check is skipped with a note") {
  const fs::path root = write_catalog("nobudget");
  const catalog_scan scan = scan_mods_directory(root);
  const plan p =
      resolve_plan(scan.catalog, zombies({"ws.100"}, "ws.200", false));
  CHECK(p.ok()); // nothing is known to be over
  CHECK(!p.zone_memory.budget.has_value());
  CHECK(p.zone_memory.maps_over.empty());
  CHECK_EQ(p.zone_memory.stack_bytes, 452 * MiB); // still added up
  const diagnostic *note = find_diag(p, "plan.memory.unknown");
  REQUIRE(note != nullptr);
  CHECK(note->level == severity::info);
  CHECK(contains(note->message, "(zm)"));
  CHECK(find_diag(p, "plan.memory.maps_over") == nullptr);
  const std::string text = plan_to_json(p);
  CHECK(contains(text, R"("zone_memory":{"baseline_mib":null,"checked":false,)"
                       R"("larger_pool":false,"larger_pool_mib":null,)"
                       R"("map_mib":801,"maps_over":[],"maps_unread":[],)"
                       R"("pool_mib":null,)"
                       R"("stack_mib":452,"total_mib":null,"unread":[]})"));
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("zone memory: unread headers are named, and counted as nothing") {
  const fs::path root = write_catalog("unread");
  workshop_item(
      root, "102", "mod", "Broken Mod",
      {{"core_mod.ff", "TAff0000 too short"}, {"zm_mod.ff", ff(0, 10 * MiB)}});
  const catalog_scan scan = scan_mods_directory(root);
  const plan p = resolve_plan(scan.catalog, zombies({"ws.102"}, "ws.202"));
  CHECK_EQ(p.zone_memory.stack_bytes, 10 * MiB);
  CHECK(p.zone_memory.unread == std::vector<std::string>{"ws.102"});
  CHECK(contains(plan_to_json(p), R"("unread":["ws.102"])"));
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("zone memory: installed maps with an unread header are not called "
          "fine") {
  // Map G's main zone has another fastfile version and Map I's another
  // magic, so only G's language copy (1 MiB) is known: both are named as not
  // checked. Map H's language copy is too short, but its main zone alone is
  // over, so it is over.
  const fs::path root = write_catalog("unread_maps");
  workshop_item(root, "206", "map", "Map G",
                {{"zm_g.ff", ff(0, 1160 * MiB, fastfile_magic, 0x252)},
                 {"en_zm_g.ff", ff(1 * MiB, 0)}});
  workshop_item(
      root, "207", "map", "Map H",
      {{"zm_h.ff", ff(0, 1300 * MiB)}, {"en_zm_h.ff", "TAff0000 too short"}});
  workshop_item(root, "208", "map", "Map I",
                {{"zm_i.ff", ff(0, 900 * MiB, "XXff0000")}});
  const catalog_scan scan = scan_mods_directory(root);
  const plan p = resolve_plan(scan.catalog, zombies({"ws.101"}, "ws.202"));
  REQUIRE(p.ok());
  const zone_memory_report &memory = p.zone_memory;
  CHECK(memory.unread.empty()); // the stack and the plan's map were read
  CHECK(over_ids(p) == (std::vector<std::string>{"ws.207", "ws.204"}));
  REQUIRE_EQ(memory.maps_unread.size(), std::size_t(2));
  CHECK_EQ(memory.maps_unread[0].id, std::string("ws.206"));
  CHECK_EQ(memory.maps_unread[0].name, std::string("Map G"));
  CHECK_EQ(memory.maps_unread[1].id, std::string("ws.208"));
  const diagnostic *unread = find_diag(p, "plan.memory.maps_unread");
  REQUIRE(unread != nullptr);
  CHECK(unread->level == severity::warning);
  CHECK_EQ(unread->message,
           std::string("Xerces could not read part of 2 maps, Map G and Map "
                       "I, so it did not check whether they fit in the "
                       "game's memory. If Steam is updating them, check "
                       "again once Steam is done."));
  CHECK(contains(plan_to_json(p),
                 R"("maps_unread":[{"id":"ws.206","name":"Map G"},)"
                 R"({"id":"ws.208","name":"Map I"}],"pool_mib":2048,)"));

  // The plan's own map unread: in unread and named as not checked, but not
  // refused, since nothing says it is over.
  const plan own = resolve_plan(scan.catalog, zombies({"ws.101"}, "ws.206"));
  CHECK(own.ok());
  CHECK(own.zone_memory.unread == std::vector<std::string>{"ws.206"});
  CHECK_EQ(own.zone_memory.map_bytes, std::optional(1 * MiB));
  REQUIRE_EQ(own.zone_memory.maps_unread.size(), std::size_t(2));
  CHECK_EQ(own.zone_memory.maps_unread[0].id, std::string("ws.206"));

  // Without figures nothing is checked, so nothing is named.
  const plan unknown =
      resolve_plan(scan.catalog, zombies({"ws.101"}, "ws.202", false));
  CHECK(unknown.zone_memory.maps_unread.empty());
  CHECK(find_diag(unknown, "plan.memory.maps_unread") == nullptr);
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("zone memory: not part of plan_id or content_id") {
  const fs::path all = write_catalog("ids_all");
  const fs::path fewer = write_catalog("ids_fewer", false); // no Map E
  const catalog_scan with_e = scan_mods_directory(all);
  const catalog_scan without_e = scan_mods_directory(fewer);
  const plan a = resolve_plan(with_e.catalog, zombies({"ws.100"}, "ws.202"));
  const plan b =
      resolve_plan(with_e.catalog, zombies({"ws.100"}, "ws.202", false));
  const plan c = resolve_plan(without_e.catalog, zombies({"ws.100"}, "ws.202"));
  plan_request other_budget = zombies({"ws.100"}, "ws.202");
  other_budget.zone_budget = zone_budget{4096, 100};
  const plan d = resolve_plan(with_e.catalog, other_budget);
  CHECK(a.zone_memory.maps_over.size() != b.zone_memory.maps_over.size());
  CHECK(a.zone_memory.maps_over.size() != c.zone_memory.maps_over.size());
  CHECK(d.zone_memory.maps_over.empty());
  for (const plan *other : {&b, &c, &d}) {
    CHECK_EQ(other->plan_id, a.plan_id);
    CHECK_EQ(other->content_id, a.content_id);
    CHECK_EQ(other->canonical, a.canonical);
  }
  CHECK(!contains(a.canonical, "zone"));
  CHECK(!contains(a.canonical, "maps_over"));
  std::error_code ec;
  fs::remove_all(all, ec);
  fs::remove_all(fewer, ec);
}

TEST_CASE("zone memory: the Mods screen shows it as a plan-wide note") {
  const fs::path root = write_catalog("notes");
  const catalog_scan scan = scan_mods_directory(root);
  const plan p =
      resolve_plan(scan.catalog, zombies({"ws.100", "ws.101"}, "ws.200"));
  std::vector<std::pair<std::string, std::string>> names;
  for (const auto &item : scan.catalog) {
    names.emplace_back(item.info.id, item.info.name);
  }
  const mods_notes notes = mods_attribute(p, names);
  // The messages name maps and the biggest mod, but belong to no mod.
  for (const auto &[id, list] : notes.by_id) {
    for (const auto &note : list) {
      CHECK(!note.code.starts_with("plan.memory."));
    }
  }
  std::vector<std::string> general;
  for (const auto &note : notes.general) {
    general.push_back(note.code);
  }
  CHECK(std::ranges::find(general, "plan.memory.over") != general.end());
  CHECK(std::ranges::find(general, "plan.memory.maps_over") != general.end());
  CHECK(std::ranges::find(general, "plan.memory.maps_too_big") !=
        general.end());
  std::error_code ec;
  fs::remove_all(root, ec);
}

// ------------------------------------------------- the launcher's profile
TEST_CASE("zone memory: a profile's request takes the build's figures") {
  launcher_config config;
  profile_for(config, "zm").mods = {"ws.100"};
  zone_memory_build build;
  build.name = "test";
  build.steam_build = 24784313;
  build.db_main_pool_mib = 2048;
  build.baseline_mib = {{"zm", 849}};
  const plan_request zm =
      profile_request(config, "zm", std::uint64_t(24784313), "", build);
  REQUIRE(zm.zone_budget.has_value());
  CHECK_EQ(zm.zone_budget->pool_mib, 2048.0);
  CHECK_EQ(zm.zone_budget->baseline_mib, 849.0);
  CHECK(!profile_request(config, "mp", std::uint64_t(24784313), "", build)
             .zone_budget.has_value());
  CHECK(!profile_request(config, "zm", std::uint64_t(24784313))
             .zone_budget.has_value());
  // With the larger zone memory in the settings, the plan is checked against
  // it.
  config.extra_flags = {"-console", "--xerces-db-pool"};
  const plan_request larger =
      profile_request(config, "zm", std::uint64_t(24784313), "", build);
  REQUIRE(larger.zone_budget.has_value());
  CHECK_EQ(larger.zone_budget->pool_mib, 4096.0);
  CHECK(larger.zone_budget->larger_pool);
}

TEST_CASE("zone memory: the launcher's game finds its figures and refuses") {
  // A fake game whose exe has a checksum only this test's compat\ knows, a
  // mod in mods\ and two maps in usermaps\, Mod Tools layout.
  const fs::path root = fresh("launcher");
  const fs::path game = root / "steamapps" / "common" / "game";
  std::string pe(0x40 + 24 + 96, '\0');
  pe[0] = 'M';
  pe[1] = 'Z';
  put32(pe, 0x3C, 0x40);
  put32(pe, 0x40, 0x00004550);
  put32(pe, 0x40 + 24 + 64, 0x7E570002);
  write(game / "BlackOps3.exe", pe);
  write(root / "compat" / "builds.json",
        R"({"schema":"xerces.builds/1","builds":[{"name":"Fake","exe":)"
        R"("BlackOps3.exe","checksum":"0x7E570002","status":"supported"}]})");
  write(root / "compat" / "zone_memory.json",
        R"({"schema":"xerces.zone_memory/1","builds":[{"name":"Fake",)"
        R"("checksum":"0x7E570002","db_main_pool_mib":2048,)"
        R"("baseline_mib":{"zm":849}}]})");
  write(game / "mods" / "heavy" / "zone" / "core_mod.ff", ff(0, 400 * MiB));
  write(game / "mods" / "heavy" / "zone" / "zm_mod.ff", ff(0, 50 * MiB));
  for (const auto &[name, mib] :
       std::vector<std::pair<std::string, std::uint64_t>>{{"zm_big", 900},
                                                          {"zm_small", 300}}) {
    const fs::path map = game / "usermaps" / name / "zone";
    write(map / "workshop.json", R"({"Type":"map","Title":")" + name + R"("})");
    write(map / (name + ".ff"), ff(0, mib * MiB));
  }

  const game_state state = detect_game_state(path_utf8(game), root / "compat");
  REQUIRE(state.can_launch());
  REQUIRE(state.zone_memory.has_value());
  CHECK_EQ(state.zone_memory->db_main_pool_mib, 2048.0);
  const catalog_state catalog = scan_catalog(state, root / "hashes.txt");
  REQUIRE_EQ(catalog.items.size(), std::size_t(3));

  launcher_config config;
  mode_profile &zm = profile_for(config, "zm");
  zm.mods = {"local.heavy"};
  const plan p = resolve_profile(catalog.items, config, "zm", state.steam_build,
                                 nullptr, "", state.zone_memory);
  REQUIRE(p.ok());
  CHECK_EQ(p.zone_memory.stack_bytes, 450 * MiB);
  CHECK(over_ids(p) == std::vector<std::string>{"local.zm_big"});

  // Home's Launch with the big map refuses before anything starts.
  zm.map = "local.zm_big";
  launch_inputs in;
  in.config = &config;
  in.game = &state;
  in.catalog = &catalog.items;
  in.mode = "zm";
  const launch_outcome out = preview_launch(in);
  CHECK(out.refusal == launch_refusal::plan);
  CHECK(std::ranges::any_of(out.diags.items(), [](const diagnostic &item) {
    return item.code == "plan.memory.over";
  }));
  // With the larger zone memory the big one fits (849 + 450 + 900 MiB) and
  // launches, with the flag; a culprit-search test launches without it, so
  // it is checked against the stock pool and refused.
  config.extra_flags = {"-xerces-db-pool"};
  const launch_outcome larger = preview_launch(in);
  CHECK(larger.refusal == launch_refusal::none);
  CHECK(larger.db_pool);
  in.crash_followup = true;
  const launch_outcome followup = preview_launch(in);
  CHECK(followup.refusal == launch_refusal::plan);
  CHECK(!followup.db_pool);
  in.crash_followup = false;
  config.extra_flags.clear();
  // The small one launches.
  zm.map = "local.zm_small";
  CHECK(preview_launch(in).refusal == launch_refusal::none);
  std::error_code ec;
  fs::remove_all(root, ec);
}
