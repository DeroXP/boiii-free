// Signature registry, install detection and safe mode, on synthetic data.

#include "harness.hpp"

#include <algorithm>
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
  // The notice is one fixed sentence per phase, so the launcher can look it
  // up as it is in a translation (no mod count or other run-time parts).
  launch_journal bigger = journal;
  bigger.stack = {"a.mod", "b.mod", "c.mod"};
  CHECK_EQ(decide_safe_mode(bigger).message, crash.message);
  CHECK(std::ranges::none_of(crash.message,
                             [](char c) { return c >= '0' && c <= '9'; }));
  CHECK((crash.message_parts == std::vector<std::string>{crash.message}));
  // With an experimental setting: the phase's sentence for settings, then
  // one fixed sentence naming it, each translated on its own.
  bigger.db_pool = true;
  const safe_mode_decision with_pool = decide_safe_mode(bigger);
  REQUIRE_EQ(with_pool.message_parts.size(), std::size_t{2});
  CHECK(with_pool.message_parts[0].find("while loading a map") !=
        std::string::npos);
  CHECK(with_pool.message_parts[0].find("experimental settings") !=
        std::string::npos);
  CHECK_EQ(with_pool.message_parts[1],
           "Larger zone memory was on (Settings > Launch).");
  CHECK_EQ(with_pool.message,
           with_pool.message_parts[0] + " " + with_pool.message_parts[1]);
  CHECK_EQ(
      safe_mode_text(with_pool,
                     [](const std::string &part) { return "[" + part + "]"; }),
      "[" + with_pool.message_parts[0] + "] [" + with_pool.message_parts[1] +
          "]");
  bigger.stack = {"a.mod"};
  CHECK_EQ(decide_safe_mode(bigger).message, with_pool.message);

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

TEST_CASE("journal: mode and launch id round trip; older journals parse") {
  launch_journal journal;
  journal.plan_id = "sha256:abc";
  journal.stack = {"a.mod"};
  journal.phase = launch_phase::in_game;
  journal.started = "2026-11-05T01:02:03.456Z";
  journal.mode = "mp";
  journal.launch_id = new_launch_id();
  const std::string text = journal_to_json(journal);
  const auto again = parse_journal(text);
  REQUIRE(again.has_value());
  CHECK_EQ(again->mode, std::string("mp"));
  CHECK_EQ(again->launch_id, journal.launch_id);
  CHECK_EQ(journal_to_json(*again), text); // canonical: same bytes again

  // Written before mode and launch_id existed.
  const auto old = parse_journal(
      R"({"schema":1,"plan_id":"sha256:abc","stack":["a.mod"],"phase":"in_game",)"
      R"("started":"2026-11-01T00:00:00.000Z","updated":"","safe_mode":false})");
  REQUIRE(old.has_value());
  CHECK(old->mode.empty());
  CHECK(old->launch_id.empty());
  CHECK(!old->db_pool);
  CHECK(old->phase == launch_phase::in_game);
  const safe_mode_decision crash = decide_safe_mode(old);
  CHECK(crash.safe_mode);
  CHECK(crash.crashed_mode.empty());
  CHECK_EQ(crash.crashed_started, std::string("2026-11-01T00:00:00.000Z"));

  const safe_mode_decision with_mode = decide_safe_mode(journal);
  CHECK_EQ(with_mode.crashed_mode, std::string("mp"));
  CHECK_EQ(with_mode.crashed_started, journal.started);
}

TEST_CASE("journal: launch ids are fresh hex") {
  const std::string a = new_launch_id();
  const std::string b = new_launch_id();
  CHECK_EQ(a.size(), 32u);
  CHECK(std::ranges::all_of(a, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  }));
  CHECK(a != b);
}

TEST_CASE("journal: only the launch the launcher recorded takes it over") {
  launch_journal journal;
  journal.stack = {"a.mod", "b.mod"};
  journal.launch_id = "0123456789abcdef0123456789abcdef";
  CHECK(journal_claimable(journal, journal.launch_id));
  CHECK(!journal_claimable(journal, "fedcba9876543210fedcba9876543210"));
  CHECK(!journal_claimable(journal, "")); // no XERCES_LAUNCH_ID
  CHECK(!journal_claimable(std::nullopt, journal.launch_id));
  for (const launch_phase phase :
       {launch_phase::symbols, launch_phase::frontend, launch_phase::in_game,
        launch_phase::clean_exit}) {
    launch_journal later = journal;
    later.phase = phase;
    CHECK(!journal_claimable(later, journal.launch_id));
  }
  launch_journal claimed = journal; // the game clears the id on take-over
  claimed.launch_id.clear();
  CHECK(!journal_claimable(claimed, journal.launch_id));
  CHECK(!journal_claimable(claimed, ""));
}

TEST_CASE("journal: exit code 0 is clean only before the game took over") {
  launch_journal journal;
  journal.stack = {"a.mod"};
  journal.launch_id = new_launch_id();
  // Quit before Xerces' code ran: nothing else will ever say so.
  CHECK(exit_counts_as_clean(journal, 0));
  CHECK(!exit_counts_as_clean(journal, 0xC0000005u));

  // Taken over: the journal decides, whatever the exit code.
  journal.launch_id.clear();
  CHECK(!exit_counts_as_clean(journal, 0)); // fatal error during engine start
  journal.phase = launch_phase::in_game;
  CHECK(!exit_counts_as_clean(journal, 0));
  journal.phase = launch_phase::clean_exit;
  CHECK(!journal_crashed(journal));
  journal.phase = launch_phase::frontend;
  CHECK(journal_crashed(journal));
  CHECK(journal_crashed(std::nullopt));
}

TEST_CASE("journal: Restart game after a crash keeps the crash") {
  // The launcher records the launch and passes the id.
  launch_journal journal;
  journal.plan_id = "sha256:abc";
  journal.stack = {"a.mod", "b.mod"};
  journal.mode = "zm";
  journal.launch_id = new_launch_id();
  const std::string passed = journal.launch_id;

  // The game takes it over and crashes in a match.
  REQUIRE(journal_claimable(journal, passed));
  journal.launch_id.clear();
  journal.phase = launch_phase::in_game;

  // "Restart game": the copy inherits the environment but not the id, which
  // the first game removed; restart_game ends the old process with code 0.
  CHECK(!journal_claimable(journal, ""));
  CHECK(!exit_counts_as_clean(journal, 0));
  const safe_mode_decision next = decide_safe_mode(journal);
  CHECK(next.safe_mode);
  CHECK(next.crashed_in == launch_phase::in_game);
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
  CHECK_EQ(search.launches(), 3u); // [c, d] crash, [d] fine, [c] crashes

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

TEST_CASE(
    "safe mode: a crash larger zone memory causes is not blamed on a mod") {
  // Larger zone memory on with three mods, and the detours crash the game
  // before the menu whatever the mods load.
  launch_journal journal;
  journal.plan_id = "sha256:abc";
  journal.stack = {"a", "b", "c"};
  journal.mode = "zm";
  journal.db_pool = true;
  const std::string text = journal_to_json(journal);
  const auto back = parse_journal(text);
  REQUIRE(back.has_value());
  CHECK(back->db_pool);
  CHECK_EQ(journal_to_json(*back), text);
  const safe_mode_decision decision = decide_safe_mode(back);
  REQUIRE(decision.safe_mode);
  CHECK(decision.crashed_with_db_pool);
  CHECK(decision.message.find("Larger zone memory was on") !=
        std::string::npos);
  CHECK(culprit_search_possible(decision));

  // Safe mode ran fine. Every test runs without the setting too
  // (launch_inputs::crash_followup), so the first one, the whole stack
  // without it, runs fine as well: the setting is the culprit, no mod is.
  const auto crashes = [](const std::vector<std::string> &, bool db_pool) {
    return db_pool;
  };
  culprit_search search(decision.crashed_stack, decision.crashed_with_db_pool);
  CHECK(search.testing_settings());
  CHECK(search.next_test() == decision.crashed_stack);
  while (!search.done()) {
    search.record(crashes(search.next_test(), false));
  }
  CHECK(search.settings_culprit());
  CHECK(!search.culprit().has_value());
  CHECK(!search.needs_combination());
  CHECK_EQ(search.launches(), 1u);
  CHECK(search.next_test().empty());

  // Without the flag the decision and its message stay as before.
  journal.db_pool = false;
  const safe_mode_decision plain = decide_safe_mode(journal);
  CHECK(!plain.crashed_with_db_pool);
  CHECK(plain.message.find("zone memory") == std::string::npos);
}

TEST_CASE("safe mode: a crash the precise FPS limit causes is not blamed on "
          "a mod") {
  // The precise frame limiter on with three mods, and it hangs or crashes
  // the game whatever the mods load.
  launch_journal journal;
  journal.plan_id = "sha256:abc";
  journal.stack = {"a", "b", "c"};
  journal.mode = "zm";
  journal.precise_limiter = true;
  const std::string text = journal_to_json(journal);
  const auto back = parse_journal(text);
  REQUIRE(back.has_value());
  CHECK(back->precise_limiter);
  CHECK(!back->db_pool);
  CHECK_EQ(journal_to_json(*back), text);
  const safe_mode_decision decision = decide_safe_mode(back);
  REQUIRE(decision.safe_mode);
  CHECK(decision.crashed_with_precise_limiter);
  CHECK(!decision.crashed_with_db_pool);
  CHECK(decision.message.find("precise FPS limit") != std::string::npos);
  CHECK(decision.message.find("Settings > Display") != std::string::npos);
  CHECK(decision.message.find("zone memory") == std::string::npos);
  CHECK((crashed_settings(decision) ==
         std::vector<std::string>{"xerces-precise-limiter"}));
  CHECK(culprit_search_possible(decision));

  // Every test runs without the setting, so the first one, the whole stack,
  // runs fine: the setting is the culprit, no mod is. Without that step the
  // halves would both run fine and the mods would be called a combination.
  const auto crashes = [](const std::vector<std::string> &, bool limiter) {
    return limiter;
  };
  culprit_search search(decision.crashed_stack,
                        !crashed_settings(decision).empty());
  CHECK(search.testing_settings());
  CHECK(search.next_test() == decision.crashed_stack);
  while (!search.done()) {
    search.record(crashes(search.next_test(), false));
  }
  CHECK(search.settings_culprit());
  CHECK(!search.culprit().has_value());
  CHECK(!search.needs_combination());
  CHECK_EQ(search.launches(), 1u);

  // One mod and the setting are two suspects.
  safe_mode_decision one = decision;
  one.crashed_stack = {"a"};
  CHECK(culprit_search_possible(one));
  one.crashed_with_precise_limiter = false;
  CHECK(!culprit_search_possible(one));

  // Both settings: one step leaves both out, and the message names both.
  journal.db_pool = true;
  const safe_mode_decision both = decide_safe_mode(journal);
  CHECK(both.crashed_with_db_pool);
  CHECK(both.crashed_with_precise_limiter);
  CHECK((crashed_settings(both) ==
         std::vector<std::string>{"xerces-db-pool", "xerces-precise-limiter"}));
  CHECK((both.message_parts ==
         std::vector<std::string>{
             both.message_parts[0],
             "Larger zone memory was on (Settings > Launch).",
             "The precise FPS limit was on (Settings > Display)."}));

  // A journal from before the field reads as without it.
  const auto older =
      parse_journal(R"({"schema":1,"plan_id":"sha256:abc","stack":["a"],)"
                    R"("phase":"in_game","db_pool":false})");
  REQUIRE(older.has_value());
  CHECK(!older->precise_limiter);
}

TEST_CASE("safe mode: a crash flip model causes is not blamed on a mod") {
  // Flip-model presentation on with two mods, and the picture goes black or
  // the game crashes whatever the mods load.
  launch_journal journal;
  journal.plan_id = "sha256:abc";
  journal.stack = {"a", "b"};
  journal.mode = "zm";
  journal.flip_model = true;
  const std::string text = journal_to_json(journal);
  CHECK(text.find(R"("flip_model":true)") != std::string::npos);
  const auto back = parse_journal(text);
  REQUIRE(back.has_value());
  CHECK(back->flip_model);
  CHECK(!back->db_pool);
  CHECK(!back->precise_limiter);
  CHECK_EQ(journal_to_json(*back), text);
  const safe_mode_decision decision = decide_safe_mode(back);
  REQUIRE(decision.safe_mode);
  CHECK(decision.crashed_with_flip_model);
  CHECK(!decision.crashed_with_db_pool);
  CHECK(decision.crashed_with_settings());
  CHECK((crashed_settings(decision) ==
         std::vector<std::string>{"xerces-flip-model"}));
  REQUIRE_EQ(decision.message_parts.size(), std::size_t{2});
  CHECK_EQ(decision.message_parts[1],
           "Flip-model presentation was on (Settings > Display).");
  CHECK(decision.message.find("zone memory") == std::string::npos);

  // Safe mode and every test run without it, so the first test, the whole
  // stack without it, runs fine: the setting is the culprit, no mod is.
  culprit_search search(decision.crashed_stack,
                        decision.crashed_with_settings());
  CHECK(search.testing_settings());
  CHECK(search.next_test() == decision.crashed_stack);
  search.record(false);
  CHECK(search.done());
  CHECK(search.settings_culprit());
  CHECK(!search.culprit().has_value());
  CHECK_EQ(search.launches(), 1u);

  // One mod and the setting are two suspects.
  safe_mode_decision one;
  one.safe_mode = true;
  one.crashed_stack = {"a"};
  CHECK(!culprit_search_possible(one));
  one.crashed_with_flip_model = true;
  CHECK(culprit_search_possible(one));

  // With larger zone memory too: one suspect, one first test, both named.
  journal.db_pool = true;
  const safe_mode_decision both = decide_safe_mode(journal);
  CHECK(both.crashed_with_db_pool);
  CHECK(both.crashed_with_flip_model);
  CHECK((crashed_settings(both) ==
         std::vector<std::string>{"xerces-db-pool", "xerces-flip-model"}));
  CHECK((both.message_parts ==
         std::vector<std::string>{
             both.message_parts[0],
             "Larger zone memory was on (Settings > Launch).",
             "Flip-model presentation was on (Settings > Display)."}));
  culprit_search two(both.crashed_stack, both.crashed_with_settings());
  CHECK(two.next_test() == both.crashed_stack);
  two.record(true); // crashed without them too: the mods are bisected
  CHECK(two.settings_step() == settings_suspect::cleared);
  CHECK((two.next_test() == std::vector<std::string>{"b"})); // lower part

  // A journal from before flip model was recorded reads as off.
  const auto older = parse_journal(
      R"({"schema":1,"plan_id":"p","stack":["a"],"phase":"in_game",)"
      R"("started":"","updated":"","safe_mode":false,"mode":"zm",)"
      R"("launch_id":"","db_pool":false})");
  REQUIRE(older.has_value());
  CHECK(!older->flip_model);
}

TEST_CASE("safe mode: three experimental settings are one suspect") {
  // Larger zone memory, the precise FPS limit and flip-model presentation all
  // on, three mods, a crash in a match.
  launch_journal journal;
  journal.plan_id = "sha256:abc";
  journal.stack = {"a", "b", "c"};
  journal.mode = "zm";
  journal.phase = launch_phase::in_game;
  journal.db_pool = journal.precise_limiter = journal.flip_model = true;
  const auto back = parse_journal(journal_to_json(journal));
  REQUIRE(back.has_value());
  CHECK(back->db_pool);
  CHECK(back->precise_limiter);
  CHECK(back->flip_model);
  const safe_mode_decision decision = decide_safe_mode(back);
  REQUIRE(decision.safe_mode);
  const std::vector<std::string> settings = crashed_settings(decision);
  CHECK((settings == std::vector<std::string>{"xerces-db-pool",
                                              "xerces-precise-limiter",
                                              "xerces-flip-model"}));
  CHECK(settings == suspect_settings());
  // The phase's sentence for settings, then one per setting, in that order;
  // no part depends on the mods.
  CHECK((decision.message_parts ==
         std::vector<std::string>{
             "The last launch with mods and experimental settings crashed in "
             "a match. This launch starts with mods and those settings off. "
             "They may be the cause: find the culprit runs the same mods "
             "without them first. Or retry with mods, report the crash, or "
             "turn them off in Settings.",
             "Larger zone memory was on (Settings > Launch).",
             "The precise FPS limit was on (Settings > Display).",
             "Flip-model presentation was on (Settings > Display)."}));
  for (const auto &flag : settings) {
    CHECK(std::string(setting_name(flag)).size() > 0);
  }
  CHECK(std::string(setting_name("xerces-other")).empty());

  // One mod and the three settings: still something to tell apart, and the
  // three count once.
  safe_mode_decision one = decision;
  one.crashed_stack = {"a"};
  CHECK(culprit_search_possible(one));

  // The settings caused it: one test, the whole stack without all three.
  culprit_search found(decision.crashed_stack,
                       decision.crashed_with_settings());
  CHECK(found.next_test() == decision.crashed_stack);
  found.record(false);
  CHECK(found.settings_culprit());
  CHECK(!found.culprit().has_value());
  CHECK_EQ(found.launches(), 1u);

  // A mod caused it ("c", the lowest): the settings step first, then
  // fix2-redo's search, lower part first.
  culprit_search mods(decision.crashed_stack, decision.crashed_with_settings());
  std::vector<std::vector<std::string>> tested;
  while (!mods.done() && tested.size() < 8) {
    tested.push_back(mods.next_test());
    mods.record(std::ranges::find(tested.back(), "c") != tested.back().end());
  }
  REQUIRE(tested.size() >= 2);
  CHECK(tested[0] == decision.crashed_stack);
  CHECK((tested[1] == std::vector<std::string>{"b", "c"}));
  CHECK(mods.settings_step() == settings_suspect::cleared);
  REQUIRE(mods.culprit().has_value());
  CHECK_EQ(*mods.culprit(), "c");
  CHECK(mods.ran_fine() == false);

  // culprit.json keeps the three and the step.
  culprit_progress progress;
  progress.mode = "zm";
  progress.crashed_stack = decision.crashed_stack;
  progress.settings = settings;
  progress.search = culprit_search(decision.crashed_stack, true);
  const std::string saved = culprit_progress_to_json(progress);
  CHECK(saved.find(R"("db_pool":"untested")") != std::string::npos);
  const auto again = parse_culprit_progress(saved);
  REQUIRE(again.has_value());
  CHECK(again->settings == settings);
  CHECK(again->has_setting("xerces-flip-model"));
  CHECK(!again->has_setting("xerces-other"));
  CHECK(again->search.testing_settings());

  // A development build of flip model kept the step under "settings" as a
  // string: not a search this version continues.
  std::string string_step = saved;
  const std::size_t at = string_step.find(R"("settings":[)");
  REQUIRE(at != std::string::npos);
  const std::size_t end = string_step.find(']', at);
  string_step.replace(at, end - at + 1, R"("settings":"untested")");
  CHECK(!parse_culprit_progress(string_step).has_value());
}

TEST_CASE(
    "safe mode: a crash that needs no larger zone memory bisects the mods") {
  culprit_search search({"a", "b", "c"}, true);
  const auto crashes = [](const std::vector<std::string> &mods) {
    return std::ranges::find(mods, "c") != mods.end();
  };
  std::vector<std::vector<std::string>> tested;
  while (!search.done()) {
    tested.push_back(search.next_test());
    search.record(crashes(tested.back()));
  }
  CHECK(search.settings_step() == settings_suspect::cleared);
  REQUIRE(search.culprit().has_value());
  CHECK_EQ(*search.culprit(), "c");
  REQUIRE(!tested.empty());
  CHECK((tested.front() == std::vector<std::string>{"a", "b", "c"}));
  CHECK_EQ(search.launches(), tested.size());

  // One mod and the setting are two suspects.
  culprit_search one({"a"}, true);
  CHECK(!one.done());
  CHECK((one.next_test() == std::vector<std::string>{"a"}));
  one.record(true); // the mod crashes without the setting too
  CHECK(one.done());
  REQUIRE(one.culprit().has_value());
  CHECK_EQ(*one.culprit(), "a");
  culprit_search fine({"a"}, true);
  fine.record(false);
  CHECK(fine.done());
  CHECK(fine.settings_culprit());
  CHECK(!fine.culprit().has_value());

  safe_mode_decision decision;
  decision.safe_mode = true;
  decision.crashed_stack = {"a"};
  CHECK(!culprit_search_possible(decision)); // one mod: nothing to tell apart
  decision.crashed_with_db_pool = true;
  CHECK(culprit_search_possible(decision));
  decision.safe_mode = false;
  CHECK(!culprit_search_possible(decision));
  decision.safe_mode = true;
  decision.crashed_with_db_pool = false;
  decision.crashed_stack = {"a", "b"};
  CHECK(culprit_search_possible(decision));
}
