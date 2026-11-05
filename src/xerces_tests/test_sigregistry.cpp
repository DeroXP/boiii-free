// The signature registry file and reading a module image's layout
// (docs/DESIGN.md 4.3 and 4.5), on synthetic data.

#include "harness.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <xerces_core/sigregistry.hpp>
#include <xerces_core/sigscan.hpp>

using namespace xerces_core;

namespace {
void put16(std::vector<std::uint8_t> &bytes, std::size_t at,
           std::uint16_t value) {
  std::memcpy(bytes.data() + at, &value, 2);
}

void put32(std::vector<std::uint8_t> &bytes, std::size_t at,
           std::uint32_t value) {
  std::memcpy(bytes.data() + at, &value, 4);
}

void put_bytes(std::vector<std::uint8_t> &bytes, std::size_t at,
               std::initializer_list<std::uint8_t> values) {
  std::size_t i = at;
  for (const auto value : values) {
    bytes[i++] = value;
  }
}

void put_section(std::vector<std::uint8_t> &image, std::size_t entry,
                 const char *name, std::uint32_t rva, std::uint32_t size) {
  std::memcpy(image.data() + entry, name, std::strlen(name));
  put32(image, entry + 8, size);
  put32(image, entry + 12, rva);
}

// A PE32+ image laid out by RVA with two .text sections (0x1000 and 0x2000)
// and checksum 0x6531394, like a dump of the game.
std::vector<std::uint8_t> tiny_image() {
  std::vector<std::uint8_t> image(0x3000, 0);
  image[0] = 'M';
  image[1] = 'Z';
  put32(image, 0x3C, 0x40);
  put32(image, 0x40, 0x00004550); // "PE\0\0"
  put16(image, 0x44, 0x8664);
  put16(image, 0x46, 2);    // sections
  put16(image, 0x54, 0xF0); // SizeOfOptionalHeader
  const std::size_t optional = 0x58;
  put16(image, optional, 0x20B);
  put32(image, optional + 56, 0x3000);    // SizeOfImage
  put32(image, optional + 64, 0x6531394); // CheckSum
  put_section(image, optional + 0xF0, ".text", 0x1000, 0x1000);
  put_section(image, optional + 0xF0 + 40, ".text", 0x2000, 0x1000);
  for (std::size_t i = 0x1000; i < 0x3000; ++i) {
    image[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0x7F);
  }
  return image;
}

const char *valid_registry = R"({
  "schema": "xerces.signatures/1",
  "signatures": [
    {"id": "db.override_asset", "symbol": "DB_OverrideAsset",
     "kind": "function",
     "patterns": ["?? ?? ?? ?? ?? 48 89 5C 24 08 57"],
     "known": {"0x6531394": "0x1010"}, "features": ["multi_mod"]},
    {"id": "db.pool", "symbol": "s_assetPools", "kind": "data_rip",
     "section": ".text", "patterns": ["48 8B 05 ?? ?? ?? ?? C3"],
     "disp_offset": 3, "insn_end": 7, "note": "ignored by the parser"}
  ]
})";
} // namespace

TEST_CASE("signature registry: a valid file parses") {
  const signature_registry registry = parse_signature_registry(valid_registry);
  REQUIRE(registry.ok());
  REQUIRE_EQ(registry.signatures.size(), 2u);
  const signature &first = registry.signatures[0];
  CHECK_EQ(first.id, std::string("db.override_asset"));
  CHECK_EQ(first.symbol, std::string("DB_OverrideAsset"));
  CHECK(first.kind == symbol_kind::function);
  CHECK_EQ(first.section, std::string(".text"));
  CHECK_EQ(first.patterns.size(), 1u);
  CHECK_EQ(first.known.at(0x6531394), 0x1010u);
  CHECK(first.features == std::vector<std::string>{"multi_mod"});
  const signature &second = registry.signatures[1];
  CHECK(second.kind == symbol_kind::data_rip);
  CHECK_EQ(second.disp_offset, 3);
  CHECK_EQ(second.insn_end, 7);
  CHECK(registry.diags.empty());
}

TEST_CASE("signature registry: mistakes are reported") {
  CHECK(parse_signature_registry(R"({"schema":"other","signatures":[]})")
            .diags.has("signature.schema"));
  CHECK(parse_signature_registry("{").diags.has("signature.json"));

  const auto bad = parse_signature_registry(R"({
    "schema": "xerces.signatures/1",
    "signatures": [
      {"id": "a.one", "symbol": "A", "kind": "function", "patterns": ["C3 C3"]},
      {"id": "a.one", "symbol": "A", "kind": "function", "patterns": ["C3 C4"]},
      {"id": "Bad Id", "symbol": "B", "kind": "function", "patterns": ["C3"]},
      {"id": "c.pattern", "symbol": "C", "kind": "function", "patterns": ["?? ??"]},
      {"id": "d.data", "symbol": "D", "kind": "data_rip", "patterns": ["8B 05"]},
      {"id": "e.known", "symbol": "E", "kind": "function", "patterns": ["C3"],
       "known": {"106107796": "0x10"}},
      {"id": "f.kind", "symbol": "F", "kind": "thing", "patterns": ["C3"]},
      {"id": "g.section", "symbol": "G", "kind": "function",
       "section": ".data", "patterns": ["C3"]},
      {"id": "h.extra", "symbol": "H", "kind": "function", "patterns": ["C3"],
       "color": "red"}
    ]})");
  CHECK(!bad.ok());
  CHECK(bad.diags.has("signature.id.duplicate"));
  CHECK(bad.diags.has("signature.id"));
  CHECK(bad.diags.has("signature.pattern"));
  CHECK(bad.diags.has("signature.data_rip"));
  CHECK(bad.diags.has("signature.known.entry"));
  CHECK(bad.diags.has("signature.kind"));
  CHECK(bad.diags.has("signature.section"));
  CHECK(bad.diags.has("signature.key.unknown")); // a warning, not an error
}

TEST_CASE("signature registry: the layout of an image laid out by RVA") {
  const auto image = tiny_image();
  const auto layout = read_image_layout(image);
  REQUIRE(layout.has_value());
  CHECK_EQ(layout->checksum, 0x6531394u);
  CHECK_EQ(layout->size_of_image, 0x3000u);
  REQUIRE_EQ(layout->sections.size(), 2u);
  CHECK_EQ(layout->sections[0].name, std::string(".text"));
  CHECK_EQ(layout->sections[0].rva, 0x1000u);
  CHECK_EQ(layout->sections[0].bytes.size(), 0x1000u);
  CHECK_EQ(layout->sections[1].rva, 0x2000u);

  std::string error;
  const std::vector<std::uint8_t> truncated(image.begin(),
                                            image.begin() + 0x50);
  CHECK(!read_image_layout(truncated, &error).has_value());
  CHECK(!error.empty());
  auto not_pe = image;
  not_pe[0] = 'X';
  CHECK(!read_image_layout(not_pe).has_value());
}

TEST_CASE("signature registry: a hooked entry still resolves to its address") {
  // The running client writes a 5-byte jmp over a function it hooks, so
  // patterns made from a dump wildcard those bytes. The same bytes in the
  // second .text never count.
  auto image = tiny_image();
  put_bytes(image, 0x1010,
            {0xE9, 0x11, 0x22, 0x33, 0x44, 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57});
  put_bytes(image, 0x2010,
            {0x40, 0x53, 0x48, 0x83, 0xEC, 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57});
  put_bytes(image, 0x1200, {0x48, 0x8B, 0x05, 0x00, 0x01, 0x00, 0x00, 0xC3});
  const auto layout = read_image_layout(image);
  REQUIRE(layout.has_value());
  const signature_registry registry = parse_signature_registry(valid_registry);
  REQUIRE(registry.ok());
  const signature_report report = resolve_signatures(
      registry.signatures, layout->sections, layout->checksum);
  REQUIRE(report.find("db.override_asset")->ok);
  CHECK_EQ(report.find("db.override_asset")->rva, 0x1010u);
  REQUIRE(report.find("db.pool")->ok);
  CHECK_EQ(report.find("db.pool")->rva, 0x1200u + 7u + 0x100u);
  CHECK(report.feature_enabled("multi_mod"));
}
