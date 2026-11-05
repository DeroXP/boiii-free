// Signature registry, install detection and safe mode, on synthetic data.

#include "harness.hpp"

#include <cstring>
#include <map>
#include <set>

#include <xerces_core/detect.hpp>
#include <xerces_core/journal.hpp>
#include <xerces_core/sigscan.hpp>

using namespace xerces_core;

namespace {
std::vector<std::uint8_t> filler(std::size_t size) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] =
        static_cast<std::uint8_t>((i * 7 + 3) & 0x7f); // never 0xC3 or 0x8B
  }
  return bytes;
}

void put(std::vector<std::uint8_t> &bytes, std::size_t at,
         std::initializer_list<std::uint8_t> values) {
  std::size_t i = at;
  for (const auto value : values) {
    bytes[i++] = value;
  }
}

signature sig(std::string id, std::vector<std::string> patterns) {
  signature s;
  s.id = std::move(id);
  s.symbol = "Symbol";
  s.patterns = std::move(patterns);
  return s;
}
} // namespace

TEST_CASE("signatures: patterns parse with wildcards") {
  const auto p = parse_pattern("48 8B 05 ?? ?? ? ?? C3");
  REQUIRE(p.has_value());
  CHECK_EQ(p->bytes.size(), 8u);
  CHECK(p->fixed[0] && p->fixed[2] && !p->fixed[3] && !p->fixed[5] &&
        p->fixed[7]);
  std::string error;
  CHECK(!parse_pattern("4G 00", &error).has_value());
  CHECK(!error.empty());
  CHECK(!parse_pattern("?? ??").has_value());
  CHECK(!parse_pattern("").has_value());
}

TEST_CASE("signatures: a symbol must match exactly once") {
  auto text = filler(0x400);
  put(text, 0x100, {0x48, 0x8B, 0x8B, 0xC3});
  put(text, 0x200, {0x55, 0x8B, 0xC3, 0x90});
  put(text, 0x300, {0x55, 0x8B, 0xC3, 0x91});
  const std::vector<section_view> sections = {{".text", 0x1000, text}};
  const signature_report report = resolve_signatures(
      {sig("unique", {"48 8B 8B C3"}), sig("twice", {"55 8B C3"}),
       sig("never", {"DE AD BE EF"}),
       sig("fallback", {"55 8B C3", "55 8B C3 91"})},
      sections, 0x6531394);
  CHECK(report.find("unique")->ok);
  CHECK_EQ(report.find("unique")->rva, 0x1100u);
  CHECK(!report.find("twice")->ok);
  CHECK_EQ(report.find("twice")->error, "more than one match");
  CHECK(!report.find("never")->ok);
  CHECK_EQ(report.find("never")->error, "0 matches");
  CHECK(report.find("fallback")->ok);
  CHECK_EQ(report.find("fallback")->rva, 0x1300u);
}

TEST_CASE("signatures: the second .text is never scanned") {
  auto first = filler(0x100);
  auto second = filler(0x100);
  put(second, 0x10, {0xC3, 0xC3, 0x8B, 0xC3});
  const std::vector<section_view> sections = {{".text", 0x1000, first},
                                              {".text", 0x8000, second}};
  const auto report =
      resolve_signatures({sig("hidden", {"C3 C3 8B C3"})}, sections, 1);
  CHECK(!report.find("hidden")->ok);
}

TEST_CASE("signatures: RIP-relative data and known addresses") {
  auto text = filler(0x200);
  const std::int32_t displacement = 0x200;
  put(text, 0x40, {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3});
  std::memcpy(text.data() + 0x43, &displacement, 4);
  const std::vector<section_view> sections = {{".text", 0x1000, text}};
  signature data = sig("data", {"48 8B 05 ?? ?? ?? ?? C3"});
  data.kind = symbol_kind::data_rip;
  data.disp_offset = 3;
  data.insn_end = 7;
  auto report = resolve_signatures({data}, sections, 7);
  REQUIRE(report.find("data")->ok);
  CHECK_EQ(report.find("data")->rva, 0x1040u + 7u + 0x200u);

  data.known[7] = 0x1247;
  CHECK(resolve_signatures({data}, sections, 7).find("data")->ok);
  data.known[7] = 0x9999;
  report = resolve_signatures({data}, sections, 7);
  CHECK(!report.find("data")->ok);
  CHECK(report.find("data")->error.find("known address") != std::string::npos);
}

TEST_CASE("signatures: a miss turns its features off with a clear message") {
  const auto text = filler(0x100);
  const std::vector<section_view> sections = {{".text", 0x1000, text}};
  signature missing = sig("db.override_asset", {"DE AD BE EF"});
  missing.symbol = "DB_OverrideAsset";
  missing.features = {"multi_mod"};
  const auto report = resolve_signatures({missing}, sections, 1);
  CHECK(!report.feature_enabled("multi_mod"));
  CHECK(report.feature_enabled("upscaling"));
  CHECK_EQ(report.message("multi_mod", "Multi-mod loading",
                          "Vanilla play and single mods still work."),
           "Multi-mod loading is off on this game build. Xerces could not find "
           "DB_OverrideAsset (signature db.override_asset, 0 matches). Vanilla "
           "play and single mods still work.");
}

TEST_CASE("signatures: the cache is re-validated by bytes") {
  auto text = filler(0x200);
  put(text, 0x80, {0x48, 0x8B, 0x8B, 0xC3});
  std::vector<section_view> sections = {{".text", 0x1000, text}};
  const std::vector<signature> sigs = {sig("one", {"48 8B 8B C3"})};
  const auto report = resolve_signatures(sigs, sections, 42);
  const std::string cache = signature_cache_to_json(report, 42, sections);
  const auto loaded = load_signature_cache(cache, 42, sections, sigs);
  REQUIRE(loaded.has_value());
  CHECK_EQ(loaded->find("one")->rva, 0x1080u);
  CHECK(!load_signature_cache(cache, 43, sections, sigs).has_value());
  auto more = sigs;
  more.push_back(sig("two", {"C3"}));
  CHECK(!load_signature_cache(cache, 42, sections, more).has_value());
  text[0x81] = 0x89; // the code changed
  sections = {{".text", 0x1000, text}};
  CHECK(!load_signature_cache(cache, 42, sections, sigs).has_value());
}

TEST_CASE("detection: libraryfolders.vdf, both layouts") {
  const std::string modern = R"(// comment
"libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"apps" { "228980" "1" }
	}
	"1"
	{
		"path"		"E:\\SteamLibrary"
		"label"		"games \"fast\""
	}
	"contentstatsid"	"123"
})";
  const auto libraries = steam_libraries(modern);
  REQUIRE_EQ(libraries.size(), 2u);
  CHECK_EQ(libraries[0].string(), "C:\\Program Files (x86)\\Steam");
  CHECK_EQ(libraries[1].string(), "E:\\SteamLibrary");

  const std::string old = "\"LibraryFolders\" { \"TimeNextStatsReport\" \"1\" "
                          "\"1\" \"D:\\\\Games\" }";
  REQUIRE_EQ(steam_libraries(old).size(), 1u);
  CHECK_EQ(steam_libraries(old)[0].string(), "D:\\Games");

  std::string error;
  CHECK(!parse_vdf("\"a\" { \"b\" \"c\"", &error).has_value());
  CHECK(!parse_vdf("\"a\" \"open").has_value());
}

TEST_CASE("detection: appmanifest installdir is a plain folder name") {
  CHECK_EQ(
      appmanifest_installdir(
          R"("AppState" { "appid" "311210" "installdir" "Call of Duty Black Ops III" })")
          .value_or(""),
      "Call of Duty Black Ops III");
  CHECK(!appmanifest_installdir(R"("AppState" { "installdir" "..\\..\\x" })")
             .has_value());
  CHECK(!appmanifest_installdir(R"("AppState" { })").has_value());
}

TEST_CASE("detection: saved path first, then every Steam library") {
  const std::filesystem::path steam = "C:\\Steam";
  const std::filesystem::path library = "F:\\Lib";
  std::map<std::string, std::string> files = {
      {(steam / "steamapps" / "libraryfolders.vdf").string(),
       R"("libraryfolders" { "0" { "path" "C:\\Steam" } "1" { "path" "F:\\Lib" } })"},
      {(library / "steamapps" / "appmanifest_311210.acf").string(),
       R"("AppState" { "installdir" "Call of Duty Black Ops III" })"}};
  const std::filesystem::path game =
      library / "steamapps" / "common" / "Call of Duty Black Ops III";
  std::set<std::string> existing = {(game / "BlackOps3.exe").string()};
  const detection_io io{
      [&](const std::filesystem::path &p) -> std::optional<std::string> {
        const auto it = files.find(p.string());
        return it == files.end() ? std::nullopt : std::optional(it->second);
      },
      [&](const std::filesystem::path &p) {
        return existing.contains(p.string());
      }};

  const detection found = detect_game(std::nullopt, {steam}, io);
  REQUIRE(found.game_dir.has_value());
  CHECK_EQ(found.game_dir->string(), game.string());

  existing.insert("G:\\Saved\\BlackOps3.exe");
  CHECK_EQ(detect_game(std::filesystem::path("G:\\Saved"), {steam}, io)
               .game_dir->string(),
           "G:\\Saved");

  existing.clear();
  const detection missing = detect_game(std::nullopt, {steam}, io);
  CHECK(!missing.game_dir.has_value());
  CHECK(!missing.tried.empty());
}

TEST_CASE("safe mode: a crash with mods turns the next launch safe") {
  launch_journal journal;
  journal.plan_id = "sha256:abc";
  journal.stack = {"a.mod", "b.mod"};
  journal.phase = launch_phase::loading_map;
  const auto again = parse_journal(journal_to_json(journal));
  REQUIRE(again.has_value());
  CHECK_EQ(again->stack, journal.stack);
  CHECK(again->phase == launch_phase::loading_map);

  const safe_mode_decision crash = decide_safe_mode(journal);
  CHECK(crash.safe_mode);
  CHECK_EQ(crash.crashed_plan_id, "sha256:abc");
  CHECK(crash.message.find("while loading a map") != std::string::npos);

  journal.phase = launch_phase::clean_exit;
  CHECK(!decide_safe_mode(journal).safe_mode);
  journal.phase = launch_phase::in_game;
  journal.safe_mode = true;
  CHECK(!decide_safe_mode(journal).safe_mode);
  journal.safe_mode = false;
  journal.stack.clear();
  CHECK(!decide_safe_mode(journal).safe_mode);
  CHECK(!decide_safe_mode(std::nullopt).safe_mode);
  CHECK(!parse_journal(R"({"phase":"nonsense"})").has_value());
}

TEST_CASE("safe mode: finding the culprit halves the stack") {
  culprit_search search({"a", "b", "c", "d"});
  const auto crashes = [](const std::vector<std::string> &mods) {
    return std::ranges::find(mods, "c") != mods.end();
  };
  while (!search.done()) {
    search.record(crashes(search.next_test()));
  }
  REQUIRE(search.culprit().has_value());
  CHECK_EQ(*search.culprit(), "c");
  CHECK_EQ(search.launches(), 3u);

  culprit_search pair({"a", "b", "c", "d"});
  const auto needs_a_and_d = [](const std::vector<std::string> &mods) {
    return std::ranges::find(mods, "a") != mods.end() &&
           std::ranges::find(mods, "d") != mods.end();
  };
  while (!pair.done()) {
    pair.record(needs_a_and_d(pair.next_test()));
  }
  CHECK(pair.needs_combination());
  CHECK(!pair.culprit().has_value());
}
