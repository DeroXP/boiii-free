#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
// SemVer 2.0.0. Build metadata is kept but never affects ordering.
struct version {
  std::uint64_t major = 0;
  std::uint64_t minor = 0;
  std::uint64_t patch = 0;
  std::vector<std::string> prerelease;
  std::string build;

  static std::optional<version> parse(std::string_view text);
  std::string to_string() const;
  bool is_prerelease() const { return !prerelease.empty(); }
  bool same_core(const version &other) const;

  friend std::strong_ordering operator<=>(const version &left,
                                          const version &right);
  friend bool operator==(const version &left, const version &right);
};

// npm-style version ranges: comparators (<, <=, >, >=, =), x-ranges and
// partial versions, ~ and ^, hyphen ranges, space for "and", || for "or".
// A prerelease satisfies a range only if a comparator in the same set names a
// prerelease of the same major.minor.patch (npm's rule).
class version_range {
public:
  enum class op { lt, le, gt, ge, eq };
  struct comparator {
    op kind = op::ge;
    version bound;
  };

  static std::optional<version_range> parse(std::string_view text,
                                            std::string *error = nullptr);
  static version_range any();
  static version_range none(); // satisfied by nothing

  bool satisfied_by(const version &candidate) const;
  const std::string &text() const { return text_; }

private:
  std::vector<std::vector<comparator>> sets_;
  std::string text_;
};
} // namespace xerces_core
