#pragma once

// Joining another player's self-hosted game (docs/DESIGN.md 11.10): the game
// role's side of xerces_core/join_check.hpp. party.cpp (getinfo replies) and
// nat.cpp (the lobby content) ask check_host before a join takes the host's
// mods; getinfo.cpp and nat.cpp advertise own_content_ids.

#include <functional>
#include <string>
#include <string_view>

namespace xerces_join {
// What a host said it runs, from its getinfo reply or its lobby content.
struct host_offer {
  std::string main_mod;        // modId ("" or "usermaps": none)
  std::string main_title;      // modName, when the host said
  std::string stack;           // modStack, ',' separated
  std::string map;             // a custom map's name; "" for a stock map
  std::string map_workshop_id; // usermapId or workshop_id
  std::string content_ids;     // contentIds, or the lobby content's sixth field
};

// Checks the offer against what this PC has. Everything missing is named in
// one dialog that offers each item's Workshop page in Steam, and the join
// stops there. Items whose content id differs from the host's are named in a
// dialog whose "Yes" joins anyway; once agreed, the same offer joins without
// asking again for a minute (a join reaches here twice: from getinfo, then
// from the lobby). Call it on the main thread: with nothing to ask, proceed
// runs before it returns; after a "Yes", on the next main-thread frame.
void check_host(const host_offer &offer, std::function<void()> proceed);

// This game's content ids for its main mod, its stacked mods and the map
// given (a custom map's name and Workshop id; "" for none), from the
// installed index the launcher wrote. "" when the game was not started by
// the launcher, so older behaviour stays.
std::string own_content_ids(std::string_view map,
                            std::string_view map_workshop_id);

// A join takes the host's mods (fs_game and fs_extramods): the journal says
// so, with the host's stack, so a crash on the host's game is not blamed on
// this launch's plan (journal.hpp, launch_journal::joined).
void note_joined(std::string_view main_mod, std::string_view stack);
} // namespace xerces_join
