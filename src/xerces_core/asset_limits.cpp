#include "asset_limits.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <rapidjson/document.h>

namespace xerces_core {
namespace {
// What utils::string::replace (src/common/utils/string.cpp) returns, in one
// pass. Upstream's loop replaces at position 0 before it ever searches, so
// the first from.length() characters (all of them, in a shorter text) become
// `to` whatever they are; it then searches from just after that `to`, and
// after each `to` it puts in, so it replaces the non-overlapping matches of
// the rest from left to right and never looks inside a `to`. Upstream calls
// str.replace in place for each match, which moves the whole tail every time:
// a 1 MiB file of "\r" took over a minute. This builds the same text in
// linear time.
std::string upstream_replace(const std::string &str, const std::string &from,
                             const std::string &to) {
  if (from.empty()) {
    return str;
  }

  std::string out;
  out.reserve(str.size() + to.size());
  out += to;
  std::size_t pos = std::min(from.size(), str.size());
  for (std::size_t found = str.find(from, pos); found != std::string::npos;
       found = str.find(from, pos)) {
    out.append(str, pos, found - pos);
    out += to;
    pos = found + from.size();
  }
  out.append(str, pos);
  return out;
}

// utils::string::split(const std::string &, char): every part, empty ones
// included, and always one more part than there are delimiters.
std::vector<std::string> upstream_split(const std::string &s, char delim) {
  std::vector<std::string> result;
  size_t start = 0;
  size_t end = s.find(delim);

  while (end != std::string::npos) {
    result.push_back(s.substr(start, end - start));
    start = end + 1;
    end = s.find(delim, start);
  }
  result.push_back(s.substr(start));

  return result;
}

// asset_limits::parse_int: the whole part, base 16 after "0x", else 10.
template <typename T> std::optional<T> parse_int(const std::string_view &str) {
  const char *first = str.data();
  const char *last = str.data() + str.size();
  int32_t base = 10;
  if (str.starts_with("0x")) {
    base = 16;
    first = str.data() + 2;
  }

  T result = {};
  auto [ptr, ec] = std::from_chars(first, last, result, base);

  if (ec == std::errc() && ptr == last) {
    return result;
  }
  return std::nullopt;
}

// valid_xassettype (src/client/game/structs/db/xasset/core.hpp).
constexpr bool valid_asset_type(std::int32_t type) {
  return type >= 0 && type < asset_type_count;
}

// DEFAULT_POOL_CONFIGS in src/client/component/asset_limits.cpp, one row per
// type in type order; names from XAssetType (core.hpp), labels from
// upstream's launcher (data/launcher/main.html, Asset Pools). fixed is where
// reallocate_asset_pool (src/client/game/impl/db/db.cpp) returns before it
// grows anything: is_fixed_pool's five types, and UI_MAP and WEAPON_FULL,
// for which DB_GetXAssetTypeSize_Impl (the same file) gives -1, the only
// types it has no entry size for.
constexpr asset_pool_info pool_table[] = {
    {0x00, "PHYSPRESET", "Phys Preset", "ap_physpreset", 0x113, false},
    {0x01, "PHYSCONSTRAINTS", "Phys Constraints", "ap_physconstraints", 0x80,
     false},
    {0x02, "DESTRUCTIBLEDEF", "Destructible Def", "ap_destructibledef", 0x80,
     false},
    {0x03, "XANIMPARTS", "XAnim", "ap_xanim", 0x6270, false},
    {0x04, "XMODEL", "XModel", "ap_xmodel", 0x2C00, false},
    {0x05, "XMODELMESH", "XModel Mesh", "ap_xmodelmesh", 0x8C00, true},
    {0x06, "MATERIAL", "Material", "ap_material", 0x5800, false},
    {0x07, "COMPUTE_SHADER_SET", "Compute Shader Set", "ap_computeshaderset",
     0x100, false},
    {0x08, "TECHNIQUE_SET", "Technique Set", "ap_techset", 0x400, false},
    {0x09, "IMAGE", "Image", "ap_image", 0xC000, true},
    {0x0A, "SOUND", "Sound", "ap_sound", 0x20, false},
    {0x0B, "SOUND_PATCH", "Sound Patch", "ap_sound_patch", 0x10, false},
    {0x0C, "CLIPMAP", "Col Map", "ap_col_map", 0x2, false},
    {0x0D, "COMWORLD", "Com Map", "ap_com_map", 0x2, true},
    {0x0E, "GAMEWORLD", "Game Map", "ap_game_map", 0x2, true},
    {0x0F, "MAP_ENTS", "Map Ents", "ap_map_ents", 0x2, false},
    {0x10, "GFXWORLD", "Gfx Map", "ap_gfx_map", 0x2, true},
    {0x11, "LIGHT_DEF", "Light Def", "ap_lightdef", 0x20, false},
    {0x12, "LENSFLARE_DEF", "Lensflare Def", "ap_lensflaredef", 0x46, false},
    {0x13, "UI_MAP", "UI Map", "ap_ui_map", 0x00, true},
    {0x14, "FONT", "Font", "ap_font", 0x10, false},
    {0x15, "FONTICON", "Font Icon", "ap_fonticon", 0x10, false},
    {0x16, "LOCALIZE_ENTRY", "Localize", "ap_localize", 0x6400, false},
    {0x17, "WEAPON", "Weapon", "ap_weapon", 0x600, false},
    {0x18, "WEAPONDEF", "Weapon Def", "ap_weapondef", 0x00, false},
    {0x19, "WEAPON_VARIANT", "Weapon Variant", "ap_weaponvariant", 0x00, false},
    {0x1A, "WEAPON_FULL", "Weapon Full", "ap_weaponfull", 0x00, true},
    {0x1B, "CGMEDIA", "CG Media Table", "ap_cgmediatable", 0x5, false},
    {0x1C, "PLAYERSOUNDS", "Player Sounds Table", "ap_playersoundstable", 0x10,
     false},
    {0x1D, "PLAYERFX", "Player FX Table", "ap_playerfxtable", 0x10, false},
    {0x1E, "SHAREDWEAPONSOUNDS", "Shared Weapon Sounds",
     "ap_sharedweaponsounds", 0x40, false},
    {0x1F, "ATTACHMENT", "Attachment", "ap_attachment", 0x80, false},
    {0x20, "ATTACHMENT_UNIQUE", "Attachment Unique", "ap_attachmentunique",
     0x864, false},
    {0x21, "WEAPON_CAMO", "Weapon Camo", "ap_weaponcamo", 0x200, false},
    {0x22, "CUSTOMIZATION_TABLE", "Customization Table",
     "ap_customizationtable", 0x8, false},
    {0x23, "CUSTOMIZATION_TABLE_FE_IMAGES", "Customization Table FE Images",
     "ap_customizationtable_feimages", 0x8, false},
    {0x24, "CUSTOMIZATION_TABLE_COLOR", "Customization Table Color",
     "ap_customizationtablecolor", 0x400, false},
    {0x25, "SNDDRIVER_GLOBALS", "Snd Driver Globals", "ap_snddriverglobals",
     0x1, false},
    {0x26, "FX", "FX", "ap_fx", 0x7D0, false},
    {0x27, "TAGFX", "Tag FX", "ap_tagfx", 0x40, false},
    {0x28, "NEW_LENSFLARE_DEF", "KLF (New Lensflare)", "ap_klf", 0x46, false},
    {0x29, "IMPACT_FX", "Impacts FX Table", "ap_impactsfxtable", 0x100, false},
    {0x2A, "IMPACT_SOUND", "Impact Sounds Table", "ap_impactsoundstable", 0x40,
     false},
    {0x2B, "PLAYER_CHARACTER", "Player Character", "ap_player_character", 0x8,
     false},
    {0x2C, "AITYPE", "AI Type", "ap_aitype", 0x60, false},
    {0x2D, "CHARACTER", "Character", "ap_character", 0x96, false},
    {0x2E, "XMODELALIAS", "XModel Alias", "ap_xmodelalias", 0x30, false},
    {0x2F, "RAWFILE", "Raw File", "ap_rawfile", 0x1388, false},
    {0x30, "STRINGTABLE", "String Table", "ap_stringtable", 0xDC, false},
    {0x31, "STRUCTURED_TABLE", "Structured Table", "ap_structuredtable", 0x69,
     false},
    {0x32, "LEADERBOARD", "Leaderboard Def", "ap_leaderboarddef", 0x100, false},
    {0x33, "DDL", "DDL", "ap_ddl", 0x40, false},
    {0x34, "GLASSES", "Glasses", "ap_glasses", 0x2, false},
    {0x35, "TEXTURELIST", "Texture List", "ap_texturelist", 0x8, false},
    {0x36, "SCRIPTPARSETREE", "Script Parse Tree", "ap_scriptparsetree", 0x47E,
     false},
    {0x37, "KEYVALUEPAIRS", "Key Value Pairs", "ap_keyvaluepairs", 0x40, false},
    {0x38, "VEHICLEDEF", "Vehicle", "ap_vehicle", 0x40, false},
    {0x39, "ADDON_MAP_ENTS", "Addon Map Ents", "ap_addon_map_ents", 0x1, false},
    {0x3A, "TRACER", "Tracer", "ap_tracer", 0x64, false},
    {0x3B, "SLUG", "Slug", "ap_slug", 0x5, false},
    {0x3C, "SURFACEFX_TABLE", "Surface FX Table", "ap_surfacefxtable", 0x40,
     false},
    {0x3D, "SURFACESOUNDDEF", "Surface Sound Def", "ap_surfacesounddef", 0x100,
     false},
    {0x3E, "FOOTSTEP_TABLE", "Footstep Table", "ap_footsteptable", 0x20, false},
    {0x3F, "ENTITYFXIMPACTS", "Entity FX Impacts", "ap_entityfximpacts", 0x100,
     false},
    {0x40, "ENTITYSOUNDIMPACTS", "Entity Sound Impacts",
     "ap_entitysoundimpacts", 0x100, false},
    {0x41, "ZBARRIER", "ZBarrier", "ap_zbarrier", 0x10, false},
    {0x42, "VEHICLEFXDEF", "Vehicle FX Def", "ap_vehiclefxdef", 0x20, false},
    {0x43, "VEHICLESOUNDDEF", "Vehicle Sound Def", "ap_vehiclesounddef", 0x20,
     false},
    {0x44, "TYPEINFO", "Type Info", "ap_typeinfo", 0x00, false},
    {0x45, "SCRIPTBUNDLE", "Script Bundle", "ap_scriptbundle", 0x400, false},
    {0x46, "SCRIPTBUNDLELIST", "Script Bundle List", "ap_scriptbundlelist",
     0x40, false},
    {0x47, "RUMBLE", "Rumble", "ap_rumble", 0x118, false},
    {0x48, "BULLETPENETRATION", "Bullet Penetration", "ap_bulletpenetration",
     0x1, false},
    {0x49, "LOCDMGTABLE", "Loc Dmg Table", "ap_locdmgtable", 0x1, false},
    {0x4A, "AIMTABLE", "Aim Table", "ap_aimtable", 0xC, false},
    {0x4B, "ANIMSELECTORTABLESET", "Anim Selector Table",
     "ap_animselectortable", 0x40, false},
    {0x4C, "ANIMMAPPINGTABLE", "Anim Mapping Table", "ap_animmappingtable",
     0x40, false},
    {0x4D, "ANIMSTATEMACHINE", "Anim State Machine", "ap_animstatemachine",
     0x40, false},
    {0x4E, "BEHAVIORTREE", "Behavior Tree", "ap_behaviortree", 0x40, false},
    {0x4F, "BEHAVIORSTATEMACHINE", "Behavior State Machine",
     "ap_behaviorstatemachine", 0x80, false},
    {0x50, "TTF", "TrueType Font", "ap_ttf", 0x30, false},
    {0x51, "SANIM", "SAnim", "ap_sanim", 0x400, false},
    {0x52, "LIGHT_DESCRIPTION", "Light Description", "ap_lightdescription",
     0x226, false},
    {0x53, "SHELLSHOCK", "Shellshock", "ap_shellshock", 0x40, false},
    {0x54, "XCAM", "XCam", "ap_xcam", 0x214, false},
    {0x55, "BG_CACHE", "BG Cache", "ap_bgcache", 0x20, false},
    {0x56, "TEXTURE_COMBO", "Texture Combo", "ap_texturecombo", 0x10, false},
    {0x57, "FLAMETABLE", "Flame Table", "ap_flametable", 0x10, false},
    {0x58, "BITFIELD", "Bitfield", "ap_bitfield", 0x34, false},
    {0x59, "ATTACHMENT_COSMETIC_VARIANT", "Attachment Cosmetic Variant",
     "ap_attachmentcosmeticvariant", 0x280, false},
    {0x5A, "MAPTABLE", "Map Table", "ap_maptable", 0x19, false},
    {0x5B, "MAPTABLE_LOADING_IMAGES", "Map Table Loading Images",
     "ap_maptableloadingimages", 0x19, false},
    {0x5C, "MEDAL", "Medal", "ap_medal", 0x300, false},
    {0x5D, "MEDALTABLE", "Medal Table", "ap_medaltable", 0x20, false},
    {0x5E, "OBJECTIVE", "Objective", "ap_objective", 0x100, false},
    {0x5F, "OBJECTIVE_LIST", "Objective List", "ap_objectivelist", 0x40, false},
    {0x60, "UMBRA_TOME", "Umbra Tome", "ap_umbra_tome", 0x00, false},
    {0x61, "NAVMESH", "Navmesh", "ap_navmesh", 0x2, false},
    {0x62, "NAVVOLUME", "Nav Volume", "ap_navvolume", 0x2, false},
    {0x63, "BINARYHTML", "Binary HTML", "ap_binaryhtml", 0x800, false},
    {0x64, "LASER", "Laser", "ap_laser", 0x32, false},
    {0x65, "BEAM", "Beam", "ap_beam", 0x32, false},
    {0x66, "STREAMER_HINT", "Streamer Hint", "ap_streamerhint", 0x32, false},
};
static_assert(std::size(pool_table) == asset_type_count);

constexpr bool table_in_type_order() {
  for (std::size_t i = 0; i < std::size(pool_table); ++i) {
    if (pool_table[i].type != static_cast<std::int32_t>(i)) {
      return false;
    }
  }
  return true;
}
static_assert(table_in_type_order());
} // namespace

std::vector<pool_limit> parse_pool_limits(std::string data) {
  // asset_limits::parse_list, step by step.
  std::vector<pool_limit> result;
  data = upstream_replace(
      upstream_replace(upstream_replace(data, "\r\n", "\n"), "\r", ""), "\t",
      " ");
  const std::vector<std::string> lines = upstream_split(data, '\n');
  for (const std::string &line : lines) {
    // T7Recharged syntax:
    // int_type_value string_type_name default_pool_size extended_pool_size
    // Minimal syntax: int_type_value extended_pool_size
    const std::vector<std::string> parts = upstream_split(line, ' ');
    if (parts.size() > 1) {
      const std::optional<int32_t> parsed_type_val =
          parse_int<int32_t>(parts[0]);
      if (parsed_type_val.has_value() &&
          valid_asset_type(parsed_type_val.value())) {
        const std::optional<uint32_t> parsed_size_val =
            parse_int<uint32_t>(parts[parts.size() - 1]);
        if (parsed_size_val.has_value()) {
          result.push_back({.type = parsed_type_val.value(),
                            .size = parsed_size_val.value()});
        }
      }
    }
  }
  return result;
}

std::filesystem::path pool_search_root(const std::filesystem::path &folder,
                                       content_layout layout) {
  if (layout == content_layout::game_folder) {
    std::error_code ec;
    if (std::filesystem::is_directory(folder / "zone", ec)) {
      return folder / "zone";
    }
  }
  return folder;
}

std::optional<std::filesystem::path>
find_pool_file(const std::filesystem::path &root) {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    return std::nullopt;
  }
  // utils::io::list_files(root, true, false): recursive_directory_iterator
  // with its default options, every entry that is not a directory.
  for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
       !ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_directory(ec)) {
      continue;
    }
    const std::filesystem::path name = it->path().filename();
    for (const std::string_view wanted : pool_file_names) {
      if (name == std::filesystem::path(wanted)) {
        return it->path();
      }
    }
  }
  return std::nullopt;
}

std::span<const asset_pool_info> asset_pools() { return pool_table; }

const asset_pool_info *find_asset_pool(std::int32_t type) {
  return valid_asset_type(type) ? &pool_table[type] : nullptr;
}

pool_settings parse_pool_settings(std::string_view launcher_settings_json) {
  // load_settings_doc: Parse(data.c_str()) with the default flags, so the
  // text ends at its first NUL; an empty file, a parse error or a non-object
  // is {}. The iterative parser accepts the same text without recursing.
  pool_settings out;
  rapidjson::Document doc;
  const std::string text(launcher_settings_json);
  if (text.empty() ||
      doc.Parse<rapidjson::kParseIterativeFlag>(text.c_str()).HasParseError() ||
      !doc.IsObject()) {
    doc.SetObject();
  }
  for (const auto &member : doc.GetObject()) {
    // get_setting: FindMember finds the first of repeated names (compared
    // whole), whatever its value; a string becomes a std::string through its
    // C string, anything else is "".
    out.values.try_emplace(
        std::string(member.name.GetString(), member.name.GetStringLength()),
        member.value.IsString() ? member.value.GetString() : "");
  }
  const auto setting = [&](std::string_view key) {
    const auto it = out.values.find(key);
    return it == out.values.end() ? std::string() : it->second;
  };
  // is_enabled
  const std::string val = setting("asset_limits_enabled");
  out.enabled =
      (val.empty() || val == "1") && setting("disable_asset_pools") != "1";
  return out;
}

std::uint32_t pool_size_setting(const pool_settings &settings,
                                std::int32_t type) {
  const asset_pool_info *pool = find_asset_pool(type);
  if (pool == nullptr) {
    return 0;
  }
  // get_pool_size
  const auto it = settings.values.find(pool->setting_key);
  const std::string val = it == settings.values.end() ? "" : it->second;
  if (!val.empty()) {
    try {
      const uint32_t parsed = std::stoul(val);
      if (parsed <= std::numeric_limits<int32_t>::max()) {
        return parsed;
      }
    } catch (...) {
    }
  }
  return pool->default_size;
}

std::optional<std::uint32_t> default_pool_size(const pool_settings &settings,
                                               std::int32_t type) {
  const asset_pool_info *pool = find_asset_pool(type);
  if (pool == nullptr || pool->fixed || !settings.enabled) {
    return std::nullopt;
  }
  const std::uint32_t size = pool_size_setting(settings, type);
  if (size == 0) {
    return std::nullopt;
  }
  return size;
}

std::vector<pool_need> largest_limits(std::span<const mod_pool_limits> mods,
                                      const pool_settings &settings) {
  std::map<std::int32_t, pool_need> by_type;
  for (const mod_pool_limits &mod : mods) {
    for (const pool_limit &limit : mod.limits) {
      // reallocate_asset_pool grows only while itemCount <
      // static_cast<int32_t>(new_size) (db.cpp): a size above INT32_MAX is
      // negative there and changes nothing, so it is no one's largest.
      if (!valid_asset_type(limit.type) || limit.size > max_pool_size) {
        continue;
      }
      const auto [it, added] = by_type.try_emplace(limit.type);
      if (added || limit.size > it->second.size) {
        it->second.type = limit.type;
        it->second.size = limit.size;
        it->second.mod = mod.id;
      }
    }
  }
  std::vector<pool_need> out;
  out.reserve(by_type.size());
  for (auto &[type, need] : by_type) {
    need.default_size = default_pool_size(settings, type);
    need.fixed = find_asset_pool(type)->fixed;
    // A size of 0 never grows anything (itemCount < 0 is never true).
    need.raises = !need.fixed && need.size > 0 &&
                  (!need.default_size || need.size > *need.default_size);
    out.push_back(std::move(need));
  }
  return out;
}
} // namespace xerces_core
