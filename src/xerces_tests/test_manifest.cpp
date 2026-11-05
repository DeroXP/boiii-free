#include "harness.hpp"

#include <xerces_core/manifest.hpp>

using namespace xerces_core;

namespace {
manifest_result parse_text(const std::string &text) {
  return parse_manifest(text);
}

std::string minimal(const std::string &extra = {}) {
  return R"({"schema":1,"id":"test.mod","name":"Test","version":"1.0.0")" +
         extra + "}";
}
} // namespace

TEST_CASE("manifest: the design document's example parses") {
  const auto result = parse_manifest(xerces_tests::read_file(
      xerces_tests::fixtures_dir() / "manifests" / "design-example.json"));
  for (const auto &d : result.diags.items()) {
    if (d.level == severity::error) {
      xerces_tests::check_failed(d.code.c_str(), __FILE__, __LINE__,
                                 d.path + ": " + d.message);
    }
  }
  REQUIRE(result.ok());
  const manifest &m = result.value;
  CHECK_EQ(m.id, "vela.perk_rebalance");
  CHECK_EQ(m.ver.to_string(), "2.1.0");
  CHECK_EQ(m.modes.size(), 1u);
  CHECK(m.supports_mode("zm"));
  CHECK(!m.supports_mode("mp"));
  REQUIRE(m.workshop_id.has_value());
  CHECK_EQ(*m.workshop_id, 1234567890ull);
  REQUIRE_EQ(m.dependencies.size(), 1u);
  CHECK_EQ(m.dependencies[0].id, "orrin.better_hud");
  CHECK(m.dependencies[0].range.satisfied_by(*version::parse("1.5.0")));
  CHECK_EQ(m.conflicts.size(), 1u);
  CHECK_EQ(m.order.after.size(), 1u);
  CHECK(m.declares_override("scriptparsetree", "scripts/zm/_zm_perks.gsc"));
  REQUIRE_EQ(m.extends.size(), 1u);
  CHECK_EQ(m.extends[0].key.size(), 1u);
  REQUIRE_EQ(m.hooks.size(), 1u);
  CHECK(m.hooks[0].kind == hook_kind::wrap);
  REQUIRE_EQ(m.settings.size(), 1u);
  CHECK_EQ(m.settings[0].scope, "server");
  CHECK_EQ(m.settings[0].default_json, "1");
  CHECK_EQ(m.integrity.size(), 2u);
  CHECK(m.unknown_fields.empty());
}

TEST_CASE("manifest: required fields and id rules") {
  auto result = parse_text(R"({"schema":1,"name":"x","version":"1.0.0"})");
  CHECK(result.diags.has("manifest.field.missing"));
  result =
      parse_text(R"({"schema":1,"id":"Bad_ID","name":"x","version":"1.0.0"})");
  CHECK(result.diags.has("manifest.id.invalid"));
  result = parse_text(
      R"({"schema":2,"id":"test.mod","name":"x","version":"1.0.0"})");
  CHECK(result.diags.has("manifest.schema.unsupported"));
  result =
      parse_text(R"({"schema":1,"id":"test.mod","name":"x","version":"1.0"})");
  CHECK(result.diags.has("manifest.version.invalid"));
  CHECK(parse_text("not json").diags.has("manifest.json"));
  CHECK(parse_text(minimal()).ok());
  CHECK(valid_mod_id("ws.1944930083"));
  CHECK(!valid_mod_id("ab"));
  CHECK(!valid_mod_id(".abc"));
  CHECK(!valid_mod_id("abc def"));
}

TEST_CASE("manifest: unknown fields are kept and warned about") {
  const auto result =
      parse_text(minimal(R"(,"colour":"red","assets":{"wat":[]})"));
  CHECK(result.ok());
  CHECK(result.diags.has("manifest.field.unknown"));
  REQUIRE_EQ(result.value.unknown_fields.size(), 2u);
  CHECK_EQ(result.value.unknown_fields[0], "/assets/wat");
  CHECK_EQ(result.value.unknown_fields[1], "/colour");
}

TEST_CASE("manifest: a replace hook needs a declared override") {
  const auto result = parse_text(minimal(
      R"(,"scripts":{"hooks":[{"target":"scripts/zm/a.gsc::f","handler":"scripts/zm/b.gsc::g","kind":"replace"}]})"));
  CHECK(result.diags.has("manifest.hook.replace_undeclared"));
  const auto declared = parse_text(minimal(
      R"(,"assets":{"overrides":[{"type":"scriptparsetree","name":"scripts/zm/a.gsc","reason":"r"}]},"scripts":{"hooks":[{"target":"scripts/zm/a.gsc::f","handler":"scripts/zm/b.gsc::g","kind":"replace"}]})"));
  CHECK(declared.ok());
  const auto bad_ref = parse_text(minimal(
      R"(,"scripts":{"hooks":[{"target":"a.gsc","handler":"b.gsc::g"}]})"));
  CHECK(bad_ref.diags.has("manifest.hook.reference"));
}

TEST_CASE("manifest: settings are type checked") {
  CHECK(parse_text(
            minimal(R"(,"settings":[{"key":"k","type":"int","default":"x"}])"))
            .diags.has("manifest.setting.default"));
  CHECK(
      parse_text(
          minimal(
              R"(,"settings":[{"key":"k","type":"float","default":3,"min":0,"max":2}])"))
          .diags.has("manifest.setting.default"));
  CHECK(
      parse_text(
          minimal(
              R"(,"settings":[{"key":"k","type":"enum","default":"b","options":["a"]}])"))
          .diags.has("manifest.setting.default"));
  CHECK(
      parse_text(
          minimal(
              R"(,"settings":[{"key":"k","type":"bool","default":true},{"key":"k","type":"bool","default":false}])"))
          .diags.has("manifest.setting.duplicate"));
  CHECK(
      parse_text(
          minimal(
              R"(,"settings":[{"key":"k","type":"bool","default":true,"scope":"client"}])"))
          .ok());
}

TEST_CASE("manifest: dependencies, conflicts and order are checked") {
  CHECK(parse_text(minimal(R"(,"dependencies":[{"id":"test.mod"}])"))
            .diags.has("manifest.dependency.self"));
  CHECK(
      parse_text(minimal(R"(,"dependencies":[{"id":"a.b.c"},{"id":"a.b.c"}])"))
          .diags.has("manifest.dependency.duplicate"));
  CHECK(
      parse_text(minimal(R"(,"dependencies":[{"id":"a.b.c","version":"^^1"}])"))
          .diags.has("manifest.range.invalid"));
  CHECK(
      parse_text(minimal(R"(,"order":{"after":["x.y.z"],"before":["x.y.z"]})"))
          .diags.has("manifest.order.contradiction"));
}

TEST_CASE("manifest: integrity entries are relative and well formed") {
  CHECK(parse_text(minimal(R"(,"integrity":{"a.ff":"md5:00"})"))
            .diags.has("manifest.integrity.hash"));
  CHECK(
      parse_text(
          minimal(
              R"(,"integrity":{"../a.ff":"sha256:0000000000000000000000000000000000000000000000000000000000000000"})"))
          .diags.has("manifest.integrity.path"));
}

TEST_CASE("manifest: legacy mods get a synthesized manifest") {
  legacy_input workshop;
  workshop.folder = "1944930083";
  workshop.workshop_json =
      R"({"PublisherID":"1944930083","Title":"Solo Easter Eggs","Type":"mod"})";
  workshop.files = {"zone/core_mod.ff", "zone/workshop.json"};
  workshop.time_updated = 1700000000;
  const manifest ws = synthesize_legacy(workshop, {});
  CHECK(ws.legacy);
  CHECK_EQ(ws.id, "ws.1944930083");
  CHECK_EQ(ws.name, "Solo Easter Eggs");
  CHECK_EQ(ws.ver.to_string(), "0.0.0+ws.1700000000");
  CHECK(!ws.caps.native_code);

  legacy_input local;
  local.folder = "My Mod!";
  local.files = {"zone/core_mod.ff", "bin/helper.DLL"};
  local.content_hash =
      "sha256:abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  const manifest lm = synthesize_legacy(local, {});
  CHECK_EQ(lm.id, "local.my_mod_");
  CHECK(valid_mod_id(lm.id));
  CHECK(lm.caps.native_code);
  CHECK_EQ(lm.ver.to_string(), "0.0.0+sha.abcdef012345");

  const manifest clash = synthesize_legacy(local, {"local.my_mod_"});
  CHECK(clash.id != "local.my_mod_");
  CHECK(clash.id.starts_with("local.my_mod_-"));
  CHECK(valid_mod_id(clash.id));

  legacy_input map;
  map.folder = "zm_test";
  map.workshop_json =
      R"({"PublisherID":"3735179543","Title":"Test Map","Type":"map"})";
  CHECK(synthesize_legacy(map, {}).kind == content_kind::map);
}

TEST_CASE("manifest: canonical JSON round trip") {
  const auto result = parse_manifest(xerces_tests::read_file(
      xerces_tests::fixtures_dir() / "manifests" / "design-example.json"));
  REQUIRE(result.ok());
  const std::string text = manifest_to_json(result.value);
  const auto again = parse_manifest(text);
  CHECK(again.ok());
  CHECK_EQ(again.value.id, result.value.id);
  CHECK_EQ(manifest_to_json(again.value), text);
}
