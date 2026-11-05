#include "harness.hpp"

#include <xerces_core/merge.hpp>

using namespace xerces_core;

namespace {
table csv(const std::string &text) {
  const auto parsed = parse_csv(text);
  if (!parsed) {
    throw std::runtime_error("bad test csv");
  }
  return *parsed;
}
} // namespace

TEST_CASE("csv: quotes, commas and line endings round trip") {
  const table t =
      csv("a,\"b,c\",\"say \"\"hi\"\"\"\r\nx,,z\n\"multi\nline\",2,3");
  REQUIRE_EQ(t.rows.size(), 3u);
  CHECK_EQ(t.rows[0][1], "b,c");
  CHECK_EQ(t.rows[0][2], "say \"hi\"");
  CHECK_EQ(t.rows[1][1], "");
  CHECK_EQ(t.rows[2][0], "multi\nline");
  CHECK_EQ(write_csv(*parse_csv(write_csv(t))), write_csv(t));
  std::string error;
  CHECK(!parse_csv("a,\"open", &error).has_value());
  CHECK(!error.empty());
}

TEST_CASE("stringtables: both mods' added and changed rows apply") {
  const table base =
      csv("row1,base one,10\nrow2,base two,20\nrow3,base three,30\n");
  const table alpha =
      csv("row1,base one,10\nrow2,alpha two,20\nrow3,base three,30\n"
          "alpha_row,added by alpha,40\n");
  const table beta =
      csv("row1,base one,10\nrow2,base two,20\nrow3,beta three,30\n"
          "beta_row,added by beta,50\n");
  const table_merge result =
      merge_tables(base, {{"beta", beta}, {"alpha", alpha}}, {0});
  CHECK_EQ(write_csv(result.merged),
           "row1,base one,10\nrow2,alpha two,20\nrow3,beta three,30\n"
           "beta_row,added by beta,50\nalpha_row,added by alpha,40\n");
  CHECK(result.stats.conflicts.empty());
  CHECK_EQ(result.stats.added.at("alpha"), 1u);
  CHECK_EQ(result.stats.changed.at("beta"), 1u);
}

TEST_CASE("stringtables: the higher mod wins a cell both changed, reported") {
  const table base = csv("k,old\n");
  const table low = csv("k,low\n");
  const table high = csv("k,high\n");
  const table_merge result =
      merge_tables(base, {{"low.mod", low}, {"high.mod", high}}, {0});
  CHECK_EQ(write_csv(result.merged), "k,high\n");
  REQUIRE_EQ(result.stats.conflicts.size(), 1u);
  CHECK_EQ(result.stats.conflicts[0].winner, "high.mod");
  CHECK_EQ(result.stats.conflicts[0].loser, "low.mod");
  CHECK_EQ(result.stats.conflicts[0].column, 1u);
}

TEST_CASE("stringtables: removals need a real base") {
  const table base = csv("a,1\nb,2\n");
  const table removes_b = csv("a,1\n");
  const table_merge removed = merge_tables(base, {{"m", removes_b}}, {0});
  CHECK_EQ(write_csv(removed.merged), "a,1\n");
  CHECK_EQ(removed.stats.removed.at("m"), 1u);

  // No stock table: the lowest copy is the base, and rows another mod lacks
  // are kept, because that mod never knew about them.
  const table_merge no_base = merge_tables(
      std::nullopt, {{"low", csv("a,1\nb,2\n")}, {"high", csv("a,1\nc,3\n")}},
      {0});
  CHECK_EQ(write_csv(no_base.merged), "a,1\nb,2\nc,3\n");
  CHECK(no_base.stats.removed.empty());
}

TEST_CASE("stringtables: composite keys and repeated keys") {
  const table base = csv("x,1,a\nx,2,b\n");
  const table mod = csv("x,1,A\nx,2,b\n");
  CHECK_EQ(write_csv(merge_tables(base, {{"m", mod}}, {0, 1}).merged),
           "x,1,A\nx,2,b\n");
  // Repeats of key column 0 match by occurrence.
  const table repeats = csv("x,1,a\nx,2,B\n");
  CHECK_EQ(write_csv(merge_tables(base, {{"m", repeats}}, {0}).merged),
           "x,1,a\nx,2,B\n");
}

TEST_CASE("configs: .cfg merges by dvar name") {
  const std::string base = "set a 1\nseta b 2\nbind F1 \"say hi\"\n";
  const text_merge result =
      merge_cfg(base, {{"low", "set a 1\nseta b 5\nset c 3\n"},
                       {"high", "set A 9\nbind F2 \"say yo\"\n"}});
  CHECK_EQ(
      result.merged,
      "set A 9\nseta b 5\nbind F1 \"say hi\"\nset c 3\nbind F2 \"say yo\"\n");
  CHECK(result.stats.conflicts.empty());
  const text_merge clash =
      merge_cfg(base, {{"low", "seta b 5\n"}, {"high", "seta b 7\n"}});
  REQUIRE_EQ(clash.stats.conflicts.size(), 1u);
  CHECK_EQ(clash.stats.conflicts[0].key, "b");
  CHECK_EQ(clash.stats.conflicts[0].winner, "high");
}

TEST_CASE("configs: key=value and JSON merge by key") {
  const text_merge kv =
      merge_key_value(std::string("# comment\nspeed=1\nsize=2\n"),
                      {{"low", "speed=3\n"}, {"high", "size=4\nnew=5\n"}});
  CHECK_EQ(kv.merged, "# comment\nspeed=3\nsize=4\nnew=5\n");

  const text_merge doc =
      merge_json_documents(std::string(R"({"a":{"x":1,"y":2},"b":[1]})"),
                           {{"low", R"({"a":{"x":1,"y":3}})"},
                            {"high", R"({"a":{"z":4},"b":[2]})"}});
  CHECK(doc.stats.conflicts.empty());
  CHECK(doc.merged.find("\"y\": 3") != std::string::npos);
  CHECK(doc.merged.find("\"z\": 4") != std::string::npos);
  CHECK(doc.merged.find("2") != std::string::npos);

  const text_merge clash =
      merge_json_documents(std::string(R"({"a":1})"),
                           {{"low", R"({"a":2})"}, {"high", R"({"a":3})"}});
  REQUIRE_EQ(clash.stats.conflicts.size(), 1u);
  CHECK_EQ(clash.stats.conflicts[0].key, "/a");
}

TEST_CASE("localized strings merge by key, resolved by order") {
  const strings_merge result =
      merge_strings(std::map<std::string, std::string>{{"HELLO", "Hello"}},
                    {{"low", {{"HELLO", "Hi"}, {"BYE", "Bye"}}},
                     {"high", {{"HELLO", "Hey"}, {"NEW", "New"}}}});
  CHECK_EQ(result.merged.at("HELLO"), "Hey");
  CHECK_EQ(result.merged.at("BYE"), "Bye");
  CHECK_EQ(result.merged.at("NEW"), "New");
  REQUIRE_EQ(result.stats.conflicts.size(), 1u);
  CHECK_EQ(result.stats.conflicts[0].winner, "high");
}
