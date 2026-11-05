#pragma once

// The last lines of a text stream, such as the game console (DESIGN.md 5.10,
// 15.1). The text lives in a fixed buffer inside the object: adding text never
// allocates, and the object needs no destructor, so the game can still read it
// while it shuts down or from its crash handler. Not thread-safe; the caller
// locks.

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
class text_tail {
public:
  static constexpr std::size_t capacity = 16384;

  // Adds text as it arrives; lines may come in pieces. "\r\n" and "\r" count
  // as one line end.
  void append(std::string_view text);

  // Up to `count` of the most recent lines, oldest first, without their line
  // ends. A last line still waiting for its end is included; a line whose
  // start the buffer no longer holds is dropped, and so are empty lines.
  std::vector<std::string> last_lines(std::size_t count) const;

  void clear();

private:
  void put(char c);

  std::array<char, capacity> buffer_{};
  std::size_t next_ = 0; // where the next character goes
  bool wrapped_ = false; // the buffer has been filled at least once
  bool last_was_cr_ = false;
};
} // namespace xerces_core
