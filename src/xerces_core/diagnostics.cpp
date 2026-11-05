#include "diagnostics.hpp"

#include <algorithm>

namespace xerces_core {
const char *to_string(severity value) {
  switch (value) {
  case severity::info:
    return "info";
  case severity::warning:
    return "warning";
  case severity::error:
    return "error";
  }
  return "error";
}

void diagnostics::add(severity level, std::string code, std::string path,
                      std::string message) {
  items_.push_back(
      {level, std::move(code), std::move(path), std::move(message)});
}

void diagnostics::error(std::string code, std::string path,
                        std::string message) {
  add(severity::error, std::move(code), std::move(path), std::move(message));
}

void diagnostics::warning(std::string code, std::string path,
                          std::string message) {
  add(severity::warning, std::move(code), std::move(path), std::move(message));
}

void diagnostics::info(std::string code, std::string path,
                       std::string message) {
  add(severity::info, std::move(code), std::move(path), std::move(message));
}

void diagnostics::append(const diagnostics &other) {
  items_.insert(items_.end(), other.items_.begin(), other.items_.end());
}

bool diagnostics::has_errors() const { return count(severity::error) > 0; }

bool diagnostics::has(const std::string &code) const {
  return std::ranges::any_of(
      items_, [&](const diagnostic &item) { return item.code == code; });
}

std::size_t diagnostics::count(severity level) const {
  return static_cast<std::size_t>(std::ranges::count_if(
      items_, [&](const diagnostic &item) { return item.level == level; }));
}
} // namespace xerces_core
