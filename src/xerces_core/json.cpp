#include "json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

#include <rapidjson/error/en.h>

namespace xerces_core::json {
namespace {
void write_string(std::string &out, std::string_view text) {
  static constexpr char hex_digits[] = "0123456789abcdef";
  out.push_back('"');
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        out += "\\u00";
        out.push_back(hex_digits[c >> 4]);
        out.push_back(hex_digits[c & 0x0f]);
      } else {
        out.push_back(raw);
      }
    }
  }
  out.push_back('"');
}

void write_number(std::string &out, const rapidjson::Value &value) {
  char buffer[64];
  std::to_chars_result result{};
  if (value.IsUint64()) {
    result = std::to_chars(buffer, buffer + sizeof(buffer), value.GetUint64());
  } else if (value.IsInt64()) {
    result = std::to_chars(buffer, buffer + sizeof(buffer), value.GetInt64());
  } else {
    const double number = value.GetDouble();
    if (!std::isfinite(number)) {
      out += "null";
      return;
    }
    double whole = 0;
    if (std::modf(number, &whole) == 0.0 && std::fabs(number) < 9.0e15) {
      result = std::to_chars(buffer, buffer + sizeof(buffer),
                             static_cast<std::int64_t>(number));
    } else {
      result = std::to_chars(buffer, buffer + sizeof(buffer), number);
    }
  }
  out.append(buffer, result.ptr);
}

std::vector<const rapidjson::Value::Member *>
sorted_members(const rapidjson::Value &object) {
  std::vector<const rapidjson::Value::Member *> members;
  for (auto it = object.MemberBegin(); it != object.MemberEnd(); ++it) {
    members.push_back(&*it);
  }
  std::ranges::sort(members, [](const auto *left, const auto *right) {
    const std::string_view a(left->name.GetString(),
                             left->name.GetStringLength());
    const std::string_view b(right->name.GetString(),
                             right->name.GetStringLength());
    return a < b;
  });
  return members;
}

void write(std::string &out, const rapidjson::Value &value, int indent,
           int depth) {
  const auto newline = [&](int level) {
    if (indent > 0) {
      out.push_back('\n');
      out.append(static_cast<std::size_t>(indent * level), ' ');
    }
  };

  switch (value.GetType()) {
  case rapidjson::kNullType:
    out += "null";
    break;
  case rapidjson::kFalseType:
    out += "false";
    break;
  case rapidjson::kTrueType:
    out += "true";
    break;
  case rapidjson::kNumberType:
    write_number(out, value);
    break;
  case rapidjson::kStringType:
    write_string(out,
                 std::string_view(value.GetString(), value.GetStringLength()));
    break;
  case rapidjson::kArrayType: {
    out.push_back('[');
    bool first = true;
    for (const auto &item : value.GetArray()) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      newline(depth + 1);
      write(out, item, indent, depth + 1);
    }
    if (!first) {
      newline(depth);
    }
    out.push_back(']');
    break;
  }
  case rapidjson::kObjectType: {
    out.push_back('{');
    bool first = true;
    for (const auto *member : sorted_members(value)) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      newline(depth + 1);
      write_string(out, std::string_view(member->name.GetString(),
                                         member->name.GetStringLength()));
      out.push_back(':');
      if (indent > 0) {
        out.push_back(' ');
      }
      write(out, member->value, indent, depth + 1);
    }
    if (!first) {
      newline(depth);
    }
    out.push_back('}');
    break;
  }
  }
}
} // namespace

parse_result parse(std::string_view text) {
  parse_result result;
  result.document.Parse<rapidjson::kParseValidateEncodingFlag |
                        rapidjson::kParseFullPrecisionFlag>(text.data(),
                                                            text.size());
  if (!result.document.HasParseError()) {
    result.ok = true;
    return result;
  }
  const std::size_t offset =
      std::min(result.document.GetErrorOffset(), text.size());
  result.line = 1;
  result.column = 1;
  for (std::size_t i = 0; i < offset; ++i) {
    if (text[i] == '\n') {
      ++result.line;
      result.column = 1;
    } else {
      ++result.column;
    }
  }
  result.error = rapidjson::GetParseError_En(result.document.GetParseError());
  return result;
}

std::string canonical(const rapidjson::Value &value) {
  std::string out;
  write(out, value, 0, 0);
  return out;
}

std::string pretty(const rapidjson::Value &value) {
  std::string out;
  write(out, value, 2, 0);
  return out;
}

std::string quote(std::string_view text) {
  std::string out;
  write_string(out, text);
  return out;
}

std::string pointer(const std::string &base, std::string_view token) {
  std::string out = base;
  out.push_back('/');
  for (const char c : token) {
    if (c == '~') {
      out += "~0";
    } else if (c == '/') {
      out += "~1";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string pointer(const std::string &base, std::size_t index) {
  return base + "/" + std::to_string(index);
}

const rapidjson::Value *member(const rapidjson::Value &object,
                               std::string_view name) {
  if (!object.IsObject()) {
    return nullptr;
  }
  const auto it = object.FindMember(rapidjson::Value(rapidjson::StringRef(
      name.data(), static_cast<rapidjson::SizeType>(name.size()))));
  return it == object.MemberEnd() ? nullptr : &it->value;
}

std::optional<std::string> get_string(const rapidjson::Value &object,
                                      std::string_view name) {
  const rapidjson::Value *value = member(object, name);
  if (value == nullptr || !value->IsString()) {
    return std::nullopt;
  }
  return std::string(value->GetString(), value->GetStringLength());
}

rapidjson::Value string_value(std::string_view text, allocator &alloc) {
  return rapidjson::Value(text.data(),
                          static_cast<rapidjson::SizeType>(text.size()), alloc);
}

void set(rapidjson::Value &object, std::string_view key, rapidjson::Value value,
         allocator &alloc) {
  if (auto it = object.FindMember(rapidjson::Value(rapidjson::StringRef(
          key.data(), static_cast<rapidjson::SizeType>(key.size()))));
      it != object.MemberEnd()) {
    it->value = std::move(value);
    return;
  }
  object.AddMember(string_value(key, alloc), std::move(value), alloc);
}

void set(rapidjson::Value &object, std::string_view key, std::string_view text,
         allocator &alloc) {
  set(object, key, string_value(text, alloc), alloc);
}

void set(rapidjson::Value &object, std::string_view key, const char *text,
         allocator &alloc) {
  set(object, key, std::string_view(text), alloc);
}

void set(rapidjson::Value &object, std::string_view key, std::int64_t number,
         allocator &alloc) {
  set(object, key, rapidjson::Value(number), alloc);
}

void set(rapidjson::Value &object, std::string_view key, bool flag,
         allocator &alloc) {
  set(object, key, rapidjson::Value(flag), alloc);
}

rapidjson::Value string_array(const std::vector<std::string> &items,
                              allocator &alloc) {
  rapidjson::Value array(rapidjson::kArrayType);
  for (const auto &item : items) {
    array.PushBack(string_value(item, alloc), alloc);
  }
  return array;
}

rapidjson::Value copy(const rapidjson::Value &value, allocator &alloc) {
  return rapidjson::Value(value, alloc);
}
} // namespace xerces_core::json
