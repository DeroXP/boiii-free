#include "sigscan.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "json.hpp"

namespace xerces_core {
namespace {
constexpr std::size_t cached_byte_count = 16;

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

// The first section with this name; for ".text" that is never the second one.
const section_view *section_named(const std::vector<section_view> &sections,
                                  std::string_view name) {
  const auto it = std::ranges::find_if(
      sections, [&](const section_view &s) { return s.name == name; });
  return it == sections.end() ? nullptr : &*it;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (const std::uint8_t b : bytes) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0x0f]);
  }
  return out;
}

std::optional<std::span<const std::uint8_t>>
bytes_at(const std::vector<section_view> &sections, std::uint64_t rva,
         std::size_t count) {
  for (const auto &section : sections) {
    if (rva >= section.rva &&
        rva - section.rva + count <= section.bytes.size()) {
      if (section.name == ".text" &&
          &section != section_named(sections, ".text")) {
        continue; // never the second .text
      }
      return section.bytes.subspan(static_cast<std::size_t>(rva - section.rva),
                                   count);
    }
  }
  return std::nullopt;
}
} // namespace

std::optional<byte_pattern> parse_pattern(std::string_view text,
                                          std::string *error) {
  byte_pattern out;
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == ' ' || text[i] == '\t') {
      ++i;
      continue;
    }
    if (text[i] == '?') {
      out.bytes.push_back(0);
      out.fixed.push_back(false);
      i += (i + 1 < text.size() && text[i + 1] == '?') ? 2 : 1;
      continue;
    }
    if (i + 1 >= text.size() || hex_value(text[i]) < 0 ||
        hex_value(text[i + 1]) < 0) {
      if (error != nullptr) {
        *error = "bad byte at position " + std::to_string(i);
      }
      return std::nullopt;
    }
    out.bytes.push_back(static_cast<std::uint8_t>(hex_value(text[i]) * 16 +
                                                  hex_value(text[i + 1])));
    out.fixed.push_back(true);
    i += 2;
  }
  if (out.bytes.empty() || std::ranges::none_of(out.fixed, std::identity{})) {
    if (error != nullptr) {
      *error = "a pattern needs at least one fixed byte";
    }
    return std::nullopt;
  }
  return out;
}

std::vector<std::size_t> find_matches(std::span<const std::uint8_t> haystack,
                                      const byte_pattern &pattern,
                                      std::size_t limit) {
  std::vector<std::size_t> out;
  const std::size_t size = pattern.bytes.size();
  if (size == 0 || haystack.size() < size) {
    return out;
  }
  // Anchor on the first fixed byte so most positions are rejected by memchr.
  const auto first_fixed = static_cast<std::size_t>(
      std::ranges::find(pattern.fixed, true) - pattern.fixed.begin());
  const std::uint8_t anchor = pattern.bytes[first_fixed];
  std::size_t pos = 0;
  while (pos + size <= haystack.size()) {
    const void *hit = std::memchr(haystack.data() + pos + first_fixed, anchor,
                                  haystack.size() - size - pos + 1);
    if (hit == nullptr) {
      break;
    }
    const std::size_t start =
        static_cast<std::size_t>(static_cast<const std::uint8_t *>(hit) -
                                 haystack.data()) -
        first_fixed;
    bool matched = true;
    for (std::size_t j = 0; j < size; ++j) {
      if (pattern.fixed[j] && haystack[start + j] != pattern.bytes[j]) {
        matched = false;
        break;
      }
    }
    if (matched) {
      out.push_back(start);
      if (out.size() >= limit) {
        break;
      }
    }
    pos = start + 1;
  }
  return out;
}

signature_report resolve_signatures(const std::vector<signature> &signatures,
                                    const std::vector<section_view> &sections,
                                    std::uint32_t build_checksum) {
  signature_report report;
  for (const auto &sig : signatures) {
    signature_result result;
    result.id = sig.id;
    result.symbol = sig.symbol;
    const section_view *section = section_named(sections, sig.section);
    if (section == nullptr) {
      result.error = "section " + sig.section + " not found";
    } else {
      for (const auto &text : sig.patterns) {
        std::string error;
        const auto pattern = parse_pattern(text, &error);
        if (!pattern) {
          result.error = "invalid pattern: " + error;
          continue;
        }
        const auto matches = find_matches(section->bytes, *pattern, 2);
        result.matches = matches.size();
        if (matches.size() != 1) {
          result.error = matches.empty() ? "0 matches" : "more than one match";
          continue;
        }
        const std::uint64_t match_rva = section->rva + matches[0];
        std::uint64_t rva = match_rva + static_cast<std::uint64_t>(sig.offset);
        if (sig.kind == symbol_kind::data_rip) {
          const auto disp_bytes = bytes_at(
              sections, match_rva + static_cast<std::uint64_t>(sig.disp_offset),
              4);
          if (!disp_bytes) {
            result.error = "displacement outside the section";
            continue;
          }
          std::int32_t displacement = 0;
          std::memcpy(&displacement, disp_bytes->data(), 4);
          rva = match_rva + static_cast<std::uint64_t>(sig.insn_end) +
                static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(displacement));
        }
        if (const auto known = sig.known.find(build_checksum);
            known != sig.known.end() && known->second != rva) {
          result.error = "disagrees with the known address for this build";
          continue;
        }
        result.ok = true;
        result.rva = rva;
        result.pattern = text;
        result.error.clear();
        break;
      }
    }
    if (!result.ok) {
      for (const auto &feature : sig.features) {
        report.disabled[feature].push_back(sig.id);
      }
    }
    report.results.push_back(std::move(result));
  }
  return report;
}

const signature_result *signature_report::find(std::string_view id) const {
  const auto it = std::ranges::find_if(
      results, [&](const signature_result &r) { return r.id == id; });
  return it == results.end() ? nullptr : &*it;
}

bool signature_report::feature_enabled(std::string_view feature) const {
  const auto it = disabled.find(std::string(feature));
  return it == disabled.end() || it->second.empty();
}

std::string signature_report::message(std::string_view feature,
                                      std::string_view feature_label,
                                      std::string_view still_works) const {
  const auto it = disabled.find(std::string(feature));
  if (it == disabled.end() || it->second.empty()) {
    return {};
  }
  std::string out = std::string(feature_label) +
                    " is off on this game build. Xerces could not find ";
  for (std::size_t i = 0; i < it->second.size(); ++i) {
    const signature_result *result = find(it->second[i]);
    out += (i == 0 ? "" : ", ") +
           (result != nullptr ? result->symbol : it->second[i]) +
           " (signature " + it->second[i] + ", " +
           (result != nullptr ? result->error : std::string("missing")) + ")";
  }
  out += ". " + std::string(still_works);
  return out;
}

std::string signature_cache_to_json(const signature_report &report,
                                    std::uint32_t build_checksum,
                                    const std::vector<section_view> &sections) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "build", static_cast<std::int64_t>(build_checksum), alloc);
  rapidjson::Value symbols(rapidjson::kObjectType);
  for (const auto &result : report.results) {
    if (!result.ok) {
      continue;
    }
    const auto bytes = bytes_at(sections, result.rva, cached_byte_count);
    if (!bytes) {
      continue;
    }
    rapidjson::Value entry(rapidjson::kObjectType);
    json::set(entry, "rva", static_cast<std::int64_t>(result.rva), alloc);
    json::set(entry, "symbol", result.symbol, alloc);
    json::set(entry, "bytes", to_hex(*bytes), alloc);
    json::set(symbols, result.id, std::move(entry), alloc);
  }
  json::set(doc, "symbols", std::move(symbols), alloc);
  return json::canonical(doc);
}

std::optional<signature_report>
load_signature_cache(std::string_view text, std::uint32_t build_checksum,
                     const std::vector<section_view> &sections,
                     const std::vector<signature> &signatures) {
  const auto parsed = json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  const auto *build = json::member(parsed.document, "build");
  const auto *symbols = json::member(parsed.document, "symbols");
  if (build == nullptr || !build->IsInt64() ||
      static_cast<std::uint32_t>(build->GetInt64()) != build_checksum ||
      symbols == nullptr || !symbols->IsObject()) {
    return std::nullopt;
  }
  signature_report report;
  for (auto it = symbols->MemberBegin(); it != symbols->MemberEnd(); ++it) {
    const auto &entry = it->value;
    const auto *rva = json::member(entry, "rva");
    const auto bytes = json::get_string(entry, "bytes");
    if (rva == nullptr || !rva->IsInt64() || !bytes) {
      return std::nullopt;
    }
    const auto current =
        bytes_at(sections, static_cast<std::uint64_t>(rva->GetInt64()),
                 cached_byte_count);
    if (!current || to_hex(*current) != *bytes) {
      return std::nullopt; // the image changed: scan again
    }
    signature_result result;
    result.id = std::string(it->name.GetString(), it->name.GetStringLength());
    result.symbol = json::get_string(entry, "symbol").value_or(result.id);
    result.ok = true;
    result.rva = static_cast<std::uint64_t>(rva->GetInt64());
    result.matches = 1;
    report.results.push_back(std::move(result));
  }
  for (const auto &sig : signatures) {
    if (report.find(sig.id) == nullptr) {
      return std::nullopt; // a signature the cache does not know
    }
  }
  return report;
}
} // namespace xerces_core
