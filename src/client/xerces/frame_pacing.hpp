#pragma once

// The precise frame limiter (docs/DESIGN.md 12.4 item 4; the schedule and
// the waits are xerces_core/frame_pacing.hpp). On only when the game starts
// with -xerces-precise-limiter, on the new client. Two calls reach it, both on
// the main thread: scheduler.cpp's detour of Com_Frame_Try_Block_Function,
// before the engine's frame, and client_patches.cpp's frame_limiter_sleep,
// the 1 ms wait upstream put in the engine's limiter. When it is off, each
// costs one atomic load and a branch.

#include <atomic>
#include <cstdint>

namespace xerces_frame_pacing {
namespace detail {
extern std::atomic_bool armed;
void frame_start();
void limiter_wait(std::uint32_t ms);
} // namespace detail

// Before the engine's frame.
inline void on_frame_start() {
  if (detail::armed.load(std::memory_order_acquire)) {
    detail::frame_start();
  }
}

// The engine's limiter asks for its wait. True when the pacer waited; false
// means upstream's wait runs as before.
inline bool on_limiter_wait(const std::uint32_t ms) {
  if (!detail::armed.load(std::memory_order_acquire)) {
    return false;
  }
  detail::limiter_wait(ms);
  return true;
}
} // namespace xerces_frame_pacing
