#pragma once

#include <atomic>
#include <cstdint>

namespace xerces_core {
// IN_SetActiveDevice and the local client it is given (docs/TESTING.md,
// Menu crash at the first main frame). The engine indexes its
// per-local-client tables with that number, and it has two local clients
// (LOCAL_CLIENT_COUNT). Upstream's hook (client_patches.cpp, from
// f8f7b735) runs a call made off the main thread on the next main frame. A
// mouse-button change on the game window while the game loads reaches
// IN_MouseEvent on the window thread before the engine has a primary local
// client (-1); that deferred call then ran with -1, and the engine faulted
// in Storage_IsFileReady with controller -1 (the dump of 2026-11-05 17:13).
// A call for a local client outside 0..count-1 is dropped instead: there is
// no device to make active for a client the engine does not have yet.

// Whether local_client is one of the engine's: 0 to count - 1.
constexpr bool local_client_in_range(std::int32_t local_client,
                                     std::int32_t count) {
  return local_client >= 0 && local_client < count;
}

// The decision for each call, made where the original would be called (on
// the direct path and on the deferred one), with the first drop told apart
// so that it is logged once.
class active_device_filter {
public:
  enum class verdict {
    call,       // a local client the engine has: call the original
    drop_first, // out of range, the first time: drop it and log it
    drop,       // out of range again: drop it quietly
  };

  verdict check(std::int32_t local_client, std::int32_t count);
  // Calls dropped so far.
  std::uint64_t dropped() const {
    return dropped_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<std::uint64_t> dropped_{0};
};
} // namespace xerces_core
