// The loose half of the both-apply proof (DESIGN.md 6.9): two fixture mods
// edit the same table and hook the same function, and both apply.

#include "harness.hpp"

#include <algorithm>

#include <xerces_core/catalog.hpp>
#include <xerces_core/conflicts.hpp>
#include <xerces_core/hooks.hpp>
#include <xerces_core/merge.hpp>

using namespace xerces_core;

namespace {
const std::string table_name = "gamedata/xerces_test/both_apply.csv";

struct loaded {
  catalog_scan scan;
  plan resolved;
  std::map<std::string, manifest> manifests;
};

loaded load(std::vector<std::string> order) {
  loaded out;
  out.scan = scan_mods_directory(xerces_tests::fixtures_dir() / "mods");
  plan_request request;
  request.mode = "zm";
  request.mods = std::move(order);
  request.game_build = "0x6531394";
  request.xerces_version = *version::parse("1.0.0");
  out.resolved = resolve_plan(out.scan.catalog, request);
  for (const auto &content : out.scan.catalog) {
    out.manifests[content.info.id] = content.info;
  }
  return out;
}

std::string file_of(const loaded &state, const std::string &id,
                    const std::string &relative) {
  return xerces_tests::read_file(state.scan.roots.at(id) / relative);
}
} // namespace

TEST_CASE("both-apply: the fixture catalog loads cleanly") {
  const loaded state = load({"fixture.alpha", "fixture.beta"});
  CHECK(!state.scan.diags.has_errors());
  CHECK_EQ(state.scan.catalog.size(), 3u);
  CHECK(state.resolved.ok());
  std::vector<std::string> ids;
  for (const auto &entry : state.resolved.stack) {
    ids.push_back(entry.id);
  }
  CHECK_EQ(ids, (std::vector<std::string>{"fixture.alpha", "fixture.beta",
                                          "fixture.probe"}));
  CHECK_EQ(state.resolved.fs_extramods(), "fixture.beta,fixture.probe");
}

TEST_CASE("both-apply: the shared table merges, nothing needs a decision") {
  const loaded state = load({"fixture.alpha", "fixture.beta"});
  conflict_inputs inputs;
  inputs.resolved = &state.resolved;
  inputs.manifests = state.manifests;
  for (const auto &entry : state.resolved.stack) {
    inputs.indexes.push_back(
        index_loose_files(entry.id, read_files(state.scan.roots.at(entry.id))));
  }
  inputs.stock = index_loose_files(
      "stock", read_files(xerces_tests::fixtures_dir() / "stock"));
  const conflict_report report = check_conflicts(inputs);
  CHECK_EQ(report.decisions, 0u);
  CHECK_EQ(report.blocked, 0u);
  CHECK_EQ(report.merges, 1u);
  const auto entry = std::ranges::find_if(
      report.entries, [](const auto &e) { return e.name == table_name; });
  REQUIRE(entry != report.entries.end());
  CHECK(entry->outcome == asset_outcome::merged);

  // Sources go from the lowest priority up: beta, then alpha.
  const auto base = parse_csv(xerces_tests::read_file(
      xerces_tests::fixtures_dir() / "stock" / table_name));
  REQUIRE(base.has_value());
  const table_merge merged = merge_tables(
      base,
      {{"fixture.beta", *parse_csv(file_of(state, "fixture.beta", table_name))},
       {"fixture.alpha",
        *parse_csv(file_of(state, "fixture.alpha", table_name))}},
      state.manifests.at("fixture.alpha").extends[0].key);
  CHECK_EQ(write_csv(merged.merged),
           "row1,base one,10\nrow2,alpha changed two,20\n"
           "row3,beta changed three,30\nbeta_row,added by beta,50\n"
           "alpha_row,added by alpha,40\n");
  CHECK(merged.stats.conflicts.empty());
}

TEST_CASE("both-apply: both hooks chain, the higher mod outermost") {
  const loaded state = load({"fixture.alpha", "fixture.beta"});
  const hook_plan hooks = plan_hooks(state.resolved, state.manifests);
  CHECK(!hooks.diags.has_errors());
  REQUIRE_EQ(hooks.chains.size(), 1u);
  CHECK_EQ(hooks.chains[0].target, "scripts/zm/xerces_test_probe.gsc::record");
  CHECK_EQ(simulate_chain(hooks.chains[0]),
           (std::vector<std::string>{"enter:fixture.alpha",
                                     "enter:fixture.beta", "original"}));

  // Swapping the order swaps the chain, and still both apply.
  const loaded swapped = load({"fixture.beta", "fixture.alpha"});
  CHECK_EQ(
      simulate_chain(plan_hooks(swapped.resolved, swapped.manifests).chains[0]),
      (std::vector<std::string>{"enter:fixture.beta", "enter:fixture.alpha",
                                "original"}));
}

TEST_CASE("both-apply: the plan is stable across scans") {
  const loaded first = load({"fixture.alpha", "fixture.beta"});
  const loaded second = load({"fixture.alpha", "fixture.beta"});
  CHECK_EQ(first.resolved.plan_id, second.resolved.plan_id);
  CHECK_EQ(first.resolved.content_id, second.resolved.content_id);
}
