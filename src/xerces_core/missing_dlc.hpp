#pragma once

// What the client says when a server plays one of the game's own maps and the
// player's game does not have it (DESIGN.md D9). Upstream opened a "download
// page" outside Steam for the official Zombies maps. Xerces says the server
// uses DLC the player does not own or has not installed, and offers Black Ops
// III's page in the Steam store, which sells the game's DLC. Some official maps
// come with the base game; for those the notice says to verify the game files
// in Steam. Headless, so the text is tested.

#include <string>
#include <string_view>

namespace xerces_core {
// Black Ops III's store page, opened in the Steam client the way Workshop
// pages are (steam://url/CommunityFilePage/<id>).
inline constexpr char bo3_store_page[] = "steam://store/311210";

struct dlc_notice {
  std::string title;
  std::string message; // ends with the question the dialog's Yes answers
};

// map is the server's map name, for example "zm_castle". A name that is not a
// plain map name (a-z, 0-9 and '_', at most 64 characters) is left out of the
// text; it comes from the server.
dlc_notice missing_dlc_notice(std::string_view map);
} // namespace xerces_core
