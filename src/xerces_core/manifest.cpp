#include "manifest.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

#include "json.hpp"
#include "sha256.hpp"

namespace xerces_core {
namespace {
using rapidjson::Value;

std::string_view view(const Value &value) {
  return {value.GetString(), value.GetStringLength()};
}

bool is_identifier(std::string_view text) {
  if (text.empty() || text.size() > 64) {
    return false;
  }
  if (!std::isalpha(static_cast<unsigned char>(text.front())) &&
      text.front() != '_') {
    return false;
  }
  return std::ranges::all_of(text, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  });
}

bool is_sha256_tag(std::string_view text) {
  if (!text.starts_with("sha256:") || text.size() != 7 + 64) {
    return false;
  }
  return std::ranges::all_of(text.substr(7), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

std::string lower_ascii(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

bool is_executable_file(std::string_view path) {
  std::string lower(path);
  std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  for (const std::string_view ext : {".dll", ".exe", ".asi", ".sys", ".com"}) {
    if (lower.ends_with(ext)) {
      return true;
    }
  }
  return false;
}

// Reads one object, tracking which keys were understood.
class object_reader {
public:
  object_reader(const Value &object, std::string path, diagnostics &diags)
      : object_(object), path_(std::move(path)), diags_(diags) {}

  const Value *get(std::string_view key) {
    known_.insert(std::string(key));
    return json::member(object_, key);
  }

  std::optional<std::string> string(std::string_view key, bool required,
                                    std::size_t max_length = 4096) {
    const Value *value = get(key);
    if (value == nullptr) {
      if (required) {
        diags_.error("manifest.field.missing", json::pointer(path_, key),
                     "\"" + std::string(key) + "\" is required.");
      }
      return std::nullopt;
    }
    if (!value->IsString()) {
      diags_.error("manifest.field.type", json::pointer(path_, key),
                   "\"" + std::string(key) + "\" must be a string.");
      return std::nullopt;
    }
    if (json::length(view(*value)) > max_length) {
      diags_.error("manifest.field.too_long", json::pointer(path_, key),
                   "\"" + std::string(key) + "\" is longer than " +
                       std::to_string(max_length) + " characters.");
      return std::nullopt;
    }
    return std::string(view(*value));
  }

  std::optional<bool> boolean(std::string_view key) {
    const Value *value = get(key);
    if (value == nullptr) {
      return std::nullopt;
    }
    if (!value->IsBool()) {
      diags_.error("manifest.field.type", json::pointer(path_, key),
                   "\"" + std::string(key) + "\" must be true or false.");
      return std::nullopt;
    }
    return value->GetBool();
  }

  std::optional<std::vector<std::string>> strings(std::string_view key) {
    const Value *value = get(key);
    if (value == nullptr) {
      return std::nullopt;
    }
    if (!value->IsArray()) {
      diags_.error("manifest.field.type", json::pointer(path_, key),
                   "\"" + std::string(key) + "\" must be a list of strings.");
      return std::nullopt;
    }
    std::vector<std::string> out;
    std::size_t index = 0;
    for (const auto &item : value->GetArray()) {
      if (!item.IsString()) {
        diags_.error("manifest.field.type",
                     json::pointer(json::pointer(path_, key), index),
                     "Every entry of \"" + std::string(key) +
                         "\" must be a string.");
      } else {
        out.emplace_back(view(item));
      }
      ++index;
    }
    return out;
  }

  const Value *object(std::string_view key) {
    const Value *value = get(key);
    if (value != nullptr && !value->IsObject()) {
      diags_.error("manifest.field.type", json::pointer(path_, key),
                   "\"" + std::string(key) + "\" must be an object.");
      return nullptr;
    }
    return value;
  }

  const Value *array(std::string_view key) {
    const Value *value = get(key);
    if (value != nullptr && !value->IsArray()) {
      diags_.error("manifest.field.type", json::pointer(path_, key),
                   "\"" + std::string(key) + "\" must be a list.");
      return nullptr;
    }
    return value;
  }

  // Unknown fields are kept (the manifest is not rewritten) and warned about.
  void finish(std::vector<std::string> &unknown) {
    for (auto it = object_.MemberBegin(); it != object_.MemberEnd(); ++it) {
      const std::string key(view(it->name));
      if (!known_.contains(key)) {
        const std::string pointer = json::pointer(path_, key);
        unknown.push_back(pointer);
        diags_.warning("manifest.field.unknown", pointer,
                       "Unknown field \"" + key + "\" is ignored.");
      }
    }
  }

  const std::string &path() const { return path_; }
  diagnostics &diags() { return diags_; }

private:
  const Value &object_;
  std::string path_;
  diagnostics &diags_;
  std::set<std::string> known_;
};

void read_assets(const Value *list, const std::string &path, bool overrides,
                 bool extends, std::vector<asset_ref> &out, manifest &m,
                 diagnostics &diags) {
  if (list == nullptr) {
    return;
  }
  std::size_t index = 0;
  for (const auto &item : list->GetArray()) {
    const std::string item_path = json::pointer(path, index++);
    if (!item.IsObject()) {
      diags.error("manifest.field.type", item_path,
                  "Every asset entry must be an object.");
      continue;
    }
    object_reader reader(item, item_path, diags);
    asset_ref ref;
    ref.type = lower_ascii(reader.string("type", true, 64).value_or(""));
    ref.name =
        normalize_asset_name(reader.string("name", true, 256).value_or(""));
    ref.reason = reader.string("reason", false, 512).value_or("");
    if (overrides && ref.reason.empty()) {
      diags.warning("manifest.override.reason", item_path,
                    "Give a reason for every override; reports show it.");
    }
    if (const Value *key = reader.get("key"); key != nullptr) {
      if (!extends || !key->IsArray()) {
        diags.error("manifest.field.type", json::pointer(item_path, "key"),
                    "\"key\" is a list of column numbers on \"extends\" "
                    "entries.");
      } else {
        for (const auto &column : key->GetArray()) {
          if (!column.IsInt() || column.GetInt() < 0) {
            diags.error("manifest.field.type", json::pointer(item_path, "key"),
                        "Key columns are numbers from 0.");
          } else {
            ref.key.push_back(column.GetInt());
          }
        }
      }
    }
    if (extends && ref.key.empty()) {
      ref.key = {0};
    }
    reader.finish(m.unknown_fields);
    if (!ref.type.empty() && !ref.name.empty()) {
      out.push_back(std::move(ref));
    }
  }
}

std::optional<hook_kind> parse_hook_kind(std::string_view text) {
  if (text == "before") {
    return hook_kind::before;
  }
  if (text == "after") {
    return hook_kind::after;
  }
  if (text == "wrap") {
    return hook_kind::wrap;
  }
  if (text == "replace") {
    return hook_kind::replace;
  }
  return std::nullopt;
}

bool valid_function_ref(std::string_view text) {
  const std::size_t sep = text.find("::");
  return sep != std::string_view::npos && sep > 0 &&
         is_identifier(text.substr(sep + 2)) &&
         (text.substr(0, sep).ends_with(".gsc") ||
          text.substr(0, sep).ends_with(".csc"));
}

void read_setting(const Value &item, const std::string &path, manifest &m,
                  diagnostics &diags) {
  object_reader reader(item, path, diags);
  setting_decl setting;
  setting.key = reader.string("key", true, 64).value_or("");
  if (!setting.key.empty() && !is_identifier(setting.key)) {
    diags.error("manifest.setting.key", json::pointer(path, "key"),
                "Setting keys use letters, digits and underscores.");
  }
  setting.type = reader.string("type", true, 16).value_or("");
  setting.label = reader.string("label", false, 120).value_or(setting.key);
  setting.dvar = reader.string("dvar", false, 64).value_or("");
  if (!setting.dvar.empty() && !is_identifier(setting.dvar)) {
    diags.error("manifest.setting.dvar", json::pointer(path, "dvar"),
                "Dvar names use letters, digits and underscores.");
  }
  setting.scope = reader.string("scope", false, 16).value_or("server");
  if (setting.scope != "server" && setting.scope != "client") {
    diags.error("manifest.setting.scope", json::pointer(path, "scope"),
                "\"scope\" is \"server\" or \"client\".");
  }
  if (auto options = reader.strings("options")) {
    setting.options = *options;
  }
  const auto read_bound = [&](std::string_view key) -> std::optional<double> {
    const Value *bound = reader.get(key);
    if (bound == nullptr) {
      return std::nullopt;
    }
    if (!bound->IsNumber()) {
      diags.error("manifest.field.type", json::pointer(path, key),
                  "\"" + std::string(key) + "\" must be a number.");
      return std::nullopt;
    }
    return bound->GetDouble();
  };
  setting.min = read_bound("min");
  setting.max = read_bound("max");

  const Value *default_value = reader.get("default");
  const std::string default_path = json::pointer(path, "default");
  if (default_value == nullptr) {
    diags.error("manifest.field.missing", default_path,
                "Every setting needs a \"default\".");
  } else {
    bool fits = false;
    if (setting.type == "bool") {
      fits = default_value->IsBool();
    } else if (setting.type == "int") {
      fits = default_value->IsInt64();
    } else if (setting.type == "float") {
      fits = default_value->IsNumber();
    } else if (setting.type == "string") {
      fits = default_value->IsString();
    } else if (setting.type == "enum") {
      fits = default_value->IsString() &&
             std::ranges::find(setting.options,
                               std::string(view(*default_value))) !=
                 setting.options.end();
      if (setting.options.empty()) {
        diags.error("manifest.setting.options", json::pointer(path, "options"),
                    "An enum setting lists its \"options\".");
      }
    } else if (!setting.type.empty()) {
      diags.error("manifest.setting.type", json::pointer(path, "type"),
                  "Setting types are bool, int, float, string and enum.");
      fits = true;
    }
    if (!fits) {
      diags.error("manifest.setting.default", default_path,
                  "The default does not match the setting's type.");
    } else if (default_value->IsNumber() &&
               ((setting.min && default_value->GetDouble() < *setting.min) ||
                (setting.max && default_value->GetDouble() > *setting.max))) {
      diags.error("manifest.setting.default", default_path,
                  "The default is outside min and max.");
    }
    setting.default_json = json::canonical(*default_value);
  }
  if (setting.min && setting.max && *setting.min > *setting.max) {
    diags.error("manifest.setting.range", path, "\"min\" is above \"max\".");
  }
  reader.finish(m.unknown_fields);
  if (std::ranges::any_of(m.settings, [&](const setting_decl &existing) {
        return existing.key == setting.key;
      })) {
    diags.error("manifest.setting.duplicate", path,
                "Setting \"" + setting.key + "\" is declared twice.");
    return;
  }
  m.settings.push_back(std::move(setting));
}

void put_assets(Value &object, std::string_view key,
                const std::vector<asset_ref> &assets, bool extends,
                json::allocator &alloc) {
  if (assets.empty()) {
    return;
  }
  Value list(rapidjson::kArrayType);
  for (const auto &asset : assets) {
    Value entry(rapidjson::kObjectType);
    json::set(entry, "type", asset.type, alloc);
    json::set(entry, "name", asset.name, alloc);
    if (!asset.reason.empty()) {
      json::set(entry, "reason", asset.reason, alloc);
    }
    if (extends) {
      Value columns(rapidjson::kArrayType);
      for (const int column : asset.key) {
        columns.PushBack(column, alloc);
      }
      json::set(entry, "key", std::move(columns), alloc);
    }
    list.PushBack(std::move(entry), alloc);
  }
  json::set(object, key, std::move(list), alloc);
}
} // namespace

const char *to_string(content_kind kind) {
  return kind == content_kind::map ? "map" : "mod";
}

const char *to_string(hook_kind kind) {
  switch (kind) {
  case hook_kind::before:
    return "before";
  case hook_kind::after:
    return "after";
  case hook_kind::wrap:
    return "wrap";
  case hook_kind::replace:
    return "replace";
  }
  return "wrap";
}

bool manifest::supports_mode(std::string_view mode) const {
  return modes.empty() || std::ranges::find(modes, mode) != modes.end();
}

bool manifest::declares_override(std::string_view type,
                                 std::string_view name) const {
  return std::ranges::any_of(overrides, [&](const asset_ref &ref) {
    return ref.type == type && ref.name == name;
  });
}

bool valid_mod_id(std::string_view id) {
  if (id.size() < 3 || id.size() > 64) {
    return false;
  }
  const auto lower_alnum = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  if (!lower_alnum(id.front())) {
    return false;
  }
  return std::ranges::all_of(id.substr(1), [&](char c) {
    return lower_alnum(c) || c == '.' || c == '_' || c == '-';
  });
}

bool valid_mode(std::string_view mode) {
  return mode == "zm" || mode == "mp" || mode == "cp";
}

std::string normalize_asset_name(std::string_view name) {
  std::string out = lower_ascii(name);
  std::ranges::replace(out, '\\', '/');
  while (out.starts_with("./")) {
    out.erase(0, 2);
  }
  return out;
}

manifest_result parse_manifest(std::string_view text) {
  manifest_result result;
  manifest &m = result.value;
  diagnostics &diags = result.diags;

  const json::parse_result parsed = json::parse(text);
  if (!parsed.ok) {
    diags.error("manifest.json", "",
                "Not valid JSON (line " + std::to_string(parsed.line) +
                    ", column " + std::to_string(parsed.column) +
                    "): " + parsed.error);
    return result;
  }
  const Value &root = parsed.document;
  if (!root.IsObject()) {
    diags.error("manifest.json", "", "The manifest must be a JSON object.");
    return result;
  }

  object_reader reader(root, "", diags);
  if (const Value *schema = reader.get("schema"); schema == nullptr) {
    diags.error("manifest.field.missing", "/schema", "\"schema\" is required.");
  } else if (!schema->IsInt64() || schema->GetInt64() != 1) {
    diags.error("manifest.schema.unsupported", "/schema",
                "This Xerces reads manifest schema 1.");
  }

  m.id = reader.string("id", true, 64).value_or("");
  if (!m.id.empty() && !valid_mod_id(m.id)) {
    diags.error("manifest.id.invalid", "/id",
                "Ids are 3 to 64 characters: lower-case letters, digits, '.', "
                "'_' and '-', starting with a letter or digit.");
  }

  if (const auto kind = reader.string("kind", false, 8)) {
    if (*kind == "map") {
      m.kind = content_kind::map;
    } else if (*kind != "mod") {
      diags.error("manifest.kind", "/kind", "\"kind\" is \"mod\" or \"map\".");
    }
  }

  m.name = reader.string("name", true, 120).value_or("");
  if (const auto text_version = reader.string("version", true, 128)) {
    if (const auto parsed_version = version::parse(*text_version)) {
      m.ver = *parsed_version;
    } else {
      diags.error("manifest.version.invalid", "/version",
                  "\"" + *text_version + "\" is not a SemVer 2.0 version.");
    }
  }
  if (auto authors = reader.strings("authors")) {
    m.authors = *authors;
  }
  m.license = reader.string("license", false, 128).value_or("");

  if (const Value *requires_value = reader.object("requires")) {
    object_reader requires_reader(*requires_value, "/requires", diags);
    if (const auto range_text = requires_reader.string("xerces", false, 128)) {
      std::string error;
      if (auto range = version_range::parse(*range_text, &error)) {
        m.requires_xerces = std::move(*range);
        m.requires_xerces_text = *range_text;
      } else {
        diags.error("manifest.range.invalid", "/requires/xerces", error);
      }
    }
    if (auto modes = requires_reader.strings("modes")) {
      for (std::size_t i = 0; i < modes->size(); ++i) {
        if (!valid_mode((*modes)[i])) {
          diags.error("manifest.mode.invalid",
                      json::pointer("/requires/modes", i),
                      "Modes are \"zm\", \"mp\" and \"cp\".");
        }
      }
      m.modes = *modes;
    }
    requires_reader.finish(m.unknown_fields);
  }

  if (const Value *source = reader.object("source")) {
    object_reader source_reader(*source, "/source", diags);
    if (const Value *workshop = source_reader.get("workshop")) {
      if (workshop->IsUint64() && workshop->GetUint64() > 0) {
        m.workshop_id = workshop->GetUint64();
      } else {
        diags.error("manifest.source.workshop", "/source/workshop",
                    "\"workshop\" is the item's published file id.");
      }
    }
    source_reader.finish(m.unknown_fields);
  }

  if (const Value *dependencies = reader.array("dependencies")) {
    std::size_t index = 0;
    for (const auto &item : dependencies->GetArray()) {
      const std::string path = json::pointer("/dependencies", index++);
      if (!item.IsObject()) {
        diags.error("manifest.field.type", path,
                    "Every dependency must be an object.");
        continue;
      }
      object_reader dep_reader(item, path, diags);
      dependency_ref dep;
      dep.id = dep_reader.string("id", true, 64).value_or("");
      if (!dep.id.empty() && !valid_mod_id(dep.id)) {
        diags.error("manifest.id.invalid", json::pointer(path, "id"),
                    "\"" + dep.id + "\" is not a valid mod id.");
      }
      const auto range_text = dep_reader.string("version", false, 128);
      std::string error;
      if (auto range = range_text ? version_range::parse(*range_text, &error)
                                  : std::nullopt) {
        dep.range = std::move(*range);
        dep.range_text = *range_text;
      } else if (json::member(item, "version") != nullptr) {
        // A range that does not parse fits nothing; it must not become "*".
        if (range_text) {
          diags.error("manifest.range.invalid", json::pointer(path, "version"),
                      error);
        }
        dep.range = version_range::none();
        dep.range_text = range_text.value_or("(invalid)");
      }
      dep.optional = dep_reader.boolean("optional").value_or(false);
      dep_reader.finish(m.unknown_fields);
      if (dep.id == m.id && !dep.id.empty()) {
        diags.error("manifest.dependency.self", path,
                    "A mod cannot depend on itself.");
      } else if (std::ranges::any_of(m.dependencies, [&](const auto &other) {
                   return other.id == dep.id;
                 })) {
        diags.error("manifest.dependency.duplicate", path,
                    "\"" + dep.id + "\" is listed twice.");
      } else if (!dep.id.empty()) {
        m.dependencies.push_back(std::move(dep));
      }
    }
  }

  if (const Value *conflicts = reader.array("conflicts")) {
    std::size_t index = 0;
    for (const auto &item : conflicts->GetArray()) {
      const std::string path = json::pointer("/conflicts", index++);
      if (!item.IsObject()) {
        diags.error("manifest.field.type", path,
                    "Every conflict must be an object.");
        continue;
      }
      object_reader conflict_reader(item, path, diags);
      conflict_ref conflict;
      conflict.id = conflict_reader.string("id", true, 64).value_or("");
      conflict.reason =
          conflict_reader.string("reason", false, 512).value_or("");
      conflict_reader.finish(m.unknown_fields);
      if (!conflict.id.empty()) {
        m.conflicts.push_back(std::move(conflict));
      }
    }
  }

  if (const Value *order = reader.object("order")) {
    object_reader order_reader(*order, "/order", diags);
    m.order.after =
        order_reader.strings("after").value_or(std::vector<std::string>{});
    m.order.before =
        order_reader.strings("before").value_or(std::vector<std::string>{});
    if (const Value *priority = order_reader.get("priority")) {
      if (priority->IsInt64()) {
        m.order.priority = priority->GetInt64();
      } else {
        diags.error("manifest.field.type", "/order/priority",
                    "\"priority\" must be a whole number.");
      }
    }
    order_reader.finish(m.unknown_fields);
    for (const auto &id : m.order.after) {
      if (std::ranges::find(m.order.before, id) != m.order.before.end()) {
        diags.error("manifest.order.contradiction", "/order",
                    "\"" + id + "\" is both in \"after\" and \"before\".");
      }
    }
  }

  if (const Value *assets = reader.object("assets")) {
    object_reader assets_reader(*assets, "/assets", diags);
    read_assets(assets_reader.array("provides"), "/assets/provides", false,
                false, m.provides, m, diags);
    read_assets(assets_reader.array("overrides"), "/assets/overrides", true,
                false, m.overrides, m, diags);
    read_assets(assets_reader.array("extends"), "/assets/extends", false, true,
                m.extends, m, diags);
    assets_reader.finish(m.unknown_fields);
  }

  if (const Value *scripts = reader.object("scripts")) {
    object_reader scripts_reader(*scripts, "/scripts", diags);
    if (auto entry = scripts_reader.strings("entry")) {
      for (auto &path : *entry) {
        m.entry_scripts.push_back(normalize_asset_name(path));
      }
    }
    if (const Value *hooks = scripts_reader.array("hooks")) {
      std::size_t index = 0;
      for (const auto &item : hooks->GetArray()) {
        const std::string path = json::pointer("/scripts/hooks", index++);
        if (!item.IsObject()) {
          diags.error("manifest.field.type", path,
                      "Every hook must be an object.");
          continue;
        }
        object_reader hook_reader(item, path, diags);
        hook_decl hook;
        hook.target = normalize_asset_name(
            hook_reader.string("target", true, 320).value_or(""));
        hook.handler = normalize_asset_name(
            hook_reader.string("handler", true, 320).value_or(""));
        const auto kind_text = hook_reader.string("kind", false, 16);
        if (kind_text) {
          if (const auto kind = parse_hook_kind(*kind_text)) {
            hook.kind = *kind;
          } else {
            diags.error("manifest.hook.kind", json::pointer(path, "kind"),
                        "Hook kinds are before, after, wrap and replace.");
          }
        }
        hook_reader.finish(m.unknown_fields);
        for (const auto &[field, value] :
             {std::pair{"target", &hook.target}, {"handler", &hook.handler}}) {
          if (!value->empty() && !valid_function_ref(*value)) {
            diags.error("manifest.hook.reference", json::pointer(path, field),
                        "Write functions as \"path/script.gsc::function\".");
          }
        }
        m.hooks.push_back(std::move(hook));
      }
    }
    scripts_reader.finish(m.unknown_fields);
  }

  if (const Value *caps = reader.object("capabilities")) {
    object_reader caps_reader(*caps, "/capabilities", diags);
    m.caps.native_code = caps_reader.boolean("native_code").value_or(false);
    if (const Value *lua = caps_reader.object("lua")) {
      object_reader lua_reader(*lua, "/capabilities/lua", diags);
      m.caps.lua_unsafe = lua_reader.boolean("unsafe").value_or(false);
      m.caps.lua_net =
          lua_reader.strings("net").value_or(std::vector<std::string>{});
      m.caps.lua_files =
          lua_reader.strings("files").value_or(std::vector<std::string>{});
      lua_reader.finish(m.unknown_fields);
    }
    if (const Value *gsc = caps_reader.object("gsc")) {
      object_reader gsc_reader(*gsc, "/capabilities/gsc", diags);
      m.caps.gsc_exec_command =
          gsc_reader.boolean("exec_command").value_or(false);
      m.caps.gsc_files = gsc_reader.boolean("files").value_or(false);
      m.caps.gsc_replacefunc =
          gsc_reader.boolean("replacefunc").value_or(false);
      gsc_reader.finish(m.unknown_fields);
    }
    caps_reader.finish(m.unknown_fields);
  }

  if (const Value *settings = reader.array("settings")) {
    std::size_t index = 0;
    for (const auto &item : settings->GetArray()) {
      const std::string path = json::pointer("/settings", index++);
      if (!item.IsObject()) {
        diags.error("manifest.field.type", path,
                    "Every setting must be an object.");
        continue;
      }
      read_setting(item, path, m, diags);
    }
  }

  if (const Value *integrity = reader.object("integrity")) {
    for (auto it = integrity->MemberBegin(); it != integrity->MemberEnd();
         ++it) {
      const std::string file = normalize_asset_name(view(it->name));
      const std::string path = json::pointer("/integrity", view(it->name));
      if (!it->value.IsString() || !is_sha256_tag(view(it->value))) {
        diags.error("manifest.integrity.hash", path,
                    "Hashes are written \"sha256:\" and 64 lower-case hex "
                    "digits.");
        continue;
      }
      if (file.starts_with("/") || file.find("..") != std::string::npos ||
          file.find(':') != std::string::npos) {
        diags.error("manifest.integrity.path", path,
                    "Integrity paths are relative to the mod folder.");
        continue;
      }
      m.integrity[file] = std::string(view(it->value));
    }
  }

  // Written by manifest_to_json for synthesized manifests; the catalog does
  // not honour it in a mod's own file.
  m.legacy = reader.boolean("legacy").value_or(false);

  reader.finish(m.unknown_fields);

  // A replace hook takes a script over completely, so it needs the same
  // declared override as a whole-script replacement (6.6).
  for (std::size_t i = 0; i < m.hooks.size(); ++i) {
    const hook_decl &hook = m.hooks[i];
    if (hook.kind != hook_kind::replace) {
      continue;
    }
    const std::string script = hook.target.substr(0, hook.target.find("::"));
    if (!m.declares_override("scriptparsetree", script)) {
      diags.error("manifest.hook.replace_undeclared",
                  json::pointer("/scripts/hooks", i),
                  "A \"replace\" hook on " + script +
                      " needs a matching entry in assets.overrides.");
    }
  }
  if (m.caps.native_code) {
    diags.info("manifest.native_code", "/capabilities/native_code",
               "This mod declares native code; players are asked before it "
               "runs.");
  }
  return result;
}

std::string manifest_to_json(const manifest &value) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "schema", value.schema, alloc);
  json::set(doc, "id", value.id, alloc);
  json::set(doc, "kind", to_string(value.kind), alloc);
  json::set(doc, "name", value.name, alloc);
  json::set(doc, "version", value.ver.to_string(), alloc);
  if (!value.authors.empty()) {
    json::set(doc, "authors", json::string_array(value.authors, alloc), alloc);
  }
  if (!value.license.empty()) {
    json::set(doc, "license", value.license, alloc);
  }
  if (!value.requires_xerces_text.empty() || !value.modes.empty()) {
    Value requires_value(rapidjson::kObjectType);
    if (!value.requires_xerces_text.empty()) {
      json::set(requires_value, "xerces", value.requires_xerces_text, alloc);
    }
    if (!value.modes.empty()) {
      json::set(requires_value, "modes", json::string_array(value.modes, alloc),
                alloc);
    }
    json::set(doc, "requires", std::move(requires_value), alloc);
  }
  if (value.workshop_id) {
    Value source(rapidjson::kObjectType);
    source.AddMember("workshop", Value(*value.workshop_id), alloc);
    json::set(doc, "source", std::move(source), alloc);
  }
  if (!value.dependencies.empty()) {
    Value list(rapidjson::kArrayType);
    for (const auto &dep : value.dependencies) {
      Value entry(rapidjson::kObjectType);
      json::set(entry, "id", dep.id, alloc);
      json::set(entry, "version", dep.range_text, alloc);
      json::set(entry, "optional", dep.optional, alloc);
      list.PushBack(std::move(entry), alloc);
    }
    json::set(doc, "dependencies", std::move(list), alloc);
  }
  if (!value.conflicts.empty()) {
    Value list(rapidjson::kArrayType);
    for (const auto &conflict : value.conflicts) {
      Value entry(rapidjson::kObjectType);
      json::set(entry, "id", conflict.id, alloc);
      json::set(entry, "reason", conflict.reason, alloc);
      list.PushBack(std::move(entry), alloc);
    }
    json::set(doc, "conflicts", std::move(list), alloc);
  }
  if (!value.order.after.empty() || !value.order.before.empty() ||
      value.order.priority != 0) {
    Value order(rapidjson::kObjectType);
    json::set(order, "after", json::string_array(value.order.after, alloc),
              alloc);
    json::set(order, "before", json::string_array(value.order.before, alloc),
              alloc);
    json::set(order, "priority", value.order.priority, alloc);
    json::set(doc, "order", std::move(order), alloc);
  }
  if (!value.provides.empty() || !value.overrides.empty() ||
      !value.extends.empty()) {
    Value assets(rapidjson::kObjectType);
    put_assets(assets, "provides", value.provides, false, alloc);
    put_assets(assets, "overrides", value.overrides, false, alloc);
    put_assets(assets, "extends", value.extends, true, alloc);
    json::set(doc, "assets", std::move(assets), alloc);
  }
  if (!value.entry_scripts.empty() || !value.hooks.empty()) {
    Value scripts(rapidjson::kObjectType);
    json::set(scripts, "entry", json::string_array(value.entry_scripts, alloc),
              alloc);
    Value hooks(rapidjson::kArrayType);
    for (const auto &hook : value.hooks) {
      Value entry(rapidjson::kObjectType);
      json::set(entry, "target", hook.target, alloc);
      json::set(entry, "handler", hook.handler, alloc);
      json::set(entry, "kind", to_string(hook.kind), alloc);
      hooks.PushBack(std::move(entry), alloc);
    }
    json::set(scripts, "hooks", std::move(hooks), alloc);
    json::set(doc, "scripts", std::move(scripts), alloc);
  }
  {
    Value caps(rapidjson::kObjectType);
    json::set(caps, "native_code", value.caps.native_code, alloc);
    Value lua(rapidjson::kObjectType);
    json::set(lua, "unsafe", value.caps.lua_unsafe, alloc);
    json::set(lua, "net", json::string_array(value.caps.lua_net, alloc), alloc);
    json::set(lua, "files", json::string_array(value.caps.lua_files, alloc),
              alloc);
    json::set(caps, "lua", std::move(lua), alloc);
    Value gsc(rapidjson::kObjectType);
    json::set(gsc, "exec_command", value.caps.gsc_exec_command, alloc);
    json::set(gsc, "files", value.caps.gsc_files, alloc);
    json::set(gsc, "replacefunc", value.caps.gsc_replacefunc, alloc);
    json::set(caps, "gsc", std::move(gsc), alloc);
    json::set(doc, "capabilities", std::move(caps), alloc);
  }
  if (!value.settings.empty()) {
    Value list(rapidjson::kArrayType);
    for (const auto &setting : value.settings) {
      Value entry(rapidjson::kObjectType);
      json::set(entry, "key", setting.key, alloc);
      json::set(entry, "type", setting.type, alloc);
      json::set(entry, "label", setting.label, alloc);
      json::set(entry, "scope", setting.scope, alloc);
      if (!setting.dvar.empty()) {
        json::set(entry, "dvar", setting.dvar, alloc);
      }
      if (!setting.default_json.empty()) {
        auto parsed = json::parse(setting.default_json);
        if (parsed.ok) {
          json::set(entry, "default", json::copy(parsed.document, alloc),
                    alloc);
        }
      }
      if (setting.min) {
        entry.AddMember("min", Value(*setting.min), alloc);
      }
      if (setting.max) {
        entry.AddMember("max", Value(*setting.max), alloc);
      }
      if (!setting.options.empty()) {
        json::set(entry, "options", json::string_array(setting.options, alloc),
                  alloc);
      }
      list.PushBack(std::move(entry), alloc);
    }
    json::set(doc, "settings", std::move(list), alloc);
  }
  if (!value.integrity.empty()) {
    Value integrity(rapidjson::kObjectType);
    for (const auto &[file, hash] : value.integrity) {
      json::set(integrity, file, hash, alloc);
    }
    json::set(doc, "integrity", std::move(integrity), alloc);
  }
  if (value.legacy) {
    json::set(doc, "legacy", true, alloc);
  }
  return json::canonical(doc);
}

bool workshop_json_is_map(std::string_view workshop_json) {
  const auto parsed = json::parse(workshop_json);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return false;
  }
  const auto type = json::get_string(parsed.document, "Type");
  return type && lower_ascii(*type) == "map";
}

manifest synthesize_legacy(const legacy_input &input,
                           const std::set<std::string> &taken_ids,
                           diagnostics *diags) {
  manifest m;
  m.legacy = true;

  std::optional<std::string> publisher_id;
  std::optional<std::string> title;
  if (input.workshop_json) {
    const auto parsed = json::parse(*input.workshop_json);
    if (parsed.ok && parsed.document.IsObject()) {
      publisher_id = json::get_string(parsed.document, "PublisherID");
      title = json::get_string(parsed.document, "Title");
      if (const auto type = json::get_string(parsed.document, "Type");
          type && lower_ascii(*type) == "map") {
        m.kind = content_kind::map;
      }
    } else if (diags != nullptr) {
      diags->warning("legacy.workshop_json", input.folder,
                     "workshop.json in " + input.folder +
                         " is not valid JSON; the folder name is used.");
    }
  }

  // A published file id; digits past 64 bits fall back to the folder name.
  std::optional<std::uint64_t> workshop_id;
  if (publisher_id && !publisher_id->empty()) {
    std::uint64_t number = 0;
    const char *end = publisher_id->data() + publisher_id->size();
    if (const auto parsed = std::from_chars(publisher_id->data(), end, number);
        parsed.ec == std::errc{} && parsed.ptr == end) {
      workshop_id = number;
    }
  }
  std::string id;
  if (workshop_id) {
    id = "ws." + *publisher_id;
    m.workshop_id = workshop_id;
  } else {
    std::string clean;
    for (const char c : lower_ascii(input.folder)) {
      const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-';
      clean.push_back(ok ? c : '_');
    }
    id = "local." + clean.substr(0, 64 - 6 - 9);
  }
  if (taken_ids.contains(id)) {
    id = id.substr(0, 64 - 9) + "-" + sha256::hex(input.folder).substr(0, 8);
  }
  m.id = id;
  m.name = title && !title->empty() ? *title : input.folder;

  if (input.time_updated) {
    m.ver.build = "ws." + std::to_string(*input.time_updated);
  } else if (input.content_hash.starts_with("sha256:")) {
    m.ver.build = "sha." + input.content_hash.substr(7, 12);
  }
  for (const auto &file : input.files) {
    if (is_executable_file(file)) {
      m.caps.native_code = true;
      break;
    }
  }
  return m;
}
std::string strip_color_codes(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '^' && i + 1 < text.size() && text[i + 1] >= '0' &&
        text[i + 1] <= '9') {
      ++i;
      continue;
    }
    out.push_back(text[i]);
  }
  // Codes between words can leave double or edge spaces behind.
  std::string tidy;
  for (const char c : out) {
    if (c == ' ' && (tidy.empty() || tidy.back() == ' ')) {
      continue;
    }
    tidy.push_back(c);
  }
  while (!tidy.empty() && tidy.back() == ' ') {
    tidy.pop_back();
  }
  return tidy.empty() ? std::string(text) : tidy;
}

std::string scaffold_requires_xerces(const version &running) {
  return ">=" + std::to_string(running.major) + "." +
         std::to_string(running.minor) + " <" +
         std::to_string(running.major + 1);
}
} // namespace xerces_core
