#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Reads and writes of game memory that may be gone or not what it was taken
// for, without asking Windows about it first: VirtualQuery measures the whole
// region an address is in, and on the game's multi-gigabyte zone memory that
// took 5 to 20 ms a call (2026-11-12: 2000 calls, 9.7 s). A fault is caught
// instead. Nothing here may hold an object with a destructor.
//
// The access itself is in a function of its own that is never inlined
// (detail::). With clang, which builds this without -fasync-exceptions, a
// __try block catches a fault raised inside a function it calls, not one
// raised by its own instructions: the first version copied inside the __try
// block, the copy was compiled to plain loads there, and a fault would have
// gone past the handler (found on 2026-11-12 by reading the exe's scope
// tables; `xerces_crash_test guard` reads a bad address through here).
namespace xerces_safe {
namespace detail {
__declspec(noinline) inline void copy(void *out, const void *in, size_t size) {
  auto *to = static_cast<volatile unsigned char *>(out);
  const auto *from = static_cast<const volatile unsigned char *>(in);
  for (size_t i = 0; i < size; ++i) {
    to[i] = from[i];
  }
}

__declspec(noinline) inline size_t text(const void *address, char *out,
                                        size_t cap) {
  const auto *from = static_cast<const volatile char *>(address);
  size_t length = 0;
  while (length + 1 < cap) {
    const char c = from[length];
    if (c < 0x20 || c >= 0x7F) {
      break;
    }
    out[length] = c;
    ++length;
  }
  if (length == 0 || from[length] != 0) {
    return 0;
  }
  out[length] = 0;
  return length;
}
} // namespace detail

inline bool plausible(const void *address) noexcept {
  const auto value = reinterpret_cast<uintptr_t>(address);
  return value >= 0x10000 && value < 0x00007FFFFFFF0000ull;
}

inline bool read(const void *address, void *out, size_t size) noexcept {
  if (!plausible(address)) {
    return false;
  }
  __try {
    detail::copy(out, address, size);
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
    detail::copy(address, in, size);
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
    return detail::text(address, out, cap);
  } __except (1) {
    return 0;
  }
}
} // namespace xerces_safe
