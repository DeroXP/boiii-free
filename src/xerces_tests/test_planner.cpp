#include "harness.hpp"

#include <algorithm>

#include <xerces_core/planner.hpp>

using namespace xerces_core;

namespace {
installed_content make(const std::string &fields, std::string folder = {},
                       std::vector<std::string> files = {}) {
  auto parsed = parse_manifest("{\"schema\":1," + fields + "}");
  for (const auto &d : parsed.diags.items()) {
    if (d.level == severity::error) {
      throw std::runtime_error("bad test manifest: " + d.path + " " +
                               d.message);
    }
  }
  installed_content content;
  content.info = parsed.value;
  content.folder = folder.empty() ? content.info.id : folder;
  content.files = std::move(files);
  content.content_hash = "sha256:" + std::string(64, '0');
  return content;
}

std::string mod(const std::string &id, const std::string &version,
                const std::string &extra = {}) {
  return "\"id\":\"" + id + "\",\"name\":\"" + id + "\",\"version\":\"" +
         version + "\"" + extra;
}

plan_request request_for(std::vector<std::string> mods) {
  plan_request request;
  request.mode = "zm";
  request.mods = std::move(mods);
  request.game_build = "0x6531394";
  request.xerces_version = *version::parse("1.0.0");
  return request;
}

std::vector<std::string> ids(const plan &value) {
  std::vector<std::string> out;
  for (const auto &entry : value.stack) {
    out.push_back(entry.id);
  }
  return out;
}

bool explains(const plan &value, const std::string &code,
              const std::string &subject, const std::string &object) {
  return std::ranges::any_of(value.explanations, [&](const explanation &e) {
    return e.code == code && e.subject == subject && e.object == object;
  });
}
} // namespace

TEST_CASE("planner: dependents rank above their dependencies") {
  const std::vector<installed_content> catalog = {
      make(mod("vela.perks", "2.1.0",
               R"(,"dependencies":[{"id":"orrin.hud","version":"^1.4.0"}])")),
      make(mod("orrin.hud", "1.4.2"))};
  const plan result = resolve_plan(catalog, request_for({"vela.perks"}));
  CHECK(result.ok());
  CHECK_EQ(ids(result), (std::vector<std::string>{"vela.perks", "orrin.hud"}));
  CHECK(result.stack[0].requested);
  CHECK(!result.stack[1].requested);
  CHECK(explains(result, "order.dependency", "vela.perks", "orrin.hud"));
  CHECK_EQ(result.fs_game(), "vela.perks");
  CHECK_EQ(result.fs_extramods(), "orrin.hud");
}

TEST_CASE("planner: the highest version that fits every range is chosen") {
  std::vector<installed_content> catalog = {
      make(mod("a.mod", "1.0.0",
               R"(,"dependencies":[{"id":"b.mod","version":"^1.0"}])")),
      make(mod("c.mod", "1.0.0",
               R"(,"dependencies":[{"id":"b.mod","version":">=1.3 <1.5"}])")),
      make(mod("b.mod", "1.2.0")), make(mod("b.mod", "1.5.0")),
      make(mod("b.mod", "2.0.0"))};
  plan only_a = resolve_plan(catalog, request_for({"a.mod"}));
  CHECK(only_a.ok());
  CHECK_EQ(only_a.stack[1].ver.to_string(), "1.5.0");

  plan both = resolve_plan(catalog, request_for({"a.mod", "c.mod"}));
  CHECK(both.diags.has("plan.dependency.unsatisfiable") ||
        both.diags.has("plan.mod.version"));
  CHECK(!both.fixes.empty());

  catalog.push_back(make(mod("b.mod", "1.4.0")));
  both = resolve_plan(catalog, request_for({"a.mod", "c.mod"}));
  CHECK(both.ok());
  const auto b = std::ranges::find_if(
      both.stack, [](const plan_entry &e) { return e.id == "b.mod"; });
  REQUIRE(b != both.stack.end());
  CHECK_EQ(b->ver.to_string(), "1.4.0");
}

TEST_CASE("planner: missing mods and dependencies become fix actions") {
  const std::vector<installed_content> catalog = {make(mod(
      "a.mod", "1.0.0", R"(,"dependencies":[{"id":"x.mod","version":"^2"}])"))};
  const plan result = resolve_plan(catalog, request_for({"a.mod", "y.mod"}));
  CHECK(result.diags.has("plan.dependency.missing"));
  CHECK(result.diags.has("plan.mod.missing"));
  CHECK(std::ranges::any_of(result.fixes, [](const fix_action &f) {
    return f.kind == "install" && f.id == "x.mod" && f.detail == "^2";
  }));
  CHECK(std::ranges::any_of(result.fixes, [](const fix_action &f) {
    return f.kind == "install" && f.id == "y.mod";
  }));
}

TEST_CASE("planner: declared conflicts block the plan unless allowed") {
  const std::vector<installed_content> catalog = {
      make(mod(
          "vela.perks", "1.0.0",
          R"(,"conflicts":[{"id":"pell.revive","reason":"Both rewrite perk purchase"}])")),
      make(mod("pell.revive", "1.0.0"))};
  plan_request request = request_for({"vela.perks", "pell.revive"});
  plan blocked = resolve_plan(catalog, request);
  CHECK(blocked.diags.has("plan.conflict.declared"));
  CHECK(std::ranges::any_of(blocked.fixes, [](const fix_action &f) {
    return f.kind == "allow_conflict";
  }));

  request.allowed_conflicts.insert({"pell.revive", "vela.perks"});
  plan allowed = resolve_plan(catalog, request);
  CHECK(allowed.ok());
  CHECK(allowed.diags.has("plan.conflict.allowed"));
}

TEST_CASE("planner: the user's order wins over manifest hints") {
  const std::vector<installed_content> catalog = {
      make(mod("a.mod", "1.0.0", R"(,"order":{"before":["b.mod"]})")),
      make(mod("b.mod", "1.0.0"))};
  const plan result = resolve_plan(catalog, request_for({"a.mod", "b.mod"}));
  CHECK(result.ok());
  CHECK_EQ(ids(result), (std::vector<std::string>{"a.mod", "b.mod"}));
  CHECK(result.diags.has("plan.order.hint_ignored"));
  CHECK(explains(result, "order.user", "a.mod", "b.mod"));
}

TEST_CASE("planner: an order that breaks a dependency is an error") {
  const std::vector<installed_content> catalog = {
      make(mod("a.mod", "1.0.0", R"(,"dependencies":[{"id":"b.mod"}])")),
      make(mod("b.mod", "1.0.0"))};
  const plan result = resolve_plan(catalog, request_for({"b.mod", "a.mod"}));
  CHECK(result.diags.has("plan.order.breaks_dependency"));
  CHECK_EQ(ids(result), (std::vector<std::string>{"a.mod", "b.mod"}));
}

TEST_CASE("planner: hints decide where nothing else does") {
  const std::vector<installed_content> catalog = {
      make(mod("x.mod", "1.0.0",
               R"(,"dependencies":[{"id":"a.mod"},{"id":"c.mod"}])")),
      make(mod("a.mod", "1.0.0")),
      make(mod("c.mod", "1.0.0", R"(,"order":{"after":["a.mod"]})"))};
  const plan result = resolve_plan(catalog, request_for({"x.mod"}));
  CHECK(result.ok());
  CHECK_EQ(ids(result), (std::vector<std::string>{"x.mod", "c.mod", "a.mod"}));
  CHECK(explains(result, "order.hint", "c.mod", "a.mod"));
}

TEST_CASE("planner: ties break by priority, saved position, then id") {
  const std::vector<installed_content> catalog = {
      make(mod(
          "x.mod", "1.0.0",
          R"(,"dependencies":[{"id":"a.mod"},{"id":"b.mod"},{"id":"c.mod"}])")),
      make(mod("a.mod", "1.0.0")),
      make(mod("b.mod", "1.0.0", R"(,"order":{"priority":5})")),
      make(mod("c.mod", "1.0.0"))};
  plan_request request = request_for({"x.mod"});
  CHECK_EQ(ids(resolve_plan(catalog, request)),
           (std::vector<std::string>{"x.mod", "b.mod", "a.mod", "c.mod"}));
  request.saved_positions = {{"c.mod", 1}, {"a.mod", 2}};
  const plan saved = resolve_plan(catalog, request);
  CHECK_EQ(ids(saved),
           (std::vector<std::string>{"x.mod", "b.mod", "c.mod", "a.mod"}));
  CHECK(std::ranges::any_of(saved.explanations, [](const explanation &e) {
    return e.code == "order.tie" && e.subject == "b.mod";
  }));
}

TEST_CASE("planner: modes and the Xerces version are checked") {
  const std::vector<installed_content> catalog = {
      make(mod("mp.only", "1.0.0", R"(,"requires":{"modes":["mp"]})")),
      make(mod("future.mod", "1.0.0", R"(,"requires":{"xerces":">=2"})"))};
  const plan result =
      resolve_plan(catalog, request_for({"mp.only", "future.mod"}));
  CHECK(result.diags.has("plan.mode.unsupported"));
  CHECK(result.diags.has("plan.xerces.version"));
}

TEST_CASE("planner: mod-stack's limits on stacked mods") {
  const std::vector<installed_content> catalog = {
      make(mod("a.mod", "1.0.0")),
      make(mod("b.mod", "1.0.0"), std::string(40, 'f')),
      make(mod("c.mod", "1.0.0"), "has,comma")};
  const plan result =
      resolve_plan(catalog, request_for({"a.mod", "b.mod", "c.mod"}));
  CHECK(result.diags.has("plan.stack.name_too_long"));
  CHECK(result.diags.has("plan.stack.comma"));
}

TEST_CASE("planner: a mod that only runs as the top mod is named below it") {
  // PEGASUS (Workshop 2804432292) quits the game unless it is fs_game.
  installed_content pegasus = make(mod("ws.2804432292", "0.0.0"), "2804432292");
  pegasus.info.workshop_id = 2804432292;
  pegasus.info.legacy = true;
  const std::vector<installed_content> catalog = {make(mod("a.mod", "1.0.0")),
                                                  pegasus};
  const plan below =
      resolve_plan(catalog, request_for({"a.mod", "ws.2804432292"}));
  CHECK(below.ok()); // a warning: the player may still launch
  CHECK(below.diags.has("plan.stack.top_only"));
  CHECK(std::ranges::any_of(below.fixes, [](const fix_action &f) {
    return f.kind == "move_to_top" && f.id == "ws.2804432292" &&
           f.detail.empty();
  }));

  const plan on_top =
      resolve_plan(catalog, request_for({"ws.2804432292", "a.mod"}));
  CHECK(on_top.ok());
  CHECK(!on_top.diags.has("plan.stack.top_only"));
  CHECK(on_top.fixes.empty());
}

TEST_CASE("planner: the zone budget counts the files that are present") {
  const std::vector<installed_content> catalog = {
      make(mod("a.mod", "1.0.0"), "a.mod",
           {"zone/core_mod.ff", "zone/en_core_mod.ff", "zone/zm_mod.ff",
            "zone/workshop.json", "zone/sub/ignored.ff"}),
      make(mod("b.mod", "1.0.0"), "b.mod",
           {"zone/core_mod.ff", "zone/core_mod_patch.ff"})};
  plan_request request = request_for({"a.mod", "b.mod"});
  plan unknown = resolve_plan(catalog, request);
  CHECK_EQ(unknown.zones_added, 5u);
  CHECK(unknown.diags.has("plan.budget.stock_unknown"));
  CHECK(unknown.ok());

  request.stock_zones = 60;
  plan over = resolve_plan(catalog, request);
  CHECK(over.diags.has("plan.budget.zones"));
  REQUIRE(over.zones_total.has_value());
  CHECK_EQ(*over.zones_total, 65u);
}

TEST_CASE("planner: Workshop folders keep fastfiles at their root") {
  // As Steam lays out 1944930083 (Solo Easter Eggs): 13 language copies.
  std::vector<std::string> workshop = {"core_mod.ff", "workshop.json"};
  for (const char *language : {"bp", "ea", "en", "es", "fr", "ge", "it", "ja",
                               "po", "ru", "sc", "tc"}) {
    workshop.push_back(std::string(language) + "_core_mod.ff");
  }
  CHECK_EQ(count_zone_files(workshop, "zm"), std::size_t{2});
  // Mode tiers: only the launched mode's counts.
  const std::vector<std::string> tiers = {"zone/core_mod.ff", "zone/zm_mod.ff",
                                          "zone/en_zm_mod.ff", "zone/mp_mod.ff",
                                          "zone/mp_mod_patch.ff"};
  CHECK_EQ(count_zone_files(tiers, "zm"), std::size_t{3});
  CHECK_EQ(count_zone_files(tiers, "mp"), std::size_t{3});
  CHECK_EQ(count_zone_files(tiers), std::size_t{5});
}

TEST_CASE("planner: settings are resolved, validated and scoped") {
  const std::string settings =
      R"(,"settings":[{"key":"mult","type":"float","default":1.0,"min":0.25,"max":2.0},)"
      R"({"key":"hud","type":"bool","default":true,"scope":"client"}])";
  const std::vector<installed_content> catalog = {
      make(mod("a.mod", "1.0.0", settings))};
  plan_request request = request_for({"a.mod"});
  const plan base = resolve_plan(catalog, request);
  CHECK(base.ok());
  CHECK_EQ(base.stack[0].settings.at("mult"), "1");

  request.settings["a.mod"]["hud"] = "false";
  const plan client_change = resolve_plan(catalog, request);
  CHECK(client_change.plan_id != base.plan_id);
  CHECK_EQ(client_change.content_id, base.content_id);

  request.settings["a.mod"]["mult"] = "1.5";
  const plan server_change = resolve_plan(catalog, request);
  CHECK(server_change.content_id != base.content_id);

  request.settings["a.mod"]["mult"] = "9";
  request.settings["a.mod"]["nope"] = "1";
  const plan invalid = resolve_plan(catalog, request);
  CHECK(invalid.diags.has("plan.setting.invalid"));
  CHECK(invalid.diags.has("plan.setting.unknown"));
}

TEST_CASE("planner: a stored setting the chosen version rejects can be reset") {
  // A pack stored rounds=5 when x.mod took up to 10; 2.0.0 takes up to 3.
  // Its dependency's enum lost the option the pack stored.
  const std::vector<installed_content> catalog = {
      make(mod("x.mod", "2.0.0",
               R"(,"dependencies":[{"id":"y.dep","version":"^1"}],)"
               R"("settings":[{"key":"rounds","type":"int","default":2,)"
               R"("min":1,"max":3,"label":"Rounds"}])")),
      make(mod("y.dep", "1.0.0",
               R"(,"settings":[{"key":"style","type":"enum","default":"a",)"
               R"("options":["a","b"]}])"))};
  plan_request request = request_for({"x.mod"});
  request.settings["x.mod"]["rounds"] = "5";
  request.settings["x.mod"]["speed"] = "1"; // unknown: a warning, no fix
  request.settings["y.dep"]["style"] = "\"c\"";
  const plan bad = resolve_plan(catalog, request);
  CHECK(bad.diags.has("plan.setting.invalid"));
  const auto offers = [&](const std::string &id, const std::string &key) {
    return std::ranges::any_of(bad.fixes, [&](const fix_action &f) {
      return f.kind == "reset_setting" && f.id == id && f.detail == key;
    });
  };
  CHECK(offers("x.mod", "rounds"));
  CHECK(offers("y.dep", "style")); // a dependency's own value
  CHECK(!offers("x.mod", "speed"));

  // Dropping the two values (Mods' "Use the default") clears the errors.
  request.settings["x.mod"].erase("rounds");
  request.settings["y.dep"].erase("style");
  const plan fixed = resolve_plan(catalog, request);
  CHECK(fixed.ok());
  CHECK(fixed.fixes.empty());
  CHECK_EQ(fixed.stack[0].settings.at("rounds"), "2");
}

TEST_CASE("planner: maps sit outside the stack") {
  const std::vector<installed_content> catalog = {
      make(mod("ws.3735179543", "0.0.0", R"(,"kind":"map")"), "zm_test"),
      make(mod("a.mod", "1.0.0"))};
  plan_request request = request_for({"a.mod"});
  request.map = "ws.3735179543";
  const plan result = resolve_plan(catalog, request);
  CHECK(result.ok());
  REQUIRE(result.map.has_value());
  CHECK_EQ(result.map->folder, "zm_test");
  CHECK_EQ(result.stack.size(), 1u);

  const plan wrong = resolve_plan(catalog, request_for({"ws.3735179543"}));
  CHECK(wrong.diags.has("plan.map.in_stack"));
  plan_request mod_as_map = request_for({});
  mod_as_map.map = "a.mod";
  CHECK(resolve_plan(catalog, mod_as_map).diags.has("plan.map.kind"));
}

TEST_CASE("planner: pins choose exact versions") {
  const std::vector<installed_content> catalog = {make(mod("b.mod", "1.2.0")),
                                                  make(mod("b.mod", "1.5.0"))};
  plan_request request = request_for({"b.mod"});
  request.pins["b.mod"] = "1.2.0";
  const plan pinned = resolve_plan(catalog, request);
  CHECK(pinned.ok());
  CHECK_EQ(pinned.stack[0].ver.to_string(), "1.2.0");
  request.pins["b.mod"] = "9.9.9";
  CHECK(resolve_plan(catalog, request).diags.has("plan.mod.version"));
}

TEST_CASE("planner: optional dependencies join only when installed") {
  std::vector<installed_content> catalog = {
      make(mod("a.mod", "1.0.0",
               R"(,"dependencies":[{"id":"z.mod","optional":true}])"))};
  CHECK(resolve_plan(catalog, request_for({"a.mod"})).ok());
  CHECK_EQ(resolve_plan(catalog, request_for({"a.mod"})).stack.size(), 1u);
  catalog.push_back(make(mod("z.mod", "1.0.0")));
  const plan with = resolve_plan(catalog, request_for({"a.mod"}));
  CHECK_EQ(ids(with), (std::vector<std::string>{"a.mod", "z.mod"}));
}

TEST_CASE("planner: dependency cycles are reported") {
  const std::vector<installed_content> catalog = {
      make(mod("a.mod", "1.0.0", R"(,"dependencies":[{"id":"b.mod"}])")),
      make(mod("b.mod", "1.0.0", R"(,"dependencies":[{"id":"a.mod"}])"))};
  const plan result = resolve_plan(catalog, request_for({"a.mod"}));
  CHECK(result.diags.has("plan.dependency.cycle"));
  CHECK_EQ(result.stack.size(), 2u);
}

TEST_CASE("planner: shuffled inputs give byte-identical plans (6.8)") {
  std::vector<installed_content> catalog = {
      make(mod(
          "x.mod", "1.0.0",
          R"(,"dependencies":[{"id":"a.mod"},{"id":"b.mod"},{"id":"c.mod","version":"^1"}])")),
      make(mod("a.mod", "1.0.0", R"(,"order":{"after":["c.mod"]})")),
      make(mod("b.mod", "1.0.0", R"(,"order":{"priority":2})")),
      make(mod("c.mod", "1.1.0")),
      make(mod("c.mod", "1.3.0")),
      make(mod("c.mod", "2.0.0")),
      make(mod("y.mod", "1.0.0", R"(,"order":{"before":["x.mod"]})")),
      make(mod("z.mod", "3.0.0"))};
  const plan_request request = request_for({"y.mod", "x.mod", "z.mod"});
  const plan reference = resolve_plan(catalog, request);
  CHECK(reference.ok());
  for (int round = 0; round < 24; ++round) {
    std::ranges::next_permutation(catalog, [](const auto &a, const auto &b) {
      return std::tie(a.info.id, a.info.ver) < std::tie(b.info.id, b.info.ver);
    });
    std::ranges::rotate(catalog, catalog.begin() + (round % catalog.size()));
    const plan again = resolve_plan(catalog, request);
    CHECK_EQ(again.canonical, reference.canonical);
    CHECK_EQ(plan_to_json(again), plan_to_json(reference));
  }
  CHECK(reference.plan_id.starts_with("sha256:"));
  CHECK_EQ(reference.plan_id.size(), 7u + 64u);
}

TEST_CASE("planner: order changes both ids") {
  const std::vector<installed_content> catalog = {make(mod("a.mod", "1.0.0")),
                                                  make(mod("b.mod", "1.0.0"))};
  const plan ab = resolve_plan(catalog, request_for({"a.mod", "b.mod"}));
  const plan ba = resolve_plan(catalog, request_for({"b.mod", "a.mod"}));
  CHECK(ab.plan_id != ba.plan_id);
  CHECK(ab.content_id != ba.content_id);
}
