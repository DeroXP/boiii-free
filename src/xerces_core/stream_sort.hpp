#pragma once

// The sort the client puts in place of the engine's own for the texture
// streamer's two lists (src/client/xerces/stream_sort.cpp, docs/DESIGN.md
// 12.4 item 7), kept here so it can be tested without the game.
//
// The engine's r_stream_sort job puts two lists in order: the image parts it
// wants and has not loaded, and the ones it has loaded. When the first list
// is as long as it was the last time, the engine takes both to be almost in
// order already and uses an insertion sort, which is quick then and takes a
// step for every pair that is out of order otherwise: 151 ms for a list of
// 13,800 parts back to front, and the game's main thread waits for the job
// at the start of the next frame.
//
// This is a natural merge sort: it finds the stretches that are in order
// already and merges them, pair by pair. A list in order costs one comparison
// an item and no writes, like the engine's; any other costs less than
// items x (passes + 1) comparisons, passes being log2(stretches) rounded up:
// one pass to find the stretches, and one for each halving of their number.
//
// It leaves the list in order by the same comparison, and keeps equal items
// (parts on the same side of the importance line, both a part 0 or neither,
// in the same file at the same offset) in the order they had. The engine's
// insertion sort does not
// keep that order: an item it moves goes in front of the items it is equal
// to. So the two leave the same list wherever no two items are equal, and
// lists that differ only in the order of equal items otherwise; the engine's
// other sort for these lists, std::sort, does not keep that order either, so
// nothing in the game can depend on it.
//
// The engine's comparison reads values that other threads may change while a
// list is being sorted, so nothing here trusts it: whatever it answers, every
// read and write stays inside the list and the scratch, and the list ends up
// with the items it had. (That is why the merge is written out and
// std::merge is not used: it may assume its inputs are in order, and a
// checked build of the standard library stops the program when they are
// not.)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "launch.hpp"

namespace xerces_core::stream_sort {
// The feature's name in the signature registry and the client's report.
inline constexpr std::string_view feature = "stream_sort";

// The launch flag that keeps the engine's own sort (typed into the
// launcher's Settings > Launch, Extra launch flags): for telling whether a
// problem is the merge's. "-name" or "--name", before the first "+" command.
inline constexpr std::string_view engine_flag = "-xerces-stream-sort-engine";

inline bool engine_sort_requested(const std::vector<std::string> &args) {
  return has_launch_flag(args, engine_flag) ||
         has_launch_flag(args, "-" + std::string(engine_flag));
}

// The engine keeps both lists in one array of this many entries, so neither
// is ever longer.
inline constexpr std::size_t most_items = 0x30040;

// What the sort needs besides the list. Given its room once; the sort itself
// then asks for no memory, so it cannot fail with a list half sorted.
struct scratch {
  std::vector<std::uint32_t> buffer;
  std::vector<std::uint32_t> starts;

  // Room for lists of up to `items`. May throw std::bad_alloc: call it where
  // that can be handled.
  void reserve(const std::size_t items) {
    buffer.assign(items, 0);
    starts.assign(items + 1, 0);
  }
  // The longest list there is room for
  std::size_t room() const {
    return starts.empty() ? 0 : std::min(buffer.size(), starts.size() - 1);
  }
};

struct result {
  // False: the scratch has no room for this list. Nothing was read or
  // written; the caller sorts some other way.
  bool sorted = false;
  // The stretches found in order (1: the list was in order, nothing written)
  std::size_t runs = 0;
  // Passes over the list that merged stretches
  std::size_t passes = 0;
  // The pairs that were out of order. The engine's insertion sort makes a
  // move for each of them (and one more for every equal item a moved item
  // passes), so this is the least it would have cost.
  std::uint64_t steps = 0;
};

// Sorts `items` by `before(a, b)` ("a goes before b"), keeping equal items in
// the order they had.
template <typename Before>
result sort(std::span<std::uint32_t> items, Before &&before, scratch &work) {
  result out;
  const std::size_t count = items.size();
  if (count < 2) {
    out.sorted = true;
    out.runs = count;
    return out;
  }
  if (work.room() < count || count > UINT32_MAX) {
    return out;
  }
  // Where each stretch starts, and one mark past the last
  std::uint32_t *const starts = work.starts.data();
  std::size_t runs = 0;
  starts[runs++] = 0;
  for (std::size_t i = 1; i < count; ++i) {
    if (before(items[i], items[i - 1])) {
      starts[runs++] = static_cast<std::uint32_t>(i);
    }
  }
  out.sorted = true;
  out.runs = runs;
  if (runs == 1) {
    return out;
  }
  starts[runs] = static_cast<std::uint32_t>(count);

  std::uint32_t *from = items.data();
  std::uint32_t *to = work.buffer.data();
  // Each pass merges the stretches two by two and halves their number. The
  // marks of the merged stretches go to the front of `starts`, never past an
  // entry the pass has still to read.
  while (runs > 1) {
    std::size_t kept = 0;
    for (std::size_t r = 0; r < runs; r += 2) {
      const std::size_t first = starts[r];
      const std::size_t middle = starts[r + 1];
      const std::size_t last = r + 1 < runs ? starts[r + 2] : middle;
      std::size_t a = first;
      std::size_t b = middle;
      std::size_t at = first;
      while (a < middle && b < last) {
        if (before(from[b], from[a])) {
          // It passes everything left of the first stretch
          out.steps += middle - a;
          to[at++] = from[b++];
        } else {
          // Equal items: the first stretch's goes first
          to[at++] = from[a++];
        }
      }
      while (a < middle) {
        to[at++] = from[a++];
      }
      while (b < last) {
        to[at++] = from[b++];
      }
      starts[kept++] = static_cast<std::uint32_t>(first);
    }
    starts[kept] = static_cast<std::uint32_t>(count);
    runs = kept;
    std::swap(from, to);
    ++out.passes;
  }
  if (from != items.data()) {
    std::copy_n(from, count, items.data());
  }
  return out;
}
} // namespace xerces_core::stream_sort
