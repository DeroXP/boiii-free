#include "harness.hpp"

#include <xerces_core/json.hpp>
#include <xerces_core/semver.hpp>
#include <xerces_core/sha256.hpp>

using namespace xerces_core;

TEST_CASE("sha256: FIPS 180-4 test vectors") {
  CHECK_EQ(sha256::hex(""),
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK_EQ(sha256::hex("abc"),
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK_EQ(
      sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK_EQ(sha256::hex(std::string(1000000, 'a')),
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256: incremental updates match one shot") {
  const std::string text(1000, 'x');
  sha256 hasher;
  for (std::size_t i = 0; i < text.size(); i += 37) {
    hasher.update(std::string_view(text).substr(i, 37));
  }
  CHECK_EQ(sha256::to_hex(hasher.finish()), sha256::hex(text));
  CHECK(sha256_tagged("abc").starts_with("sha256:ba7816bf"));
}

TEST_CASE("semver: parse and print") {
  const auto v = version::parse("1.2.3-alpha.1+build.5");
  REQUIRE(v.has_value());
  CHECK_EQ(v->major, 1u);
  CHECK_EQ(v->prerelease.size(), 2u);
  CHECK_EQ(v->build, "build.5");
  CHECK_EQ(v->to_string(), "1.2.3-alpha.1+build.5");
  CHECK(version::parse("0.0.0+ws.1700000000").has_value());
  for (const char *bad : {"1.2", "01.2.3", "1.2.3-01", "1.2.3-", "1.2.3+",
                          "a.b.c", "1.2.3.4", "", "-1.2.3"}) {
    CHECK(!version::parse(bad).has_value());
  }
}

TEST_CASE("semver: precedence follows the specification") {
  const char *ordered[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta",
                           "1.0.0-beta",  "1.0.0-beta.2",  "1.0.0-beta.11",
                           "1.0.0-rc.1",  "1.0.0",         "1.0.1",
                           "1.1.0",       "2.0.0"};
  for (std::size_t i = 0; i + 1 < std::size(ordered); ++i) {
    const auto a = version::parse(ordered[i]);
    const auto b = version::parse(ordered[i + 1]);
    REQUIRE(a && b);
    CHECK(*a < *b);
  }
  CHECK(*version::parse("1.0.0+a") == *version::parse("1.0.0+b"));
}

namespace {
bool satisfies(const char *range, const char *candidate) {
  const auto r = version_range::parse(range);
  const auto v = version::parse(candidate);
  return r && v && r->satisfied_by(*v);
}
} // namespace

TEST_CASE("semver: npm-style ranges") {
  CHECK(satisfies("^1.4.0", "1.4.0"));
  CHECK(satisfies("^1.4.0", "1.9.9"));
  CHECK(!satisfies("^1.4.0", "2.0.0"));
  CHECK(!satisfies("^1.4.0", "1.3.9"));
  CHECK(!satisfies("^1.4.0", "2.0.0-alpha"));
  CHECK(satisfies("^0.2.3", "0.2.9"));
  CHECK(!satisfies("^0.2.3", "0.3.0"));
  CHECK(satisfies("^0.0.3", "0.0.3"));
  CHECK(!satisfies("^0.0.3", "0.0.4"));
  CHECK(satisfies("~1.2.3", "1.2.9"));
  CHECK(!satisfies("~1.2.3", "1.3.0"));
  CHECK(satisfies("~1.2", "1.2.0"));
  CHECK(!satisfies("~1", "2.0.0"));
  CHECK(satisfies("~1", "1.9.0"));
  CHECK(satisfies(">=1.0 <2", "1.0.0"));
  CHECK(satisfies(">=1.0 <2", "1.99.0"));
  CHECK(!satisfies(">=1.0 <2", "2.0.0"));
  CHECK(satisfies("1.2.x", "1.2.5"));
  CHECK(!satisfies("1.2.x", "1.3.0"));
  CHECK(satisfies("*", "3.4.5"));
  CHECK(!satisfies("*", "1.0.0-beta"));
  CHECK(satisfies("1.2.3 - 2.3.4", "2.3.4"));
  CHECK(!satisfies("1.2.3 - 2.3.4", "2.3.5"));
  CHECK(satisfies("1.2 - 2", "2.9.9"));
  CHECK(!satisfies("1.2 - 2", "3.0.0"));
  CHECK(satisfies("<1.2", "1.1.9"));
  CHECK(!satisfies("<1.2", "1.2.0"));
  CHECK(satisfies(">1.2", "1.3.0"));
  CHECK(!satisfies(">1.2", "1.2.9"));
  CHECK(satisfies("<=1.2", "1.2.9"));
  CHECK(!satisfies("<=1.2", "1.3.0"));
  CHECK(satisfies(">1", "2.0.0"));
  CHECK(!satisfies(">1", "1.9.9"));
  CHECK(satisfies("1.x || >=2.5.0", "1.5.0"));
  CHECK(!satisfies("1.x || >=2.5.0", "2.4.0"));
  CHECK(satisfies("1.x || >=2.5.0", "2.5.0"));
  CHECK(satisfies(">= 1.2.3", "1.2.3"));
  CHECK(satisfies("=1.2.3", "1.2.3"));
  CHECK(satisfies("1.2.3", "1.2.3+build"));
}

TEST_CASE("semver: prereleases only match ranges that name them") {
  CHECK(satisfies(">=1.2.3-beta.2", "1.2.3-beta.4"));
  CHECK(!satisfies(">=1.2.3-beta.2", "1.2.4-beta.1"));
  CHECK(satisfies("^1.2.3-beta.2", "1.2.3-beta.4"));
  CHECK(satisfies("^1.2.3-beta.2", "1.3.0"));
  CHECK(!satisfies("^1.2.3-beta.2", "1.3.0-beta"));
}

TEST_CASE("semver: invalid ranges are rejected with a reason") {
  std::string error;
  CHECK(!version_range::parse(">=", &error).has_value());
  CHECK(!error.empty());
  CHECK(!version_range::parse("1.2.3.4").has_value());
  CHECK(!version_range::parse("foo").has_value());
  CHECK(!version_range::parse("1.x.3").has_value());
}

TEST_CASE("json: canonical form sorts keys and drops whitespace") {
  const auto parsed =
      json::parse(R"({ "b": 1, "a": { "d": true, "c": null }, "e": [3, 2] })");
  REQUIRE(parsed.ok);
  CHECK_EQ(json::canonical(parsed.document),
           R"({"a":{"c":null,"d":true},"b":1,"e":[3,2]})");
}

TEST_CASE("json: canonical strings and numbers") {
  const auto parsed = json::parse(
      "{\"s\":\"a\\\"b\\\\c\\n\\u0001\",\"x\":1.0,\"y\":0.5,\"z\":-3}");
  REQUIRE(parsed.ok);
  CHECK_EQ(json::canonical(parsed.document),
           "{\"s\":\"a\\\"b\\\\c\\n\\u0001\",\"x\":1,\"y\":0.5,\"z\":-3}");
}

TEST_CASE("json: parse errors report line and column") {
  const auto parsed = json::parse("{\n  \"a\": ,\n}");
  CHECK(!parsed.ok);
  CHECK_EQ(parsed.line, 2u);
  CHECK(!parsed.error.empty());
  CHECK(!json::parse("\"\xff\"").ok); // invalid UTF-8
}

TEST_CASE("json: pointers escape ~ and /") {
  CHECK_EQ(json::pointer("/a", "b/c~d"), "/a/b~1c~0d");
  CHECK_EQ(json::pointer("/list", std::size_t{3}), "/list/3");
}
