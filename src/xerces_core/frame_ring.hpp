#pragma once

// Fixed-size rings for the game's frame probe and its timing records
// (docs/DESIGN.md 12.2). Storage is set aside once, up front: pushing never
// allocates and never locks, so it is safe on the Present path and while the
// game shuts down.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace xerces_core {

// One writer thread; readers copy a range out later. The writer stores the
// item, then publishes the new count with a release store. A reader copies a
// range it saw published, then drops whatever the writer may have come round
// over while it copied.
template <typename T> class frame_ring {
  static_assert(std::is_trivially_copyable_v<T>);

public:
  // Holds 2^capacity_log2 items.
  explicit frame_ring(unsigned capacity_log2)
      : mask_((std::uint64_t{1} << capacity_log2) - 1),
        items_(std::make_unique<T[]>(static_cast<std::size_t>(mask_ + 1))) {}

  frame_ring(const frame_ring &) = delete;
  frame_ring &operator=(const frame_ring &) = delete;

  std::uint64_t capacity() const { return mask_ + 1; }

  // The writer thread only.
  void push(const T &item) {
    const std::uint64_t n = count_.load(std::memory_order_relaxed);
    items_[static_cast<std::size_t>(n & mask_)] = item;
    count_.store(n + 1, std::memory_order_release);
  }

  // Items pushed so far: the index the next push gets.
  std::uint64_t count() const { return count_.load(std::memory_order_acquire); }

  // Copies the items with an index in [begin, end) that the ring still holds
  // into `out` (replacing its contents). `end` is clamped to count(). Returns
  // the index of the first item copied: `begin`, or later when the writer had
  // already overwritten the start of the range.
  std::uint64_t copy(std::uint64_t begin, std::uint64_t end,
                     std::vector<T> &out) const {
    out.clear();
    const std::uint64_t published = count();
    if (end > published) {
      end = published;
    }
    std::uint64_t first = begin;
    // The push of index i + capacity rewrites i's slot; it may be under way
    // once count() reaches i + capacity.
    if (published + 1 > capacity() && first < published + 1 - capacity()) {
      first = published + 1 - capacity();
    }
    if (first >= end) {
      return end;
    }
    out.reserve(static_cast<std::size_t>(end - first));
    for (std::uint64_t i = first; i < end; ++i) {
      out.push_back(items_[static_cast<std::size_t>(i & mask_)]);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint64_t after = count();
    if (after + 1 > capacity()) {
      const std::uint64_t valid_from = after + 1 - capacity();
      if (valid_from > first) {
        const std::uint64_t torn = valid_from - first;
        if (torn >= out.size()) {
          out.clear();
          return end;
        }
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(torn));
        first = valid_from;
      }
    }
    return first;
  }

private:
  std::uint64_t mask_;
  std::unique_ptr<T[]> items_;
  std::atomic<std::uint64_t> count_{0};
};

// Any number of writer threads, one reader; lossy when the reader falls a
// whole ring behind. Each slot carries a sequence number: odd while a writer
// fills it, even when it holds item n (2n + 2). No destructor work, so it can
// be a static that code running during static destruction still writes to.
template <typename T, std::size_t Capacity> class event_ring {
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(Capacity > 0);

public:
  constexpr event_ring() = default;
  event_ring(const event_ring &) = delete;
  event_ring &operator=(const event_ring &) = delete;

  void push(const T &item) {
    const std::uint64_t n = next_.fetch_add(1, std::memory_order_relaxed);
    slot &s = slots_[static_cast<std::size_t>(n % Capacity)];
    s.seq.store(2 * n + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s.item = item;
    s.seq.store(2 * n + 2, std::memory_order_release);
  }

  // The reader thread only. Calls fn(item) for each item in push order that
  // is ready, stopping at the first one a writer is still filling. Returns
  // how many items were lost because writers came round over them first.
  template <typename F> std::uint64_t drain(F &&fn) {
    std::uint64_t lost = 0;
    while (true) {
      const std::uint64_t pushed = next_.load(std::memory_order_acquire);
      if (read_ >= pushed) {
        return lost;
      }
      if (pushed - read_ > Capacity) {
        lost += pushed - Capacity - read_;
        read_ = pushed - Capacity;
        continue;
      }
      slot &s = slots_[static_cast<std::size_t>(read_ % Capacity)];
      const std::uint64_t seq = s.seq.load(std::memory_order_acquire);
      if (seq < 2 * read_ + 2) {
        return lost; // still being written
      }
      T copy = s.item;
      std::atomic_thread_fence(std::memory_order_acquire);
      if (seq != 2 * read_ + 2 ||
          s.seq.load(std::memory_order_relaxed) != seq) {
        ++lost; // a later lap took the slot
        ++read_;
        continue;
      }
      ++read_;
      fn(copy);
    }
  }

private:
  struct slot {
    std::atomic<std::uint64_t> seq{0};
    T item{};
  };
  std::array<slot, Capacity> slots_{};
  std::atomic<std::uint64_t> next_{0};
  std::uint64_t read_ = 0;
};
} // namespace xerces_core
