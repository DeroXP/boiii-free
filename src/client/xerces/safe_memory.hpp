#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Reads and writes of game memory that may be gone or not what it was taken
// for, without asking Windows about it first: VirtualQuery measures the whole
// region an address is in, and on the game's multi-gigabyte zone memory that
// took 5 to 20 ms a call (2026-11-12: 2000 calls, 9.7 s). A fault is caught
// instead. Nothing here may hold an object with a destructor.
namespace xerces_safe {
inline bool plausible(const void *address) noexcept {
  const auto value = reinterpret_cast<uintptr_t>(address);
  return value >= 0x10000 && value < 0x00007FFFFFFF0000ull;
}

inline bool read(const void *address, void *out, size_t size) noexcept {
  if (!plausible(address)) {
    return false;
  }
  __try {
    std::memcpy(out, address, size);
    return true;
  } __except (1) {
    return false;
  }
}

inline bool write(void *address, const void *in, size_t size) noexcept {
  if (!plausible(address)) {
    return false;
  }
  __try {
    std::memcpy(address, in, size);
    return true;
  } __except (1) {
    return false;
  }
}

// A printable string of 1 to cap - 1 characters at this address, copied with
// its end; 0 when there is none
inline size_t read_text(const void *address, char *out, size_t cap) noexcept {
  if (!plausible(address) || cap < 2) {
    return 0;
  }
  __try {
    const char *text = static_cast<const char *>(address);
    size_t length = 0;
    while (length + 1 < cap && text[length] >= 0x20 && text[length] < 0x7F) {
      out[length] = text[length];
      ++length;
    }
    if (length == 0 || text[length] != 0) {
      return 0;
    }
    out[length] = 0;
    return length;
  } __except (1) {
    return 0;
  }
}
} // namespace xerces_safe
