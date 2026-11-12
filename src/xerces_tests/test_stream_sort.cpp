// The texture streamer's quicker sort (docs/DESIGN.md 12.4 item 7): the merge
// of the stretches in order must leave exactly what a stable insertion sort
// leaves, cost one comparison an item when the list is in order, and stay
// near items x log2(stretches) when it is not; and the registry must hold the
// three entries the client hooks with.

#include "harness.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <xerces_core/sigregistry.hpp>
#include <xerces_core/sigruntime.hpp>
#include <xerces_core/sigscan.hpp>
#include <xerces_core/stream_sort.hpp>

using namespace xerces_core;
namespace ss = xerces_core::stream_sort;

namespace {
// The engine's order in small: an item is an index; `matters` says which side
// of the line it is on (first key), and the rest of the order is by `place`
// (the file and offset). Several items share a place, so equal items exist.
struct order {
  std::vector<std::uint8_t> matters;
  std::vector<std::uint32_t> place;
  mutable std::uint64_t asked = 0;

  bool operator()(const std::uint32_t a, const std::uint32_t b) const {
    ++asked;
    if (matters[a] != matters[b]) {
      return matters[a] != 0;
    }
    return place[a] > place[b];
  }
};

// What the engine's insertion sort does (0x141D01870), without the unrolling
template <typename Before>
void insertion_sort(std::vector<std::uint32_t> &items, Before &&before) {
  for (std::size_t i = 1; i < items.size(); ++i) {
    const std::uint32_t value = items[i];
    std::size_t at = i;
    while (at > 0 && before(value, items[at - 1])) {
      items[at] = items[at - 1];
      --at;
    }
    items[at] = value;
  }
}

order make_order(const std::size_t count, std::mt19937 &rng) {
  order o;
  o.matters.resize(count);
  o.place.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    o.matters[i] = static_cast<std::uint8_t>(rng() % 3 == 0);
    o.place[i] = static_cast<std::uint32_t>(rng() % (count / 2 + 1));
  }
  return o;
}

std::vector<std::uint32_t> indices(const std::size_t count) {
  std::vector<std::uint32_t> items(count);
  for (std::size_t i = 0; i < count; ++i) {
    items[i] = static_cast<std::uint32_t>(i);
  }
  return items;
}

std::size_t log2_up(std::size_t value) {
  std::size_t bits = 0;
  while ((std::size_t{1} << bits) < value) {
    ++bits;
  }
  return bits;
}
} // namespace

TEST_CASE("stream sort: the same result as the engine's insertion sort") {
  std::mt19937 rng(20261112);
  ss::scratch work;
  for (const std::size_t count :
       {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3},
        std::size_t{7}, std::size_t{64}, std::size_t{257}, std::size_t{4000}}) {
    for (int round = 0; round < 6; ++round) {
      order o = make_order(count, rng);
      std::vector<std::uint32_t> items = indices(count);
      std::shuffle(items.begin(), items.end(), rng);
      std::vector<std::uint32_t> expected = items;
      insertion_sort(expected, o);

      const ss::result done =
          ss::sort(std::span<std::uint32_t>(items), std::ref(o), work);
      CHECK(items == expected);
      if (count >= 2) {
        CHECK(done.runs >= 1);
        CHECK(done.passes == log2_up(done.runs));
      }
    }
  }
}

TEST_CASE("stream sort: a list in order costs one comparison an item") {
  std::mt19937 rng(7);
  const std::size_t count = 50'000;
  order o = make_order(count, rng);
  std::vector<std::uint32_t> items = indices(count);
  insertion_sort(items, o);
  const std::vector<std::uint32_t> before = items;

  ss::scratch work;
  o.asked = 0;
  const ss::result done =
      ss::sort(std::span<std::uint32_t>(items), std::ref(o), work);
  CHECK(done.runs == 1);
  CHECK(done.passes == 0);
  CHECK(o.asked == count - 1);
  CHECK(items == before);
  // Nothing was copied: the buffer was never asked for
  CHECK(work.buffer.empty());
}

TEST_CASE("stream sort: parts that cross the line cost items x log2, not "
          "items squared") {
  // A list in order, then thousands of parts, in clumps, change sides of the
  // order's first key.
  std::mt19937 rng(99);
  const std::size_t count = 30'000;
  order o = make_order(count, rng);
  std::vector<std::uint32_t> items = indices(count);
  std::stable_sort(items.begin(), items.end(), std::ref(o));
  for (std::size_t clump = 0; clump < 120; ++clump) {
    const std::size_t at = rng() % (count - 40);
    for (std::size_t i = 0; i < 40; ++i) {
      o.matters[items[at + i]] ^= 1;
    }
  }
  std::vector<std::uint32_t> expected = items;
  std::stable_sort(expected.begin(), expected.end(), std::ref(o));

  // What the engine's sort would need: a step for every pair out of order
  std::vector<std::uint32_t> engine = items;
  o.asked = 0;
  insertion_sort(engine, o);
  const std::uint64_t engine_asked = o.asked;
  CHECK(engine == expected);

  ss::scratch work;
  o.asked = 0;
  const ss::result done =
      ss::sort(std::span<std::uint32_t>(items), std::ref(o), work);
  CHECK(items == expected);
  CHECK(done.runs > 1);
  CHECK(done.passes == log2_up(done.runs));
  // One pass to find the stretches, then at most one comparison an item a
  // pass
  CHECK(o.asked <= count * (done.passes + 1));
  // The engine's needs far more here (tens of millions against a few
  // hundred thousand)
  CHECK(engine_asked > 50 * o.asked);
}

TEST_CASE("stream sort: the worst list is still items x log2(items)") {
  const std::size_t count = 4096;
  order o;
  o.matters.assign(count, 1);
  o.place.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    o.place[i] = static_cast<std::uint32_t>(i); // largest place goes first
  }
  std::vector<std::uint32_t> items = indices(count); // exactly backwards
  ss::scratch work;
  const ss::result done =
      ss::sort(std::span<std::uint32_t>(items), std::ref(o), work);
  CHECK(done.runs == count);
  CHECK(done.passes == 12);
  CHECK(o.asked <= count * 13);
  for (std::size_t i = 0; i < count; ++i) {
    CHECK(items[i] == count - 1 - i);
  }
  // The same scratch sorts a second, shorter list
  std::vector<std::uint32_t> few = {5, 9, 7};
  ss::sort(std::span<std::uint32_t>(few), std::ref(o), work);
  CHECK((few == std::vector<std::uint32_t>{9, 7, 5}));
}

TEST_CASE("stream sort: the registry has the two calls and the comparison") {
  const auto file = xerces_tests::fixtures_dir().parent_path().parent_path() /
                    "compat" / "signatures.json";
  REQUIRE(std::filesystem::exists(file));
  const signature_registry registry =
      parse_signature_registry(xerces_tests::read_file(file));
  REQUIRE(registry.ok());
  const auto entry_of = [&](std::string_view id) -> const signature * {
    const auto it = std::ranges::find_if(
        registry.signatures, [&](const signature &s) { return s.id == id; });
    return it == registry.signatures.end() ? nullptr : &*it;
  };
  struct expected {
    std::string_view id;
    std::uint64_t rva;
    bool site;
  };
  const expected all[] = {
      {stream_sort_signatures::wanted_insertion_call, 0x1D0883C, true},
      {stream_sort_signatures::loaded_insertion_call, 0x1D08865, true},
      {stream_sort_signatures::compare, 0x1D036B0, false},
  };
  for (const auto &e : all) {
    const signature *sig = entry_of(e.id);
    REQUIRE(sig != nullptr);
    CHECK(sig->features == std::vector<std::string>{std::string(ss::feature)});
    CHECK_EQ(sig->known.at(0x6531394), e.rva);
    CHECK(sig->kind == (e.site ? symbol_kind::site : symbol_kind::function));
    const auto pattern = parse_pattern(sig->patterns.front());
    REQUIRE(pattern.has_value());
    if (e.site) {
      // The call itself, with its target left out: the client reads the
      // target and writes its own
      const auto at = static_cast<std::size_t>(sig->offset);
      REQUIRE(pattern->bytes.size() >= at + 5);
      CHECK(pattern->fixed[at]);
      CHECK_EQ(pattern->bytes[at], std::uint8_t{0xE8});
      for (std::size_t i = 1; i < 5; ++i) {
        CHECK(!pattern->fixed[at + i]);
      }
    } else {
      // Only called, never written: its first bytes are the function's own,
      // the ones the client checks before it uses the address
      const std::uint8_t entry[] = {0x48, 0x83, 0xEC, 0x08, 0xF3};
      for (std::size_t i = 0; i < 5; ++i) {
        CHECK(pattern->fixed[i]);
        CHECK_EQ(pattern->bytes[i], entry[i]);
      }
      std::vector<std::uint8_t> code(pattern->bytes.size(), 0);
      for (std::size_t i = 0; i < code.size(); ++i) {
        if (pattern->fixed[i]) {
          code[i] = pattern->bytes[i];
        }
      }
      CHECK(entry_code_matches(code, entry, *pattern));
    }
  }
  for (const auto &entry : registry.signatures) {
    if (std::ranges::find(entry.features, ss::feature) !=
        entry.features.end()) {
      CHECK(std::ranges::find(stream_sort_signatures::all, entry.id) !=
            stream_sort_signatures::all.end());
    }
  }

  // Missing from a swapped registry: only this feature goes off
  signature_report report;
  for (const std::string_view id : multi_mod_signatures::all) {
    signature_result ok;
    ok.id = std::string(id);
    ok.ok = true;
    report.results.push_back(ok);
  }
  require_signatures(report, "multi_mod", multi_mod_signatures::all, "absent");
  require_signatures(report, ss::feature, stream_sort_signatures::all,
                     "absent");
  CHECK(report.feature_enabled("multi_mod"));
  CHECK(!report.feature_enabled(ss::feature));
}
