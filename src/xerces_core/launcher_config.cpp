#include "launcher_config.hpp"

#include <set>

#include "json.hpp"

namespace xerces_core {
namespace {
const std::set<std::string, std::less<>> known_keys = {
    "schema",
    "game_dir",
    "first_run_done",
    "mode",
    "profiles",
    "saved_positions",
    "player_name",
    "nointro",
    "close_on_launch",
    "extra_flags",
    "dvars",
    "theme",
    "ui_scale_percent",
    "reduced_motion",
    "language",
    "check_updates",
    "favorites",
    "recent_servers",
    "send_crash_reports",
    "friends_invite_only",
    "friends_follow",
};

bool valid_mode(std::string_view mode) {
  return mode == "zm" || mode == "mp" || mode == "cp";
}

bool valid_theme(std::string_view theme) {
  return theme == "dark" || theme == "light" || theme == "contrast";
}

std::vector<std::string> strings_of(const rapidjson::Value *array) {
  std::vector<std::string> out;
  if (array == nullptr || !array->IsArray()) {
    return out;
  }
  for (const auto &item : array->GetArray()) {
    if (item.IsString()) {
      out.emplace_back(item.GetString(), item.GetStringLength());
    }
  }
  return out;
}

bool bool_of(const rapidjson::Value &root, std::string_view key, bool fallback) {
  const auto *value = json::member(root, key);
  return value != nullptr && value->IsBool() ? value->GetBool() : fallback;
}
} // namespace

mode_profile &profile_for(launcher_config &config, const std::string &mode) {
  return config.profiles[mode];
}

launcher_config_result parse_launcher_config(std::string_view text) {
  launcher_config_result out;
  auto parsed = json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject()) {
    out.diags.error("config.parse", "",
                    "The launcher config is not a JSON object (line " +
                        std::to_string(parsed.line) + "): " + parsed.error);
    return out;
  }
  const rapidjson::Value &root = parsed.document;
  const auto schema = json::get_string(root, "schema");
  if (schema && *schema != launcher_config_schema) {
    out.diags.warning("config.schema", "/schema",
                      "Unknown config schema \"" + *schema +
                          "\"; reading what this version understands.");
  }
  launcher_config &c = out.value;
  if (const auto dir = json::get_string(root, "game_dir");
      dir && !dir->empty()) {
    c.game_dir = *dir;
  }
  c.first_run_done = bool_of(root, "first_run_done", false);
  if (const auto mode = json::get_string(root, "mode");
      mode && valid_mode(*mode)) {
    c.mode = *mode;
  }
  if (const auto *profiles = json::member(root, "profiles");
      profiles != nullptr && profiles->IsObject()) {
    for (const auto &member : profiles->GetObject()) {
      const std::string mode(member.name.GetString(),
                             member.name.GetStringLength());
      if (!valid_mode(mode) || !member.value.IsObject()) {
        continue;
      }
      mode_profile profile;
      profile.mods = strings_of(json::member(member.value, "mods"));
      if (const auto map = json::get_string(member.value, "map");
          map && !map->empty()) {
        profile.map = *map;
      }
      profile.modded = bool_of(member.value, "modded", true);
      if (const auto *pins = json::member(member.value, "pins");
          pins != nullptr && pins->IsObject()) {
        for (const auto &pin : pins->GetObject()) {
          if (pin.value.IsString()) {
            profile.pins[std::string(pin.name.GetString(),
                                     pin.name.GetStringLength())] =
                std::string(pin.value.GetString(), pin.value.GetStringLength());
          }
        }
      }
      profile.pack_name = json::get_string(member.value, "pack_name").value_or("");
      profile.pack_id = json::get_string(member.value, "pack_id").value_or("");
      if (const auto *pairs = json::member(member.value, "allowed_conflicts");
          pairs != nullptr && pairs->IsArray()) {
        for (const auto &pair : pairs->GetArray()) {
          if (pair.IsArray() && pair.Size() == 2 && pair[0].IsString() &&
              pair[1].IsString()) {
            std::string a(pair[0].GetString(), pair[0].GetStringLength());
            std::string b(pair[1].GetString(), pair[1].GetStringLength());
            if (b < a) {
              std::swap(a, b);
            }
            profile.allowed_conflicts.emplace(std::move(a), std::move(b));
          }
        }
      }
      if (const auto *settings = json::member(member.value, "settings");
          settings != nullptr && settings->IsObject()) {
        for (const auto &mod : settings->GetObject()) {
          if (!mod.value.IsObject()) {
            continue;
          }
          auto &values = profile.settings[std::string(
              mod.name.GetString(), mod.name.GetStringLength())];
          for (const auto &setting : mod.value.GetObject()) {
            values[std::string(setting.name.GetString(),
                               setting.name.GetStringLength())] =
                json::canonical(setting.value);
          }
        }
      }
      c.profiles[mode] = std::move(profile);
    }
  }
  if (const auto *positions = json::member(root, "saved_positions");
      positions != nullptr && positions->IsObject()) {
    for (const auto &member : positions->GetObject()) {
      if (member.value.IsInt64()) {
        c.saved_positions[std::string(member.name.GetString(),
                                      member.name.GetStringLength())] =
            member.value.GetInt64();
      }
    }
  }
  c.player_name = json::get_string(root, "player_name").value_or("");
  c.nointro = bool_of(root, "nointro", true);
  c.close_on_launch = bool_of(root, "close_on_launch", false);
  c.extra_flags = strings_of(json::member(root, "extra_flags"));
  if (const auto *dvars = json::member(root, "dvars");
      dvars != nullptr && dvars->IsObject()) {
    for (const auto &member : dvars->GetObject()) {
      if (member.value.IsString()) {
        c.dvars[std::string(member.name.GetString(),
                            member.name.GetStringLength())] =
            std::string(member.value.GetString(),
                        member.value.GetStringLength());
      }
    }
  }
  if (const auto theme = json::get_string(root, "theme");
      theme && valid_theme(*theme)) {
    c.theme = *theme;
  }
  if (const auto *scale = json::member(root, "ui_scale_percent");
      scale != nullptr && scale->IsInt()) {
    const int value = scale->GetInt();
    c.ui_scale_percent = value == 0 || (value >= 75 && value <= 200) ? value : 0;
  }
  c.reduced_motion = bool_of(root, "reduced_motion", false);
  if (const auto language = json::get_string(root, "language");
      language && !language->empty() && language->size() <= 16) {
    c.language = *language;
  }
  c.check_updates = bool_of(root, "check_updates", true);
  c.send_crash_reports = bool_of(root, "send_crash_reports", true);
  c.friends_invite_only = bool_of(root, "friends_invite_only", true);
  c.friends_follow = bool_of(root, "friends_follow", true);
  if (const auto *favorites = json::member(root, "favorites");
      favorites != nullptr && favorites->IsArray()) {
    for (const auto &item : favorites->GetArray()) {
      if (!item.IsObject()) {
        continue;
      }
      favorite_server server;
      server.name = json::get_string(item, "name").value_or("");
      server.address = json::get_string(item, "address").value_or("");
      if (!server.address.empty()) {
        c.favorites.push_back(std::move(server));
      }
    }
  }
  c.recent_servers = strings_of(json::member(root, "recent_servers"));

  for (const auto &member : root.GetObject()) {
    const std::string_view key(member.name.GetString(),
                               member.name.GetStringLength());
    if (!known_keys.contains(key)) {
      c.unknown[std::string(key)] = json::canonical(member.value);
    }
  }
  return out;
}

std::string launcher_config_to_json(const launcher_config &c) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "schema", launcher_config_schema, alloc);
  if (c.game_dir) {
    json::set(doc, "game_dir", *c.game_dir, alloc);
  }
  json::set(doc, "first_run_done", c.first_run_done, alloc);
  json::set(doc, "mode", c.mode, alloc);

  rapidjson::Value profiles(rapidjson::kObjectType);
  for (const auto &[mode, profile] : c.profiles) {
    rapidjson::Value item(rapidjson::kObjectType);
    json::set(item, "mods", json::string_array(profile.mods, alloc), alloc);
    if (profile.map) {
      json::set(item, "map", *profile.map, alloc);
    }
    json::set(item, "modded", profile.modded, alloc);
    rapidjson::Value pins(rapidjson::kObjectType);
    for (const auto &[id, ver] : profile.pins) {
      json::set(pins, id, ver, alloc);
    }
    json::set(item, "pins", std::move(pins), alloc);
    if (!profile.pack_name.empty()) {
      json::set(item, "pack_name", profile.pack_name, alloc);
    }
    if (!profile.pack_id.empty()) {
      json::set(item, "pack_id", profile.pack_id, alloc);
    }
    if (!profile.allowed_conflicts.empty()) {
      rapidjson::Value pairs(rapidjson::kArrayType);
      for (const auto &[a, b] : profile.allowed_conflicts) {
        pairs.PushBack(json::string_array({a, b}, alloc), alloc);
      }
      json::set(item, "allowed_conflicts", std::move(pairs), alloc);
    }
    if (!profile.settings.empty()) {
      rapidjson::Value settings(rapidjson::kObjectType);
      for (const auto &[id, values] : profile.settings) {
        rapidjson::Value object(rapidjson::kObjectType);
        for (const auto &[key, canonical] : values) {
          auto parsed = json::parse(canonical);
          if (parsed.ok) {
            json::set(object, key, json::copy(parsed.document, alloc), alloc);
          }
        }
        json::set(settings, id, std::move(object), alloc);
      }
      json::set(item, "settings", std::move(settings), alloc);
    }
    json::set(profiles, mode, std::move(item), alloc);
  }
  json::set(doc, "profiles", std::move(profiles), alloc);

  rapidjson::Value positions(rapidjson::kObjectType);
  for (const auto &[id, position] : c.saved_positions) {
    json::set(positions, id, std::int64_t{position}, alloc);
  }
  json::set(doc, "saved_positions", std::move(positions), alloc);

  json::set(doc, "player_name", c.player_name, alloc);
  json::set(doc, "nointro", c.nointro, alloc);
  json::set(doc, "close_on_launch", c.close_on_launch, alloc);
  json::set(doc, "extra_flags", json::string_array(c.extra_flags, alloc),
            alloc);
  rapidjson::Value dvars(rapidjson::kObjectType);
  for (const auto &[name, value] : c.dvars) {
    json::set(dvars, name, value, alloc);
  }
  json::set(doc, "dvars", std::move(dvars), alloc);
  json::set(doc, "theme", c.theme, alloc);
  json::set(doc, "ui_scale_percent", std::int64_t{c.ui_scale_percent}, alloc);
  json::set(doc, "reduced_motion", c.reduced_motion, alloc);
  json::set(doc, "language", c.language, alloc);
  json::set(doc, "check_updates", c.check_updates, alloc);
  json::set(doc, "send_crash_reports", c.send_crash_reports, alloc);
  json::set(doc, "friends_invite_only", c.friends_invite_only, alloc);
  json::set(doc, "friends_follow", c.friends_follow, alloc);

  rapidjson::Value favorites(rapidjson::kArrayType);
  for (const auto &server : c.favorites) {
    rapidjson::Value item(rapidjson::kObjectType);
    json::set(item, "name", server.name, alloc);
    json::set(item, "address", server.address, alloc);
    favorites.PushBack(item, alloc);
  }
  json::set(doc, "favorites", std::move(favorites), alloc);
  json::set(doc, "recent_servers", json::string_array(c.recent_servers, alloc),
            alloc);

  for (const auto &[key, canonical] : c.unknown) {
    auto parsed = json::parse(canonical);
    if (parsed.ok && !known_keys.contains(key)) {
      json::set(doc, key, json::copy(parsed.document, alloc), alloc);
    }
  }
  return json::pretty(doc);
}
} // namespace xerces_core
