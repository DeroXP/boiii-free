// The Packs screen's headless half: the pack library on disk, imports, names
// and the installed-content check (src/xerces_core/packs_store.hpp).

#include "harness.hpp"

#include <fstream>

#include <xerces_core/packs_store.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
struct temp_dir {
  fs::path path;
  explicit temp_dir(const char *name) : path(fs::temp_directory_path() / name) {
    fs::remove_all(path);
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

const std::string hash_a = "sha256:" + std::string(64, 'a');
const std::string hash_b = "sha256:" + std::string(64, 'b');
const std::string hash_c = "sha256:" + std::string(64, 'c');

version ver(const char *text) { return *version::parse(text); }

pack sample(const std::string &id, const std::string &name) {
  pack p;
  p.id = id;
  p.name = name;
  p.mode = "zm";
  pack_mod a;
  a.id = "ws.1944930083";
  a.ver = ver("0.0.0+ws.1700000000");
  a.workshop_id = 1944930083ull;
  a.content_hash = hash_a;
  pack_mod b;
  b.id = "local.better_hud";
  b.ver = ver("1.2.0");
  b.content_hash = hash_b;
  p.mods = {a, b};
  p.order = {"local.better_hud", "ws.1944930083"};
  return p;
}

installed_content installed(const std::string &id, const char *v,
                            const std::string &hash,
                            const std::string &name = {}) {
  installed_content c;
  c.info.id = id;
  c.info.name = name.empty() ? id : name;
  c.info.ver = ver(v);
  c.content_hash = hash;
  return c;
}

const std::string id_1 = "11111111-1111-4111-8111-111111111111";
const std::string id_2 = "22222222-2222-4222-8222-222222222222";
const std::string id_3 = "33333333-3333-4333-8333-333333333333";
} // namespace

TEST_CASE("packs store: save, list sorted by name, delete") {
  temp_dir root("xerces_tests_packs_store");
  const fs::path folder = root.path / "packs";
  CHECK(read_pack_library(folder).packs.empty()); // no folder yet: no error
  CHECK(read_pack_library(folder).diags.empty());

  diagnostics diags;
  REQUIRE(write_pack(folder, sample(id_1, "zombies weekend"), diags));
  REQUIRE(write_pack(folder, sample(id_2, "Alpha"), diags));
  CHECK(diags.empty());
  CHECK(fs::exists(folder / (id_1 + ".json")));
  CHECK(!fs::exists(folder / (id_1 + ".json.tmp")));

  pack_library library = read_pack_library(folder);
  REQUIRE_EQ(library.packs.size(), 2u);
  CHECK_EQ(library.packs[0].value.name, "Alpha"); // case-insensitive order
  CHECK_EQ(library.packs[1].value.name, "zombies weekend");
  REQUIRE(library.find(id_1) != nullptr);
  // The file is the canonical JSON, so it reads back identical.
  CHECK_EQ(pack_to_json(library.find(id_1)->value),
           pack_to_json(sample(id_1, "zombies weekend")));

  // Saving again under the same id replaces the file.
  REQUIRE(write_pack(folder, sample(id_1, "Zombies, renamed"), diags));
  library = read_pack_library(folder);
  REQUIRE_EQ(library.packs.size(), 2u);
  CHECK_EQ(library.find(id_1)->value.name, "Zombies, renamed");

  CHECK(remove_pack(folder, id_1));
  CHECK(!remove_pack(folder, id_1));
  library = read_pack_library(folder);
  REQUIRE_EQ(library.packs.size(), 1u);
  CHECK_EQ(library.packs[0].value.id, id_2);
}

TEST_CASE("packs store: broken files are skipped and reported, not deleted") {
  temp_dir root("xerces_tests_packs_broken");
  const fs::path folder = root.path;
  diagnostics diags;
  REQUIRE(write_pack(folder, sample(id_1, "Good"), diags));
  write(folder / "broken.json", "{ not json");
  write(folder / "notes.txt", "not a pack, not a .json file");
  // The same pack under another file name: listed once.
  write(folder / "copy.json", pack_to_json(sample(id_1, "Good")));

  const pack_library library = read_pack_library(folder);
  REQUIRE_EQ(library.packs.size(), 1u);
  CHECK_EQ(library.packs[0].file.filename(), fs::path(id_1 + ".json"));
  CHECK(library.diags.has("packs.file.invalid"));
  CHECK(library.diags.has("packs.file.duplicate"));
  CHECK(fs::exists(folder / "broken.json"));

  // Deleting the pack removes the copy too, so it does not come back.
  CHECK(remove_pack(folder, id_1));
  CHECK(read_pack_library(folder).packs.empty());
  CHECK(!fs::exists(folder / "copy.json"));
  CHECK(fs::exists(folder / "broken.json"));
}

TEST_CASE("packs store: delete removes every copy of a pack") {
  temp_dir root("xerces_tests_packs_copies");
  const fs::path folder = root.path;
  diagnostics diags;
  REQUIRE(write_pack(folder, sample(id_1, "Good"), diags));
  REQUIRE(write_pack(folder, sample(id_2, "Other"), diags));
  // The library lists one file per id, so two strays are only reported.
  write(folder / "copy-a.json", pack_to_json(sample(id_1, "Good")));
  write(folder / "copy-b.json", pack_to_json(sample(id_1, "Good")));
  REQUIRE_EQ(read_pack_library(folder).packs.size(), 2u);

  CHECK(remove_pack(folder, id_1));
  const pack_library library = read_pack_library(folder);
  REQUIRE_EQ(library.packs.size(), 1u);
  CHECK_EQ(library.packs[0].value.id, id_2);
  CHECK(!fs::exists(folder / "copy-a.json"));
  CHECK(!fs::exists(folder / "copy-b.json"));

  // Only strays are left: they still go, and the call reports it.
  write(folder / "stray.json", pack_to_json(sample(id_1, "Good")));
  CHECK(remove_pack(folder, id_1));
  CHECK(!fs::exists(folder / "stray.json"));
  CHECK(fs::exists(pack_library_file(folder, id_2)));
}

TEST_CASE("packs store: a map with no mods is a pack") {
  temp_dir root("xerces_tests_packs_map_only");
  pack p;
  p.id = id_1;
  p.name = "Just the map";
  p.mode = "zm";
  p.map = pack_map{"ws.3735179543", hash_c};
  diagnostics diags;
  REQUIRE(write_pack(root.path, p, diags));
  CHECK(diags.empty());
  const pack_library library = read_pack_library(root.path);
  REQUIRE_EQ(library.packs.size(), 1u);
  REQUIRE(library.packs[0].value.map.has_value());
  CHECK_EQ(library.packs[0].value.map->id, "ws.3735179543");
  CHECK(library.packs[0].value.mods.empty());
  CHECK(read_pasted_pack(encode_share_code(p)).ok());
}

TEST_CASE("packs store: invalid packs are not written") {
  temp_dir root("xerces_tests_packs_invalid");
  diagnostics diags;
  pack bad = sample(id_1, "");
  CHECK(!write_pack(root.path, bad, diags));
  CHECK(diags.has("pack.name"));

  diagnostics path_diags;
  pack local = sample(id_2, "Has a path");
  local.mods[0].settings["dir"] = "\"C:\\\\Users\\\\someone\\\\mods\"";
  CHECK(!write_pack(root.path, local, path_diags));
  CHECK(path_diags.has("pack.local_path"));
  CHECK(read_pack_library(root.path).packs.empty());
}

TEST_CASE("packs store: reading files and share codes") {
  temp_dir root("xerces_tests_packs_files");
  const pack p = sample(id_1, "Shared");

  write(root.path / "a.xpack.json", "\xEF\xBB\xBF" + pack_to_json(p));
  pack_result from_json = read_pack_file(root.path / "a.xpack.json");
  REQUIRE(from_json.ok());
  CHECK_EQ(from_json.value.name, "Shared");

  write(root.path / "code.txt", "  " + encode_share_code(p) + "\r\n");
  pack_result from_code = read_pack_file(root.path / "code.txt");
  REQUIRE(from_code.ok());
  CHECK_EQ(pack_to_json(from_code.value), pack_to_json(p));

  CHECK(read_pack_file(root.path / "missing.json").diags.has("pack.file.read"));
  write(root.path / "empty.json", "   ");
  CHECK(read_pack_file(root.path / "empty.json").diags.has("pack.file.empty"));
  write(root.path / "big.json",
        std::string(static_cast<std::size_t>(pack_file_max_bytes) + 1, ' '));
  CHECK(read_pack_file(root.path / "big.json").diags.has("pack.file.size"));
  CHECK(read_pack_text("XRC1.!!!").diags.has("pack.code.encoding"));
  CHECK(!read_pack_text("{\"schema\": 2}").ok());

  // The share-code field: codes, and JSON pasted in.
  CHECK(read_pasted_pack("  " + encode_share_code(p) + "\n").ok());
  CHECK(read_pasted_pack(" " + pack_to_json(p)).ok());
  CHECK(read_pasted_pack("\xEF\xBB\xBF" + pack_to_json(p)).ok());
  // Anything else is a share code that is wrong, said as such.
  CHECK(read_pasted_pack("XRC-ABCD1234").diags.has("pack.code.prefix"));
  CHECK(read_pasted_pack("xrc1.abc").diags.has("pack.code.prefix"));
  CHECK(!read_pasted_pack("XRC-ABCD1234").diags.has("pack.json"));
  CHECK(read_pasted_pack("XRC1.!!!").diags.has("pack.code.encoding"));
  CHECK(read_pasted_pack("{ not json").diags.has("pack.json"));
}

TEST_CASE("packs store: export file names get the pack extension") {
  const auto named = [](const char *text) {
    return with_pack_extension(fs::path("C:/out") / text).filename().string();
  };
  CHECK_EQ(named("Weekend"), "Weekend.xpack.json");
  // A dot in the name is not an extension.
  CHECK_EQ(named("Zombies v1.2"), "Zombies v1.2.xpack.json");
  CHECK_EQ(named("weekend.v2"), "weekend.v2.xpack.json");
  CHECK_EQ(named("Weekend.xpack"), "Weekend.xpack.json");
  // Names that already end in .json are what the player chose.
  CHECK_EQ(named("Weekend.xpack.json"), "Weekend.xpack.json");
  CHECK_EQ(named("Weekend.XPACK.JSON"), "Weekend.XPACK.JSON");
  CHECK_EQ(named("weekend.json"), "weekend.json");
  CHECK_EQ(with_pack_extension(fs::path("C:/out") / "Zombies v1.2").parent_path(),
           fs::path("C:/out"));
}

TEST_CASE("packs store: imports never overwrite a different pack") {
  temp_dir root("xerces_tests_packs_import");
  diagnostics diags;
  REQUIRE(write_pack(root.path, sample(id_1, "Mine"), diags));
  const pack_library library = read_pack_library(root.path);
  const auto fixed_id = [] { return id_3; };

  const import_placement fresh =
      place_import(library, sample(id_2, "New"), fixed_id);
  CHECK(fresh.kind == import_kind::added);
  CHECK_EQ(fresh.value.id, id_2);

  const import_placement same =
      place_import(library, sample(id_1, "Mine"), fixed_id);
  CHECK(same.kind == import_kind::duplicate);
  CHECK_EQ(same.value.id, id_1);

  const import_placement other =
      place_import(library, sample(id_1, "Someone else's"), fixed_id);
  CHECK(other.kind == import_kind::renumbered);
  CHECK_EQ(other.value.id, id_3);
  CHECK_EQ(other.value.name, "Someone else's");
}

TEST_CASE("packs store: names") {
  CHECK_EQ(trim_pack_name("  Weekend \t"), "Weekend");
  CHECK_EQ(pack_name_problem("   "), "Give the pack a name.");
  CHECK(pack_name_problem("Weekend Zombies").empty());
  CHECK(pack_name_problem(std::string(80, 'x')).empty());
  CHECK(!pack_name_problem(std::string(81, 'x')).empty());
  // 80 characters of two bytes each are still 80 characters.
  std::string accented;
  for (int i = 0; i < 80; ++i) {
    accented += "\xC3\xA9";
  }
  CHECK(pack_name_problem(accented).empty());
  CHECK(!pack_name_problem("tab\x01name").empty());

  temp_dir root("xerces_tests_packs_names");
  diagnostics diags;
  REQUIRE(write_pack(root.path, sample(id_1, "Weekend Zombies"), diags));
  const pack_library library = read_pack_library(root.path);
  CHECK(find_by_name(library, " weekend zombies ") != nullptr);
  CHECK(find_by_name(library, "Weekend") == nullptr);

  CHECK_EQ(strip_colour_codes("^3[^5PZM^3] ^9PEGASUS^7 v0.2"), "[PZM] PEGASUS v0.2");
  CHECK_EQ(strip_colour_codes("50^ off"), "50^ off");
  CHECK_EQ(pack_file_stem("Weekend: Zombies/MP?"), "Weekend_ Zombies_MP_");
  CHECK_EQ(pack_file_stem("..\\..\\evil"), "_.._evil");
  CHECK_EQ(pack_file_stem("   "), "pack");
  CHECK_EQ(pack_file_stem("CON"), "CON_pack");
  CHECK_EQ(pack_file_stem("Caf\xC3\xA9"), "Caf\xC3\xA9");
}

TEST_CASE("packs store: the installed-content check") {
  pack p = sample(id_1, "Check");
  p.map = pack_map{"ws.3735179543", hash_c};

  // Nothing installed: everything missing, Workshop ids where known.
  pack_check none = check_pack(p, {});
  REQUIRE_EQ(none.items.size(), 3u);
  CHECK_EQ(none.items[0].id, "local.better_hud"); // load order, then the map
  CHECK_EQ(none.items[1].id, "ws.1944930083");
  CHECK(none.items[2].is_map);
  CHECK_EQ(none.missing, 3u);
  CHECK(!none.ready());
  CHECK(!none.items[0].workshop_id.has_value());
  CHECK_EQ(none.items[1].workshop_id.value_or(0), 1944930083ull);
  CHECK_EQ(none.items[2].workshop_id.value_or(0), 3735179543ull);

  // Everything as pinned (the Workshop title loses its colour codes).
  std::vector<installed_content> catalog = {
      installed("ws.1944930083", "0.0.0+ws.1700000000", hash_a,
                "^3Solo ^7Easter Eggs"),
      installed("local.better_hud", "1.2.0", hash_b),
      installed("ws.3735179543", "0.0.0+ws.1", hash_c, "BACKROOMZ")};
  pack_check all = check_pack(p, catalog);
  CHECK(all.ready());
  CHECK_EQ(all.items[1].name, "Solo Easter Eggs");
  CHECK_EQ(all.items[2].name, "BACKROOMZ");

  // A Workshop update changes the build part of the version: other version.
  catalog[0].info.ver = ver("0.0.0+ws.1800000000");
  // Same version, different files: changed.
  catalog[1].content_hash = hash_c;
  // The map's files changed.
  catalog[2].content_hash = hash_a;
  pack_check drift = check_pack(p, catalog);
  CHECK_EQ(drift.other_version, 1u);
  CHECK_EQ(drift.changed, 2u);
  CHECK_EQ(drift.problems(), 3u);
  CHECK(drift.items[1].state == pack_item_state::other_version);
  CHECK_EQ(drift.items[1].installed, "0.0.0+ws.1800000000");
  CHECK(drift.items[0].state == pack_item_state::changed);
  CHECK(drift.items[2].state == pack_item_state::changed);

  // Two copies of one version: any copy with the pinned files is enough.
  catalog.push_back(installed("local.better_hud", "1.2.0", hash_b));
  CHECK(check_pack(p, catalog).items[0].state == pack_item_state::installed);
}
