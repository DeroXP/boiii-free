#pragma once

// Bounded C-string copies with the strscpy interface (docs/DESIGN.md 16.5).
// The names and the contract follow the Linux kernel's documented strscpy
// API; the code was written for Xerces from that contract and from what the
// callers in src/ need. The file it replaces held GPL-2.0-only kernel code,
// which cannot be combined with this GPL-3.0 program.
//
// For a destination of `count` bytes:
// - At most count - 1 characters are copied and a NUL always follows them.
//   When count is 0, or above INT_MAX (a size made from a negative int),
//   nothing is written.
// - The return value is the number of characters copied, not counting the
//   NUL. When the source did not fit, or nothing was written, it is
//   strscpy_truncated (-E2BIG as a size_t). A source that did not fit is still
//   copied as far as it goes and terminated.
// - A C-string source is read up to its NUL and never past count bytes (the
//   byte at count - 1 is read to tell whether the source fits).
// - The string_view overloads never read past the view. A NUL inside the view
//   ends the string there.
// - A null source copies as an empty string.
// - The _pad versions also zero every byte after the NUL. The others leave
//   those bytes as they were.
// - Source and destination must not overlap.
//
// Everything except the volatile overloads works in constant expressions
// (party.cpp builds its mode table with it). The volatile overloads write
// through the volatile destination one byte at a time.

#include <cerrno>
#include <climits>
#include <cstddef>
#include <stddef.h>
#include <string_view>

// Not used here. Kept so that files which picked it up through this header
// still build.
#include <cstring>

// Defined in str.cpp (OpenBSD's strlcpy, ISC licence). constexpr functions are
// inline, so no other file can call it; nothing does.
constexpr size_t strlcpy(char *dst, const char *src, size_t siz);

// What the strscpy family returns when the source did not fit or the
// destination has no room: -E2BIG, as the kernel API documents.
inline constexpr size_t strscpy_truncated = static_cast<size_t>(-E2BIG);

namespace str_detail {
// No length limit: a C-string source ends at its NUL only.
inline constexpr size_t unbounded = static_cast<size_t>(-1);

// The one copy loop behind every function below. Char is char or volatile
// char. The source ends at its first NUL or after src_len characters,
// whichever comes first.
template <typename Char>
constexpr size_t copy(Char *dest, const char *src, size_t src_len, size_t count,
                      bool pad) noexcept {
  // A count this large can only be a bug, such as a negative int turned
  // into a size_t: refuse it rather than write past the buffer.
  if (count == 0 || count > static_cast<size_t>(INT_MAX)) {
    return strscpy_truncated;
  }
  if (src == nullptr) {
    src_len = 0;
  }

  const size_t room = count - 1;
  size_t length = 0;
  while (length < room && length < src_len && src[length] != '\0') {
    dest[length] = src[length];
    ++length;
  }

  // The loop stopped at the room limit and the source goes on.
  const bool truncated =
      length == room && length < src_len && src[length] != '\0';

  dest[length] = '\0';
  if (pad) {
    for (size_t i = length + 1; i < count; ++i) {
      dest[i] = '\0';
    }
  }
  return truncated ? strscpy_truncated : length;
}
} // namespace str_detail

// Copies src into dest, a buffer of count bytes.
inline constexpr size_t sized_strscpy(char *dest, const char *src,
                                      size_t count) {
  return str_detail::copy(dest, src, str_detail::unbounded, count, false);
}

// sized_strscpy, then zeroes the rest of dest.
inline constexpr size_t sized_strscpy_pad(char *dest, const char *src,
                                          size_t count) {
  return str_detail::copy(dest, src, str_detail::unbounded, count, true);
}

inline constexpr size_t strscpy(char *dst, const char *src, size_t size) {
  return sized_strscpy(dst, src, size);
}

inline constexpr size_t strscpy(volatile char *dst, const char *src,
                                size_t size) {
  return str_detail::copy(dst, src, str_detail::unbounded, size, false);
}

inline constexpr size_t strscpy_pad(char *dst, const char *src, size_t size) {
  return sized_strscpy_pad(dst, src, size);
}

// The array overloads take the destination size from the array.

template <size_t N>
inline constexpr size_t strscpy(char (&dst)[N], const char *src) {
  return sized_strscpy(dst, src, N);
}

template <size_t N>
inline constexpr size_t strscpy(volatile char (&dst)[N], const char *src) {
  return str_detail::copy(dst, src, str_detail::unbounded, N, false);
}

template <size_t N>
inline constexpr size_t strscpy_pad(char (&dst)[N], const char *src) {
  return sized_strscpy_pad(dst, src, N);
}

template <size_t N>
inline constexpr size_t strscpy(char (&dst)[N], const std::string_view &src) {
  return str_detail::copy(dst, src.data(), src.size(), N, false);
}

template <size_t N>
inline constexpr size_t strscpy(volatile char (&dst)[N],
                                const std::string_view &src) {
  return str_detail::copy(dst, src.data(), src.size(), N, false);
}

template <size_t N>
inline constexpr size_t strscpy_pad(char (&dst)[N],
                                    const std::string_view &src) {
  return str_detail::copy(dst, src.data(), src.size(), N, true);
}
