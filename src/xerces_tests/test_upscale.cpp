// The upscaler's rules (xerces_core/upscale.hpp, docs/DESIGN.md 13.3) and its
// registry entry.

#include "harness.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>
#include <vector>

#include <xerces_core/sigregistry.hpp>
#include <xerces_core/sigruntime.hpp>
#include <xerces_core/upscale.hpp>

using namespace xerces_core;
namespace up = xerces_core::upscale;

namespace {
float as_float(const std::uint32_t bits) { return std::bit_cast<float>(bits); }
} // namespace

TEST_CASE("upscale: FSR 1 off, or nothing to do, leaves the game's stretch") {
  CHECK(!up::plan(up::mode::off, 8, 3072, 1728, 3840, 2160).active);
  // The scene as large as the back buffer and no sharpening.
  CHECK(!up::plan(up::mode::fsr1, 0, 3840, 2160, 3840, 2160).active);
  // Larger than the back buffer (a ratio above 100%): EASU only enlarges.
  CHECK(!up::plan(up::mode::fsr1, 8, 4608, 2592, 3840, 2160).active);
  CHECK(!up::plan(up::mode::fsr1, 8, 3840, 2200, 3840, 2160).active);
  // No sizes.
  CHECK(!up::plan(up::mode::fsr1, 8, 0, 1728, 3840, 2160).active);
  CHECK(!up::plan(up::mode::fsr1, 8, 3072, 1728, 0, 0).active);
}

TEST_CASE("upscale: the passes for a smaller scene and for the same size") {
  // The user's own setup: 80% of 4K.
  const up::passes scaled = up::plan(up::mode::fsr1, 8, 3072, 1728, 3840, 2160);
  CHECK(scaled.active);
  CHECK(scaled.easu);
  CHECK(scaled.rcas);
  const up::passes unsharpened =
      up::plan(up::mode::fsr1, 0, 3072, 1728, 3840, 2160);
  CHECK(unsharpened.active);
  CHECK(unsharpened.easu);
  CHECK(!unsharpened.rcas);
  // 100%: sharpening alone.
  const up::passes native = up::plan(up::mode::fsr1, 5, 3840, 2160, 3840, 2160);
  CHECK(native.active);
  CHECK(!native.easu);
  CHECK(native.rcas);
}

TEST_CASE("upscale: sharpness 0 to 10 is 2 to 0 stops") {
  CHECK_EQ(up::rcas_stops(10), 0.0f);
  CHECK_EQ(up::rcas_stops(0), 2.0f);
  CHECK_EQ(up::rcas_stops(5), 1.0f);
  CHECK(std::fabs(up::rcas_stops(8) - 0.4f) < 1e-6f);
  // Out of range values are clamped.
  CHECK_EQ(up::rcas_stops(-3), 2.0f);
  CHECK_EQ(up::rcas_stops(14), 0.0f);
  // FsrRcasCon: 2^-stops in con.x, the rest 0 for the FP32 shader.
  const auto con = up::rcas_constants(1.0f);
  CHECK_EQ(as_float(con[0]), 0.5f);
  CHECK_EQ(con[1], 0u);
  CHECK_EQ(con[2], 0u);
  CHECK_EQ(con[3], 0u);
  CHECK_EQ(as_float(up::rcas_constants(0.0f)[0]), 1.0f);
}

TEST_CASE("upscale: EASU constants match FsrEasuCon for 3072x1728 to 4K") {
  const auto con = up::easu_constants(3072.0f, 1728.0f, 3840.0f, 2160.0f);
  // con0: output pixel to input pixel (scale, then the half-pixel offset).
  CHECK(std::fabs(as_float(con[0]) - 0.8f) < 1e-6f);
  CHECK(std::fabs(as_float(con[1]) - 0.8f) < 1e-6f);
  CHECK(std::fabs(as_float(con[2]) - (0.5f * 0.8f - 0.5f)) < 1e-6f);
  CHECK(std::fabs(as_float(con[3]) - (0.5f * 0.8f - 0.5f)) < 1e-6f);
  // con1..con3: texel steps of the input.
  const float x = 1.0f / 3072.0f;
  const float y = 1.0f / 1728.0f;
  CHECK_EQ(as_float(con[4]), x);
  CHECK_EQ(as_float(con[5]), y);
  CHECK_EQ(as_float(con[6]), x);
  CHECK_EQ(as_float(con[7]), -y);
  CHECK_EQ(as_float(con[8]), -x);
  CHECK_EQ(as_float(con[9]), 2.0f * y);
  CHECK_EQ(as_float(con[10]), x);
  CHECK_EQ(as_float(con[11]), 2.0f * y);
  CHECK_EQ(as_float(con[12]), 0.0f);
  CHECK_EQ(as_float(con[13]), 4.0f * y);
  CHECK_EQ(con[14], 0u);
  CHECK_EQ(con[15], 0u);
}

TEST_CASE("signature registry: the shipped file has the final composite") {
  const auto file = xerces_tests::fixtures_dir().parent_path().parent_path() /
                    "compat" / "signatures.json";
  REQUIRE(std::filesystem::exists(file));
  const signature_registry registry =
      parse_signature_registry(xerces_tests::read_file(file));
  REQUIRE(registry.ok());
  const auto it = std::ranges::find_if(registry.signatures, [](const auto &s) {
    return s.id == upscale_signatures::final_composite;
  });
  REQUIRE(it != registry.signatures.end());
  const signature &sig = *it;
  CHECK(std::ranges::find(sig.features, up::feature) != sig.features.end());
  CHECK(sig.kind == symbol_kind::function);
  CHECK_EQ(sig.offset, std::int64_t{0});
  REQUIRE(!sig.patterns.empty());
  for (const std::string &text : sig.patterns) {
    const auto pattern = parse_pattern(text);
    REQUIRE(pattern.has_value());
    REQUIRE(pattern->bytes.size() > up::composite_entry.size());
    // The detour's five bytes are wildcards; the client checks them itself.
    for (std::size_t i = 0; i < up::composite_entry.size(); ++i) {
      CHECK(!pattern->fixed[i]);
    }
    // The entry bytes and the pattern agree on the instruction they split:
    // mov [rax+20h], rbx is 48 89 58 20.
    CHECK(pattern->fixed[5]);
    CHECK_EQ(pattern->bytes[5], std::uint8_t{0x58});
    CHECK_EQ(pattern->bytes[6], std::uint8_t{0x20});
  }
  CHECK_EQ(sig.known.at(0x6531394), std::uint64_t{0x1C617E0});
  // Nothing else is tagged upscale_spatial.
  for (const auto &other : registry.signatures) {
    if (std::ranges::find(other.features, up::feature) !=
        other.features.end()) {
      CHECK(std::ranges::find(upscale_signatures::all, other.id) !=
            upscale_signatures::all.end());
    }
  }
  // The entry check: the dump's bytes pass, anything else does not.
  const std::vector<std::uint8_t> original = {0x48, 0x8B, 0xC4, 0x48, 0x89,
                                              0x58, 0x20, 0x55, 0x56, 0x57};
  const auto pattern = parse_pattern(sig.patterns.front());
  REQUIRE(pattern.has_value());
  std::vector<std::uint8_t> code = original;
  code.resize(pattern->bytes.size(), 0);
  for (std::size_t i = 0; i < pattern->bytes.size(); ++i) {
    if (pattern->fixed[i]) {
      code[i] = pattern->bytes[i];
    }
  }
  CHECK(entry_code_matches(code, up::composite_entry, *pattern));
  code[0] = 0xE9; // a jmp someone else wrote
  CHECK(!entry_code_matches(code, up::composite_entry, *pattern));
}
