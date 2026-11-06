#pragma once

// Voice chat starts as push-to-talk on a new game profile (docs/DESIGN.md
// 10.5, decided 2026-11-05).
//
// The game keeps that choice in the player's profile, not in a dvar: the
// "Push to talk" switch in its voice options is the "user" profile setting
// snd_voicechat_open_mic, 1 for an open mic and 0 for push-to-talk
// (data/ui_scripts/lua_fixes/__init__.lua), and the talk key is the +talk
// bind, B in the game's default bindings. No dvar of that name exists (looked
// up in the dvar pool of a game dump), so config.cfg never holds it, and the
// profile cannot say whether the player ever chose.
//
// So Xerces sets push-to-talk only on a profile the game has not made yet:
// the client looks for it in <game>\boiii_players (where the client sends the
// game's players folder; the user profile is user_settings_<n>.cgp there) as
// it starts, before the game has read or made it
// (src/client/xerces/voice_default.cpp). Any profile already there, from
// BOIII or an earlier Xerces start, is the player's and keeps its setting.
// The game's Lua (data/ui_scripts/xerces_voice/__init__.lua) tries once per
// session, at the first front end, and the profile the game then saves is the
// player's from the next start on. No game headers, so the tests cover the
// rule.

#include <filesystem>
#include <string_view>

namespace xerces_core {
// Not archived: the session's state, which the game's Lua reads and moves on.
inline constexpr std::string_view voice_state_dvar = "xerces_voice_ptt";
// The game's profile setting behind its "Push to talk" switch.
inline constexpr std::string_view voice_open_mic_setting =
    "snd_voicechat_open_mic";

// The values of voice_state_dvar.
enum class voice_state : int {
  keep = 0,    // a profile was there: the player's setting stays
  due = 1,     // no profile yet: the first front end sets push-to-talk
  set = 2,     // set, and the profile read back push-to-talk
  not_set = 3, // tried once and not set; not tried again this session
};

// What the client finds in <game>\boiii_players as it starts.
enum class game_profile {
  none,    // no folder, or no profile file in it
  found,   // a profile file is there
  unknown, // the folder could not be read
};

// A file name of the game's user profile: user_settings_<n>.cgp, in any case.
bool is_game_profile_file(std::string_view file_name);

// Looks in players_dir (<game>\boiii_players) for the game's user profile.
// Reading errors give unknown, never none.
game_profile find_game_profile(const std::filesystem::path &players_dir);

// The session's start state: due only when there is no profile at all, so a
// profile that exists or could not be checked keeps the player's setting.
voice_state voice_start_state(game_profile profile);
} // namespace xerces_core
