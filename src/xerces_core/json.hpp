#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rapidjson/document.h>

namespace xerces_core::json {
struct parse_result {
  rapidjson::Document document;
  bool ok = false;
  std::size_t line = 0;
  std::size_t column = 0;
  std::string error;
};

// Arrays and objects nested deeper than this are refused, so code that walks a
// parsed document recursively cannot run out of stack.
inline constexpr int max_depth = 64;

// Strict UTF-8 JSON, at most max_depth levels deep. On failure, `line` and
// `column` are 1-based.
parse_result parse(std::string_view text);

// Canonical form: object keys sorted by their UTF-8 bytes, no whitespace,
// integral numbers below 1e21 written out in full, other numbers in their
// shortest round-trip form, strings with only the escapes JSON requires.
// Hashes are taken over this.
std::string canonical(const rapidjson::Value &value);

// Indented, keys in canonical order, for people.
std::string pretty(const rapidjson::Value &value);

std::string quote(std::string_view text);

// Characters (code points) in UTF-8 text, as a schema's maxLength counts them.
std::size_t length(std::string_view text);

// JSON pointer (RFC 6901) helpers for diagnostics.
std::string pointer(const std::string &base, std::string_view token);
std::string pointer(const std::string &base, std::size_t index);

const rapidjson::Value *member(const rapidjson::Value &object,
                               std::string_view name);
std::optional<std::string> get_string(const rapidjson::Value &object,
                                      std::string_view name);

using allocator = rapidjson::Document::AllocatorType;
rapidjson::Value string_value(std::string_view text, allocator &alloc);
void set(rapidjson::Value &object, std::string_view key, rapidjson::Value value,
         allocator &alloc);
void set(rapidjson::Value &object, std::string_view key, std::string_view text,
         allocator &alloc);
void set(rapidjson::Value &object, std::string_view key, const char *text,
         allocator &alloc);
void set(rapidjson::Value &object, std::string_view key, std::int64_t number,
         allocator &alloc);
void set(rapidjson::Value &object, std::string_view key, bool flag,
         allocator &alloc);
rapidjson::Value string_array(const std::vector<std::string> &items,
                              allocator &alloc);
rapidjson::Value copy(const rapidjson::Value &value, allocator &alloc);
} // namespace xerces_core::json
