#include "pack.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <random>

#include <zstd.h>

#include "json.hpp"
#include "manifest.hpp"

namespace xerces_core {
namespace {
using rapidjson::Value;

constexpr char base64url_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string_view view(const Value &value) {
  return {value.GetString(), value.GetStringLength()};
}

bool is_sha256_tag(std::string_view text) {
  return text.starts_with("sha256:") && text.size() == 7 + 64 &&
         std::ranges::all_of(text.substr(7), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::string lower(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

// Packs never carry local paths or user names (8.1).
bool looks_like_local_path(std::string_view text) {
  const auto alnum = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
  };
  // "D:\" or "D:/" anywhere, unless the letter ends a word ("http://").
  for (std::size_t i = 0; i + 2 < text.size(); ++i) {
    if (std::isalpha(static_cast<unsigned char>(text[i])) &&
        text[i + 1] == ':' && (text[i + 2] == '\\' || text[i + 2] == '/') &&
        (i == 0 || !alnum(text[i - 1]))) {
      return true;
    }
  }
  if (text.size() >= 3 && text.starts_with("//") && alnum(text[2])) {
    return true; // a network share written with forward slashes
  }
  const std::string l = lower(text);
  return l.find("\\\\") != std::string::npos ||
         l.find("file:/") != std::string::npos ||
         l.find("/users/") != std::string::npos ||
         l.find("/home/") != std::string::npos ||
         l.find("\\users\\") != std::string::npos ||
         l.find("%userprofile%") != std::string::npos ||
         l.find("%appdata%") != std::string::npos ||
         l.find("%localappdata%") != std::string::npos;
}

void scan_strings(const Value &value, const std::string &path,
                  diagnostics &diags) {
  if (value.IsString()) {
    if (looks_like_local_path(view(value))) {
      diags.error("pack.local_path", path,
                  "Packs must not contain local paths.");
    }
  } else if (value.IsArray()) {
    std::size_t index = 0;
    for (const auto &item : value.GetArray()) {
      scan_strings(item, json::pointer(path, index++), diags);
    }
  } else if (value.IsObject()) {
    for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
      const std::string child = json::pointer(path, view(it->name));
      if (looks_like_local_path(view(it->name))) {
        diags.error("pack.local_path", child,
                    "Packs must not contain local paths.");
      }
      scan_strings(it->value, child, diags);
    }
  }
}

void warn_unknown(const Value &object, const std::string &path,
                  std::initializer_list<std::string_view> known,
                  diagnostics &diags) {
  for (auto it = object.MemberBegin(); it != object.MemberEnd(); ++it) {
    const std::string_view key = view(it->name);
    if (std::ranges::find(known, key) == known.end()) {
      diags.warning("pack.field.unknown", json::pointer(path, key),
                    "Unknown field \"" + std::string(key) + "\" is ignored.");
    }
  }
}
} // namespace

bool valid_uuid(std::string_view text) {
  if (text.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') {
        return false;
      }
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::string new_pack_id() {
  std::random_device device;
  std::array<std::uint8_t, 16> bytes{};
  for (auto &byte : bytes) {
    byte = static_cast<std::uint8_t>(device() & 0xff);
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40); // version 4
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80); // RFC 4122
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out.push_back('-');
    }
    out.push_back(digits[bytes[i] >> 4]);
    out.push_back(digits[bytes[i] & 0x0f]);
  }
  return out;
}

std::string base64url_encode(std::string_view bytes) {
  std::string out;
  out.reserve((bytes.size() * 4 + 2) / 3);
  std::size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const auto n =
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i]))
         << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 1]))
         << 8) |
        static_cast<std::uint8_t>(bytes[i + 2]);
    out.push_back(base64url_alphabet[(n >> 18) & 63]);
    out.push_back(base64url_alphabet[(n >> 12) & 63]);
    out.push_back(base64url_alphabet[(n >> 6) & 63]);
    out.push_back(base64url_alphabet[n & 63]);
  }
  if (const std::size_t rest = bytes.size() - i; rest > 0) {
    std::uint32_t n =
        static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i])) << 16;
    if (rest == 2) {
      n |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 1]))
           << 8;
    }
    out.push_back(base64url_alphabet[(n >> 18) & 63]);
    out.push_back(base64url_alphabet[(n >> 12) & 63]);
    if (rest == 2) {
      out.push_back(base64url_alphabet[(n >> 6) & 63]);
    }
  }
  return out;
}

std::optional<std::string> base64url_decode(std::string_view text) {
  std::array<int, 256> lookup{};
  lookup.fill(-1);
  for (int i = 0; i < 64; ++i) {
    lookup[static_cast<unsigned char>(base64url_alphabet[i])] = i;
  }
  if (text.size() % 4 == 1) {
    return std::nullopt;
  }
  std::string out;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char c : text) {
    const int value = lookup[static_cast<unsigned char>(c)];
    if (value < 0) {
      return std::nullopt;
    }
    buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xff));
    }
  }
  return out;
}

pack_result parse_pack(std::string_view text) {
  pack_result result;
  pack &p = result.value;
  diagnostics &diags = result.diags;
  const auto parsed = json::parse(text);
  if (!parsed.ok) {
    diags.error("pack.json", "",
                "Not valid JSON (line " + std::to_string(parsed.line) +
                    "): " + parsed.error);
    return result;
  }
  const Value &root = parsed.document;
  if (!root.IsObject()) {
    diags.error("pack.json", "", "A pack is a JSON object.");
    return result;
  }
  scan_strings(root, "", diags);
  warn_unknown(root, "",
               {"schema", "id", "name", "mode", "map", "mods", "order",
                "requires", "decisions"},
               diags);

  if (const Value *schema = json::member(root, "schema");
      schema == nullptr || !schema->IsInt64() || schema->GetInt64() != 1) {
    diags.error("pack.schema", "/schema", "This Xerces reads pack schema 1.");
  }
  p.id = json::get_string(root, "id").value_or("");
  if (!valid_uuid(p.id)) {
    diags.error("pack.id", "/id", "A pack id is a lower-case UUID.");
  }
  p.name = json::get_string(root, "name").value_or("");
  if (p.name.empty() || json::length(p.name) > 80) {
    diags.error("pack.name", "/name",
                "A pack needs a name of 1 to 80 characters.");
  }
  p.mode = json::get_string(root, "mode").value_or("");
  if (!valid_mode(p.mode)) {
    diags.error("pack.mode", "/mode", "Modes are \"zm\", \"mp\" and \"cp\".");
  }

  if (const Value *map = json::member(root, "map");
      map != nullptr && !map->IsNull()) {
    pack_map entry;
    entry.id = map->IsObject() ? json::get_string(*map, "id").value_or("") : "";
    entry.content_hash =
        map->IsObject() ? json::get_string(*map, "content_hash").value_or("")
                        : "";
    if (!valid_mod_id(entry.id)) {
      diags.error("pack.map.id", "/map/id", "The map needs a valid id.");
    }
    if (!is_sha256_tag(entry.content_hash)) {
      diags.error("pack.hash", "/map/content_hash",
                  "Content hashes are \"sha256:\" and 64 hex digits.");
    }
    p.map = entry;
  }

  std::set<std::string> ids;
  if (const Value *mods = json::member(root, "mods");
      mods == nullptr || !mods->IsArray()) {
    diags.error("pack.mods", "/mods", "\"mods\" is a list.");
  } else {
    std::size_t index = 0;
    for (const auto &item : mods->GetArray()) {
      const std::string path = json::pointer("/mods", index++);
      if (!item.IsObject()) {
        diags.error("pack.mods", path, "Every mod entry is an object.");
        continue;
      }
      warn_unknown(item, path,
                   {"id", "version", "source", "content_hash", "settings"},
                   diags);
      pack_mod entry;
      entry.id = json::get_string(item, "id").value_or("");
      if (!valid_mod_id(entry.id)) {
        diags.error("pack.mod.id", json::pointer(path, "id"),
                    "\"" + entry.id + "\" is not a valid mod id.");
      } else if (!ids.insert(entry.id).second) {
        diags.error("pack.mod.duplicate", json::pointer(path, "id"),
                    "\"" + entry.id + "\" is listed twice.");
      }
      const auto text_version = json::get_string(item, "version").value_or("");
      if (const auto parsed_version = version::parse(text_version)) {
        entry.ver = *parsed_version;
      } else {
        diags.error("pack.mod.version", json::pointer(path, "version"),
                    "Packs pin exact SemVer versions.");
      }
      if (const Value *source = json::member(item, "source");
          source != nullptr) {
        const Value *workshop =
            source->IsObject() ? json::member(*source, "workshop") : nullptr;
        if (workshop != nullptr && workshop->IsUint64()) {
          entry.workshop_id = workshop->GetUint64();
        } else if (workshop != nullptr) {
          diags.error("pack.mod.source", json::pointer(path, "source"),
                      "\"workshop\" is the published file id.");
        }
      }
      entry.content_hash = json::get_string(item, "content_hash").value_or("");
      if (!is_sha256_tag(entry.content_hash)) {
        diags.error("pack.hash", json::pointer(path, "content_hash"),
                    "Content hashes are \"sha256:\" and 64 hex digits.");
      }
      if (const Value *settings = json::member(item, "settings");
          settings != nullptr) {
        if (!settings->IsObject()) {
          diags.error("pack.mod.settings", json::pointer(path, "settings"),
                      "\"settings\" is an object.");
        } else {
          for (auto it = settings->MemberBegin(); it != settings->MemberEnd();
               ++it) {
            entry.settings[std::string(view(it->name))] =
                json::canonical(it->value);
          }
        }
      }
      p.mods.push_back(std::move(entry));
    }
  }

  if (const Value *order = json::member(root, "order");
      order == nullptr || !order->IsArray()) {
    diags.error("pack.order", "/order", "\"order\" lists the mod ids.");
  } else {
    for (const auto &item : order->GetArray()) {
      if (item.IsString()) {
        p.order.emplace_back(view(item));
      }
    }
    std::set<std::string> ordered(p.order.begin(), p.order.end());
    if (ordered != ids || ordered.size() != p.order.size()) {
      diags.error("pack.order", "/order",
                  "\"order\" must list every mod in \"mods\" exactly once.");
    }
  }

  if (const Value *requires_value = json::member(root, "requires");
      requires_value != nullptr && requires_value->IsObject()) {
    p.requires_xerces =
        json::get_string(*requires_value, "xerces").value_or("");
    if (!p.requires_xerces.empty() &&
        !version_range::parse(p.requires_xerces)) {
      diags.error("pack.requires", "/requires/xerces",
                  "\"" + p.requires_xerces + "\" is not a version range.");
    }
    p.game_build = json::get_string(*requires_value, "game_build").value_or("");
  }

  if (const Value *decisions = json::member(root, "decisions");
      decisions != nullptr && decisions->IsObject()) {
    if (const Value *pairs = json::member(*decisions, "allow_conflicts");
        pairs != nullptr && pairs->IsArray()) {
      for (const auto &pair : pairs->GetArray()) {
        if (pair.IsArray() && pair.Size() == 2 && pair[0].IsString() &&
            pair[1].IsString()) {
          std::string a(view(pair[0]));
          std::string b(view(pair[1]));
          if (b < a) {
            std::swap(a, b);
          }
          p.decisions.allow_conflicts.insert({a, b});
        }
      }
    }
    for (const auto &[key, target] :
         {std::pair{"allow_overrides", &p.decisions.allow_overrides},
          {"keep_order", &p.decisions.keep_order}}) {
      if (const Value *list = json::member(*decisions, key);
          list != nullptr && list->IsArray()) {
        for (const auto &item : list->GetArray()) {
          if (item.IsString()) {
            target->insert(std::string(view(item)));
          }
        }
      }
    }
    if (const Value *first = json::member(*decisions, "map_scripts_first");
        first != nullptr && first->IsBool()) {
      p.decisions.map_scripts_first = first->GetBool();
    }
  }
  return result;
}

std::string pack_to_json(const pack &value) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "schema", value.schema, alloc);
  json::set(doc, "id", value.id, alloc);
  json::set(doc, "name", value.name, alloc);
  json::set(doc, "mode", value.mode, alloc);
  if (value.map) {
    Value map(rapidjson::kObjectType);
    json::set(map, "id", value.map->id, alloc);
    json::set(map, "content_hash", value.map->content_hash, alloc);
    json::set(doc, "map", std::move(map), alloc);
  }
  Value mods(rapidjson::kArrayType);
  for (const auto &mod : value.mods) {
    Value entry(rapidjson::kObjectType);
    json::set(entry, "id", mod.id, alloc);
    json::set(entry, "version", mod.ver.to_string(), alloc);
    if (mod.workshop_id) {
      Value source(rapidjson::kObjectType);
      source.AddMember("workshop", Value(*mod.workshop_id), alloc);
      json::set(entry, "source", std::move(source), alloc);
    }
    json::set(entry, "content_hash", mod.content_hash, alloc);
    Value settings(rapidjson::kObjectType);
    for (const auto &[key, raw] : mod.settings) {
      auto parsed = json::parse(raw);
      json::set(settings, key,
                parsed.ok ? json::copy(parsed.document, alloc) : Value(),
                alloc);
    }
    json::set(entry, "settings", std::move(settings), alloc);
    mods.PushBack(std::move(entry), alloc);
  }
  json::set(doc, "mods", std::move(mods), alloc);
  json::set(doc, "order", json::string_array(value.order, alloc), alloc);
  Value requires_value(rapidjson::kObjectType);
  if (!value.requires_xerces.empty()) {
    json::set(requires_value, "xerces", value.requires_xerces, alloc);
  }
  if (!value.game_build.empty()) {
    json::set(requires_value, "game_build", value.game_build, alloc);
  }
  json::set(doc, "requires", std::move(requires_value), alloc);

  const auto &d = value.decisions;
  if (!d.allow_conflicts.empty() || !d.allow_overrides.empty() ||
      !d.keep_order.empty() || !d.map_scripts_first) {
    Value decisions(rapidjson::kObjectType);
    Value pairs(rapidjson::kArrayType);
    for (const auto &[a, b] : d.allow_conflicts) {
      pairs.PushBack(json::string_array({a, b}, alloc), alloc);
    }
    json::set(decisions, "allow_conflicts", std::move(pairs), alloc);
    json::set(decisions, "allow_overrides",
              json::string_array(
                  {d.allow_overrides.begin(), d.allow_overrides.end()}, alloc),
              alloc);
    json::set(
        decisions, "keep_order",
        json::string_array({d.keep_order.begin(), d.keep_order.end()}, alloc),
        alloc);
    json::set(decisions, "map_scripts_first", d.map_scripts_first, alloc);
    json::set(doc, "decisions", std::move(decisions), alloc);
  }
  return json::canonical(doc);
}

std::string encode_share_code(const pack &value) {
  const std::string text = pack_to_json(value);
  std::string compressed(ZSTD_compressBound(text.size()), '\0');
  const std::size_t size = ZSTD_compress(compressed.data(), compressed.size(),
                                         text.data(), text.size(), 19);
  if (ZSTD_isError(size)) {
    return {};
  }
  compressed.resize(size);
  return std::string(share_code_prefix) + base64url_encode(compressed);
}

pack_result decode_share_code(std::string_view code) {
  pack_result failed;
  std::string_view trimmed = code;
  while (!trimmed.empty() &&
         std::isspace(static_cast<unsigned char>(trimmed.back()))) {
    trimmed.remove_suffix(1);
  }
  while (!trimmed.empty() &&
         std::isspace(static_cast<unsigned char>(trimmed.front()))) {
    trimmed.remove_prefix(1);
  }
  if (!trimmed.starts_with(share_code_prefix)) {
    failed.diags.error("pack.code.prefix", "",
                       "Offline share codes start with \"XRC1.\".");
    return failed;
  }
  const auto bytes = base64url_decode(trimmed.substr(share_code_prefix.size()));
  if (!bytes) {
    failed.diags.error("pack.code.encoding", "", "The share code is damaged.");
    return failed;
  }
  const unsigned long long declared =
      ZSTD_getFrameContentSize(bytes->data(), bytes->size());
  if (declared == ZSTD_CONTENTSIZE_ERROR ||
      declared == ZSTD_CONTENTSIZE_UNKNOWN || declared > share_code_max_json) {
    failed.diags.error("pack.code.size", "",
                       "The share code is damaged or too large.");
    return failed;
  }
  std::string text(static_cast<std::size_t>(declared), '\0');
  const std::size_t size =
      ZSTD_decompress(text.data(), text.size(), bytes->data(), bytes->size());
  if (ZSTD_isError(size) || size != declared) {
    failed.diags.error("pack.code.encoding", "", "The share code is damaged.");
    return failed;
  }
  return parse_pack(text);
}

plan_request pack_to_request(const pack &value, const std::string &game_build,
                             const version &xerces_version) {
  plan_request request;
  request.mode = value.mode;
  request.mods = value.order;
  if (value.map) {
    request.map = value.map->id;
  }
  for (const auto &mod : value.mods) {
    request.pins[mod.id] = mod.ver.to_string();
    if (!mod.settings.empty()) {
      request.settings[mod.id] = mod.settings;
    }
  }
  request.allowed_conflicts = value.decisions.allow_conflicts;
  request.game_build = game_build;
  request.xerces_version = xerces_version;
  for (std::size_t i = 0; i < value.order.size(); ++i) {
    request.saved_positions[value.order[i]] = static_cast<std::int64_t>(i);
  }
  return request;
}

diagnostics verify_pack_content(const pack &value, const plan &resolved) {
  diagnostics diags;
  for (const auto &mod : value.mods) {
    const auto entry = std::ranges::find_if(
        resolved.stack, [&](const plan_entry &e) { return e.id == mod.id; });
    if (entry != resolved.stack.end() &&
        entry->content_hash != mod.content_hash) {
      diags.error("pack.hash.mismatch", mod.id,
                  entry->name + "'s installed files differ from the ones this "
                                "pack pinned (was it updated?).");
    }
  }
  if (value.map && resolved.map && resolved.map->id == value.map->id &&
      resolved.map->content_hash != value.map->content_hash) {
    diags.error("pack.hash.mismatch", value.map->id,
                resolved.map->name + "'s installed files differ from the ones "
                                     "this pack pinned.");
  }
  return diags;
}

pack pack_from_plan(const plan &resolved, const std::string &name,
                    const std::string &id,
                    const std::map<std::string, std::uint64_t> &workshop_ids) {
  pack out;
  out.id = id;
  out.name = name;
  out.mode = resolved.mode;
  const auto workshop_of =
      [&](const std::string &mod_id) -> std::optional<std::uint64_t> {
    if (const auto it = workshop_ids.find(mod_id); it != workshop_ids.end()) {
      return it->second;
    }
    std::uint64_t number = 0;
    const char *end = mod_id.data() + mod_id.size();
    if (mod_id.starts_with("ws.") && mod_id.size() > 3) {
      // Digits past 64 bits are not a published file id.
      if (const auto parsed = std::from_chars(mod_id.data() + 3, end, number);
          parsed.ec == std::errc{} && parsed.ptr == end) {
        return number;
      }
    }
    return std::nullopt;
  };
  for (const auto &entry : resolved.stack) {
    pack_mod mod;
    mod.id = entry.id;
    mod.ver = entry.ver;
    mod.workshop_id = workshop_of(entry.id);
    mod.content_hash = entry.content_hash;
    mod.settings = entry.settings;
    out.mods.push_back(std::move(mod));
    out.order.push_back(entry.id);
  }
  if (resolved.map) {
    out.map = pack_map{resolved.map->id, resolved.map->content_hash};
  }
  return out;
}
} // namespace xerces_core
