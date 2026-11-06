#pragma once

// Voice chat starts as push-to-talk in a Xerces install (docs/DESIGN.md 10.5,
// decided 2026-11-05).
//
// The game keeps that choice in the player's profile, not in a dvar: the
// "Push to talk" switch in its voice options is the profile setting
// snd_voicechat_open_mic, 1 for an open mic and 0 for push-to-talk
// (data/ui_scripts/lua_fixes/__init__.lua), and the talk key is the +talk
// bind, B in the game's default bindings. No dvar of that name exists (looked
// up in the dvar pool of a game dump), so <game>\boiii_players\user\config.cfg
// never holds it, and the profile cannot say whether the player ever chose.
//
// So Xerces keeps its own archived dvar in config.cfg, voice_default_dvar: off
// or absent until Xerces has set push-to-talk once on this install, on after
// that. The client reads config.cfg as it starts
// (src/client/xerces/voice_default.cpp) and tells the game's Lua
// (data/ui_scripts/xerces_voice/__init__.lua), which sets the profile setting
// when the front end loads and then turns the dvar on. From then on the
// game's switch is the player's, and Xerces never changes it again. No game
// headers, so the tests cover the rule.

#include <string_view>

namespace xerces_core {
// Archived, saved in config.cfg: on once Xerces has set push-to-talk.
inline constexpr std::string_view voice_default_dvar =
    "xerces_voice_default_applied";
// Not archived: on while push-to-talk is still to be set this session; the
// game's Lua reads it.
inline constexpr std::string_view voice_due_dvar = "xerces_voice_ptt_due";
// The game's profile setting behind its "Push to talk" switch.
inline constexpr std::string_view voice_open_mic_setting =
    "snd_voicechat_open_mic";

// Whether Xerces still has to set push-to-talk, from config.cfg's text: true
// when the file sets no voice_default_dvar (a new install, or no file at
// all) or sets it off (saved before the game's Lua could set push-to-talk);
// false when it is on, so the player's own choice is kept.
bool voice_push_to_talk_due(std::string_view config_text);
} // namespace xerces_core
