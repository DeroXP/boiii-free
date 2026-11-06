// src/common/str.hpp, the strscpy family the game client copies names, paths
// and messages with (docs/DESIGN.md 16.5). The header is compiled into the
// tests as is. These pin what its callers rely on: the destination size
// counts the NUL, the result is always terminated, long sources are cut, and
// the mode table in party.cpp can be built at compile time.

#include "harness.hpp"

#include <cerrno>
#include <climits>
#include <cstring>
#include <string>
#include <string_view>

#include <common/str.hpp>

namespace {
// Fills a buffer with a marker so a test can see which bytes were written.
template <size_t N> void fill(char (&buffer)[N], char marker = 'x') {
  std::memset(buffer, marker, N);
}

// Every byte of a volatile buffer, read one at a time.
template <size_t N> std::string snapshot(const volatile char (&buffer)[N]) {
  std::string bytes;
  for (size_t i = 0; i < N; ++i) {
    bytes += buffer[i];
  }
  return bytes;
}

// party.cpp's GameModeConfiguration: a constexpr constructor that copies
// string_views into fixed arrays.
struct mode_entry {
  char name[8] = {};
  char gametype[4] = {};

  constexpr mode_entry(std::string_view name_val,
                       std::string_view gametype_val) {
    strscpy(name, name_val);
    strscpy(gametype, gametype_val);
  }
};

constexpr mode_entry zombies("zm", "zclassic");
static_assert(zombies.name[0] == 'z' && zombies.name[1] == 'm' &&
              zombies.name[2] == '\0');
// "zclassic" does not fit in four bytes: three characters and the NUL.
static_assert(zombies.gametype[0] == 'z' && zombies.gametype[2] == 'l' &&
              zombies.gametype[3] == '\0');

constexpr size_t copied_at_compile_time(const char *src) {
  char buffer[4] = {};
  return strscpy(buffer, src);
}
static_assert(copied_at_compile_time("abc") == 3);
static_assert(copied_at_compile_time("abcd") == strscpy_truncated);
static_assert(copied_at_compile_time("") == 0);

// The promise not to read past count bytes, or past a view. Constant
// evaluation rejects any read outside an array, so each check below stops
// compiling if the copy reads one byte too many. No source ends in a NUL.

// A C-string source exactly count bytes long: its last byte is read to see
// that it does not fit, nothing after it.
constexpr bool c_string_as_long_as_count() {
  const char src[4] = {'a', 'b', 'c', 'd'};
  char dst[4] = {};
  return strscpy(dst, src, 4) == strscpy_truncated && dst[2] == 'c' &&
         dst[3] == '\0';
}
static_assert(c_string_as_long_as_count());

// A count smaller than the destination bounds the source, too.
constexpr bool c_string_count_below_the_destination() {
  const char src[2] = {'a', 'b'};
  char dst[8] = {};
  return strscpy_pad(dst, src, 2) == strscpy_truncated && dst[0] == 'a' &&
         dst[1] == '\0';
}
static_assert(c_string_count_below_the_destination());

// gsc.hpp and script.cpp pass a view's data with count size + 1: the byte
// just past the view is read (their views end at a NUL), nothing after it.
constexpr bool view_data_with_count_size_plus_one() {
  const char src[5] = {'0', '1', '2', '3', '\0'};
  const std::string_view view(src, 4);
  char dst[5] = {};
  return strscpy(dst, view.data(), view.size() + 1) == 4 && dst[3] == '3' &&
         dst[4] == '\0';
}
static_assert(view_data_with_count_size_plus_one());

// A view exactly as long as the room fits and is read to its end only.
constexpr bool view_as_long_as_the_room() {
  const char src[3] = {'a', 'b', 'c'};
  char dst[4] = {};
  return strscpy(dst, std::string_view(src, 3)) == 3 && dst[2] == 'c' &&
         dst[3] == '\0';
}
static_assert(view_as_long_as_the_room());

// A view one longer than the room: its last character is read to see that it
// does not fit.
constexpr bool view_one_past_the_room() {
  const char src[4] = {'a', 'b', 'c', 'd'};
  char dst[4] = {};
  return strscpy(dst, std::string_view(src, 4)) == strscpy_truncated &&
         dst[2] == 'c' && dst[3] == '\0';
}
static_assert(view_one_past_the_room());

// A view shorter than the room, padded: the rest is zeroed, the source is not
// read past the view.
constexpr bool view_shorter_than_the_room_padded() {
  const char src[2] = {'h', 'i'};
  char dst[6] = {'x', 'x', 'x', 'x', 'x', 'x'};
  return strscpy_pad(dst, std::string_view(src, 2)) == 2 && dst[1] == 'i' &&
         dst[2] == '\0' && dst[5] == '\0';
}
static_assert(view_shorter_than_the_room_padded());

// An empty view that starts one past the end of an array: nothing is read.
constexpr bool empty_view_at_the_end_of_an_array() {
  const char src[2] = {'a', 'b'};
  char dst[4] = {'x', 'x', 'x', 'x'};
  return strscpy(dst, std::string_view(src + 2, 0)) == 0 && dst[0] == '\0' &&
         dst[1] == 'x';
}
static_assert(empty_view_at_the_end_of_an_array());
} // namespace

TEST_CASE("strscpy: the truncation value is -E2BIG as a size_t") {
  CHECK_EQ(strscpy_truncated, static_cast<size_t>(-E2BIG));
  // No string fits in a size_t's worth of bytes, so it cannot be a length.
  CHECK(strscpy_truncated > static_cast<size_t>(1) << 62);
}

TEST_CASE("strscpy: a source that fits is copied whole") {
  char buffer[8];
  fill(buffer);
  CHECK_EQ(strscpy(buffer, "abc"), 3u);
  CHECK_EQ(std::string(buffer), std::string("abc"));
  // Bytes after the NUL are left as they were.
  CHECK_EQ(buffer[4], 'x');
  CHECK_EQ(buffer[7], 'x');
}

TEST_CASE("strscpy: count is the destination size, NUL included") {
  // Seven characters fit in eight bytes.
  char buffer[8];
  fill(buffer);
  CHECK_EQ(strscpy(buffer, "1234567"), 7u);
  CHECK_EQ(std::string(buffer), std::string("1234567"));

  // Eight do not: seven are copied and the last byte is the NUL.
  fill(buffer);
  CHECK_EQ(strscpy(buffer, "12345678"), strscpy_truncated);
  CHECK_EQ(std::string(buffer), std::string("1234567"));
  CHECK_EQ(buffer[7], '\0');

  // A much longer source is cut the same way.
  fill(buffer);
  CHECK_EQ(strscpy(buffer, "a much longer source string"), strscpy_truncated);
  CHECK_EQ(std::string(buffer), std::string("a much "));
}

TEST_CASE("strscpy: pointer and size, as utils::string::copy and sv.cpp use") {
  char buffer[16];
  fill(buffer);
  // sv.cpp sends a config string in chunks: count 5 copies four characters.
  const char *config = "abcdefghij";
  CHECK_EQ(strscpy(buffer, config + 2, 5), strscpy_truncated);
  CHECK_EQ(std::string(buffer), std::string("cdef"));
  CHECK_EQ(buffer[5], 'x');

  // A smaller count than the buffer: only count bytes are touched.
  fill(buffer);
  CHECK_EQ(strscpy(buffer, "hello", 3), strscpy_truncated);
  CHECK_EQ(std::string(buffer), std::string("he"));
  CHECK_EQ(buffer[3], 'x');

  fill(buffer);
  CHECK_EQ(sized_strscpy(buffer, "hello", 6), 5u);
  CHECK_EQ(std::string(buffer), std::string("hello"));
  CHECK_EQ(buffer[6], 'x');
}

TEST_CASE("strscpy: no room at all") {
  char buffer[4];
  fill(buffer);
  // Count 0 writes nothing.
  CHECK_EQ(strscpy(buffer, "abc", 0), strscpy_truncated);
  CHECK_EQ(buffer[0], 'x');
  CHECK_EQ(strscpy(buffer, "", 0), strscpy_truncated);
  CHECK_EQ(buffer[0], 'x');

  // Count 1 holds the NUL only.
  CHECK_EQ(strscpy(buffer, "", 1), 0u);
  CHECK_EQ(buffer[0], '\0');
  CHECK_EQ(buffer[1], 'x');
  fill(buffer);
  CHECK_EQ(strscpy(buffer, "a", 1), strscpy_truncated);
  CHECK_EQ(buffer[0], '\0');
  CHECK_EQ(buffer[1], 'x');
}

TEST_CASE("strscpy: a count above INT_MAX is refused") {
  // byte_buffer.cpp turns an int length into the count: a negative one must
  // not become a licence to write past the buffer.
  char buffer[16];
  fill(buffer);
  const int negative = -1;
  CHECK_EQ(strscpy(buffer, "hello", static_cast<size_t>(negative)),
           strscpy_truncated);
  CHECK_EQ(buffer[0], 'x');
  CHECK_EQ(strscpy(buffer, "hello", static_cast<size_t>(INT_MAX) + 1),
           strscpy_truncated);
  CHECK_EQ(buffer[0], 'x');
  CHECK_EQ(strscpy_pad(buffer, "hello", static_cast<size_t>(-8)),
           strscpy_truncated);
  CHECK_EQ(buffer[0], 'x');

  // INT_MAX itself is a size like any other; the copy stops at the NUL.
  CHECK_EQ(strscpy(buffer, "hello", static_cast<size_t>(INT_MAX)), 5u);
  CHECK_EQ(std::string(buffer), std::string("hello"));
  CHECK_EQ(buffer[6], 'x');
}

TEST_CASE("strscpy: an empty or null source gives an empty string") {
  char buffer[4];
  fill(buffer);
  CHECK_EQ(strscpy(buffer, ""), 0u);
  CHECK_EQ(buffer[0], '\0');
  CHECK_EQ(buffer[1], 'x');

  fill(buffer);
  const char *none = nullptr;
  CHECK_EQ(strscpy(buffer, none), 0u);
  CHECK_EQ(buffer[0], '\0');
  CHECK_EQ(buffer[1], 'x');

  fill(buffer);
  CHECK_EQ(strscpy(buffer, std::string_view{}), 0u);
  CHECK_EQ(buffer[0], '\0');
}

TEST_CASE("strscpy: unterminated sources and views inside a longer string") {
  // The static_asserts at the top pin that nothing past count bytes, or past
  // a view, is read. These check the results at run time.
  const char unterminated[4] = {'a', 'b', 'c', 'd'};
  char buffer[4];
  fill(buffer);
  CHECK_EQ(strscpy(buffer, unterminated, sizeof(buffer)), strscpy_truncated);
  CHECK_EQ(std::string(buffer), std::string("abc"));

  // gsc.hpp and script.cpp copy a view's characters with count size + 1,
  // which also reads the byte just past the view. When that byte is not a
  // NUL the result says the source was cut, but the copy is still the view's
  // characters, and no byte after count is written.
  const std::string text = "0123456789";
  const std::string_view part = std::string_view(text).substr(0, 4);
  char out[16];
  fill(out);
  CHECK_EQ(strscpy(out, part.data(), part.size() + 1), strscpy_truncated);
  CHECK_EQ(std::string(out), std::string("0123"));
  CHECK_EQ(out[5], 'x');
}

TEST_CASE("strscpy: string_view sources stop at the view's end") {
  const std::string text = "name,clan";
  const std::string_view name = std::string_view(text).substr(0, 4);
  char buffer[16];
  fill(buffer);
  CHECK_EQ(strscpy(buffer, name), 4u);
  CHECK_EQ(std::string(buffer), std::string("name"));
  CHECK_EQ(buffer[5], 'x');

  // A std::string converts to a view (bots.cpp, currency.cpp).
  const std::string longer = "a name that is too long";
  char small[8];
  CHECK_EQ(strscpy(small, longer), strscpy_truncated);
  CHECK_EQ(std::string(small), std::string("a name "));

  // A view exactly as long as the room fits.
  char exact[5];
  CHECK_EQ(strscpy(exact, std::string_view("abcd")), 4u);
  CHECK_EQ(std::string(exact), std::string("abcd"));

  // A NUL inside the view ends the string.
  const std::string_view with_nul("ab\0cd", 5);
  fill(buffer);
  CHECK_EQ(strscpy(buffer, with_nul), 2u);
  CHECK_EQ(std::string(buffer), std::string("ab"));
  CHECK_EQ(buffer[3], 'x');
}

TEST_CASE("strscpy_pad: zeroes the rest of the destination") {
  char buffer[8];
  fill(buffer);
  CHECK_EQ(strscpy_pad(buffer, "abc"), 3u);
  CHECK_EQ(std::string(buffer), std::string("abc"));
  for (size_t i = 3; i < sizeof(buffer); ++i) {
    CHECK_EQ(buffer[i], '\0');
  }

  // Cut sources fill the buffer, so there is nothing left to zero.
  fill(buffer);
  CHECK_EQ(strscpy_pad(buffer, "abcdefghij"), strscpy_truncated);
  CHECK_EQ(std::string(buffer), std::string("abcdefg"));

  fill(buffer);
  CHECK_EQ(strscpy_pad(buffer, std::string_view("hello world").substr(0, 2)),
           2u);
  CHECK_EQ(std::string(buffer), std::string("he"));
  for (size_t i = 2; i < sizeof(buffer); ++i) {
    CHECK_EQ(buffer[i], '\0');
  }

  // Pointer and size: only count bytes are zeroed.
  fill(buffer);
  CHECK_EQ(strscpy_pad(buffer, "a", 4), 1u);
  CHECK_EQ(buffer[1], '\0');
  CHECK_EQ(buffer[3], '\0');
  CHECK_EQ(buffer[4], 'x');

  fill(buffer);
  CHECK_EQ(sized_strscpy_pad(buffer, "", 3), 0u);
  CHECK_EQ(buffer[0], '\0');
  CHECK_EQ(buffer[2], '\0');
  CHECK_EQ(buffer[3], 'x');

  fill(buffer);
  CHECK_EQ(strscpy_pad(buffer, "abc", 0), strscpy_truncated);
  CHECK_EQ(buffer[0], 'x');
}

TEST_CASE("strscpy: volatile destinations, as bg/cache.hpp uses") {
  volatile char buffer[6] = {'x', 'x', 'x', 'x', 'x', 'x'};
  CHECK_EQ(strscpy(buffer, "abc"), 3u);
  CHECK_EQ(snapshot(buffer), std::string("abc\0xx", 6));

  CHECK_EQ(strscpy(buffer, "toolong"), strscpy_truncated);
  CHECK_EQ(snapshot(buffer), std::string("toolo\0", 6));

  CHECK_EQ(strscpy(buffer, std::string_view("xyzw").substr(1, 2)), 2u);
  CHECK_EQ(snapshot(buffer), std::string("yz\0lo\0", 6));

  volatile char *pointer = buffer;
  CHECK_EQ(strscpy(pointer, "hi", 3), 2u);
  CHECK_EQ(snapshot(buffer), std::string("hi\0lo\0", 6));
  CHECK_EQ(strscpy(pointer, "hello", 3), strscpy_truncated);
  CHECK_EQ(snapshot(buffer), std::string("he\0lo\0", 6));
}
