// "Find the culprit" (docs/DESIGN.md 5.6) with the real planner and the
// launch the launcher makes for each test: the profile with next_test() as
// its mods and its map kept (launch_inputs::mods, crash_followup), the
// result recorded with every mod that test's plan loaded. The game crashes
// whenever the bad mod loads; when the bad one is the profile's map, in every
// test.

#include "harness.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <xerces_core/journal.hpp>
#include <xerces_core/launcher_launch.hpp>
#include <xerces_core/manifest.hpp>
#include <xerces_core/planner.hpp>

using namespace xerces_core;

namespace {
// A mod (or map) with a manifest; needs: the ids it depends on.
installed_content content_with(const std::string &id,
                               const std::vector<std::string> &needs = {},
                               const std::string &kind = "mod") {
  std::string deps;
  for (const auto &need : needs) {
    deps += std::string(deps.empty() ? "" : ",") + R"({"id":")" + need +
            R"(","version":"^1.0.0"})";
  }
  auto parsed = parse_manifest(R"({"schema":1,"id":")" + id + R"(","name":")" +
                               id + R"(","version":"1.0.0","kind":")" + kind +
                               R"(","dependencies":[)" + deps + "]}");
  installed_content content;
  content.info = parsed.value;
  content.folder = id;
  content.content_hash = "sha256:" + std::string(64, '0');
  return content;
}

std::vector<std::string> stack_ids(const plan &value) {
  std::vector<std::string> out;
  for (const auto &entry : value.stack) {
    out.push_back(entry.id);
  }
  return out;
}

bool holds(const std::vector<std::string> &ids, const std::string &id) {
  return std::ranges::find(ids, id) != ids.end();
}

launcher_config profile(std::vector<std::string> mods,
                        std::optional<std::string> map = std::nullopt) {
  launcher_config config;
  mode_profile &zm = profile_for(config, "zm");
  zm.mods = std::move(mods);
  zm.map = std::move(map);
  return config;
}

struct search_run {
  culprit_search search{{}};
  std::vector<std::string> crashed_stack;
  std::size_t tests = 0;
};

// The search from start_culprit to its end, as services runs it. With
// know_map false it starts without the map's dependencies (a culprit.json
// saved without "always", or a map changed during the search).
search_run run_search(const std::vector<installed_content> &catalog,
                      const launcher_config &config, const std::string &bad,
                      bool know_map = true) {
  search_run run;
  const game_state game; // preview_launch needs no game folder
  launch_inputs in;
  in.config = &config;
  in.game = &game;
  in.catalog = &catalog;
  in.mode = "zm";
  const launch_outcome crashed = preview_launch(in);
  REQUIRE(crashed.resolved.has_value());
  REQUIRE(crashed.resolved->ok());
  run.crashed_stack = stack_ids(*crashed.resolved);
  run.search = culprit_search(
      run.crashed_stack, false,
      know_map ? culprit_always_loaded(catalog, config, "zm", std::nullopt,
                                       run.crashed_stack)
               : std::vector<std::string>{});
  const auto map = config.profiles.at("zm").map;
  while (!run.search.done() && run.tests < 32) {
    const std::vector<std::string> mods = run.search.next_test();
    in.mods = &mods;
    in.crash_followup = true;
    const launch_outcome test = preview_launch(in);
    ++run.tests;
    REQUIRE(test.resolved.has_value());
    // The test launch is not refused: it keeps every mod it asked for, what
    // they need, and the map with what the map needs.
    CHECK(test.resolved->ok());
    CHECK(test.diags.items().empty());
    const std::vector<std::string> loaded = stack_ids(*test.resolved);
    for (const auto &id : mods) {
      CHECK(holds(loaded, id));
    }
    CHECK_EQ(test.resolved->map.has_value(), map.has_value());
    run.search.record(bad == map || holds(loaded, bad), loaded);
  }
  CHECK(run.search.done());
  return run;
}
} // namespace

TEST_CASE("culprit: a crashing dependency is found, not the mod needing it") {
  // A and B both need D, which crashes the game. Each test loads D with A or
  // B; the search that tested the upper half first saw [A] (with D) crash,
  // blamed A and turned it off, and the next launch with B crashed again.
  const std::vector<installed_content> catalog = {
      content_with("a.mod", {"d.mod"}), content_with("b.mod", {"d.mod"}),
      content_with("c.mod"), content_with("d.mod")};
  const launcher_config config = profile({"a.mod", "b.mod", "c.mod"});
  const plan_request request = profile_request(config, "zm", std::nullopt);

  const search_run dependency = run_search(catalog, config, "d.mod");
  CHECK_EQ(dependency.crashed_stack.back(), std::string("d.mod"));
  REQUIRE(dependency.search.culprit().has_value());
  CHECK_EQ(*dependency.search.culprit(), std::string("d.mod"));
  // D is not one the player enabled: what turns it off is what loads it.
  CHECK((culprit_switches(catalog, request, "d.mod") ==
         std::vector<std::string>{"a.mod", "b.mod"}));

  // Every other mod crashing alone is found too, and is enough to turn off.
  for (const std::string bad : {"a.mod", "b.mod", "c.mod"}) {
    const search_run run = run_search(catalog, config, bad);
    REQUIRE(run.search.culprit().has_value());
    CHECK_EQ(*run.search.culprit(), bad);
    CHECK((culprit_switches(catalog, request, bad) ==
           std::vector<std::string>{bad}));
  }

  // D enabled by hand as well: A still pulls it in, so both go.
  plan_request both = request;
  both.mods = {"a.mod", "d.mod"};
  CHECK((culprit_switches(catalog, both, "d.mod") ==
         std::vector<std::string>{"a.mod", "d.mod"}));
}

TEST_CASE("culprit: a map's dependency loads in every test, and is told "
          "apart") {
  // The profile's map needs m.dep, so every test loads it: a test cannot
  // leave it out, and a crash with it loaded says nothing about it. The
  // first fix of the dependency case ended this search with no culprit when
  // x.mod crashed ("cannot be tested apart"); m.dep must be tested alone,
  // with the map, instead.
  const std::vector<installed_content> catalog = {
      content_with("x.mod"), content_with("m.dep"),
      content_with("the.map", {"m.dep"}, "map")};
  const launcher_config config = profile({"x.mod"}, "the.map");
  const plan_request request = profile_request(config, "zm", std::nullopt);
  for (const bool know_map : {true, false}) {
    const search_run mod = run_search(catalog, config, "x.mod", know_map);
    REQUIRE_EQ(mod.crashed_stack.size(), std::size_t(2));
    REQUIRE(mod.search.culprit().has_value());
    CHECK_EQ(*mod.search.culprit(), std::string("x.mod"));
    CHECK(!mod.search.needs_combination());
    // Known from the start, the map's dependency costs no extra launch.
    CHECK_EQ(mod.tests, know_map ? std::size_t(2) : std::size_t(3));

    const search_run dep = run_search(catalog, config, "m.dep", know_map);
    REQUIRE(dep.search.culprit().has_value());
    CHECK_EQ(*dep.search.culprit(), std::string("m.dep"));
  }
  CHECK((culprit_always_loaded(catalog, config, "zm", std::nullopt,
                               {"x.mod", "m.dep"}) ==
         std::vector<std::string>{"m.dep"}));
  // Only the map needs it: no enabled mod turns it off.
  CHECK(culprit_switches(catalog, request, "m.dep").empty());
  CHECK((culprit_switches(catalog, request, "x.mod") ==
         std::vector<std::string>{"x.mod"}));
  const culprit_remedy dep = culprit_remedy_for(catalog, request, "m.dep");
  CHECK(dep.mods.empty());
  CHECK(dep.map);
  CHECK(!dep.mods_enough());
  const culprit_remedy mod = culprit_remedy_for(catalog, request, "x.mod");
  CHECK((mod.mods == std::vector<std::string>{"x.mod"}));
  CHECK(!mod.map);
  CHECK(mod.mods_enough());
}

TEST_CASE("culprit: a dependency the map and an enabled mod both need is not "
          "turned off through the mod") {
  // a.mod and the map both need d.mod, which crashes the game. Home offered
  // to turn a.mod off, and d.mod still loaded with the map: the next launch
  // crashed again.
  const std::vector<installed_content> catalog = {
      content_with("a.mod", {"d.mod"}), content_with("c.mod"),
      content_with("d.mod"), content_with("the.map", {"d.mod"}, "map")};
  const launcher_config config = profile({"a.mod", "c.mod"}, "the.map");
  const plan_request request = profile_request(config, "zm", std::nullopt);
  for (const bool know_map : {true, false}) {
    const search_run run = run_search(catalog, config, "d.mod", know_map);
    REQUIRE(run.search.culprit().has_value());
    CHECK_EQ(*run.search.culprit(), std::string("d.mod"));
  }
  const search_run run = run_search(catalog, config, "d.mod");
  // It was only ever tested with the map that needs it.
  CHECK(holds(run.search.always(), "d.mod"));
  // a.mod loads it, and so does the map: turning a.mod off is not enough,
  // so Home offers no button and says to pick another map.
  const culprit_remedy fix = culprit_remedy_for(catalog, request, "d.mod");
  CHECK((fix.mods == std::vector<std::string>{"a.mod"}));
  CHECK(fix.map);
  CHECK(!fix.mods_enough());
  // Without that map, turning a.mod off is enough.
  plan_request no_map = request;
  no_map.map.reset();
  const culprit_remedy without = culprit_remedy_for(catalog, no_map, "d.mod");
  CHECK((without.mods == std::vector<std::string>{"a.mod"}));
  CHECK(!without.map);
  CHECK(without.mods_enough());
  // The mods the map does not need are found and turned off as usual.
  for (const std::string bad : {"a.mod", "c.mod"}) {
    const search_run mod = run_search(catalog, config, bad);
    REQUIRE(mod.search.culprit().has_value());
    CHECK_EQ(*mod.search.culprit(), bad);
    CHECK(mod.search.ran_fine());
    const culprit_remedy off = culprit_remedy_for(catalog, request, bad);
    CHECK((off.mods == std::vector<std::string>{bad}));
    CHECK(!off.map);
    CHECK(off.mods_enough());
  }
}

TEST_CASE("culprit: with a map, a search where every test crashed says so") {
  // The map is never a suspect, but every test keeps it. When the map itself
  // crashes, every test crashes and the search ends on the lowest mod: the
  // same result as that mod crashing, since no test ran without either.
  // ran_fine() tells Home to name the map as a possible cause.
  const std::vector<installed_content> catalog = {
      content_with("a.mod"), content_with("b.mod"),
      content_with("the.map", {}, "map")};
  const launcher_config config = profile({"a.mod", "b.mod"}, "the.map");
  const search_run map = run_search(catalog, config, "the.map");
  REQUIRE(map.search.culprit().has_value());
  CHECK_EQ(*map.search.culprit(), std::string("b.mod"));
  CHECK(!map.search.ran_fine());
  const search_run lowest = run_search(catalog, config, "b.mod");
  CHECK_EQ(lowest.search.culprit().value_or("none"), std::string("b.mod"));
  CHECK(!lowest.search.ran_fine());
  // When a test ran fine, the map is cleared with it.
  const search_run upper = run_search(catalog, config, "a.mod");
  CHECK_EQ(upper.search.culprit().value_or("none"), std::string("a.mod"));
  CHECK(upper.search.ran_fine());
}

TEST_CASE("culprit: every mod a test can leave out is found, in a stack with "
          "a map") {
  // Dependencies of mods and of the map in one stack: whichever mod crashes
  // alone, the search names it, and no test drops what the map needs. The
  // map needs m.dep, which needs m.base, so those two load in every test:
  // when one of them crashes, the search says they cannot be told apart.
  const std::vector<installed_content> catalog = {
      content_with("a.mod", {"d.mod"}),
      content_with("b.mod"),
      content_with("c.mod", {"d.mod"}),
      content_with("d.mod"),
      content_with("e.mod"),
      content_with("m.dep", {"m.base"}),
      content_with("m.base"),
      content_with("the.map", {"m.dep"}, "map")};
  const launcher_config config =
      profile({"a.mod", "b.mod", "c.mod", "e.mod"}, "the.map");
  const std::vector<std::string> stack =
      stack_ids(resolve_profile(catalog, config, "zm", std::nullopt));
  REQUIRE_EQ(stack.size(), std::size_t(7));
  const std::vector<std::string> with_map = {"m.dep", "m.base"};
  for (const auto &bad : stack) {
    for (const bool know_map : {true, false}) {
      const search_run run = run_search(catalog, config, bad, know_map);
      if (holds(with_map, bad)) {
        CHECK_EQ(run.search.culprit().value_or("none"), std::string("none"));
        CHECK(run.search.inseparable());
        std::vector<std::string> left = run.search.suspects();
        std::ranges::sort(left);
        CHECK((left == std::vector<std::string>{"m.base", "m.dep"}));
        continue;
      }
      CHECK_EQ(run.search.culprit().value_or("none"), bad);
      // A halving search: never more than two tests per split.
      CHECK(run.tests <= 6);
    }
  }
}

TEST_CASE("culprit: suspects that only load with the map cannot be told "
          "apart") {
  // The map needs two mods. When the crash is in one of them, every test
  // loads both, so the search ends without blaming either.
  const std::vector<installed_content> catalog = {
      content_with("x.mod"), content_with("m.one"), content_with("m.two"),
      content_with("the.map", {"m.one", "m.two"}, "map")};
  const launcher_config config = profile({"x.mod"}, "the.map");
  const search_run run = run_search(catalog, config, "m.one");
  CHECK(!run.search.culprit().has_value());
  CHECK(run.search.inseparable());
  CHECK(!run.search.needs_combination());
  std::vector<std::string> left = run.search.suspects();
  std::ranges::sort(left);
  CHECK((left == std::vector<std::string>{"m.one", "m.two"}));
}

TEST_CASE("culprit: a search in progress keeps what it learned") {
  culprit_progress progress;
  progress.mode = "zm";
  progress.crashed_stack = {"x.mod", "a.mod", "m.dep", "b.mod"};
  progress.search = culprit_search(progress.crashed_stack, false, {"m.dep"});
  // The lower part of the mods a test can leave out; m.dep comes anyway.
  CHECK((progress.search.next_test() ==
         std::vector<std::string>{"a.mod", "b.mod"}));
  progress.search.record(false, {"a.mod", "b.mod", "m.dep"});
  CHECK((progress.search.cleared() ==
         std::vector<std::string>{"a.mod", "m.dep", "b.mod"}));
  progress.testing = progress.search.next_test();
  progress.loaded = {"x.mod", "m.dep"};
  CHECK((progress.testing == std::vector<std::string>{"x.mod"}));
  CHECK(progress.search.ran_fine());
  const auto back = parse_culprit_progress(culprit_progress_to_json(progress));
  REQUIRE(back.has_value());
  CHECK(back->testing == progress.testing);
  CHECK(back->loaded == progress.loaded);
  CHECK(back->search.cleared() == progress.search.cleared());
  CHECK((back->search.always() == std::vector<std::string>{"m.dep"}));
  CHECK(back->search.ran_fine());
  culprit_search search = back->search;
  search.record(true, back->loaded);
  REQUIRE(search.culprit().has_value());
  CHECK_EQ(*search.culprit(), std::string("x.mod"));
  CHECK_EQ(search.launches(), std::size_t(2));
}

TEST_CASE("culprit: with experimental settings on, the settings are tested "
          "first and a crashing dependency is still found") {
  // The three experimental settings on, a map that needs m.dep, and A and B
  // both needing d.mod. Every test launch leaves the settings out; the first
  // runs the whole crashed stack. When the settings are not the cause, the
  // search goes on lower part first, with the map's dependency kept aside.
  const std::vector<installed_content> catalog = {
      content_with("a.mod", {"d.mod"}),
      content_with("b.mod", {"d.mod"}),
      content_with("c.mod"),
      content_with("d.mod"),
      content_with("m.dep"),
      content_with("the.map", {"m.dep"}, "map")};
  launcher_config config = profile({"a.mod", "b.mod", "c.mod"}, "the.map");
  config.extra_flags = {"-xerces-db-pool", "-xerces-precise-limiter",
                        "-xerces-flip-model"};
  const game_state game;
  launch_inputs in;
  in.config = &config;
  in.game = &game;
  in.catalog = &catalog;
  in.mode = "zm";
  const launch_outcome crashed = preview_launch(in);
  REQUIRE(crashed.resolved.has_value());
  CHECK(crashed.db_pool);
  CHECK(crashed.precise_limiter);
  CHECK(crashed.flip_model);
  launch_journal journal;
  journal.stack = stack_ids(*crashed.resolved);
  journal.phase = launch_phase::in_game;
  journal.mode = "zm";
  journal.db_pool = crashed.db_pool;
  journal.precise_limiter = crashed.precise_limiter;
  journal.flip_model = crashed.flip_model;
  const safe_mode_decision decision = decide_safe_mode(journal);
  REQUIRE(decision.safe_mode);
  REQUIRE_EQ(crashed_settings(decision).size(), std::size_t(3));

  for (const std::string bad : {"settings", "d.mod", "c.mod", "m.dep"}) {
    culprit_search search(
        decision.crashed_stack, decision.crashed_with_settings(),
        culprit_always_loaded(catalog, config, "zm", std::nullopt,
                              decision.crashed_stack));
    std::size_t tests = 0;
    while (!search.done() && tests < 16) {
      const std::vector<std::string> mods = search.next_test();
      if (tests == 0) {
        CHECK(mods == decision.crashed_stack);
      }
      in.mods = &mods;
      in.crash_followup = true;
      const launch_outcome test = preview_launch(in);
      ++tests;
      REQUIRE(test.resolved.has_value());
      CHECK(!test.db_pool);
      CHECK(!test.precise_limiter);
      CHECK(!test.flip_model);
      for (const auto &flag : suspect_settings()) {
        CHECK(test.command_line.find(flag) == std::string::npos);
      }
      const std::vector<std::string> loaded = stack_ids(*test.resolved);
      // The settings crash only with the settings on, which no test has.
      search.record(bad != "settings" && holds(loaded, bad), loaded);
    }
    CHECK(search.done());
    if (bad == "settings") {
      CHECK(search.settings_culprit());
      CHECK(!search.culprit().has_value());
      CHECK_EQ(tests, std::size_t(1));
      continue;
    }
    CHECK(search.settings_step() == settings_suspect::cleared);
    CHECK_EQ(search.culprit().value_or("none"), bad);
  }
  in.mods = nullptr;
  in.crash_followup = false;
  // The settings themselves stay on for an ordinary launch.
  CHECK(preview_launch(in).flip_model);
}

TEST_CASE("launch: -xerces-record-frames is seen in the preview") {
  launcher_config config = profile({}, "");
  const game_state game;
  const std::vector<installed_content> catalog;
  launch_inputs in;
  in.config = &config;
  in.game = &game;
  in.catalog = &catalog;
  in.mode = "zm";
  in.modded = false;
  CHECK(!preview_launch(in).record_frames);
  config.extra_flags = {"-xerces-record-frames"};
  const launch_outcome with = preview_launch(in);
  CHECK(with.record_frames);
  CHECK(with.command_line.find("-xerces-record-frames") != std::string::npos);
}
