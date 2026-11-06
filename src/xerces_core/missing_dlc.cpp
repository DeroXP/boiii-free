#include "missing_dlc.hpp"

namespace xerces_core {
namespace {
bool is_plain_map_name(std::string_view name) {
  if (name.empty() || name.size() > 64) {
    return false;
  }
  for (const char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  return true;
}
} // namespace

dlc_notice missing_dlc_notice(std::string_view map) {
  dlc_notice notice;
  notice.title = "DLC needed";
  notice.message = "This server uses DLC you don't own or haven't installed";
  if (is_plain_map_name(map)) {
    notice.message += " (the map ";
    notice.message += map;
    notice.message += ")";
  }
  // The client shows this for every official map, and the campaign, many
  // multiplayer maps and Shadows of Evil come with the base game. For those the
  // game's files are incomplete, so the notice also gives Steam's file check.
  notice.message +=
      ".\n\nIf you own it, install it in Steam: Black Ops III, Properties, "
      "DLC. If not, the game's store page in Steam sells it.\n\n"
      "If the map comes with the base game, some game files are missing: "
      "verify them in Steam (Black Ops III, Properties, Installed Files, "
      "Verify integrity of game files).\n\n"
      "Open the store page in Steam now?";
  return notice;
}
} // namespace xerces_core
