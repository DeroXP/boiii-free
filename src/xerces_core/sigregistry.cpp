#include "sigregistry.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <set>

#include "json.hpp"

namespace xerces_core {
namespace {
const std::set<std::string, std::less<>> known_keys = {
    "id",    "symbol",   "kind",        "section",  "patterns", "offset",
    "known", "features", "disp_offset", "insn_end", "note"};

const std::set<std::string, std::less<>> scannable_sections = {
    ".text", ".rdata", ".interpr"};

bool valid_id(std::string_view id) {
  return !id.empty() && std::ranges::all_of(id, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.';
  });
}

// "0x" followed by 1 to 16 hex digits.
std::optional<std::uint64_t> hex_number(std::string_view text) {
  if (!text.starts_with("0x") || text.size() < 3 || text.size() > 18) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const char *end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data() + 2, end, value, 16);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::string view_of(const rapidjson::Value &value) {
  return {value.GetString(), value.GetStringLength()};
}

std::optional<std::int64_t> integer(const rapidjson::Value &object,
                                    std::string_view name,
                                    const std::string &path,
                                    diagnostics &diags) {
  const rapidjson::Value *value = json::member(object, name);
  if (value == nullptr) {
    return std::nullopt;
  }
  if (!value->IsInt64()) {
    diags.error("signature.integer", json::pointer(path, name),
                std::string(name) + " must be a whole number.");
    return std::nullopt;
  }
  return value->GetInt64();
}

void parse_entry(const rapidjson::Value &entry, const std::string &path,
                 std::set<std::string> &ids, signature_registry &out) {
  diagnostics &diags = out.diags;
  if (!entry.IsObject()) {
    diags.error("signature.type", path, "Each signature is an object.");
    return;
  }
  for (auto it = entry.MemberBegin(); it != entry.MemberEnd(); ++it) {
    const std::string key = view_of(it->name);
    if (!known_keys.contains(key)) {
      diags.warning("signature.key.unknown", json::pointer(path, key),
                    "Unknown key \"" + key + "\"; ignored.");
    }
  }

  signature sig;
  const auto id = json::get_string(entry, "id");
  if (!id || !valid_id(*id)) {
    diags.error("signature.id", json::pointer(path, "id"),
                "id is required: lower-case letters, digits, '_' and '.'.");
    return;
  }
  sig.id = *id;
  if (!ids.insert(sig.id).second) {
    diags.error("signature.id.duplicate", json::pointer(path, "id"),
                "The id " + sig.id + " is used twice.");
  }
  sig.symbol = json::get_string(entry, "symbol").value_or("");
  if (sig.symbol.empty()) {
    diags.error("signature.symbol", json::pointer(path, "symbol"),
                "symbol is required.");
  }

  const std::string kind = json::get_string(entry, "kind").value_or("");
  if (kind == "function") {
    sig.kind = symbol_kind::function;
  } else if (kind == "site") {
    sig.kind = symbol_kind::site;
  } else if (kind == "data_rip") {
    sig.kind = symbol_kind::data_rip;
  } else {
    diags.error("signature.kind", json::pointer(path, "kind"),
                "kind is function, site or data_rip.");
  }

  sig.section = json::get_string(entry, "section").value_or(".text");
  if (!scannable_sections.contains(sig.section)) {
    diags.error("signature.section", json::pointer(path, "section"),
                "section is .text, .rdata or .interpr.");
  }

  const rapidjson::Value *patterns = json::member(entry, "patterns");
  if (patterns == nullptr || !patterns->IsArray() || patterns->Empty()) {
    diags.error("signature.patterns", json::pointer(path, "patterns"),
                "patterns is a non-empty list of byte patterns.");
  } else {
    for (rapidjson::SizeType i = 0; i < patterns->Size(); ++i) {
      const std::string where =
          json::pointer(json::pointer(path, "patterns"), i);
      const rapidjson::Value &item = (*patterns)[i];
      std::string error;
      if (!item.IsString() || !parse_pattern(view_of(item), &error)) {
        diags.error("signature.pattern", where,
                    "Not a byte pattern" + (error.empty()
                                                ? std::string(".")
                                                : ": " + error + "."));
        continue;
      }
      sig.patterns.push_back(view_of(item));
    }
  }

  sig.offset = integer(entry, "offset", path, diags).value_or(0);
  const auto disp_offset = integer(entry, "disp_offset", path, diags);
  const auto insn_end = integer(entry, "insn_end", path, diags);
  if (sig.kind == symbol_kind::data_rip) {
    if (!disp_offset || !insn_end) {
      diags.error("signature.data_rip", path,
                  "A data_rip signature needs disp_offset and insn_end.");
    }
    sig.disp_offset = disp_offset.value_or(0);
    sig.insn_end = insn_end.value_or(0);
  } else if (disp_offset || insn_end) {
    diags.warning("signature.data_rip.unused", path,
                  "disp_offset and insn_end only apply to data_rip.");
  }

  if (const rapidjson::Value *known = json::member(entry, "known")) {
    if (!known->IsObject()) {
      diags.error("signature.known", json::pointer(path, "known"),
                  "known maps a build checksum to an RVA.");
    } else {
      for (auto it = known->MemberBegin(); it != known->MemberEnd(); ++it) {
        const std::string build = view_of(it->name);
        const std::string where =
            json::pointer(json::pointer(path, "known"), build);
        const auto checksum = hex_number(build);
        const auto rva = it->value.IsString() ? hex_number(view_of(it->value))
                                              : std::nullopt;
        if (!checksum || *checksum > 0xFFFFFFFFu || !rva) {
          diags.error("signature.known.entry", where,
                      "Write builds and RVAs as \"0x\" hex strings, such as "
                      "\"0x6531394\": \"0x1423D20\".");
          continue;
        }
        sig.known[static_cast<std::uint32_t>(*checksum)] = *rva;
      }
    }
  }

  if (const rapidjson::Value *features = json::member(entry, "features")) {
    if (!features->IsArray()) {
      diags.error("signature.features", json::pointer(path, "features"),
                  "features is a list of feature names.");
    } else {
      for (const auto &feature : features->GetArray()) {
        if (feature.IsString() && feature.GetStringLength() > 0) {
          sig.features.push_back(view_of(feature));
        } else {
          diags.error("signature.features", json::pointer(path, "features"),
                      "Feature names are non-empty strings.");
        }
      }
    }
  }
  out.signatures.push_back(std::move(sig));
}

template <typename T>
bool read_at(std::span<const std::uint8_t> image, std::size_t offset,
             T &value) {
  if (offset > image.size() || image.size() - offset < sizeof(T)) {
    return false;
  }
  std::memcpy(&value, image.data() + offset, sizeof(T));
  return true;
}
} // namespace

signature_registry parse_signature_registry(std::string_view text) {
  signature_registry out;
  const json::parse_result parsed = json::parse(text);
  if (!parsed.ok) {
    out.diags.error("signature.json", "",
                    "Not JSON (line " + std::to_string(parsed.line) +
                        ", column " + std::to_string(parsed.column) +
                        "): " + parsed.error);
    return out;
  }
  const rapidjson::Value &doc = parsed.document;
  if (!doc.IsObject()) {
    out.diags.error("signature.type", "", "The registry is a JSON object.");
    return out;
  }
  if (json::get_string(doc, "schema") != signature_registry_schema) {
    out.diags.error("signature.schema", "/schema",
                    "schema must be \"" +
                        std::string(signature_registry_schema) + "\".");
    return out;
  }
  const rapidjson::Value *list = json::member(doc, "signatures");
  if (list == nullptr || !list->IsArray()) {
    out.diags.error("signature.list", "/signatures", "signatures is a list.");
    return out;
  }
  std::set<std::string> ids;
  for (rapidjson::SizeType i = 0; i < list->Size(); ++i) {
    parse_entry((*list)[i], json::pointer("/signatures", i), ids, out);
  }
  return out;
}

std::optional<image_layout>
read_image_layout(std::span<const std::uint8_t> image, std::string *error) {
  const auto fail = [&](const char *why) -> std::optional<image_layout> {
    if (error != nullptr) {
      *error = why;
    }
    return std::nullopt;
  };
  std::uint16_t mz = 0;
  std::uint32_t nt = 0;
  std::uint32_t signature_bytes = 0;
  if (!read_at(image, 0, mz) || mz != 0x5A4D || !read_at(image, 0x3C, nt) ||
      !read_at(image, nt, signature_bytes) || signature_bytes != 0x00004550) {
    return fail("not a PE image (no MZ or PE signature)");
  }
  std::uint16_t count = 0;
  std::uint16_t optional_size = 0;
  std::uint16_t magic = 0;
  const std::size_t optional = std::size_t{nt} + 24;
  if (!read_at(image, std::size_t{nt} + 6, count) ||
      !read_at(image, std::size_t{nt} + 20, optional_size) ||
      !read_at(image, optional, magic)) {
    return fail("truncated PE header");
  }
  if (magic != 0x20B) {
    return fail("not a 64-bit (PE32+) image");
  }
  image_layout layout;
  if (!read_at(image, optional + 56, layout.size_of_image) ||
      !read_at(image, optional + 64, layout.checksum)) {
    return fail("truncated optional header");
  }
  const std::size_t table = optional + optional_size;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t entry = table + i * 40;
    char name[9] = {};
    std::uint32_t virtual_size = 0;
    std::uint32_t virtual_address = 0;
    if (entry + 40 > image.size() || !read_at(image, entry + 8, virtual_size) ||
        !read_at(image, entry + 12, virtual_address)) {
      return fail("truncated section table");
    }
    std::memcpy(name, image.data() + entry, 8);
    section_view section;
    section.name = name;
    section.rva = virtual_address;
    if (virtual_address < image.size()) {
      section.bytes = image.subspan(
          virtual_address,
          std::min<std::size_t>(virtual_size, image.size() - virtual_address));
    }
    layout.sections.push_back(std::move(section));
  }
  return layout;
}
} // namespace xerces_core
