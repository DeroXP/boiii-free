#include "zone_names.hpp"

namespace xerces_core {
namespace {
constexpr std::string_view stack_prefix = "stack_";
constexpr std::string_view patch_suffix = "_patch";

bool is_language_prefix(std::string_view name) {
  return name.size() > 3 && name[2] == '_' && name[0] >= 'a' &&
         name[0] <= 'z' && name[1] >= 'a' && name[1] <= 'z';
}

std::string_view without_patch(std::string_view name) {
  if (name.size() > patch_suffix.size() && name.ends_with(patch_suffix)) {
    name.remove_suffix(patch_suffix.size());
  }
  return name;
}

// The class of a name with no language prefix; nullopt-like "stock" when the
// name is none of the stack's.
bool classify_base(std::string_view name, std::string_view active_usermap,
                   std::span<const std::string> extra_ids, zone_class &out) {
  const std::string_view base = without_patch(name);
  if (is_mod_tier(base)) {
    out = {zone_kind::main_tier, 0};
    return true;
  }
  if (base.starts_with(stack_prefix)) {
    const std::string_view rest = base.substr(stack_prefix.size());
    // Any zone the extra brings, not only its tiers (zone dependencies). The
    // longest matching id wins, as the zone paths are found (find_route), so
    // "stack_my_mod_zm_mod" is my_mod's even when "my" is also stacked.
    std::size_t best = extra_ids.size();
    for (std::size_t i = 0; i < extra_ids.size(); ++i) {
      const std::string &id = extra_ids[i];
      if (!id.empty() && rest.size() > id.size() + 1 && rest.starts_with(id) &&
          rest[id.size()] == '_' &&
          (best == extra_ids.size() || id.size() > extra_ids[best].size())) {
        best = i;
      }
    }
    if (best == extra_ids.size()) {
      return false; // an alias for a mod that is not in the stack
    }
    out = {zone_kind::extra, best};
    return true;
  }
  if (!active_usermap.empty() &&
      (name == active_usermap || base == active_usermap)) {
    out = {zone_kind::usermap, 0};
    return true;
  }
  return false;
}
} // namespace

bool is_mod_tier(std::string_view name) {
  return name == "core_mod" || name == "zm_mod" || name == "mp_mod" ||
         name == "cp_mod";
}

zone_class classify_zone(std::string_view zone, std::string_view active_usermap,
                         std::span<const std::string> extra_ids) {
  zone_class out;
  if (zone.empty()) {
    return out;
  }
  if (classify_base(zone, active_usermap, extra_ids, out)) {
    return out;
  }
  if (is_language_prefix(zone) &&
      classify_base(zone.substr(3), active_usermap, extra_ids, out)) {
    return out;
  }
  return {};
}

script_choice decide_script_override(zone_kind incoming, zone_kind existing,
                                     bool map_first) {
  if (!map_first) {
    return script_choice::engine;
  }
  const auto is_mod = [](zone_kind kind) {
    return kind == zone_kind::main_tier || kind == zone_kind::extra;
  };
  if (incoming == zone_kind::usermap && is_mod(existing)) {
    return script_choice::incoming_wins;
  }
  if (existing == zone_kind::usermap && is_mod(incoming)) {
    return script_choice::existing_stays;
  }
  return script_choice::engine;
}

std::uint8_t script_half_of(std::string_view name, std::string *base) {
  std::uint8_t half = 0;
  if (name.size() > 4 && name.ends_with(".gsc")) {
    half = half_gsc;
  } else if (name.size() > 4 && name.ends_with(".csc")) {
    half = half_csc;
  }
  if (half && base) {
    *base = std::string(name.substr(0, name.size() - 4));
  }
  return half;
}

half_choice decide_map_half(std::uint8_t half, std::uint8_t mod_halves,
                            std::uint8_t map_seen,
                            std::optional<std::uint8_t> map_known) {
  const std::uint8_t other =
      static_cast<std::uint8_t>((half_gsc | half_csc) & ~half);
  if (!(mod_halves & other)) {
    return half_choice::take; // the other half is the game's or the map's
  }
  if ((map_seen & other) || (map_known && (*map_known & other))) {
    return half_choice::take; // the map ships both halves
  }
  return map_known ? half_choice::skip : half_choice::take_tentative;
}

std::string format_map_scripts(std::string_view stamp,
                               const map_script_halves &halves) {
  std::string out = "xerces.mapscripts/1 " + std::string(stamp) + "\n";
  for (const auto &[base, mask] : halves) {
    if (!base.empty() && base.find_first_of(" \r\n") == std::string::npos &&
        mask) {
      out += base + " " + std::to_string(mask & (half_gsc | half_csc)) + "\n";
    }
  }
  return out;
}

std::optional<map_script_halves> parse_map_scripts(std::string_view text,
                                                   std::string_view stamp) {
  const auto first_end = text.find('\n');
  if (first_end == std::string_view::npos ||
      text.substr(0, first_end) !=
          "xerces.mapscripts/1 " + std::string(stamp) ||
      stamp.empty()) {
    return std::nullopt; // another map version, or not this file
  }
  map_script_halves out;
  std::size_t start = first_end + 1;
  while (start < text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    const std::string_view line = text.substr(start, end - start);
    start = end + 1;
    const auto space = line.rfind(' ');
    if (line.empty() || space == std::string_view::npos || space == 0) {
      continue;
    }
    const std::string_view mask = line.substr(space + 1);
    if (mask == "1" || mask == "2" || mask == "3") {
      out[std::string(line.substr(0, space))] |=
          static_cast<std::uint8_t>(mask[0] - '0');
    }
  }
  return out;
}
} // namespace xerces_core
