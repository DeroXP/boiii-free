#include "log.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <span>
#include <vector>

#include "json.hpp"

namespace xerces_core {
namespace {
std::string replace_all_case_insensitive(std::string text,
                                         std::string_view needle,
                                         std::string_view replacement) {
  if (needle.empty()) {
    return text;
  }
  const auto lower = [](std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return value;
  };
  const std::string haystack = lower(text);
  const std::string pattern = lower(std::string(needle));
  std::string out;
  std::size_t start = 0;
  while (true) {
    const std::size_t found = haystack.find(pattern, start);
    if (found == std::string::npos) {
      out.append(text, start, std::string::npos);
      return out;
    }
    out.append(text, start, found - start);
    out.append(replacement);
    start = found + pattern.size();
  }
}

// As UTF-8, the encoding of every path xerces_core writes; the narrow
// environment is in the ANSI code page and would never match a non-ASCII
// profile folder.
std::string profile_from_environment() {
  wchar_t *value = nullptr;
  std::size_t size = 0;
  if (_wdupenv_s(&value, &size, L"USERPROFILE") != 0 || value == nullptr) {
    return {};
  }
  const std::u8string text = std::filesystem::path(value).u8string();
  std::free(value);
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

// ASCII only: the patterns below are ASCII, and testing UTF-8 bytes with the
// <cctype> functions depends on the locale.
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool is_alnum(char c) { return is_digit(c) || is_alpha(c); }
bool is_hex(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool is_word(char c) { return is_alnum(c) || c == '_'; }

// The longest texts the scanners look at. Bounding them keeps a long run of
// colons or dots from making the scrubber quadratic.
constexpr std::size_t max_ipv6_text = 64;
constexpr std::size_t max_host_text = 260;

// Length of ":<port>" at text[at] (one to five digits, then no other digit or
// letter), or 0.
std::size_t port_length(std::string_view text, std::size_t at) {
  if (at >= text.size() || text[at] != ':') {
    return 0;
  }
  std::size_t end = at + 1;
  while (end < text.size() && is_digit(text[end]) && end - at <= 5) {
    ++end;
  }
  if (end == at + 1 || (end < text.size() && is_word(text[end]))) {
    return 0;
  }
  return end - at;
}

// Length of an IPv4 address at text[at], or 0. Four numbers of up to 255
// with nothing that continues them, so a version or build number with more
// parts or bigger numbers ("10.0.26300.1") is not one.
std::size_t ipv4_length(std::string_view text, std::size_t at) {
  std::size_t end = at;
  for (int part = 0; part < 4; ++part) {
    if (part > 0) {
      if (end >= text.size() || text[end] != '.') {
        return 0;
      }
      ++end;
    }
    int value = 0;
    std::size_t digits = 0;
    while (end < text.size() && is_digit(text[end]) && digits < 3) {
      value = value * 10 + (text[end] - '0');
      ++end;
      ++digits;
    }
    if (digits == 0 || value > 255) {
      return 0;
    }
  }
  if (end < text.size() && is_word(text[end])) {
    return 0;
  }
  if (end + 1 < text.size() && text[end] == '.' && is_digit(text[end + 1])) {
    return 0;
  }
  return end - at;
}

// Length of "[IPv6]" or "[IPv6%zone]" at text[at] (a '['), or 0. Every IPv6
// address has at least two colons.
std::size_t bracketed_ipv6_length(std::string_view text, std::size_t at) {
  std::size_t end = at + 1;
  int colons = 0;
  bool zone = false;
  for (; end < text.size() && end - at < max_ipv6_text; ++end) {
    const char c = text[end];
    if (c == ']') {
      break;
    }
    if (zone) {
      if (!is_word(c) && c != '.' && c != '-') {
        return 0;
      }
    } else if (c == '%') {
      zone = true;
    } else if (c == ':') {
      ++colons;
    } else if (!is_hex(c) && c != '.') {
      return 0;
    }
  }
  if (end >= text.size() || text[end] != ']' || colons < 2) {
    return 0;
  }
  return end + 1 - at;
}

// Length of an IPv6 address without brackets at text[at], or 0. Only the
// forms that cannot be a time of day or a "key:value" pair count: one "::"
// or all eight groups, each group at most four hex digits, optionally ending
// in an IPv4 address ("::ffff:192.0.2.1").
std::size_t bare_ipv6_length(std::string_view text, std::size_t at) {
  std::size_t end = at;
  int colons = 0;
  bool double_colon = false;
  bool any_digit = false;
  std::size_t group = 0;
  for (; end < text.size() && end - at < max_ipv6_text; ++end) {
    const char c = text[end];
    if (c == ':') {
      if (end > at && text[end - 1] == ':') {
        if (double_colon) {
          return 0; // ":::" or a second "::"
        }
        double_colon = true;
      }
      ++colons;
      group = 0;
    } else if (is_hex(c)) {
      if (++group > 4) {
        return 0;
      }
      any_digit = true;
    } else {
      break;
    }
  }
  if (end < text.size() && text[end] == '.') {
    std::size_t last = end;
    while (last > at && text[last - 1] != ':') {
      --last;
    }
    const std::size_t v4 = last == at ? 0 : ipv4_length(text, last);
    if (v4 == 0) {
      return 0;
    }
    end = last + v4;
  }
  if (colons < 2 || !any_digit || (!double_colon && colons != 7)) {
    return 0;
  }
  if (end < text.size() && is_word(text[end])) {
    return 0;
  }
  return end - at;
}

// A file's name and its line have the shape of a host and a port
// ("zm_zod.gsc:412", "luautils.lua:57", "client.cpp:7"), and they are what a
// script or Lua error is found by: scrubbed as an address, the game's own log
// said "lua/shared/<address>: function expected instead of nil" and a crash
// report lost its source lines (2026-11-12). No host name ends in one of
// these, so text that does is left as it is.
bool file_ending(std::string_view label) {
  static constexpr std::string_view endings[] = {
      "lua", "gsc", "csc", "gsh", "cpp", "hpp", "c",
      "h",   "inl", "json", "cfg", "txt", "log"};
  for (const std::string_view ending : endings) {
    if (label.size() != ending.size()) {
      continue;
    }
    bool same = true;
    for (std::size_t i = 0; same && i < label.size(); ++i) {
      char c = label[i];
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c + 32);
      }
      same = c == ending[i];
    }
    if (same) {
      return true;
    }
  }
  return false;
}

// Length of "host.name:port" at text[at], or 0: at least two labels of
// letters, digits and '-' (not at either end), the last starting with a
// letter so "1.2:3" is not one and not a file's ending, then a port.
std::size_t host_port_length(std::string_view text, std::size_t at) {
  std::size_t end = at;
  std::size_t labels = 0;
  std::size_t label_start = at;
  for (;;) {
    label_start = end;
    while (end < text.size() && end - at < max_host_text &&
           (is_alnum(text[end]) || text[end] == '-')) {
      ++end;
    }
    if (end == label_start || text[label_start] == '-' ||
        text[end - 1] == '-') {
      return 0;
    }
    ++labels;
    if (end < text.size() && text[end] == '.') {
      ++end;
      continue;
    }
    break;
  }
  if (labels < 2 || !is_alpha(text[label_start]) ||
      file_ending(text.substr(label_start, end - label_start))) {
    return 0;
  }
  const std::size_t port = port_length(text, end);
  return port == 0 ? 0 : end + port - at;
}

// prefix is lower case.
bool starts_with_case_insensitive(std::string_view text, std::size_t at,
                                  std::string_view prefix) {
  if (text.size() - at < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    char c = text[at + i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
    if (c != prefix[i]) {
      return false;
    }
  }
  return true;
}

// Characters that carry a host name or a word on; a match touching one is
// part of something longer.
bool is_host_char(char c) { return is_word(c) || c == '-'; }

// Known addresses shorter than this are not looked for: a LAN host called
// "pc" would blank that word in every line and says nothing about anyone.
constexpr std::size_t min_known_text = 3;

// The known addresses as they are looked for: lower case, each also without
// its port (logs name a server either way), longest first so a match takes
// the port along when there is one.
std::vector<std::string> known_forms(std::span<const std::string> known) {
  std::vector<std::string> forms;
  for (const std::string &address : known) {
    std::string lower = address;
    std::ranges::transform(lower, lower.begin(), [](char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
    });
    std::string host = lower;
    if (!host.empty() && host.front() == '[') {
      if (const std::size_t close = host.find(']');
          close != std::string::npos) {
        host.resize(close + 1);
      }
    } else if (const std::size_t colon = host.find(':');
               colon != std::string::npos && host.rfind(':') == colon) {
      host.resize(colon); // one colon: host:port (two or more: bare IPv6)
    }
    for (std::string *form : {&lower, &host}) {
      if (form->size() >= min_known_text && form->size() <= max_host_text) {
        forms.push_back(*form);
      }
    }
  }
  std::ranges::sort(forms, [](const std::string &a, const std::string &b) {
    return a.size() != b.size() ? a.size() > b.size() : a < b;
  });
  forms.erase(std::unique(forms.begin(), forms.end()), forms.end());
  return forms;
}

// Length of a known address (and a ":port" after it) at text[at], or 0. Only
// a whole word counts: "dave.example.org" is not inside
// "dave.example.organic". A '.' after it only ends a sentence.
std::size_t known_length(std::string_view text, std::size_t at,
                         const std::vector<std::string> &forms) {
  for (const std::string &form : forms) {
    if (!starts_with_case_insensitive(text, at, form)) {
      continue;
    }
    const std::size_t end = at + form.size();
    if (end < text.size() && is_host_char(text[end])) {
      continue;
    }
    if (end + 1 < text.size() && text[end] == '.' &&
        is_host_char(text[end + 1])) {
      continue;
    }
    return form.size() + port_length(text, end);
  }
  return 0;
}

// Length of a join keyword at text[at], or 0: "+connect" (the console's join,
// and the launcher's up to 0.1.0-beta.5), "xerces_join" (the launcher's
// "+set xerces_join <address>" since) or "xerces_join_ticket" (an invite's
// ticket for that host, which is a secret too), as a whole word.
std::size_t join_word_length(std::string_view text, std::size_t at) {
  for (const std::string_view word :
       {std::string_view("+connect"), std::string_view("xerces_join_ticket"),
        std::string_view("xerces_join")}) {
    if (!starts_with_case_insensitive(text, at, word)) {
      continue;
    }
    const std::size_t end = at + word.size();
    if (end < text.size() && is_word(text[end])) {
      continue;
    }
    if (word.front() != '+' && at > 0 && is_word(text[at - 1])) {
      continue;
    }
    return word.size();
  }
  return 0;
}

std::string redact_plain(std::string_view text,
                         const std::vector<std::string> &forms) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    // "+connect <address>" and "xerces_join <address>": the join commands,
    // whatever the address looks like (a host name without a port included).
    if (const std::size_t word = join_word_length(text, i); word != 0) {
      std::size_t end = i + word;
      while (end < text.size() && (text[end] == ' ' || text[end] == '\t')) {
        ++end;
      }
      if (end < text.size() && text[end] == '"') {
        ++end; // a quoted argument
      }
      out.append(text.substr(i, end - i));
      std::size_t token = end;
      while (token < text.size() && token - end < max_host_text &&
             (is_word(text[token]) ||
              std::string_view(".:-[]%").find(text[token]) !=
                  std::string_view::npos)) {
        ++token;
      }
      if (token > end) {
        out.append(redacted_address);
      }
      i = token;
      continue;
    }

    const char before = i == 0 ? ' ' : text[i - 1];
    std::size_t length = 0;
    // A known address may follow a '.': in "www.dave.example.org" the
    // person's server is still in there.
    if (!forms.empty() && !is_host_char(before)) {
      length = known_length(text, i, forms);
    }
    // The other patterns start only where a word starts, so the tail of
    // "v1.2.3.4" or of a longer dotted number is never taken for an address.
    const bool word_start = !is_word(before) && before != '.' && before != '-';
    if (length == 0 && text[i] == '[') {
      length = bracketed_ipv6_length(text, i);
      if (length != 0) {
        length += port_length(text, i + length);
      }
    } else if (length == 0 && word_start) {
      length = ipv4_length(text, i);
      if (length != 0) {
        length += port_length(text, i + length);
      } else {
        length = host_port_length(text, i);
        if (length == 0) {
          length = bare_ipv6_length(text, i);
        }
      }
    }
    if (length != 0) {
      out.append(redacted_address);
      i += length;
    } else {
      out.push_back(text[i]);
      ++i;
    }
  }
  return out;
}

// Appends a code point as UTF-8. Half of a surrogate pair that the text
// never completed has no UTF-8 form, and the strict parser would refuse the
// line, so it becomes U+FFFD.
void append_utf8(std::string &out, std::uint32_t cp) {
  if (cp >= 0xD800 && cp <= 0xDFFF) {
    cp = 0xFFFD;
  }
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// The four hex digits of a "\u" escape at text[at].
bool read_hex4(std::string_view text, std::size_t at, std::uint32_t &value) {
  if (at > text.size() || text.size() - at < 4) {
    return false;
  }
  value = 0;
  for (std::size_t k = at; k < at + 4; ++k) {
    const char c = text[k];
    if (!is_hex(c)) {
      return false;
    }
    value = value * 16 + static_cast<std::uint32_t>(
                             is_digit(c)   ? c - '0'
                             : c >= 'a'    ? c - 'a' + 10
                                           : c - 'A' + 10);
  }
  return true;
}

// Decodes the JSON string whose body starts at text[at] (after its opening
// quote) into `decoded`. Returns where the body ends: at the closing quote,
// or, for a string that never closes (a line cut short), at the end of its
// line. Escapes JSON does not have decode to their letter; the string is
// only written back if scrubbing changed it, and then in valid form.
std::size_t decode_json_string(std::string_view text, std::size_t at,
                               std::string &decoded) {
  std::size_t j = at;
  while (j < text.size() && text[j] != '"' && text[j] != '\n' &&
         text[j] != '\r') {
    if (text[j] != '\\') {
      decoded.push_back(text[j]);
      ++j;
      continue;
    }
    if (j + 1 >= text.size() || text[j + 1] == '\n' || text[j + 1] == '\r') {
      decoded.push_back('\\');
      ++j;
      continue;
    }
    const char escape = text[j + 1];
    j += 2;
    switch (escape) {
    case 'b':
      decoded.push_back('\b');
      break;
    case 'f':
      decoded.push_back('\f');
      break;
    case 'n':
      decoded.push_back('\n');
      break;
    case 'r':
      decoded.push_back('\r');
      break;
    case 't':
      decoded.push_back('\t');
      break;
    case 'u': {
      std::uint32_t cp = 0;
      if (!read_hex4(text, j, cp)) {
        decoded.push_back('u');
        break;
      }
      j += 4;
      std::uint32_t low = 0;
      if (cp >= 0xD800 && cp <= 0xDBFF && j + 1 < text.size() &&
          text[j] == '\\' && text[j + 1] == 'u' &&
          read_hex4(text, j + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        j += 6;
      }
      append_utf8(decoded, cp);
      break;
    }
    default:
      decoded.push_back(escape); // '"', '\\' and '/'
    }
  }
  return j;
}
} // namespace

const char *to_string(log_level level) {
  switch (level) {
  case log_level::trace:
    return "trace";
  case log_level::debug:
    return "debug";
  case log_level::info:
    return "info";
  case log_level::warn:
    return "warn";
  case log_level::error:
    return "error";
  }
  return "info";
}

std::string redact_profile(std::string_view text, std::string_view profile) {
  if (profile.size() < 3) {
    return std::string(text);
  }
  std::string backslash(profile);
  std::ranges::replace(backslash, '/', '\\');
  std::string forward(backslash);
  std::ranges::replace(forward, '\\', '/');
  std::string escaped;
  for (const char c : backslash) {
    escaped += c == '\\' ? "\\\\" : std::string(1, c);
  }
  std::string out(text);
  out = replace_all_case_insensitive(std::move(out), escaped, "%USERPROFILE%");
  out =
      replace_all_case_insensitive(std::move(out), backslash, "%USERPROFILE%");
  out = replace_all_case_insensitive(std::move(out), forward, "%USERPROFILE%");
  return out;
}

std::string redact_addresses(std::string_view text,
                             std::span<const std::string> known) {
  return redact_plain(text, known_forms(known));
}

std::string redact_addresses_in_json(std::string_view text,
                                     std::span<const std::string> known) {
  const std::vector<std::string> forms = known_forms(known);
  const std::vector<std::string> no_forms;
  std::string out;
  out.reserve(text.size());
  std::size_t outside = 0; // start of the text outside strings not yet copied
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] != '"') {
      ++i;
      continue;
    }
    out += redact_plain(text.substr(outside, i - outside), no_forms);
    std::string decoded;
    const std::size_t body_end = decode_json_string(text, i + 1, decoded);
    const bool closed = body_end < text.size() && text[body_end] == '"';
    const std::size_t end = closed ? body_end + 1 : body_end;
    const std::string scrubbed = redact_plain(decoded, forms);
    if (scrubbed == decoded) {
      out.append(text.substr(i, end - i)); // as it was, escapes and all
    } else {
      std::string quoted = json::quote(scrubbed);
      if (!closed) {
        quoted.pop_back(); // it was cut short; leave it that way
      }
      out += quoted;
    }
    i = end;
    outside = end;
  }
  out += redact_plain(text.substr(outside), no_forms);
  return out;
}

json_log::json_log(std::string role, std::string session)
    : role_(std::move(role)), session_(std::move(session)),
      profile_(profile_from_environment()) {}

std::string json_log::now_utc() {
  const auto now = std::chrono::system_clock::now();
  return std::format("{:%Y-%m-%dT%H:%M:%S}Z",
                     std::chrono::floor<std::chrono::milliseconds>(now));
}

std::string json_log::format(const log_event &event,
                             std::string_view timestamp) const {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "ts", timestamp, alloc);
  json::set(doc, "level", to_string(event.level), alloc);
  json::set(doc, "role", role_, alloc);
  json::set(doc, "component", event.component, alloc);
  json::set(doc, "event", event.event, alloc);
  // Messages and data are where callers put paths, command lines and server
  // addresses; the other fields are names chosen in code.
  json::set(doc, "msg",
            redact_addresses(redact_profile(event.message, profile_)), alloc);
  json::set(doc, "session", session_, alloc);
  if (!event.plan_id.empty()) {
    json::set(doc, "plan_id", event.plan_id, alloc);
  }
  if (!event.data_json.empty()) {
    // Scrubbed string by string, decoded: the escapes in JSON text would hide
    // an address from the plain-text patterns or be cut in half by them.
    const auto parsed = json::parse(
        redact_addresses_in_json(redact_profile(event.data_json, profile_)));
    if (parsed.ok && parsed.document.IsObject()) {
      json::set(doc, "data", json::copy(parsed.document, alloc), alloc);
    }
  }
  return json::canonical(doc);
}

bool json_log::open(const std::filesystem::path &logs_dir, std::size_t keep) {
  std::error_code ec;
  std::filesystem::create_directories(logs_dir, ec);
  const auto now = std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now());
  path_ = logs_dir /
          std::format("{:%Y%m%d-%H%M%S}-{}-{}.jsonl", now, role_, session_);
  file_.open(path_, std::ios::app | std::ios::binary);

  std::vector<std::filesystem::path> sessions;
  for (const auto &entry : std::filesystem::directory_iterator(logs_dir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") {
      sessions.push_back(entry.path());
    }
  }
  std::ranges::sort(sessions);
  while (sessions.size() > keep) {
    if (sessions.front() != path_) {
      std::filesystem::remove(sessions.front(), ec);
    }
    sessions.erase(sessions.begin());
  }
  return file_.is_open();
}

void json_log::write(const log_event &event) {
  const std::string line = format(event, now_utc());
  std::lock_guard lock(mutex_);
  if (file_.is_open()) {
    file_ << line << '\n';
    file_.flush();
  }
}
} // namespace xerces_core
