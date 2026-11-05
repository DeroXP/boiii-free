#include "session_dvars.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace xerces_core {
namespace {
std::string lower(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  });
  return out;
}

std::string_view trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

// The whole text as a finite number, or nullopt.
std::optional<double> number(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  double value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size() ||
      !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}
} // namespace

bool same_dvar_value(std::string_view a, std::string_view b) {
  a = trim(a);
  b = trim(b);
  const std::optional<double> x = number(a);
  const std::optional<double> y = number(b);
  if (x && y) {
    return *x == *y;
  }
  return lower(a) == lower(b);
}

void session_dvars::add(std::string_view name, std::string_view value,
                        std::optional<std::string> saved) {
  entries_[lower(name)] = entry{std::string(value), std::move(saved)};
}

bool session_dvars::has(std::string_view name) const {
  return entries_.contains(lower(name));
}

bool session_dvars::changed(std::string_view name, std::string_view value) {
  const auto it = entries_.find(lower(name));
  if (it == entries_.end() || same_dvar_value(it->second.value, value)) {
    return false;
  }
  entries_.erase(it);
  return true;
}

std::optional<std::string> session_dvars::to_save(std::string_view name,
                                                  std::string_view running) {
  const auto it = entries_.find(lower(name));
  if (it == entries_.end()) {
    return std::string(running);
  }
  if (!same_dvar_value(it->second.value, running)) {
    entries_.erase(it);
    return std::string(running);
  }
  return it->second.saved;
}
} // namespace xerces_core
