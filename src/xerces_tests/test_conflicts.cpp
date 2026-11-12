#include "harness.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <xerces_core/catalog.hpp>
#include <xerces_core/conflicts.hpp>
#include <xerces_core/hooks.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
manifest manifest_mod(const std::string &id, bool legacy,
                      std::vector<asset_ref> overrides = {}) {
  manifest m;
  m.id = id;
  m.name = id;
  m.legacy = legacy;
  m.overrides = std::move(overrides);
  return m;
}

plan stack_of(std::vector<std::string> ids,
              std::optional<std::string> map = std::nullopt) {
  plan p;
  for (const auto &id : ids) {
    plan_entry entry;
    entry.id = id;
    entry.name = id;
    p.stack.push_back(entry);
  }
  if (map) {
    plan_entry entry;
    entry.id = *map;
    entry.name = *map;
    p.map = entry;
  }
  return p;
}

asset_index index_of(const std::string &owner,
                     std::vector<asset_record> records) {
  return {owner, std::move(records), {}};
}

asset_record script(const std::string &name, const std::string &hash) {
  return {"scriptparsetree", name, "core_mod.ff", 100, hash};
}

const asset_ownership *find(const conflict_report &report,
                            const std::string &name) {
  const auto it = std::ranges::find_if(
      report.entries, [&](const auto &e) { return e.name == name; });
  return it == report.entries.end() ? nullptr : &*it;
}
} // namespace

TEST_CASE("conflicts: strategies follow DESIGN.md 6.4") {
  CHECK(strategy_for("stringtable", "x.csv") == asset_strategy::merge_table);
  CHECK(strategy_for("rawfile", "a/b.cfg") == asset_strategy::merge_config);
  CHECK(strategy_for("rawfile", "ui/menu.lua") == asset_strategy::exclusive);
  CHECK(strategy_for("localize", "x") == asset_strategy::merge_strings);
  CHECK(strategy_for("scriptparsetree", "a.gsc") ==
        asset_strategy::exclusive_script);
  CHECK(strategy_for("weapon", "ray_gun") == asset_strategy::exclusive);
  CHECK(has_exact_hash("scriptparsetree"));
  CHECK(!has_exact_hash("xmodel"));
}

TEST_CASE("conflicts: two legacy mods replacing a script need a decision") {
  const plan p = stack_of({"perk.rebalance", "faster.revive"});
  conflict_inputs inputs;
  inputs.resolved = &p;
  inputs.manifests = {{"perk.rebalance", manifest_mod("perk.rebalance", true)},
                      {"faster.revive", manifest_mod("faster.revive", true)}};
  inputs.indexes = {index_of("perk.rebalance",
                             {script("scripts/zm/_zm_perks.gsc", "sha256:1")}),
                    index_of("faster.revive",
                             {script("scripts/zm/_zm_perks.gsc", "sha256:2")})};
  conflict_report report = check_conflicts(inputs);
  CHECK_EQ(report.decisions, 1u);
  const auto *entry = find(report, "scripts/zm/_zm_perks.gsc");
  REQUIRE(entry != nullptr);
  CHECK(entry->outcome == asset_outcome::legacy_conflict);
  CHECK_EQ(entry->winner, "perk.rebalance");
  CHECK(conflict_report_to_text(report).find("1 conflict needs a decision.") !=
        std::string::npos);

  inputs.decided.insert("scriptparsetree:scripts/zm/_zm_perks.gsc");
  report = check_conflicts(inputs);
  CHECK_EQ(report.decisions, 0u);
}

TEST_CASE("conflicts: identical copies are never a conflict") {
  const plan p = stack_of({"a.mod", "b.mod"});
  conflict_inputs inputs;
  inputs.resolved = &p;
  inputs.manifests = {{"a.mod", manifest_mod("a.mod", true)},
                      {"b.mod", manifest_mod("b.mod", true)}};
  inputs.indexes = {
      index_of("a.mod", {script("scripts/shared.gsc", "sha256:x")}),
      index_of("b.mod", {script("scripts/shared.gsc", "sha256:x")})};
  const conflict_report report = check_conflicts(inputs);
  CHECK_EQ(report.identical, 1u);
  CHECK_EQ(report.decisions, 0u);
}

TEST_CASE("conflicts: undeclared overrides from manifest mods are blocked") {
  const plan p = stack_of({"new.mod", "old.mod"});
  conflict_inputs inputs;
  inputs.resolved = &p;
  inputs.manifests = {{"new.mod", manifest_mod("new.mod", false)},
                      {"old.mod", manifest_mod("old.mod", false)}};
  inputs.indexes = {index_of("new.mod", {script("scripts/x.gsc", "sha256:1")}),
                    index_of("old.mod", {script("scripts/x.gsc", "sha256:2")})};
  conflict_report report = check_conflicts(inputs);
  const auto *entry = find(report, "scripts/x.gsc");
  REQUIRE(entry != nullptr);
  CHECK(entry->outcome == asset_outcome::override_blocked);
  CHECK_EQ(entry->winner, "old.mod");
  CHECK_EQ(report.blocked, 1u);

  inputs.manifests["new.mod"] = manifest_mod(
      "new.mod", false, {{"scriptparsetree", "scripts/x.gsc", "why", {}}});
  report = check_conflicts(inputs);
  entry = find(report, "scripts/x.gsc");
  REQUIRE(entry != nullptr);
  CHECK(entry->outcome == asset_outcome::override_allowed);
  CHECK_EQ(entry->winner, "new.mod");

  inputs.manifests["new.mod"] = manifest_mod("new.mod", false);
  inputs.allow_undeclared.insert("new.mod");
  report = check_conflicts(inputs);
  CHECK_EQ(find(report, "scripts/x.gsc")->winner, "new.mod");
}

TEST_CASE("conflicts: replacing the game's copy follows the same rules") {
  const plan p = stack_of({"m.mod"});
  conflict_inputs inputs;
  inputs.resolved = &p;
  inputs.manifests = {{"m.mod", manifest_mod("m.mod", false)}};
  inputs.indexes = {
      index_of("m.mod", {script("scripts/zm/_zm.gsc", "sha256:1")})};
  inputs.stock = index_of("stock", {script("scripts/zm/_zm.gsc", "sha256:0")});
  conflict_report report = check_conflicts(inputs);
  REQUIRE(find(report, "scripts/zm/_zm.gsc") != nullptr);
  CHECK(find(report, "scripts/zm/_zm.gsc")->outcome ==
        asset_outcome::override_blocked);
  CHECK_EQ(find(report, "scripts/zm/_zm.gsc")->winner,
           std::string(stock_owner));

  inputs.manifests["m.mod"].legacy = true; // legacy overrides are implicit
  report = check_conflicts(inputs);
  CHECK(find(report, "scripts/zm/_zm.gsc")->outcome ==
        asset_outcome::override_allowed);
}

TEST_CASE("conflicts: other types overlap without comparison") {
  const plan p = stack_of({"a.mod", "b.mod"});
  conflict_inputs inputs;
  inputs.resolved = &p;
  inputs.manifests = {{"a.mod", manifest_mod("a.mod", true)},
                      {"b.mod", manifest_mod("b.mod", true)}};
  inputs.indexes = {index_of("a.mod", {{"weapon", "ray_gun", "z", 1, ""}}),
                    index_of("b.mod", {{"weapon", "ray_gun", "z", 1, ""}})};
  const conflict_report report = check_conflicts(inputs);
  CHECK_EQ(report.overlaps, 1u);
  CHECK_EQ(report.decisions, 0u);
}

TEST_CASE("conflicts: the map ranks first for its own scripts") {
  const plan p = stack_of({"a.mod"}, "ws.123");
  conflict_inputs inputs;
  inputs.resolved = &p;
  inputs.manifests = {{"a.mod", manifest_mod("a.mod", true)},
                      {"ws.123", manifest_mod("ws.123", true)}};
  inputs.indexes = {
      index_of("a.mod", {script("scripts/zm/zm_map.gsc", "sha256:1")}),
      index_of("ws.123", {script("scripts/zm/zm_map.gsc", "sha256:2")})};
  conflict_report report = check_conflicts(inputs);
  CHECK(find(report, "scripts/zm/zm_map.gsc")->outcome ==
        asset_outcome::map_priority);
  CHECK_EQ(find(report, "scripts/zm/zm_map.gsc")->winner, "ws.123");
  inputs.map_first_for_scripts = false;
  report = check_conflicts(inputs);
  CHECK_EQ(find(report, "scripts/zm/zm_map.gsc")->winner, "a.mod");
}

TEST_CASE("conflicts: unindexed mods are named in the report") {
  const plan p = stack_of({"a.mod", "b.mod"});
  conflict_inputs inputs;
  inputs.resolved = &p;
  inputs.indexes = {index_of("a.mod", {})};
  const conflict_report report = check_conflicts(inputs);
  CHECK(report.diags.has("conflict.index.missing"));
  CHECK(conflict_report_to_json(report).find("\"needs_decision\":false") !=
        std::string::npos);
}

TEST_CASE("conflicts: fastfiles at a Workshop item's root are not loose files") {
  // A Workshop item keeps core_mod.ff, zm_mod.ff and their packs at its
  // root, not under zone/. Only zone/ was skipped, so two such mods "both
  // replaced zm_mod.ff", and the files were read whole to get there.
  const std::vector<std::pair<std::string, std::string>> files = {
      {"core_mod.ff", "ff"},
      {"zm_mod.ff", "ff"},
      {"ZM_MOD.XPAK", "xpak"},
      {"snd/zm_mod.all.sabl", "sabl"},
      {"snd\\zm_mod.all.sabs", "sabs"},
      {"zone/zm_x.ff", "ff"},
      {"workshop.json", "{}"},
      {"scripts/zm/a.gsc", "init() {}\n"},
  };
  const asset_index a = index_loose_files("a.mod", files);
  REQUIRE_EQ(a.records.size(), 1u);
  CHECK_EQ(a.records[0].name, std::string("scripts/zm/a.gsc"));
  CHECK(loose_index_skips("zm_mod.ff"));
  CHECK(loose_index_skips("sub/any.xpak"));
  CHECK(!loose_index_skips("scripts/zm/a.gsc"));
  CHECK(!loose_index_skips("ffotd.csv")); // by extension, not by letters

  // Two mods with the same fastfile names do not conflict over them.
  std::vector<std::pair<std::string, std::string>> other = files;
  other.back() = {"scripts/zm/b.gsc", "init() {}\n"};
  const plan p = stack_of({"a.mod", "b.mod"});
  conflict_inputs inputs;
  inputs.resolved = &p;
  inputs.manifests = {{"a.mod", manifest_mod("a.mod", true)},
                      {"b.mod", manifest_mod("b.mod", true)}};
  inputs.indexes = {a, index_loose_files("b.mod", other)};
  const conflict_report report = check_conflicts(inputs);
  CHECK_EQ(report.decisions, 0u);
  CHECK(find(report, "zm_mod.ff") == nullptr);
  CHECK(find(report, "core_mod.ff") == nullptr);

  // From a folder, the packed files are not read at all: only the loose
  // files next to them come back.
  const fs::path dir = fs::temp_directory_path() / "xerces_conflicts_loose";
  fs::remove_all(dir);
  fs::create_directories(dir / "scripts" / "zm");
  std::ofstream(dir / "zm_mod.ff", std::ios::binary) << "fastfile";
  std::ofstream(dir / "zm_mod.xpak", std::ios::binary) << "pack";
  std::ofstream(dir / "scripts" / "zm" / "a.gsc", std::ios::binary)
      << "init() {}\n";
  const auto wanted = [](std::string_view name) {
    return !loose_index_skips(name);
  };
  const auto read = read_files(dir, wanted);
  REQUIRE_EQ(read.size(), 1u);
  CHECK_EQ(read[0].first, std::string("scripts/zm/a.gsc"));
  CHECK_EQ(read[0].second, std::string("init() {}\n"));
  CHECK_EQ(read_files(dir).size(), 3u); // the plain call still reads all
  const asset_index folder = index_loose_folder("a.mod", dir);
  REQUIRE_EQ(folder.records.size(), 1u);
  CHECK_EQ(folder.records[0].name, std::string("scripts/zm/a.gsc"));
  CHECK_EQ(folder.records[0].content_hash, a.records[0].content_hash);
  fs::remove_all(dir);
}

TEST_CASE("hooks: the highest mod is outermost; one replace per target") {
  const plan p = stack_of({"high.mod", "low.mod"});
  manifest high = manifest_mod("high.mod", false);
  high.hooks = {{"scripts/a.gsc::f", "scripts/h.gsc::g", hook_kind::wrap},
                {"scripts/a.gsc::f", "scripts/h.gsc::b", hook_kind::before}};
  manifest low = manifest_mod("low.mod", false);
  low.hooks = {{"scripts/a.gsc::f", "scripts/l.gsc::g", hook_kind::wrap},
               {"scripts/a.gsc::f", "scripts/l.gsc::r", hook_kind::replace},
               {"scripts/a.gsc::f", "scripts/l.gsc::a", hook_kind::after}};
  const hook_plan hooks = plan_hooks(p, {{"high.mod", high}, {"low.mod", low}});
  REQUIRE_EQ(hooks.chains.size(), 1u);
  CHECK_EQ(simulate_chain(hooks.chains[0]),
           (std::vector<std::string>{"before:high.mod", "enter:high.mod",
                                     "enter:low.mod", "replace:low.mod",
                                     "after:low.mod"}));

  manifest other = manifest_mod("other.mod", false);
  other.hooks = {{"scripts/a.gsc::f", "scripts/o.gsc::r", hook_kind::replace}};
  const plan three = stack_of({"high.mod", "low.mod", "other.mod"});
  const hook_plan clash = plan_hooks(
      three, {{"high.mod", high}, {"low.mod", low}, {"other.mod", other}});
  CHECK(clash.diags.has("hooks.replace.conflict"));
}
