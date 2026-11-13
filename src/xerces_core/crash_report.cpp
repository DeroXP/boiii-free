#include "crash_report.hpp"

// Pure: no Windows calls here, so the game client, which links this file for
// its pending crash note, gains no imports from it. The PC's facts and the
// random ids are in crash_system.cpp.

#include <algorithm>
#include <array>
#include <cstdio>
#include <format>

#include "json.hpp"
#include "launcher_config.hpp"
#include "log.hpp"

namespace xerces_core {
namespace {
using rapidjson::Value;

std::string cap(std::string_view text, std::size_t bytes = crash_name_cap) {
  return json::valid_utf8(text, bytes);
}

Value optional_string(const std::optional<std::string> &text,
                      json::allocator &alloc) {
  return text ? json::string_value(cap(*text), alloc) : Value();
}

char lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

bool hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

bool equal_at(std::string_view text, std::size_t at, std::string_view word) {
  if (at + word.size() > text.size()) {
    return false;
  }
  for (std::size_t i = 0; i < word.size(); ++i) {
    if (lower(text[at + i]) != lower(word[i])) {
      return false;
    }
  }
  return true;
}

// A word may start at i: the text's start, a character that is not part of
// a word, or the end of a colour code ("^1Name", as the console prints
// player names).
bool word_start(std::string_view text, std::size_t i) {
  return i == 0 || !word_char(text[i - 1]) ||
         (i >= 2 && text[i - 2] == '^' && text[i - 1] >= '0' &&
          text[i - 1] <= '9');
}

// Every whole-word copy of word (ASCII case ignored) becomes replacement.
std::string replace_word(std::string_view text, std::string_view word,
                         std::string_view replacement) {
  if (word.empty()) {
    return std::string(text);
  }
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (equal_at(text, i, word) && word_start(text, i) &&
        (i + word.size() == text.size() || !word_char(text[i + word.size()]))) {
      out += replacement;
      i += word.size();
    } else {
      out.push_back(text[i++]);
    }
  }
  return out;
}

// "X:\Users\<name>", in plain text ('\' or '/') and in JSON text ("\\"): the
// name becomes <user>. A profile folder redact_profile did not know (the
// game run by another account, a path typed by hand) still hides its owner.
std::string redact_user_paths(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    // A drive letter and ':' then one or two separators, then "users".
    const bool drive = i + 2 < text.size() && word_char(text[i]) &&
                       text[i + 1] == ':' &&
                       (i == 0 || !word_char(text[i - 1]));
    if (drive) {
      std::size_t at = i + 2;
      std::size_t seps = 0;
      while (at < text.size() && (text[at] == '\\' || text[at] == '/') &&
             seps < 2) {
        ++at;
        ++seps;
      }
      if (seps > 0 && equal_at(text, at, "users")) {
        std::size_t name = at + 5;
        std::size_t seps2 = 0;
        while (name < text.size() &&
               (text[name] == '\\' || text[name] == '/') && seps2 < 2) {
          ++name;
          ++seps2;
        }
        std::size_t end = name;
        while (end < text.size() && text[end] != '\\' && text[end] != '/' &&
               text[end] != '"' && text[end] != '\'' && text[end] != ' ' &&
               text[end] != '\t' && text[end] != '\r' && text[end] != '\n' &&
               text[end] != ';' && text[end] != ',') {
          ++end;
        }
        if (seps2 > 0 && end > name) {
          out.append(text.substr(i, name - i));
          out += "<user>";
          i = end;
          continue;
        }
      }
    }
    out.push_back(text[i++]);
  }
  return out;
}

// Steam ids: 7656119 and ten more digits, or the 64-bit id in hex
// (0110000100000000 and up); and whatever follows "xuid".
std::string redact_ids(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if ((i == 0 || !word_char(text[i - 1])) && text.substr(i, 7) == "7656119") {
      std::size_t end = i;
      while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
        ++end;
      }
      if (end - i == 17 && (end == text.size() || !word_char(text[end]))) {
        out += "<steam id>";
        i = end;
        continue;
      }
    }
    if ((i == 0 || !word_char(text[i - 1])) && hex_digit(text[i])) {
      std::size_t start = i;
      if (text.substr(i, 2) == "0x" || text.substr(i, 2) == "0X") {
        start += 2;
      }
      std::size_t end = start;
      while (end < text.size() && hex_digit(text[end])) {
        ++end;
      }
      // A Steam id in hex: 0x01100001 and the account's 8 digits, printed
      // with its leading zero or without. (The first rule needed 011000010,
      // which let every account id from 0x10000000 up through.)
      const std::size_t digits = end - start;
      if (((digits == 16 && equal_at(text, start, "01100001")) ||
           (digits == 15 && equal_at(text, start, "1100001"))) &&
          (end == text.size() || !word_char(text[end]))) {
        out += "<steam id>";
        i = end;
        continue;
      }
    }
    if (equal_at(text, i, "xuid") && (i == 0 || !word_char(text[i - 1]))) {
      std::size_t at = i + 4;
      std::size_t gap = 0;
      while (at < text.size() && gap < 3 &&
             (text[at] == ' ' || text[at] == ':' || text[at] == '=' ||
              text[at] == '"' || text[at] == '\'')) {
        ++at;
        ++gap;
      }
      std::size_t value = at;
      if (text.substr(value, 2) == "0x" || text.substr(value, 2) == "0X") {
        value += 2;
      }
      std::size_t end = value;
      while (end < text.size() && hex_digit(text[end])) {
        ++end;
      }
      if (end > value) {
        out.append(text.substr(i, at - i));
        out += "<xuid>";
        i = end;
        continue;
      }
    }
    out.push_back(text[i++]);
  }
  return out;
}

std::string join(const std::vector<std::string> &lines) {
  std::string out;
  for (const auto &line : lines) {
    out += line;
    out += '\n';
  }
  return out;
}

std::string base64(std::string_view bytes) {
  static constexpr char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const std::uint32_t v = (std::uint32_t(std::uint8_t(bytes[i])) << 16) |
                            (std::uint32_t(std::uint8_t(bytes[i + 1])) << 8) |
                            std::uint32_t(std::uint8_t(bytes[i + 2]));
    out.push_back(table[(v >> 18) & 63]);
    out.push_back(table[(v >> 12) & 63]);
    out.push_back(table[(v >> 6) & 63]);
    out.push_back(table[v & 63]);
  }
  if (i < bytes.size()) {
    std::uint32_t v = std::uint32_t(std::uint8_t(bytes[i])) << 16;
    if (i + 1 < bytes.size()) {
      v |= std::uint32_t(std::uint8_t(bytes[i + 1])) << 8;
    }
    out.push_back(table[(v >> 18) & 63]);
    out.push_back(table[(v >> 12) & 63]);
    out.push_back(i + 1 < bytes.size() ? table[(v >> 6) & 63] : '=');
    out.push_back('=');
  }
  return out;
}

std::optional<std::uint32_t> parse_hex32(std::string_view text) {
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
  }
  if (text.empty() || text.size() > 8) {
    return std::nullopt;
  }
  std::uint32_t value = 0;
  for (const char c : text) {
    if (!hex_digit(c)) {
      return std::nullopt;
    }
    value = value * 16 + static_cast<std::uint32_t>(
                             c <= '9' ? c - '0' : lower(c) - 'a' + 10);
  }
  return value;
}

std::optional<std::uint64_t> parse_hex64(std::string_view text) {
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
  }
  if (text.empty() || text.size() > 16) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (!hex_digit(c)) {
      return std::nullopt;
    }
    value = value * 16 + static_cast<std::uint64_t>(
                             c <= '9' ? c - '0' : lower(c) - 'a' + 10);
  }
  return value;
}

std::vector<std::string> strings_of(const rapidjson::Value &object,
                                    std::string_view key) {
  std::vector<std::string> out;
  if (const auto *array = json::member(object, key);
      array != nullptr && array->IsArray()) {
    for (const auto &item : array->GetArray()) {
      if (item.IsString()) {
        out.emplace_back(item.GetString(), item.GetStringLength());
      }
    }
  }
  return out;
}

crash_kind kind_from(std::string_view text) {
  for (const crash_kind kind :
       {crash_kind::fail_fast, crash_kind::out_of_memory,
        crash_kind::unknown_exit}) {
    if (text == to_string(kind)) {
      return kind;
    }
  }
  return crash_kind::exception;
}

// ------------------------------------------------------- minidump masking
// The layout is minidumpapiset.h's (all little-endian, packed to 4 bytes).
struct byte_range {
  std::size_t begin = 0;
  std::size_t end = 0;
};

std::optional<std::uint32_t> u32_at(std::string_view bytes, std::uint64_t at) {
  if (at + 4 > bytes.size()) {
    return std::nullopt;
  }
  std::uint32_t value = 0;
  for (int i = 3; i >= 0; --i) {
    value =
        (value << 8) | std::uint8_t(bytes[static_cast<std::size_t>(at) + i]);
  }
  return value;
}

std::optional<std::uint64_t> u64_at(std::string_view bytes, std::uint64_t at) {
  const auto low = u32_at(bytes, at);
  const auto high = u32_at(bytes, at + 4);
  if (!low || !high) {
    return std::nullopt;
  }
  return (std::uint64_t(*high) << 32) | *low;
}

// The parts of a minidump that hold text or memory rather than the dump's
// own structure: module names, PDB (CodeView) and misc records, and the
// memory ranges (the thread stacks). nullopt when the header or the stream
// directory does not fit the bytes.
std::optional<std::vector<byte_range>> minidump_regions(std::string_view dump) {
  constexpr std::uint32_t thread_list = 3, module_list = 4, memory_list = 5,
                          thread_ex_list = 8, memory64_list = 9,
                          unloaded_module_list = 14;
  constexpr std::uint32_t most = 1u << 16; // entries read from one list
  if (dump.size() < 32 || dump.substr(0, 4) != "MDMP") {
    return std::nullopt;
  }
  const auto streams = u32_at(dump, 8);
  const auto directory = u32_at(dump, 12);
  if (!streams || !directory || *streams > 4096 ||
      std::uint64_t(*directory) + std::uint64_t(*streams) * 12 > dump.size()) {
    return std::nullopt;
  }
  std::vector<byte_range> out;
  const auto add = [&](std::uint64_t at, std::uint64_t size) {
    if (at < dump.size() && size > 0) {
      // Cut at the end of the bytes (a size from the file can be anything).
      const std::uint64_t room = dump.size() - at;
      out.push_back({static_cast<std::size_t>(at),
                     static_cast<std::size_t>(at + std::min(size, room))});
    }
  };
  // MINIDUMP_STRING: its length in bytes, then UTF-16.
  const auto add_string = [&](std::uint32_t at) {
    if (const auto length = u32_at(dump, at)) {
      add(std::uint64_t(at) + 4, *length);
    }
  };
  const auto stacks = [&](std::uint64_t at, std::uint32_t entry_size) {
    const std::uint32_t count = u32_at(dump, at).value_or(0);
    for (std::uint32_t i = 0; i < count && i < most; ++i) {
      // MINIDUMP_THREAD(_EX): Stack.Memory {DataSize, Rva} at 32.
      const std::uint64_t thread = at + 4 + std::uint64_t(i) * entry_size;
      const auto size = u32_at(dump, thread + 32);
      const auto rva = u32_at(dump, thread + 36);
      if (!size || !rva) {
        break;
      }
      add(*rva, *size);
    }
  };
  for (std::uint32_t s = 0; s < *streams; ++s) {
    const std::uint64_t entry = std::uint64_t(*directory) + s * 12;
    const std::uint32_t type = *u32_at(dump, entry);
    const std::uint32_t rva = *u32_at(dump, entry + 8);
    switch (type) {
    case module_list: {
      const std::uint32_t count = u32_at(dump, rva).value_or(0);
      for (std::uint32_t i = 0; i < count && i < most; ++i) {
        // MINIDUMP_MODULE (108 bytes): ModuleNameRva at 20, CvRecord at 76,
        // MiscRecord at 84.
        const std::uint64_t module = std::uint64_t(rva) + 4 + i * 108ull;
        const auto name = u32_at(dump, module + 20);
        if (!name) {
          break;
        }
        add_string(*name);
        add(u32_at(dump, module + 80).value_or(0),
            u32_at(dump, module + 76).value_or(0));
        add(u32_at(dump, module + 88).value_or(0),
            u32_at(dump, module + 84).value_or(0));
      }
      break;
    }
    case unloaded_module_list: {
      // A header (its size, the entry size, the count), then entries with
      // ModuleNameRva at 20.
      const std::uint32_t header = u32_at(dump, rva).value_or(0);
      const std::uint32_t size =
          u32_at(dump, std::uint64_t(rva) + 4).value_or(0);
      const std::uint32_t count =
          u32_at(dump, std::uint64_t(rva) + 8).value_or(0);
      for (std::uint32_t i = 0; size >= 24 && i < count && i < most; ++i) {
        const auto name = u32_at(dump, std::uint64_t(rva) + header +
                                           std::uint64_t(i) * size + 20);
        if (!name) {
          break;
        }
        add_string(*name);
      }
      break;
    }
    case thread_list:
      stacks(rva, 48);
      break;
    case thread_ex_list:
      stacks(rva, 64);
      break;
    case memory_list: {
      const std::uint32_t count = u32_at(dump, rva).value_or(0);
      for (std::uint32_t i = 0; i < count && i < most; ++i) {
        // MINIDUMP_MEMORY_DESCRIPTOR: start, then {DataSize, Rva}.
        const std::uint64_t range = std::uint64_t(rva) + 4 + i * 16ull;
        const auto size = u32_at(dump, range + 8);
        const auto at = u32_at(dump, range + 12);
        if (!size || !at) {
          break;
        }
        add(*at, *size);
      }
      break;
    }
    case memory64_list: {
      // The count and the base RVA, then {start, size}; the data follows
      // one range after the other from the base.
      const std::uint64_t count = u64_at(dump, rva).value_or(0);
      std::uint64_t at = u64_at(dump, std::uint64_t(rva) + 8).value_or(0);
      for (std::uint64_t i = 0; i < count && i < most; ++i) {
        const auto size = u64_at(dump, std::uint64_t(rva) + 16 + i * 16 + 8);
        if (!size) {
          break;
        }
        add(at, *size);
        at += *size;
      }
      break;
    }
    default:
      break;
    }
  }
  return out;
}

// UTF-8 to UTF-16 code units; empty when the text is not UTF-8.
std::u16string utf16_of(std::string_view text) {
  std::u16string out;
  std::size_t i = 0;
  while (i < text.size()) {
    const auto lead = std::uint8_t(text[i]);
    std::uint32_t cp = 0;
    std::size_t more = 0;
    if (lead < 0x80) {
      cp = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      cp = lead & 0x1F;
      more = 1;
    } else if ((lead & 0xF0) == 0xE0) {
      cp = lead & 0x0F;
      more = 2;
    } else if ((lead & 0xF8) == 0xF0) {
      cp = lead & 0x07;
      more = 3;
    } else {
      return {};
    }
    if (i + more >= text.size() && more > 0) {
      return {};
    }
    for (std::size_t k = 1; k <= more; ++k) {
      const auto next = std::uint8_t(text[i + k]);
      if ((next & 0xC0) != 0x80) {
        return {};
      }
      cp = (cp << 6) | (next & 0x3F);
    }
    i += more + 1;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
  }
  return out;
}

// The bytes of UTF-8 text, one unit each, for the 8-bit search.
std::u16string units_of(std::string_view text) {
  std::u16string out;
  for (const char c : text) {
    out.push_back(static_cast<char16_t>(std::uint8_t(c)));
  }
  return out;
}

char16_t lower16(char16_t c) {
  return c >= u'A' && c <= u'Z' ? static_cast<char16_t>(c - u'A' + u'a') : c;
}

bool word_unit(char16_t c) {
  return c < 0x80 && word_char(static_cast<char>(c));
}

// Reads 8-bit text (unit 1) or UTF-16LE (unit 2) at any byte offset, so
// strings are found whatever their alignment in the file.
struct text_view {
  std::string &bytes;
  std::size_t unit;

  char16_t at(std::size_t pos) const {
    return unit == 2
               ? static_cast<char16_t>(std::uint8_t(bytes[pos]) |
                                       (std::uint8_t(bytes[pos + 1]) << 8))
               : static_cast<char16_t>(std::uint8_t(bytes[pos]));
  }
  void blank(std::size_t pos) const {
    bytes[pos] = '_';
    if (unit == 2) {
      bytes[pos + 1] = '\0';
    }
  }
};

// Overwrites every whole-word copy of word (ASCII case ignored) in
// [begin, end) with '_'.
void mask_word(const text_view &text, std::size_t begin, std::size_t end,
               const std::u16string &word) {
  const std::size_t unit = text.unit;
  const std::size_t length = word.size() * unit;
  if (word.empty() || end < begin || end - begin < length) {
    return;
  }
  for (std::size_t pos = begin; pos + length <= end; ++pos) {
    bool same = true;
    for (std::size_t k = 0; k < word.size() && same; ++k) {
      same = lower16(text.at(pos + k * unit)) == lower16(word[k]);
    }
    if (!same || (pos >= begin + unit && word_unit(text.at(pos - unit))) ||
        (pos + length + unit <= end && word_unit(text.at(pos + length)))) {
      continue;
    }
    for (std::size_t k = 0; k < word.size(); ++k) {
      text.blank(pos + k * unit);
    }
    pos += length - 1;
  }
}

bool shared_profile(std::u16string name) {
  for (char16_t &c : name) {
    c = lower16(c);
  }
  return name == u"public" || name == u"default" || name == u"default user" ||
         name == u"all users";
}

// The name after every "X:\Users\" (one or more '\' or '/') overwritten with
// '_', unless it is a profile every PC has (Public, Default).
void mask_profiles(const text_view &text) {
  const std::size_t unit = text.unit;
  const std::size_t size = text.bytes.size();
  const auto separator = [](char16_t c) { return c == u'\\' || c == u'/'; };
  const auto name_end = [&](char16_t c) {
    return c < 0x20 || separator(c) || c == u':' || c == u'*' || c == u'?' ||
           c == u'"' || c == u'<' || c == u'>' || c == u'|';
  };
  constexpr std::u16string_view users = u"users";
  for (std::size_t pos = 0; pos + 9 * unit <= size; ++pos) {
    const char16_t drive = lower16(text.at(pos));
    if (drive < u'a' || drive > u'z' || text.at(pos + unit) != u':' ||
        !separator(text.at(pos + 2 * unit))) {
      continue;
    }
    std::size_t next = pos + 2 * unit;
    while (next + unit <= size && separator(text.at(next))) {
      next += unit;
    }
    bool match = next + users.size() * unit <= size;
    for (std::size_t k = 0; match && k < users.size(); ++k) {
      match = lower16(text.at(next + k * unit)) == users[k];
    }
    if (!match) {
      continue;
    }
    next += users.size() * unit;
    if (next + unit > size || !separator(text.at(next))) {
      continue;
    }
    while (next + unit <= size && separator(text.at(next))) {
      next += unit;
    }
    std::size_t end = next;
    std::u16string name;
    while (end + unit <= size && !name_end(text.at(end)) && name.size() < 256) {
      name.push_back(text.at(end));
      end += unit;
    }
    if (name.empty() || shared_profile(name)) {
      continue;
    }
    for (std::size_t k = next; k < end; k += unit) {
      text.blank(k);
    }
    pos = end - 1;
  }
}
} // namespace

const char *to_string(crash_kind kind) {
  switch (kind) {
  case crash_kind::exception:
    return "exception";
  case crash_kind::fail_fast:
    return "fail_fast";
  case crash_kind::out_of_memory:
    return "out_of_memory";
  case crash_kind::unknown_exit:
    return "unknown_exit";
  }
  return "unknown_exit";
}

std::string crash_code_hex(std::uint32_t code) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08X", code);
  return text;
}

std::string crash_offset_hex(std::uint64_t offset) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%llX",
                static_cast<unsigned long long>(offset));
  return text;
}

std::string crash_frame(std::string_view module, std::uint64_t offset) {
  return std::string(module.empty() ? "unknown" : module) + "+" +
         crash_offset_hex(offset);
}

bool valid_report_id(std::string_view id) {
  return id.size() == 32 && std::ranges::all_of(id, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::string pending_crash_to_json(const pending_crash &value) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "schema", "xerces.crash.pending/1", alloc);
  json::set(doc, "report_id", value.report_id, alloc);
  json::set(doc, "created_utc", value.created_utc, alloc);
  json::set(doc, "kind", to_string(value.kind), alloc);
  json::set(doc, "code", crash_code_hex(value.code), alloc);
  json::set(doc, "module", value.module, alloc);
  json::set(doc, "offset",
            value.offset
                ? json::string_value(crash_offset_hex(*value.offset), alloc)
                : Value(),
            alloc);
  json::set(doc, "thread", value.thread, alloc);
  json::set(doc, "frames", json::string_array(value.frames, alloc), alloc);
  json::set(doc, "phase", value.phase, alloc);
  std::vector<std::string> console;
  for (const auto &line : value.console) {
    console.push_back(json::valid_utf8(line, 4096));
  }
  json::set(doc, "console", json::string_array(console, alloc), alloc);
  json::set(doc, "flags", json::string_array(value.flags, alloc), alloc);
  json::set(doc, "game_log", value.game_log, alloc);
  std::vector<std::string> names;
  for (const auto &name : value.names) {
    names.push_back(json::valid_utf8(name, crash_name_cap));
  }
  json::set(doc, "names", json::string_array(names, alloc), alloc);
  json::set(doc, "reason", cap(value.reason, 64), alloc);
  json::set(doc, "summary", cap(value.summary), alloc);
  json::set(doc, "script", cap(value.script), alloc);
  json::set(doc, "details", head_text(value.details, crash_details_cap), alloc);
  auto journal = json::parse(value.journal);
  json::set(doc, "journal",
            journal.ok && journal.document.IsObject()
                ? json::copy(journal.document, alloc)
                : Value(),
            alloc);
  return json::canonical(doc);
}

std::optional<pending_crash> parse_pending_crash(std::string_view text) {
  const auto parsed = json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  const auto &root = parsed.document;
  pending_crash out;
  out.report_id = json::get_string(root, "report_id").value_or("");
  if (!valid_report_id(out.report_id)) {
    return std::nullopt;
  }
  out.created_utc = json::get_string(root, "created_utc").value_or("");
  out.kind = kind_from(json::get_string(root, "kind").value_or(""));
  out.code =
      parse_hex32(json::get_string(root, "code").value_or("")).value_or(0);
  out.module = json::get_string(root, "module").value_or("");
  if (const auto offset = json::get_string(root, "offset")) {
    out.offset = parse_hex64(*offset);
  }
  out.thread = json::get_string(root, "thread").value_or("");
  out.frames = strings_of(root, "frames");
  out.phase = json::get_string(root, "phase").value_or("");
  out.console = strings_of(root, "console");
  out.flags = strings_of(root, "flags");
  out.game_log = json::get_string(root, "game_log").value_or("");
  out.names = strings_of(root, "names");
  // Notes written before 2026-11-12 have none of these.
  out.reason = json::get_string(root, "reason").value_or("");
  out.summary = json::get_string(root, "summary").value_or("");
  out.script = json::get_string(root, "script").value_or("");
  out.details = json::get_string(root, "details").value_or("");
  if (const auto *journal = json::member(root, "journal");
      journal != nullptr && journal->IsObject()) {
    out.journal = json::canonical(*journal);
  }
  return out;
}

std::vector<std::string>
launch_flag_names(const std::vector<std::string> &args) {
  std::vector<std::string> out;
  for (const auto &arg : args) {
    if (arg.starts_with("+")) {
      break;
    }
    if (arg.size() > 1 && arg[0] == '-' &&
        std::ranges::all_of(arg.substr(1),
                            [](char c) { return word_char(c) || c == '-'; })) {
      out.push_back(cap(arg, 64));
    }
  }
  return out;
}

bool exit_is_crash(std::uint32_t exit_code, bool journal_clean,
                   bool client_dump) {
  return client_dump || (exit_code != 0 && !journal_clean);
}

crash_kind classify_exit(std::uint32_t exit_code, bool out_of_memory_logged) {
  if (exit_code == 0xC0000409u) {
    return crash_kind::fail_fast;
  }
  if (exit_code == 0xFFFFFFFFu && out_of_memory_logged) {
    return crash_kind::out_of_memory;
  }
  return crash_kind::unknown_exit;
}

crash_facts crash_from_pending(const pending_crash &crash,
                               std::optional<std::uint32_t> exit_code) {
  crash_facts out;
  out.kind = crash.kind;
  out.code = crash_code_hex(crash.code);
  if (exit_code) {
    out.exit_code = crash_code_hex(*exit_code);
  }
  if (!crash.module.empty()) {
    out.module = crash.module;
  }
  if (crash.offset) {
    out.offset = crash_offset_hex(*crash.offset);
  }
  if (!crash.thread.empty()) {
    out.thread = crash.thread;
  }
  out.frames = crash.frames;
  if (out.frames.size() > crash_frame_cap) {
    out.frames.resize(crash_frame_cap);
  }
  out.phase = crash.phase;
  if (!crash.reason.empty()) {
    out.reason = crash.reason;
  }
  if (!crash.summary.empty()) {
    out.summary = crash.summary;
  }
  if (!crash.script.empty()) {
    out.script = crash.script;
  }
  return out;
}

crash_facts crash_from_exit(std::uint32_t exit_code, bool out_of_memory_logged,
                            std::string phase) {
  crash_facts out;
  out.kind = classify_exit(exit_code, out_of_memory_logged);
  out.code = crash_code_hex(exit_code);
  out.exit_code = crash_code_hex(exit_code);
  out.phase = std::move(phase);
  return out;
}

plan_mods read_plan_mods(std::string_view plan_json) {
  plan_mods out;
  const auto parsed = json::parse(plan_json);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return out;
  }
  const auto &root = parsed.document;
  const auto entry = [](const rapidjson::Value &item) {
    crash_mod mod;
    mod.id = json::get_string(item, "id").value_or("");
    mod.name = json::get_string(item, "name").value_or("");
    mod.version = json::get_string(item, "version").value_or("");
    return mod;
  };
  if (const auto *stack = json::member(root, "stack");
      stack != nullptr && stack->IsArray()) {
    for (const auto &item : stack->GetArray()) {
      if (item.IsObject()) {
        out.mods.push_back(entry(item));
      }
    }
  }
  if (const auto *map = json::member(root, "map");
      map != nullptr && map->IsObject()) {
    const std::string folder = json::get_string(*map, "folder").value_or("");
    const std::string name = json::get_string(*map, "name").value_or("");
    if (!folder.empty() || !name.empty()) {
      out.map = folder.empty() ? name : folder;
    }
  }
  return out;
}

std::string redact_crash_text(std::string_view text,
                              const crash_redaction &redaction, bool json) {
  std::string out = redaction.profile.empty()
                        ? std::string(text)
                        : redact_profile(text, redaction.profile);
  out = redact_user_paths(out);
  if (redaction.user_name.size() >= 3) {
    out = replace_word(out, redaction.user_name, "<user>");
  }
  out = redact_player_names(out, redaction.player_names);
  out = redact_ids(out);
  return json ? redact_addresses_in_json(out, redaction.known_addresses)
              : redact_addresses(out, redaction.known_addresses);
}

std::string redact_player_names(std::string_view text,
                                const std::vector<std::string> &names) {
  std::string out(text);
  for (const auto &name : names) {
    if (name.size() >= 2) {
      out = replace_word(out, name, "<player>");
    }
  }
  return out;
}

std::optional<std::string>
mask_minidump(std::string_view dump, const std::vector<std::string> &words) {
  auto regions = minidump_regions(dump);
  if (!regions) {
    return std::nullopt;
  }
  // A stack is in the thread list and in the memory list: search it once.
  std::ranges::sort(*regions, {}, &byte_range::begin);
  std::vector<byte_range> merged;
  for (const byte_range &range : *regions) {
    if (!merged.empty() && range.begin <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, range.end);
    } else {
      merged.push_back(range);
    }
  }
  regions = std::move(merged);
  std::string out(dump);
  // Module paths are UTF-16; PDB paths in the CodeView records are 8-bit.
  for (const std::size_t unit : {std::size_t{1}, std::size_t{2}}) {
    mask_profiles(text_view{out, unit});
  }
  for (const auto &word : words) {
    if (word.size() < 3) {
      continue; // too short to tell from other bytes
    }
    const std::u16string wide = utf16_of(word);
    const std::u16string narrow = units_of(word);
    for (const byte_range &range : *regions) {
      mask_word(text_view{out, 1}, range.begin, range.end, narrow);
      if (!wide.empty()) {
        mask_word(text_view{out, 2}, range.begin, range.end, wide);
      }
    }
  }
  return out;
}

std::string tail_text(std::string_view text, std::size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return json::valid_utf8(text, max_bytes);
  }
  std::string_view tail = text.substr(text.size() - max_bytes);
  // Start at a line: the first one is cut.
  if (const std::size_t line = tail.find('\n');
      line != std::string_view::npos && line + 1 < tail.size()) {
    tail.remove_prefix(line + 1);
  } else {
    // One long line: start at a whole character.
    while (!tail.empty() &&
           (static_cast<unsigned char>(tail[0]) & 0xC0) == 0x80) {
      tail.remove_prefix(1);
    }
  }
  return json::valid_utf8(tail, max_bytes);
}

std::string head_text(std::string_view text, std::size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return json::valid_utf8(text, max_bytes);
  }
  std::string_view head = text.substr(0, max_bytes);
  // End at a line: the last one is cut.
  if (const std::size_t line = head.rfind('\n');
      line != std::string_view::npos && line > 0) {
    head = head.substr(0, line + 1);
  }
  return json::valid_utf8(head, max_bytes);
}

std::string build_crash_report(const crash_report_input &in) {
  const crash_redaction redaction{in.profile, in.user_name, in.player_names,
                                  in.known_addresses};
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "schema", crash_schema, alloc);
  json::set(doc, "report_id", in.report_id, alloc);
  json::set(doc, "created_utc", cap(in.created_utc, 64), alloc);

  Value xerces(rapidjson::kObjectType);
  json::set(xerces, "version", cap(in.version), alloc);
  json::set(xerces, "commit", cap(in.commit, 64), alloc);
  json::set(xerces, "dirty", in.dirty, alloc);
  json::set(doc, "xerces", std::move(xerces), alloc);

  Value game(rapidjson::kObjectType);
  json::set(
      game, "build",
      in.game_checksum
          ? json::string_value(std::format("0x{:X}", *in.game_checksum), alloc)
          : Value(),
      alloc);
  json::set(game, "steam_build",
            in.steam_build ? Value(static_cast<std::int64_t>(*in.steam_build))
                           : Value(),
            alloc);
  json::set(doc, "game", std::move(game), alloc);

  Value system(rapidjson::kObjectType);
  json::set(system, "windows", cap(in.system.windows, 64), alloc);
  json::set(system, "cpu", cap(in.system.cpu), alloc);
  json::set(system, "logical_cpus", in.system.logical_cpus, alloc);
  json::set(system, "ram_mib", in.system.ram_mib, alloc);
  Value gpus(rapidjson::kArrayType);
  for (std::size_t i = 0; i < in.system.gpus.size() && i < 8; ++i) {
    Value gpu(rapidjson::kObjectType);
    json::set(gpu, "name", cap(in.system.gpus[i].name), alloc);
    json::set(gpu, "driver", cap(in.system.gpus[i].driver, 64), alloc);
    gpus.PushBack(std::move(gpu), alloc);
  }
  json::set(system, "gpus", std::move(gpus), alloc);
  json::set(system, "display", cap(in.system.display, 64), alloc);
  json::set(doc, "system", std::move(system), alloc);

  Value launch(rapidjson::kObjectType);
  json::set(launch, "mode", cap(in.mode, 8), alloc);
  json::set(launch, "map", optional_string(in.map, alloc), alloc);
  json::set(launch, "plan_id", optional_string(in.plan_id, alloc), alloc);
  Value mods(rapidjson::kArrayType);
  for (std::size_t i = 0; i < in.mods.size() && i < 64; ++i) {
    Value mod(rapidjson::kObjectType);
    json::set(mod, "id", cap(in.mods[i].id), alloc);
    json::set(mod, "name", cap(in.mods[i].name), alloc);
    json::set(mod, "version", cap(in.mods[i].version, 64), alloc);
    mods.PushBack(std::move(mod), alloc);
  }
  json::set(launch, "mods", std::move(mods), alloc);
  Value flags(rapidjson::kArrayType);
  for (std::size_t i = 0; i < in.flags.size() && i < 64; ++i) {
    flags.PushBack(json::string_value(cap(in.flags[i], 64), alloc), alloc);
  }
  json::set(launch, "flags", std::move(flags), alloc);
  json::set(launch, "safe_mode", in.safe_mode, alloc);
  json::set(doc, "launch", std::move(launch), alloc);

  Value crash(rapidjson::kObjectType);
  json::set(crash, "kind", to_string(in.crash.kind), alloc);
  json::set(crash, "code", cap(in.crash.code, 32), alloc);
  json::set(crash, "exit_code", optional_string(in.crash.exit_code, alloc),
            alloc);
  json::set(crash, "module", optional_string(in.crash.module, alloc), alloc);
  json::set(crash, "offset", optional_string(in.crash.offset, alloc), alloc);
  json::set(crash, "thread", optional_string(in.crash.thread, alloc), alloc);
  Value frames(rapidjson::kArrayType);
  for (std::size_t i = 0; i < in.crash.frames.size() && i < crash_frame_cap;
       ++i) {
    frames.PushBack(json::string_value(cap(in.crash.frames[i]), alloc), alloc);
  }
  json::set(crash, "frames", std::move(frames), alloc);
  json::set(crash, "phase", cap(in.crash.phase, 64), alloc);
  // A line each from the client's note, scrubbed as the logs are: they can
  // name a path or a player.
  const auto scrubbed = [&](const std::optional<std::string> &text,
                            std::size_t bytes) {
    return text ? json::string_value(
                      cap(redact_crash_text(*text, redaction, false), bytes),
                      alloc)
                : Value();
  };
  json::set(crash, "reason", scrubbed(in.crash.reason, 64), alloc);
  json::set(crash, "summary", scrubbed(in.crash.summary, crash_name_cap),
            alloc);
  json::set(crash, "script", scrubbed(in.crash.script, crash_name_cap), alloc);
  json::set(doc, "crash", std::move(crash), alloc);

  Value logs(rapidjson::kObjectType);
  auto journal = json::parse(in.journal_json);
  json::set(logs, "journal",
            journal.ok && journal.document.IsObject()
                ? json::copy(journal.document, alloc)
                : Value(),
            alloc);
  const auto tail_of = [&](const std::vector<std::string> &lines,
                           std::size_t keep, bool is_json) {
    const std::size_t from = lines.size() > keep ? lines.size() - keep : 0;
    const std::vector<std::string> kept(lines.begin() + std::ptrdiff_t(from),
                                        lines.end());
    return tail_text(redact_crash_text(join(kept), redaction, is_json),
                     crash_tail_cap);
  };
  json::set(logs, "game_log_tail",
            tail_of(in.game_log, crash_game_log_lines, true), alloc);
  json::set(logs, "launcher_log_tail",
            tail_of(in.launcher_log, crash_launcher_log_lines, true), alloc);
  json::set(logs, "console_tail",
            tail_of(in.console, crash_console_lines, false), alloc);
  // The client's text report: its start matters most, so that is what stays.
  json::set(logs, "details",
            in.details.empty()
                ? Value()
                : json::string_value(
                      head_text(redact_crash_text(in.details, redaction, false),
                                crash_tail_cap),
                      alloc),
            alloc);
  json::set(doc, "logs", std::move(logs), alloc);

  json::set(doc, "minidump_b64",
            !in.minidump.empty() && in.minidump.size() <= crash_minidump_cap
                ? json::string_value(base64(in.minidump), alloc)
                : Value(),
            alloc);
  return json::canonical(doc);
}

bool crash_reports_enabled(std::string_view launcher_json) {
  // No launcher.json yet: the default.
  if (std::ranges::all_of(launcher_json, [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
      })) {
    return true;
  }
  // A config that cannot be read sends nothing: the person may have turned
  // sending off in the file that broke.
  const launcher_config_result parsed = parse_launcher_config(launcher_json);
  return parsed.ok() && parsed.value.send_crash_reports;
}

crash_redaction redaction_from_config(std::string_view launcher_json) {
  crash_redaction out;
  const launcher_config_result parsed = parse_launcher_config(launcher_json);
  const launcher_config &config = parsed.value;
  if (!config.player_name.empty()) {
    out.player_names.push_back(config.player_name);
  }
  for (const auto &favorite : config.favorites) {
    if (!favorite.name.empty()) {
      out.known_addresses.push_back(favorite.name);
    }
    if (!favorite.address.empty()) {
      out.known_addresses.push_back(favorite.address);
    }
  }
  for (const auto &address : config.recent_servers) {
    out.known_addresses.push_back(address);
  }
  return out;
}

std::string crash_report_note(bool started_by_launcher, bool send,
                              bool launcher_open, bool dump) {
  const std::string by_hand =
      "To report it, open an issue at " + std::string(crash_issues_url) +
      " and attach the diagnostics bundle (Xerces > Diagnostics > Copy "
      "report).";
  if (!started_by_launcher) {
    return "Xerces did not start this game, so no crash report is sent. " +
           by_hand;
  }
  if (!send) {
    return "Crash reports are off, so nothing is sent. " + by_hand;
  }
  const std::string what =
      std::string(dump ? " (a small dump of the threads' stacks with your "
                         "user name and player names masked, redacted logs "
                         "and this PC's specs"
                       : " (redacted logs and this PC's specs") +
      "; turn it off in Xerces > Settings > Privacy).";
  return launcher_open
             ? "Xerces sends a crash report as soon as the game closes" + what
             : "Xerces sends a crash report when you open it next" + what;
}
} // namespace xerces_core
