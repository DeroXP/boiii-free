// The texture streamer's quicker sort (docs/DESIGN.md 12.4 item 7): the merge
// of the stretches in order must leave exactly what a stable sort leaves,
// which is what the engine's insertion sort leaves but for the order of equal
// items, and count the pairs out of order; cost one comparison an item when
// the list is in order, stay near
// items x log2(stretches) when it is not, touch nothing when its scratch is
// too small, and stay inside the list whatever the comparison answers; and
// the registry must hold the three entries the client hooks with.

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

// The engine's insertion sort (0x141D01870), without the unrolling: an item
// goes left only when it is before its neighbour, and then past every item
// that is not before it, the ones equal to it too. Returns the moves it made.
template <typename Before>
std::uint64_t engine_sort(std::vector<std::uint32_t> &items, Before &&before) {
  std::uint64_t moves = 0;
  for (std::size_t i = 1; i < items.size(); ++i) {
    const std::uint32_t value = items[i];
    if (!before(value, items[i - 1])) {
      continue;
    }
    std::size_t at = i;
    do {
      items[at] = items[at - 1];
      --at;
      ++moves;
    } while (at > 0 && !before(items[at - 1], value));
    items[at] = value;
  }
  return moves;
}

// Whether two lists hold the same items in order by `before`, differing at
// most in the order of items that are equal to each other
template <typename Before>
bool same_but_for_equals(const std::vector<std::uint32_t> &a,
                         const std::vector<std::uint32_t> &b, Before &&before) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (before(a[i], b[i]) || before(b[i], a[i])) {
      return false;
    }
    if (i > 0 && (before(a[i], a[i - 1]) || before(b[i], b[i - 1]))) {
      return false;
    }
  }
  std::vector<std::uint32_t> x = a;
  std::vector<std::uint32_t> y = b;
  std::sort(x.begin(), x.end());
  std::sort(y.begin(), y.end());
  return x == y;
}

// The textbook insertion sort, which keeps equal items in their order.
// Returns the moves it made: one for every pair out of order.
template <typename Before>
std::uint64_t insertion_sort(std::vector<std::uint32_t> &items,
                             Before &&before) {
  std::uint64_t moves = 0;
  for (std::size_t i = 1; i < items.size(); ++i) {
    const std::uint32_t value = items[i];
    std::size_t at = i;
    while (at > 0 && before(value, items[at - 1])) {
      items[at] = items[at - 1];
      --at;
      ++moves;
    }
    items[at] = value;
  }
  return moves;
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

ss::scratch scratch_for(const std::size_t items) {
  ss::scratch work;
  work.reserve(items);
  return work;
}
} // namespace

TEST_CASE("stream sort: what a stable sort leaves, the engine's order but "
          "for equal items, the pairs out of order counted") {
  std::mt19937 rng(20261112);
  ss::scratch work = scratch_for(4000);
  for (const std::size_t count :
       {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3},
        std::size_t{5}, std::size_t{7}, std::size_t{64}, std::size_t{257},
        std::size_t{4000}}) {
    for (int round = 0; round < 8; ++round) {
      order o = make_order(count, rng);
      std::vector<std::uint32_t> items = indices(count);
      std::shuffle(items.begin(), items.end(), rng);
      std::vector<std::uint32_t> expected = items;
      const std::uint64_t moves = insertion_sort(expected, o);
      std::vector<std::uint32_t> stable = items;
      std::stable_sort(stable.begin(), stable.end(), std::ref(o));
      CHECK(expected == stable);
      std::vector<std::uint32_t> engine = items;
      const std::uint64_t engine_moves = engine_sort(engine, o);

      const ss::result done =
          ss::sort(std::span<std::uint32_t>(items), std::ref(o), work);
      CHECK(done.sorted);
      // Equal items exist (several share a place), so this also says they
      // kept their order
      CHECK(items == stable);
      CHECK(done.steps == moves);
      // The engine's sort: the same order, equal items among themselves
      // aside, for at least as many moves
      CHECK(same_but_for_equals(items, engine, std::ref(o)));
      CHECK(engine_moves >= done.steps);
      if (count >= 2) {
        CHECK(done.runs >= 1);
        CHECK(done.runs <= count);
        CHECK(done.passes == log2_up(done.runs));
      } else {
        CHECK(done.runs == count);
        CHECK(done.passes == 0);
      }
    }
  }
}

TEST_CASE("stream sort: with no two items equal, the engine's list exactly") {
  // Every item has a place of its own, so the order of the whole list is
  // fixed: the merge and the engine's sort must agree item for item, and the
  // pairs out of order are the engine's moves.
  std::mt19937 rng(2026);
  ss::scratch work = scratch_for(3000);
  for (const std::size_t count :
       {std::size_t{2}, std::size_t{3}, std::size_t{10}, std::size_t{333},
        std::size_t{3000}}) {
    for (int round = 0; round < 6; ++round) {
      order o;
      o.matters.resize(count);
      o.place.resize(count);
      for (std::size_t i = 0; i < count; ++i) {
        o.matters[i] = static_cast<std::uint8_t>(rng() % 2);
        o.place[i] = static_cast<std::uint32_t>(i);
      }
      std::shuffle(o.place.begin(), o.place.end(), rng);
      std::vector<std::uint32_t> items = indices(count);
      std::shuffle(items.begin(), items.end(), rng);
      std::vector<std::uint32_t> engine = items;
      const std::uint64_t engine_moves = engine_sort(engine, o);

      const ss::result done =
          ss::sort(std::span<std::uint32_t>(items), std::ref(o), work);
      CHECK(done.sorted);
      CHECK(items == engine);
      CHECK(done.steps == engine_moves);
    }
  }
}

TEST_CASE("stream sort: every number of stretches, the last one unpaired or "
          "not") {
  // 1 to 33 stretches of unequal lengths: an odd count at some pass leaves a
  // stretch without a partner, which must be carried over whole
  std::mt19937 rng(5);
  ss::scratch work = scratch_for(2000);
  for (std::size_t stretches = 1; stretches <= 33; ++stretches) {
    order o;
    std::vector<std::uint32_t> items;
    for (std::size_t s = 0; s < stretches; ++s) {
      const std::size_t length = 1 + rng() % 40;
      // Inside a stretch the places fall, which is "in order" here. Each
      // stretch starts one higher than the one before it started, so its
      // first item goes before the last item of that one, whatever its
      // length: a new stretch every time. Stretches two apart share places
      // (equal items), neighbours interleave.
      for (std::size_t i = 0; i < length; ++i) {
        items.push_back(static_cast<std::uint32_t>(o.place.size()));
        o.matters.push_back(1);
        o.place.push_back(static_cast<std::uint32_t>(1001 + s - 2 * i));
      }
    }
    std::vector<std::uint32_t> expected = items;
    const std::uint64_t moves = insertion_sort(expected, o);
    const ss::result done =
        ss::sort(std::span<std::uint32_t>(items), std::ref(o), work);
    CHECK(done.sorted);
    CHECK(done.runs == stretches);
    CHECK(done.passes == log2_up(stretches));
    CHECK(items == expected);
    CHECK(done.steps == moves);
  }
}

TEST_CASE("stream sort: a list in order costs one comparison an item and no "
          "write") {
  std::mt19937 rng(7);
  const std::size_t count = 50'000;
  order o = make_order(count, rng);
  std::vector<std::uint32_t> items = indices(count);
  std::stable_sort(items.begin(), items.end(), std::ref(o));
  const std::vector<std::uint32_t> before = items;

  ss::scratch work = scratch_for(count);
  std::fill(work.buffer.begin(), work.buffer.end(), 0xABCDEF01u);
  o.asked = 0;
  const ss::result done =
      ss::sort(std::span<std::uint32_t>(items), std::ref(o), work);
  CHECK(done.sorted);
  CHECK(done.runs == 1);
  CHECK(done.passes == 0);
  CHECK(done.steps == 0);
  CHECK(o.asked == count - 1);
  CHECK(items == before);
  // Nothing was copied anywhere
  CHECK(std::ranges::all_of(work.buffer,
                            [](std::uint32_t v) { return v == 0xABCDEF01u; }));
}

TEST_CASE("stream sort: parts that change sides cost items x log2, not "
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

  // What the engine's sort needs: a move for every pair out of order, and
  // more
  std::vector<std::uint32_t> engine = items;
  o.asked = 0;
  const std::uint64_t engine_moves = engine_sort(engine, o);
  const std::uint64_t engine_asked = o.asked;
  CHECK(same_but_for_equals(engine, expected, std::ref(o)));
  std::vector<std::uint32_t> textbook = items;
  const std::uint64_t moves = insertion_sort(textbook, o);
  CHECK(textbook == expected);
  CHECK(engine_moves >= moves);

  ss::scratch work = scratch_for(count);
  o.asked = 0;
  const ss::result done =
      ss::sort(std::span<std::uint32_t>(items), std::ref(o), work);
  CHECK(done.sorted);
  CHECK(items == expected);
  CHECK(done.runs > 1);
  CHECK(done.passes == log2_up(done.runs));
  CHECK(done.steps == moves);
  // One pass to find the stretches, then less than one comparison an item a
  // pass
  CHECK(o.asked < count * (done.passes + 1));
  // The engine's needs far more here (tens of millions against a few hundred
  // thousand)
  CHECK(moves > 10'000'000);
  CHECK(engine_asked > 50 * o.asked);
}

TEST_CASE("stream sort: a list in no order, and one back to front") {
  // No order at all: the most comparisons, still under items x (passes + 1)
  std::mt19937 rng(3);
  const std::size_t count = 4096;
  order o = make_order(count, rng);
  std::vector<std::uint32_t> items = indices(count);
  std::shuffle(items.begin(), items.end(), rng);
  std::vector<std::uint32_t> expected = items;
  std::stable_sort(expected.begin(), expected.end(), std::ref(o));
  ss::scratch work = scratch_for(count);
  o.asked = 0;
  ss::result done =
      ss::sort(std::span<std::uint32_t>(items), std::ref(o), work);
  CHECK(done.sorted);
  CHECK(items == expected);
  CHECK(done.runs > count / 4);
  CHECK(done.passes == log2_up(done.runs));
  CHECK(o.asked < count * (done.passes + 1));
  CHECK(o.asked > count * 6);

  // Back to front: as many stretches as items, every pair out of order
  order falling;
  falling.matters.assign(count, 1);
  falling.place.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    falling.place[i] = static_cast<std::uint32_t>(i); // largest goes first
  }
  items = indices(count);
  done = ss::sort(std::span<std::uint32_t>(items), std::ref(falling), work);
  CHECK(done.sorted);
  CHECK(done.runs == count);
  CHECK(done.passes == 12);
  CHECK(done.steps == std::uint64_t{count} * (count - 1) / 2);
  for (std::size_t i = 0; i < count; ++i) {
    CHECK(items[i] == count - 1 - i);
  }
  // The same scratch sorts a shorter list next
  std::vector<std::uint32_t> few = {5, 9, 7};
  done = ss::sort(std::span<std::uint32_t>(few), std::ref(falling), work);
  CHECK(done.sorted);
  CHECK((few == std::vector<std::uint32_t>{9, 7, 5}));
}

TEST_CASE("stream sort: a scratch too small leaves the list alone") {
  order o;
  o.matters.assign(8, 1);
  o.place = {3, 1, 4, 1, 5, 9, 2, 6};
  std::vector<std::uint32_t> items = indices(8);
  const std::vector<std::uint32_t> before = items;

  ss::scratch none;
  CHECK(none.room() == 0);
  ss::result done =
      ss::sort(std::span<std::uint32_t>(items), std::ref(o), none);
  CHECK(!done.sorted);
  CHECK(items == before);
  CHECK(o.asked == 0);

  ss::scratch small = scratch_for(7);
  CHECK(small.room() == 7);
  // The marks need one entry more than the buffer: eight and eight is room
  // for seven
  ss::scratch uneven;
  uneven.buffer.assign(8, 0);
  uneven.starts.assign(8, 0);
  CHECK(uneven.room() == 7);
  done = ss::sort(std::span<std::uint32_t>(items), std::ref(o), small);
  CHECK(!done.sorted);
  CHECK(items == before);
  CHECK(o.asked == 0);

  // Lists of 0 and 1 need no scratch
  std::vector<std::uint32_t> one = {4};
  CHECK(ss::sort(std::span<std::uint32_t>(one), std::ref(o), none).sorted);
  CHECK(ss::sort(std::span<std::uint32_t>(), std::ref(o), none).sorted);

  ss::scratch enough = scratch_for(8);
  CHECK(enough.room() == 8);
  done = ss::sort(std::span<std::uint32_t>(items), std::ref(o), enough);
  CHECK(done.sorted);
  std::vector<std::uint32_t> expected = before;
  insertion_sort(expected, o);
  CHECK(items == expected);

  // The engine's whole array fits the scratch the client makes
  CHECK(ss::most_items == 0x30040);
}

TEST_CASE("stream sort: a comparison that contradicts itself cannot lose or "
          "invent an item") {
  // The engine's comparison reads values other threads change. Whatever it
  // answers, the list must come back with the items it had, and nothing may
  // be written past the list or past the part of the scratch a list of that
  // length may use (its buffer's first `count` entries, `count + 1` marks):
  // the entries after those carry a mark of their own. Every other round the
  // scratch has exactly that room and nothing spare, as the client's has for
  // the longest list.
  const std::uint32_t list_mark = 0xFEEDFACEu;
  const std::uint32_t scratch_mark = 0xC0FFEE00u;
  std::mt19937 rng(41);
  for (const std::size_t count :
       {std::size_t{2}, std::size_t{3}, std::size_t{17}, std::size_t{1000},
        std::size_t{5000}}) {
    for (int round = 0; round < 6; ++round) {
      std::vector<std::uint32_t> items = indices(count);
      std::shuffle(items.begin(), items.end(), rng);
      // The list inside a larger block, with marks around it that a stray
      // write would spoil
      const std::size_t guard = 16;
      const bool exact = round % 2 == 1;
      ss::scratch work;
      if (exact) {
        work = scratch_for(count);
      } else {
        work.buffer.assign(count + guard, scratch_mark);
        work.starts.assign(count + 1 + guard, scratch_mark);
      }
      CHECK(work.room() == (exact ? count : count + guard));
      std::vector<std::uint32_t> block(count + 2 * guard, list_mark);
      std::copy(items.begin(), items.end(), block.begin() + guard);

      std::mt19937 coin(rng());
      const auto liar = [&coin](std::uint32_t, std::uint32_t) {
        return coin() % 2 == 0;
      };
      const ss::result done = ss::sort(
          std::span<std::uint32_t>(block.data() + guard, count), liar, work);
      CHECK(done.sorted);
      CHECK(done.runs >= 1);
      CHECK(done.runs <= count);
      for (std::size_t i = 0; i < guard; ++i) {
        CHECK(block[i] == list_mark);
        CHECK(block[guard + count + i] == list_mark);
      }
      std::vector<std::uint32_t> got(block.begin() + guard,
                                     block.begin() + guard + count);
      std::sort(got.begin(), got.end());
      CHECK(got == indices(count));
      if (exact) {
        CHECK(work.buffer.size() == count);
        CHECK(work.starts.size() == count + 1);
      } else {
        REQUIRE(work.buffer.size() == count + guard);
        REQUIRE(work.starts.size() == count + 1 + guard);
        for (std::size_t i = 0; i < guard; ++i) {
          CHECK(work.buffer[count + i] == scratch_mark);
          CHECK(work.starts[count + 1 + i] == scratch_mark);
        }
      }
    }
  }
}

TEST_CASE("stream sort: the launch flag that keeps the game's own sort") {
  using args = std::vector<std::string>;
  CHECK(!ss::engine_sort_requested(args{}));
  CHECK(!ss::engine_sort_requested(args{"-launch", "-borderless"}));
  CHECK(ss::engine_sort_requested(args{"-xerces-stream-sort-engine"}));
  CHECK(ss::engine_sort_requested(
      args{"-launch", "--xerces-stream-sort-engine", "+set", "fs_game", "x"}));
  // Not after the first "+" command, where the client stops reading flags,
  // and not a longer or shorter name
  CHECK(!ss::engine_sort_requested(
      args{"+set", "fs_game", "x", "-xerces-stream-sort-engine"}));
  CHECK(!ss::engine_sort_requested(args{"-xerces-stream-sort"}));
  CHECK(!ss::engine_sort_requested(args{"-xerces-stream-sort-engines"}));
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
  CHECK(std::size(all) == stream_sort_signatures::all.size());
  for (const auto &e : all) {
    CHECK(std::ranges::find(stream_sort_signatures::all, e.id) !=
          stream_sort_signatures::all.end());
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
      // the five the client names when it checks the whole pattern again
      // before it uses the address. One of them changed must not pass.
      const std::uint8_t entry[] = {0x48, 0x83, 0xEC, 0x08, 0xF3};
      CHECK_EQ(sig->offset, std::int64_t{0});
      REQUIRE(pattern->bytes.size() >= 5);
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
      // The comparison's use of its arguments (the last fixed byte is part
      // of it) is in the pattern: other code there is refused
      std::vector<std::uint8_t> other = code;
      other.back() ^= 0xFF;
      CHECK(pattern->fixed[other.size() - 1]);
      CHECK(!entry_code_matches(other, entry, *pattern));
      const std::uint8_t wrong_entry[] = {0x48, 0x83, 0xEC, 0x28, 0xF3};
      CHECK(!entry_code_matches(code, wrong_entry, *pattern));
    }
  }
  // Nothing else in the registry is tagged for this feature
  for (const auto &entry : registry.signatures) {
    if (std::ranges::find(entry.features, ss::feature) !=
        entry.features.end()) {
      CHECK(std::ranges::find(stream_sort_signatures::all, entry.id) !=
            stream_sort_signatures::all.end());
    }
  }
}
