#include "text_tail.hpp"

#include <algorithm>

namespace xerces_core {
void text_tail::put(char c) {
  buffer_[next_] = c;
  next_ = (next_ + 1) % capacity;
  if (next_ == 0) {
    wrapped_ = true;
  }
}

void text_tail::append(std::string_view text) {
  for (const char c : text) {
    if (c == '\r') {
      put('\n');
      last_was_cr_ = true;
      continue;
    }
    if (c == '\n' && last_was_cr_) {
      last_was_cr_ = false; // the second half of "\r\n"
      continue;
    }
    last_was_cr_ = false;
    put(c);
  }
}

std::vector<std::string> text_tail::last_lines(std::size_t count) const {
  std::string text;
  if (wrapped_) {
    text.assign(buffer_.data() + next_, capacity - next_);
    text.append(buffer_.data(), next_);
    // The oldest line has lost its start.
    const auto first_end = text.find('\n');
    text.erase(0, first_end == std::string::npos ? text.size() : first_end + 1);
  } else {
    text.assign(buffer_.data(), next_);
  }

  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    const auto stop = end == std::string::npos ? text.size() : end;
    if (stop > start) {
      lines.emplace_back(text.substr(start, stop - start));
    }
    start = stop + 1;
  }
  if (lines.size() > count) {
    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(
                                                   lines.size() - count));
  }
  return lines;
}

void text_tail::clear() {
  next_ = 0;
  wrapped_ = false;
  last_was_cr_ = false;
}
} // namespace xerces_core
