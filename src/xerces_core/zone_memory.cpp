#include "zone_memory.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

#include "json.hpp"
#include "manifest.hpp"
#include "zone_names.hpp"

namespace xerces_core {
namespace {
const std::set<std::string, std::less<>> known_keys = {
    "name",         "steam_build", "checksum", "db_main_pool_mib",
    "baseline_mib", "note"};

std::optional<std::uint64_t> decimal_number(std::string_view text) {
  std::uint64_t value = 0;
  const char *end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::uint32_t> hex32(std::string_view text) {
  if (!text.starts_with("0x") || text.size() < 3 || text.size() > 10) {
    return std::nullopt;
  }
  std::uint32_t value = 0;
  const char *end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data() + 2, end, value, 16);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  return value;
}

void parse_build(const rapidjson::Value &entry, const std::string &path,
                 zone_memory_table &out) {
  diagnostics &diags = out.diags;
  if (!entry.IsObject()) {
    diags.error("zone_memory.type", path, "Each build is an object.");
    return;
  }
  for (auto it = entry.MemberBegin(); it != entry.MemberEnd(); ++it) {
    const std::string key(it->name.GetString(), it->name.GetStringLength());
    if (!known_keys.contains(key) && !key.starts_with("_")) {
      diags.warning("zone_memory.key.unknown", json::pointer(path, key),
                    "Unknown key \"" + key + "\"; ignored.");
    }
  }
  zone_memory_build build;
  build.name = json::get_string(entry, "name").value_or("");
  if (build.name.empty()) {
    diags.error("zone_memory.name", json::pointer(path, "name"),
                "name is required.");
  }
  if (const rapidjson::Value *value = json::member(entry, "steam_build")) {
    if (value->IsUint64()) {
      build.steam_build = value->GetUint64();
    } else {
      diags.error("zone_memory.steam_build", json::pointer(path, "steam_build"),
                  "steam_build is a whole number.");
    }
  }
  if (const rapidjson::Value *value = json::member(entry, "checksum")) {
    if (value->IsString()) {
      build.checksum =
          hex32(std::string_view(value->GetString(), value->GetStringLength()));
    }
    if (!build.checksum) {
      diags.error("zone_memory.checksum", json::pointer(path, "checksum"),
                  "checksum is a \"0x\" hex string, as in builds.json.");
    }
  }
  if (!json::member(entry, "steam_build") && !json::member(entry, "checksum")) {
    diags.error("zone_memory.build", path,
                "A build needs steam_build or checksum (or both), as "
                "compat/builds.json names it.");
  }
  const rapidjson::Value *pool = json::member(entry, "db_main_pool_mib");
  if (pool == nullptr || !pool->IsNumber() || !(pool->GetDouble() > 0)) {
    diags.error("zone_memory.pool", json::pointer(path, "db_main_pool_mib"),
                "db_main_pool_mib is a number of MiB above 0.");
  } else {
    build.db_main_pool_mib = pool->GetDouble();
  }
  if (const rapidjson::Value *baselines = json::member(entry, "baseline_mib")) {
    const std::string at = json::pointer(path, "baseline_mib");
    if (!baselines->IsObject()) {
      diags.error("zone_memory.baseline", at,
                  "baseline_mib is an object: mode -> MiB, or null.");
    } else {
      for (auto it = baselines->MemberBegin(); it != baselines->MemberEnd();
           ++it) {
        const std::string mode(it->name.GetString(),
                               it->name.GetStringLength());
        const std::string where = json::pointer(at, mode);
        if (!valid_mode(mode)) {
          diags.error("zone_memory.mode", where,
                      "Modes are \"zm\", \"mp\" and \"cp\".");
        } else if (it->value.IsNull()) {
          continue; // not measured
        } else if (!it->value.IsNumber() || it->value.GetDouble() < 0) {
          diags.error("zone_memory.baseline", where,
                      "A baseline is a number of MiB, or null when not "
                      "measured.");
        } else {
          build.baseline_mib[mode] = it->value.GetDouble();
        }
      }
    }
  }
  out.builds.push_back(std::move(build));
}

std::string lower_ascii(std::string_view text) {
  std::string out(text);
  for (char &c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

// "zone/en_core_mod.ff" -> "en_core_mod".
std::string zone_name(std::string_view relative) {
  std::string name = lower_ascii(relative);
  if (const auto slash = name.rfind('/'); slash != std::string::npos) {
    name.erase(0, slash + 1);
  }
  if (name.ends_with(".ff")) {
    name.resize(name.size() - 3);
  }
  return name;
}

bool mode_prefix(std::string_view name) {
  return name.starts_with("zm_") || name.starts_with("mp_") ||
         name.starts_with("cp_");
}

std::string_view without_patch(std::string_view name) {
  constexpr std::string_view patch = "_patch";
  if (name.size() > patch.size() && name.ends_with(patch)) {
    name.remove_suffix(patch.size());
  }
  return name;
}

// The zone a language copy belongs to: "en_core_mod" -> "core_mod" when the
// folder has core_mod (or it is a mod tier), else nullopt. A mode prefix is
// never a language ("zm_mod", "zm_backrooms"), and a two-letter prefix with no
// such zone next to it is part of the zone's own name.
std::optional<std::string>
language_copy_of(const std::string &name, const std::set<std::string> &names) {
  if (name.size() <= 3 || name[2] != '_' || name[0] < 'a' || name[0] > 'z' ||
      name[1] < 'a' || name[1] > 'z' || mode_prefix(name)) {
    return std::nullopt;
  }
  std::string rest = name.substr(3);
  if (names.contains(rest) || is_mod_tier(without_patch(rest))) {
    return rest;
  }
  return std::nullopt;
}

struct zone_group {
  std::optional<std::pair<std::string, std::optional<std::uint64_t>>> own;
  std::vector<std::pair<std::string, std::optional<std::uint64_t>>> languages;
};

std::map<std::string, zone_group> group_zones(const zone_header_map &zones) {
  std::set<std::string> names;
  for (const auto &[path, bytes] : zones) {
    names.insert(zone_name(path));
  }
  std::map<std::string, zone_group> groups;
  for (const auto &[path, bytes] : zones) {
    const std::string name = zone_name(path);
    if (const auto base = language_copy_of(name, names)) {
      groups[*base].languages.emplace_back(path, bytes);
    } else if (!groups[name].own) {
      groups[name].own.emplace(path, bytes);
    }
  }
  return groups;
}
} // namespace

zone_memory_table parse_zone_memory(std::string_view text) {
  zone_memory_table out;
  const json::parse_result parsed = json::parse(text);
  if (!parsed.ok) {
    out.diags.error("zone_memory.json", "",
                    "Not JSON (line " + std::to_string(parsed.line) +
                        ", column " + std::to_string(parsed.column) +
                        "): " + parsed.error);
    return out;
  }
  const rapidjson::Value &doc = parsed.document;
  if (!doc.IsObject() ||
      json::get_string(doc, "schema") != zone_memory_schema) {
    out.diags.error("zone_memory.schema", "/schema",
                    "schema must be \"" + std::string(zone_memory_schema) +
                        "\".");
    return out;
  }
  const rapidjson::Value *list = json::member(doc, "builds");
  if (list == nullptr || !list->IsArray()) {
    out.diags.error("zone_memory.list", "/builds", "builds is a list.");
    return out;
  }
  for (rapidjson::SizeType i = 0; i < list->Size(); ++i) {
    parse_build((*list)[i], json::pointer("/builds", i), out);
  }
  return out;
}

const zone_memory_build *
find_zone_memory_build(const zone_memory_table &table,
                       std::optional<std::uint64_t> steam_build,
                       std::optional<std::uint32_t> checksum) {
  if (steam_build) {
    for (const auto &build : table.builds) {
      if (build.steam_build == steam_build) {
        return &build;
      }
    }
  }
  if (checksum) {
    for (const auto &build : table.builds) {
      if (build.checksum == checksum) {
        return &build;
      }
    }
  }
  return nullptr;
}

std::optional<zone_budget> zone_budget_for(const zone_memory_build &build,
                                           std::string_view mode) {
  const auto it = build.baseline_mib.find(std::string(mode));
  if (it == build.baseline_mib.end() || !(build.db_main_pool_mib > 0)) {
    return std::nullopt;
  }
  return zone_budget{build.db_main_pool_mib, it->second};
}

std::optional<zone_budget> find_zone_budget(const zone_memory_table &table,
                                            std::string_view game_build,
                                            std::string_view mode) {
  const zone_memory_build *build =
      game_build.starts_with("0x")
          ? find_zone_memory_build(table, std::nullopt, hex32(game_build))
          : find_zone_memory_build(table, decimal_number(game_build));
  if (build == nullptr) {
    return std::nullopt;
  }
  return zone_budget_for(*build, mode);
}

bool is_zone_file(std::string_view relative) {
  std::string name = lower_ascii(relative);
  if (name.starts_with("zone/")) {
    name.erase(0, 5);
  }
  return name.size() > 3 && name.ends_with(".ff") &&
         name.find('/') == std::string::npos;
}

zone_memory_use zone_memory_of(const zone_header_map &zones,
                               std::string_view mode, bool mod) {
  zone_memory_use out;
  const auto groups = group_zones(zones);
  const auto add = [&](const zone_group &group) {
    if (group.own) {
      if (group.own->second) {
        out.bytes += *group.own->second;
      } else {
        out.unread.push_back(group.own->first);
      }
    }
    std::optional<std::uint64_t> largest;
    for (const auto &[path, bytes] : group.languages) {
      if (bytes && (!largest || *bytes > *largest)) {
        largest = bytes;
      }
    }
    if (largest) {
      out.bytes += *largest;
    } else if (!group.languages.empty()) {
      out.unread.push_back(group.languages.front().first);
    }
  };
  if (mod) {
    const std::string tier = std::string(mode) + "_mod";
    for (const std::string &base :
         {std::string("core_mod"), std::string("core_mod_patch"), tier,
          tier + "_patch"}) {
      if (const auto it = groups.find(base); it != groups.end()) {
        add(it->second);
      }
    }
  } else {
    for (const auto &[base, group] : groups) {
      add(group);
    }
  }
  return out;
}

std::set<std::string> map_zone_modes(const zone_header_map &zones) {
  std::set<std::string> out;
  for (const auto &[base, group] : group_zones(zones)) {
    if (mode_prefix(base) && !is_mod_tier(without_patch(base))) {
      out.insert(base.substr(0, 2));
    }
  }
  return out;
}

double round_mib(double mib) { return std::round(mib * 10.0) / 10.0; }
} // namespace xerces_core
