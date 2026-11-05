#include "harness.hpp"

#include <zstd.h>

#include <xerces_core/catalog.hpp>
#include <xerces_core/pack.hpp>

using namespace xerces_core;

namespace {
const std::string hash_a = "sha256:" + std::string(64, 'a');
const std::string hash_b = "sha256:" + std::string(64, 'b');

std::string example_pack() {
  return R"({
    "schema": 1,
    "id": "7b1e2c4a-1f0d-4f5e-9a51-3c2f8d0e9b11",
    "name": "Weekend Zombies",
    "mode": "zm",
    "map": { "id": "ws.3735179543", "content_hash": ")" +
         hash_a + R"(" },
    "mods": [
      {
        "id": "ws.1944930083",
        "version": "0.0.0+ws.1700000000",
        "source": { "workshop": 1944930083 },
        "content_hash": ")" +
         hash_b + R"(",
        "settings": { "mult": 1.5 }
      }
    ],
    "order": ["ws.1944930083"],
    "requires": { "xerces": ">=1.0", "game_build": "0x6531394" }
  })";
}
} // namespace

TEST_CASE("base64url: RFC 4648 vectors without padding") {
  const std::pair<const char *, const char *> vectors[] = {
      {"", ""},
      {"f", "Zg"},
      {"fo", "Zm8"},
      {"foo", "Zm9v"},
      {"foob", "Zm9vYg"},
      {"fooba", "Zm9vYmE"},
      {"foobar", "Zm9vYmFy"}};
  for (const auto &[plain, encoded] : vectors) {
    CHECK_EQ(base64url_encode(plain), encoded);
    CHECK_EQ(base64url_decode(encoded).value_or("?"), plain);
  }
  CHECK_EQ(base64url_encode("\xfb\xff"), "-_8");
  CHECK(!base64url_decode("a").has_value());
  CHECK(!base64url_decode("ab=c").has_value());
}

TEST_CASE("packs: the design document's example parses and round trips") {
  const pack_result result = parse_pack(example_pack());
  for (const auto &d : result.diags.items()) {
    xerces_tests::check_failed(d.code.c_str(), __FILE__, __LINE__, d.message);
  }
  REQUIRE(result.ok());
  CHECK_EQ(result.value.name, "Weekend Zombies");
  REQUIRE(result.value.map.has_value());
  REQUIRE_EQ(result.value.mods.size(), 1u);
  CHECK_EQ(*result.value.mods[0].workshop_id, 1944930083ull);
  CHECK_EQ(result.value.mods[0].settings.at("mult"), "1.5");
  const std::string canonical = pack_to_json(result.value);
  CHECK_EQ(pack_to_json(parse_pack(canonical).value), canonical);
}

TEST_CASE("packs: invalid packs are explained") {
  std::string text = example_pack();
  CHECK(parse_pack(std::string(text).replace(text.find("7b1e"), 4, "XXXX"))
            .diags.has("pack.id"));
  CHECK(parse_pack(R"({"schema":1,"id":"7b1e2c4a-1f0d-4f5e-9a51-3c2f8d0e9b11",)"
                   R"("name":"x","mode":"zm","mods":[],"order":["a.b.c"]})")
            .diags.has("pack.order"));
  const std::string local =
      R"({"schema":1,"id":"7b1e2c4a-1f0d-4f5e-9a51-3c2f8d0e9b11",)"
      R"("name":"x","mode":"zm","mods":[{"id":"a.b.c","version":"1.0.0",)"
      R"("content_hash":")" +
      hash_a + R"(","settings":{"p":"C:\\Users\\me\\x"}}],"order":["a.b.c"]})";
  CHECK(parse_pack(local).diags.has("pack.local_path"));
  CHECK(parse_pack("[]").diags.has("pack.json"));
}

TEST_CASE("packs: offline share codes round trip and stay small") {
  const pack_result result = parse_pack(example_pack());
  REQUIRE(result.ok());
  const std::string code = encode_share_code(result.value);
  CHECK(code.starts_with("XRC1."));
  CHECK(code.size() < 600);
  const pack_result decoded = decode_share_code("  " + code + "\n");
  REQUIRE(decoded.ok());
  CHECK_EQ(pack_to_json(decoded.value), pack_to_json(result.value));
}

TEST_CASE("packs: damaged or oversized share codes are refused") {
  CHECK(decode_share_code("XRC2.abc").diags.has("pack.code.prefix"));
  CHECK(decode_share_code("XRC1.!!!").diags.has("pack.code.encoding"));
  const std::string code = encode_share_code(parse_pack(example_pack()).value);
  CHECK(!decode_share_code(code.substr(0, code.size() - 8)).ok());

  // A frame that declares more than 256 KiB is refused before decompressing.
  const std::string zeros(300 * 1024, '\0');
  std::string bomb(ZSTD_compressBound(zeros.size()), '\0');
  bomb.resize(
      ZSTD_compress(bomb.data(), bomb.size(), zeros.data(), zeros.size(), 3));
  CHECK(decode_share_code("XRC1." + base64url_encode(bomb))
            .diags.has("pack.code.size"));
}

TEST_CASE("packs: a pack becomes a pinned plan request") {
  const pack_result result = parse_pack(example_pack());
  REQUIRE(result.ok());
  const plan_request request =
      pack_to_request(result.value, "0x6531394", *version::parse("1.0.0"));
  CHECK_EQ(request.mode, "zm");
  CHECK_EQ(request.mods, (std::vector<std::string>{"ws.1944930083"}));
  CHECK_EQ(request.pins.at("ws.1944930083"), "0.0.0+ws.1700000000");
  CHECK_EQ(*request.map, "ws.3735179543");
  CHECK_EQ(request.settings.at("ws.1944930083").at("mult"), "1.5");
}

TEST_CASE("packs: export and import reproduce the same plan") {
  const catalog_scan scan =
      scan_mods_directory(xerces_tests::fixtures_dir() / "mods");
  plan_request request;
  request.mode = "zm";
  request.mods = {"fixture.beta", "fixture.alpha"};
  request.game_build = "0x6531394";
  request.xerces_version = *version::parse("1.0.0");
  const plan original = resolve_plan(scan.catalog, request);
  REQUIRE(original.ok());

  pack exported = pack_from_plan(original, "Both apply", new_pack_id(), {});
  exported.game_build = "0x6531394";
  CHECK(valid_uuid(exported.id));
  const pack_result imported = decode_share_code(encode_share_code(exported));
  REQUIRE(imported.ok());
  const plan again =
      resolve_plan(scan.catalog, pack_to_request(imported.value, "0x6531394",
                                                 *version::parse("1.0.0")));
  CHECK_EQ(again.plan_id, original.plan_id);
  CHECK(!verify_pack_content(imported.value, again).has_errors());

  pack changed = imported.value;
  changed.mods[0].content_hash = hash_a;
  CHECK(verify_pack_content(changed, again).has("pack.hash.mismatch"));
}

TEST_CASE("packs: a conflict allowed in Mods stays allowed in the pack") {
  // aa.one declares it does not work with bb.two, and the map declares the
  // same of cc.three. The player allowed both in Mods, and the profile also
  // holds a pair the plan does not declare and one it does not hold.
  const auto content = [](const std::string &fields) {
    const manifest_result parsed =
        parse_manifest("{\"schema\":1," + fields + "}");
    REQUIRE(!parsed.diags.has_errors());
    installed_content out;
    out.info = parsed.value;
    out.folder = out.info.id;
    out.content_hash = hash_a;
    return out;
  };
  const std::vector<installed_content> catalog = {
      content(R"("id":"aa.one","name":"One","version":"1.0.0",)"
              R"("conflicts":[{"id":"bb.two","reason":"Both rewrite perks"}])"),
      content(R"("id":"bb.two","name":"Two","version":"1.0.0")"),
      content(R"("id":"cc.three","name":"Three","version":"1.0.0")"),
      content(R"("id":"ws.42","name":"Map","version":"0.0.0","kind":"map",)"
              R"("conflicts":[{"id":"cc.three"}])")};
  plan_request request;
  request.mode = "zm";
  request.mods = {"aa.one", "bb.two", "cc.three"};
  request.map = "ws.42";
  request.game_build = "0x6531394";
  request.xerces_version = *version::parse("1.0.0");
  request.allowed_conflicts = {{"aa.one", "bb.two"},
                               {"cc.three", "ws.42"},
                               {"aa.one", "cc.three"},  // not declared
                               {"dd.four", "ee.five"}}; // not in the plan
  const plan original = resolve_plan(catalog, request);
  REQUIRE(original.ok());
  const std::set<std::pair<std::string, std::string>> allowed = {
      {"aa.one", "bb.two"}, {"cc.three", "ws.42"}};
  CHECK(original.allowed_conflicts == allowed);

  // Save as pack: the pack, its file and its share code keep both pairs.
  const pack saved = pack_from_plan(original, "Weekend", new_pack_id(), {});
  CHECK(saved.decisions.allow_conflicts == allowed);
  const pack_result from_file = parse_pack(pack_to_json(saved));
  REQUIRE(from_file.ok());
  CHECK(from_file.value.decisions.allow_conflicts == allowed);
  const pack_result from_code = decode_share_code(encode_share_code(saved));
  REQUIRE(from_code.ok());
  CHECK(from_code.value.decisions.allow_conflicts == allowed);

  // Applying it gives the same plan, with the conflicts still allowed.
  const version xerces = *version::parse("1.0.0");
  const plan applied = resolve_plan(
      catalog, pack_to_request(from_code.value, "0x6531394", xerces));
  CHECK(applied.ok());
  CHECK(applied.diags.has("plan.conflict.allowed"));
  CHECK_EQ(applied.plan_id, original.plan_id);

  // A pack without them (as saved before) fails where the plan did not.
  pack bare = saved;
  bare.decisions = {};
  CHECK(resolve_plan(catalog, pack_to_request(bare, "0x6531394", xerces))
            .diags.has("plan.conflict.declared"));
}

TEST_CASE("packs: new ids are random version 4 UUIDs") {
  const std::string a = new_pack_id();
  const std::string b = new_pack_id();
  CHECK(valid_uuid(a));
  CHECK(a != b);
  CHECK_EQ(a[14], '4');
}
