#pragma once

// The sort the client puts in place of the engine's own for the texture
// streamer's two lists (src/client/xerces/stream_sort.cpp, docs/DESIGN.md
// 12.4 item 7), kept here so it can be tested without the game.
//
// The engine's r_stream_sort job puts two lists in order: the image parts it
// wants and has not loaded, and the ones it has loaded. When the first list
// is as long as it was the last time, the engine takes both to be almost in
// order already and uses an insertion sort, which is quick then and takes a
// step for every pair that is out of order otherwise: over 100 ms for a list
// of 14,000 parts in no order, and the game's main thread waits for the job
// at the start of the next frame.
//
// This is a natural merge sort: it finds the stretches that are in order
// already and merges them, pair by pair. A list in order costs one comparison
// an item and no writes, like the engine's; any other costs at most
// items x log2(stretches). It keeps equal items in the order they had, as an
// insertion sort does, so for the same list and the same order it leaves
// exactly what the engine's sort would have left.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace xerces_core::stream_sort {
// The feature's name in the signature registry and the client's report.
inline constexpr std::string_view feature = "stream_sort";

// What the sort needs besides the list; kept between calls so a sort does not
// ask for memory every time.
struct scratch {
  std::vector<std::uint32_t> buffer;
  std::vector<std::size_t> starts;
};

struct result {
  // The stretches found in order (1: the list was in order, nothing written)
  std::size_t runs = 0;
  // Passes over the list that merged stretches
  std::size_t passes = 0;
};

// Sorts `items` by `before(a, b)` ("a goes before b"), keeping equal items in
// the order they had.
template <typename Before>
result sort(std::span<std::uint32_t> items, Before &&before, scratch &work) {
  result out;
  const std::size_t count = items.size();
  if (count < 2) {
    out.runs = count;
    return out;
  }
  auto &starts = work.starts;
  starts.clear();
  starts.push_back(0);
  for (std::size_t i = 1; i < count; ++i) {
    if (before(items[i], items[i - 1])) {
      starts.push_back(i);
    }
  }
  out.runs = starts.size();
  if (out.runs == 1) {
    return out;
  }
  starts.push_back(count);

  work.buffer.resize(count);
  std::uint32_t *from = items.data();
  std::uint32_t *to = work.buffer.data();
  // starts holds runs + 1 marks; each pass halves the runs
  std::size_t runs = out.runs;
  while (runs > 1) {
    std::size_t kept = 0;
    for (std::size_t r = 0; r < runs; r += 2) {
      const std::size_t first = starts[r];
      const std::size_t middle = starts[r + 1];
      const std::size_t last = r + 1 < runs ? starts[r + 2] : middle;
      if (middle == last) {
        std::copy(from + first, from + middle, to + first);
      } else {
        // std::merge takes from the first stretch when two items are equal
        std::merge(from + first, from + middle, from + middle, from + last,
                   to + first, before);
      }
      starts[kept++] = first;
    }
    starts[kept] = count;
    runs = kept;
    std::swap(from, to);
    ++out.passes;
  }
  if (from != items.data()) {
    std::copy(from, from + count, items.data());
  }
  return out;
}
} // namespace xerces_core::stream_sort
