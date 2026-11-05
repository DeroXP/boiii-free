#include "semver.hpp"

#include <algorithm>
#include <charconv>

namespace xerces_core {
namespace {
bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_identifier_char(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         c == '-';
}

bool all_digits(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, is_digit);
}

std::optional<std::uint64_t> parse_number(std::string_view text) {
  if (!all_digits(text) || (text.size() > 1 && text.front() == '0')) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t pos = text.find(separator, start);
    parts.push_back(text.substr(start, pos - start));
    if (pos == std::string_view::npos) {
      break;
    }
    start = pos + 1;
  }
  return parts;
}

bool valid_identifiers(std::string_view text, bool numeric_rules) {
  for (const std::string_view part : split(text, '.')) {
    if (part.empty() || !std::ranges::all_of(part, is_identifier_char)) {
      return false;
    }
    if (numeric_rules && all_digits(part) && part.size() > 1 &&
        part.front() == '0') {
      return false;
    }
  }
  return true;
}

std::strong_ordering compare_identifiers(const std::string &left,
                                         const std::string &right) {
  const bool left_numeric = all_digits(left);
  const bool right_numeric = all_digits(right);
  if (left_numeric && right_numeric) {
    if (left.size() != right.size()) {
      return left.size() <=> right.size();
    }
    return left <=> right;
  }
  if (left_numeric != right_numeric) {
    return left_numeric ? std::strong_ordering::less
                        : std::strong_ordering::greater;
  }
  return left <=> right;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

// A partial version such as "1", "1.2", "1.x" or "1.2.3-beta".
struct partial {
  std::optional<std::uint64_t> major;
  std::optional<std::uint64_t> minor;
  std::optional<std::uint64_t> patch;
  std::vector<std::string> prerelease;
  bool valid = false;

  version floor() const {
    version out;
    out.major = major.value_or(0);
    out.minor = minor.value_or(0);
    out.patch = patch.value_or(0);
    out.prerelease = prerelease;
    return out;
  }
};

bool is_wildcard(std::string_view part) {
  return part == "x" || part == "X" || part == "*";
}

partial parse_partial(std::string_view text) {
  partial out;
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
    text.remove_prefix(1);
  }
  if (text.empty() || is_wildcard(text)) {
    out.valid = true;
    return out;
  }
  std::string_view core = text;
  std::string_view pre;
  if (const std::size_t plus = core.find('+'); plus != std::string_view::npos) {
    core = core.substr(0, plus);
  }
  if (const std::size_t dash = core.find('-'); dash != std::string_view::npos) {
    pre = core.substr(dash + 1);
    core = core.substr(0, dash);
    if (!valid_identifiers(pre, true)) {
      return out;
    }
  }
  const auto parts = split(core, '.');
  if (parts.size() > 3) {
    return out;
  }
  std::optional<std::uint64_t> *slots[] = {&out.major, &out.minor, &out.patch};
  bool wildcard_seen = false;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (is_wildcard(parts[i])) {
      wildcard_seen = true;
      continue;
    }
    if (wildcard_seen) {
      return out; // "1.x.3" is not a range
    }
    const auto number = parse_number(parts[i]);
    if (!number) {
      return out;
    }
    *slots[i] = number;
  }
  if (!pre.empty()) {
    if (!out.patch) {
      return out;
    }
    for (const auto part : split(pre, '.')) {
      out.prerelease.emplace_back(part);
    }
  }
  out.valid = true;
  return out;
}

version make(std::uint64_t major, std::uint64_t minor, std::uint64_t patch,
             bool lowest_prerelease) {
  version out;
  out.major = major;
  out.minor = minor;
  out.patch = patch;
  if (lowest_prerelease) {
    out.prerelease = {"0"};
  }
  return out;
}

using comparator = version_range::comparator;
using op = version_range::op;

// Upper bound just past a partial: "1" -> <2.0.0-0, "1.2" -> <1.3.0-0.
std::optional<comparator> upper_past(const partial &p) {
  if (!p.major) {
    return std::nullopt;
  }
  if (!p.minor) {
    return comparator{op::lt, make(*p.major + 1, 0, 0, true)};
  }
  return comparator{op::lt, make(*p.major, *p.minor + 1, 0, true)};
}

bool expand_simple(std::string_view token, std::vector<comparator> &out) {
  op kind = op::eq;
  bool tilde = false;
  bool caret = false;
  if (token.starts_with(">=")) {
    kind = op::ge;
    token.remove_prefix(2);
  } else if (token.starts_with("<=")) {
    kind = op::le;
    token.remove_prefix(2);
  } else if (token.starts_with('>')) {
    kind = op::gt;
    token.remove_prefix(1);
  } else if (token.starts_with('<')) {
    kind = op::lt;
    token.remove_prefix(1);
  } else if (token.starts_with('=')) {
    token.remove_prefix(1);
  } else if (token.starts_with('~')) {
    tilde = true;
    token.remove_prefix(token.starts_with("~>") ? 2 : 1);
  } else if (token.starts_with('^')) {
    caret = true;
    token.remove_prefix(1);
  }
  token = trim(token);
  const partial p = parse_partial(token);
  if (!p.valid) {
    return false;
  }
  const bool complete = p.major && p.minor && p.patch;

  if (tilde) {
    if (!p.major) {
      return true; // ~* is any
    }
    out.push_back({op::ge, p.floor()});
    if (p.minor) {
      out.push_back({op::lt, make(*p.major, *p.minor + 1, 0, true)});
    } else {
      out.push_back({op::lt, make(*p.major + 1, 0, 0, true)});
    }
    return true;
  }
  if (caret) {
    if (!p.major) {
      return true;
    }
    out.push_back({op::ge, p.floor()});
    if (*p.major > 0 || !p.minor) {
      out.push_back({op::lt, make(*p.major + 1, 0, 0, true)});
    } else if (*p.minor > 0 || !p.patch) {
      out.push_back({op::lt, make(0, *p.minor + 1, 0, true)});
    } else {
      out.push_back({op::lt, make(0, 0, *p.patch + 1, true)});
    }
    return true;
  }

  switch (kind) {
  case op::eq:
    if (complete) {
      out.push_back({op::eq, p.floor()});
    } else if (p.major) {
      out.push_back({op::ge, p.floor()});
      out.push_back(*upper_past(p));
    }
    return true; // a bare wildcard is any
  case op::ge:
    if (p.major) {
      out.push_back({op::ge, p.floor()});
    }
    return true;
  case op::lt:
    if (!p.major) {
      out.push_back({op::lt, make(0, 0, 0, true)}); // <* matches nothing
    } else if (complete) {
      out.push_back({op::lt, p.floor()});
    } else {
      out.push_back({op::lt, make(*p.major, p.minor.value_or(0), 0, true)});
    }
    return true;
  case op::gt:
    if (!p.major) {
      out.push_back({op::lt, make(0, 0, 0, true)});
    } else if (complete) {
      out.push_back({op::gt, p.floor()});
    } else {
      // npm: >1.2 is >=1.3.0; with -0 it would admit 1.3.0-alpha.
      version bound = upper_past(p)->bound;
      bound.prerelease.clear();
      out.push_back({op::ge, bound});
    }
    return true;
  case op::le:
    if (!p.major) {
      return true;
    }
    if (complete) {
      out.push_back({op::le, p.floor()});
    } else {
      out.push_back(*upper_past(p));
    }
    return true;
  }
  return false;
}

bool expand_hyphen(std::string_view low_text, std::string_view high_text,
                   std::vector<comparator> &out) {
  const partial low = parse_partial(trim(low_text));
  const partial high = parse_partial(trim(high_text));
  if (!low.valid || !high.valid) {
    return false;
  }
  if (low.major) {
    out.push_back({op::ge, low.floor()});
  }
  if (high.major && high.minor && high.patch) {
    out.push_back({op::le, high.floor()});
  } else if (high.major) {
    out.push_back(*upper_past(high));
  }
  return true;
}

bool test(const comparator &c, const version &v) {
  const auto order = v <=> c.bound;
  switch (c.kind) {
  case op::lt:
    return order < 0;
  case op::le:
    return order <= 0;
  case op::gt:
    return order > 0;
  case op::ge:
    return order >= 0;
  case op::eq:
    return order == 0;
  }
  return false;
}
} // namespace

std::optional<version> version::parse(std::string_view text) {
  version out;
  std::string_view core = text;
  if (const std::size_t plus = core.find('+'); plus != std::string_view::npos) {
    const std::string_view build = core.substr(plus + 1);
    if (!valid_identifiers(build, false)) {
      return std::nullopt;
    }
    out.build = std::string(build);
    core = core.substr(0, plus);
  }
  if (const std::size_t dash = core.find('-'); dash != std::string_view::npos) {
    const std::string_view pre = core.substr(dash + 1);
    if (!valid_identifiers(pre, true)) {
      return std::nullopt;
    }
    for (const auto part : split(pre, '.')) {
      out.prerelease.emplace_back(part);
    }
    core = core.substr(0, dash);
  }
  const auto parts = split(core, '.');
  if (parts.size() != 3) {
    return std::nullopt;
  }
  const auto major = parse_number(parts[0]);
  const auto minor = parse_number(parts[1]);
  const auto patch = parse_number(parts[2]);
  if (!major || !minor || !patch) {
    return std::nullopt;
  }
  out.major = *major;
  out.minor = *minor;
  out.patch = *patch;
  return out;
}

std::string version::to_string() const {
  std::string out = std::to_string(major) + "." + std::to_string(minor) + "." +
                    std::to_string(patch);
  for (std::size_t i = 0; i < prerelease.size(); ++i) {
    out += (i == 0 ? "-" : ".") + prerelease[i];
  }
  if (!build.empty()) {
    out += "+" + build;
  }
  return out;
}

bool version::same_core(const version &other) const {
  return major == other.major && minor == other.minor && patch == other.patch;
}

std::strong_ordering operator<=>(const version &left, const version &right) {
  if (const auto c = left.major <=> right.major; c != 0) {
    return c;
  }
  if (const auto c = left.minor <=> right.minor; c != 0) {
    return c;
  }
  if (const auto c = left.patch <=> right.patch; c != 0) {
    return c;
  }
  if (left.prerelease.empty() != right.prerelease.empty()) {
    return left.prerelease.empty() ? std::strong_ordering::greater
                                   : std::strong_ordering::less;
  }
  const std::size_t shared =
      std::min(left.prerelease.size(), right.prerelease.size());
  for (std::size_t i = 0; i < shared; ++i) {
    if (const auto c =
            compare_identifiers(left.prerelease[i], right.prerelease[i]);
        c != 0) {
      return c;
    }
  }
  return left.prerelease.size() <=> right.prerelease.size();
}

bool operator==(const version &left, const version &right) {
  return (left <=> right) == 0;
}

std::optional<version_range> version_range::parse(std::string_view text,
                                                  std::string *error) {
  version_range out;
  out.text_ = std::string(trim(text));
  std::string_view rest = text;
  while (true) {
    const std::size_t bar = rest.find("||");
    std::string_view set_text = trim(rest.substr(0, bar));
    std::vector<comparator> set;

    if (const std::size_t hyphen = set_text.find(" - ");
        hyphen != std::string_view::npos) {
      if (!expand_hyphen(set_text.substr(0, hyphen),
                         set_text.substr(hyphen + 3), set)) {
        if (error != nullptr) {
          *error = "invalid hyphen range \"" + std::string(set_text) + "\"";
        }
        return std::nullopt;
      }
    } else {
      // Operators may be separated from their version by spaces (">= 1.2").
      std::vector<std::string> tokens;
      std::string pending;
      std::size_t i = 0;
      while (i < set_text.size()) {
        while (i < set_text.size() && set_text[i] == ' ') {
          ++i;
        }
        const std::size_t start = i;
        while (i < set_text.size() && set_text[i] != ' ') {
          ++i;
        }
        std::string token(set_text.substr(start, i - start));
        if (token.empty()) {
          continue;
        }
        const bool only_operator =
            token.find_first_not_of("<>=~^") == std::string::npos;
        if (only_operator) {
          pending += token;
          continue;
        }
        tokens.push_back(pending + token);
        pending.clear();
      }
      if (!pending.empty()) {
        if (error != nullptr) {
          *error = "operator \"" + pending + "\" has no version";
        }
        return std::nullopt;
      }
      for (const auto &token : tokens) {
        if (!expand_simple(token, set)) {
          if (error != nullptr) {
            *error = "invalid range element \"" + token + "\"";
          }
          return std::nullopt;
        }
      }
    }
    out.sets_.push_back(std::move(set));
    if (bar == std::string_view::npos) {
      break;
    }
    rest = rest.substr(bar + 2);
  }
  return out;
}

version_range version_range::any() {
  version_range out;
  out.sets_.push_back({});
  out.text_ = "*";
  return out;
}

version_range version_range::none() {
  version_range out;
  out.sets_.push_back({{op::lt, make(0, 0, 0, true)}});
  out.text_ = "<0.0.0-0";
  return out;
}

bool version_range::satisfied_by(const version &candidate) const {
  for (const auto &set : sets_) {
    if (!std::ranges::all_of(
            set, [&](const comparator &c) { return test(c, candidate); })) {
      continue;
    }
    if (!candidate.is_prerelease()) {
      return true;
    }
    const bool allowed = std::ranges::any_of(set, [&](const comparator &c) {
      return c.bound.is_prerelease() && c.bound.same_core(candidate) &&
             !(c.bound.prerelease.size() == 1 && c.bound.prerelease[0] == "0" &&
               c.kind == op::lt);
    });
    if (allowed) {
      return true;
    }
  }
  return false;
}
} // namespace xerces_core
