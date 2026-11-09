#include <std_include.hpp>

// Zombies character slots (docs/DESIGN.md 7.10).
//
// Priority: a player who picked a character in Xerces' CHARACTERS menu keeps it.
// Another mod's own character script (DS4C's) calls SetCharacterBodyType,
// SetCharacterBodyStyle and SetCharacterHelmetStyle on the player, at spawn and
// when its own menu is used. While the player's bit is set in the xerces_char_lock
// dvar, those calls are skipped unless xerces_char_apply is 1, which Xerces' own
// script (data/custom_scripts/zm/xerces_characters.gsc) holds while it makes the
// same calls (no wait in between, so no other script runs meanwhile).
//
// Trace: with XERCES_CHAR_TRACE=1 in the environment, every script call that
// changes what a player wears (SetModel, Attach, DetachAll, SetPlayerCharacter,
// SetCharacterBodyType, SetCharacterBodyStyle) is written to the game log with
// the player and its first argument, so a mod's own character script can be told
// apart from Xerces'. At most 600 lines. Off otherwise.

#include <game/impl/scr/scr.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include "runtime.hpp"

using namespace game;
using namespace game::scr;
using game::scr::builtin::BuiltinMethod;

namespace xerces_character_slots {
namespace {
std::atomic<int> logged{0};
constexpr int max_lines = 600;

BuiltinMethod set_model_orig = nullptr;
BuiltinMethod attach_orig = nullptr;
BuiltinMethod detach_all_orig = nullptr;
BuiltinMethod set_player_character_orig = nullptr;
BuiltinMethod set_body_type_orig = nullptr;
BuiltinMethod set_body_style_orig = nullptr;

void note(const char *what, scriptInstance_t inst, scr_entref_t *entref,
          bool text) {
  if (logged.fetch_add(1) >= max_lines) {
    return;
  }
  std::string arg = "-";
  if (Scr_GetNumParam(inst) > 0) {
    arg = text ? std::string(Scr_GetString(inst, 0))
               : std::to_string(Scr_GetInt(inst, 0));
  }
  xerces_runtime::log_info(
      "char.call", std::string(what) + " " + arg,
      "{\"call\":\"" + std::string(what) + "\",\"class\":" +
          std::to_string(entref->classnum) + ",\"ent\":" +
          std::to_string(entref->u.entnum) + ",\"arg\":\"" + arg + "\"}");
}

#define WRAP(name, orig, is_text)                                              \
  void name(scriptInstance_t inst, scr_entref_t *entref) {                     \
    note(#name, inst, entref, is_text);                                        \
    orig(inst, entref);                                                        \
  }

WRAP(SetModel, set_model_orig, true)
WRAP(Attach, attach_orig, true)
WRAP(DetachAll, detach_all_orig, false)
WRAP(SetPlayerCharacter, set_player_character_orig, false)

bool tracing = false;
BuiltinMethod set_helmet_style_orig = nullptr;

// Whether a call from another script on this player is to be skipped
bool locked(scr_entref_t *entref) {
  if (entref->classnum != 0 || entref->u.entnum > 31) {
    return false;
  }
  const auto lock = game::get_dvar_int("xerces_char_lock");
  if (!lock || !((*lock >> entref->u.entnum) & 1)) {
    return false;
  }
  const auto applying = game::get_dvar_int("xerces_char_apply");
  return !applying || *applying != 1;
}

// One call from a script: traced, skipped when the player is locked, else the
// game's own
void locked_call(const char *name, BuiltinMethod original, scriptInstance_t inst,
                 scr_entref_t *entref) {
  if (tracing) {
    note(name, inst, entref, false);
  }
  if (locked(entref)) {
    if (tracing) {
      note((std::string(name) + " (skipped: locked)").c_str(), inst, entref,
           false);
    }
    return;
  }
  original(inst, entref);
}

void SetCharacterBodyType(scriptInstance_t inst, scr_entref_t *entref) {
  locked_call("SetCharacterBodyType", set_body_type_orig, inst, entref);
}
void SetCharacterBodyStyle(scriptInstance_t inst, scr_entref_t *entref) {
  locked_call("SetCharacterBodyStyle", set_body_style_orig, inst, entref);
}
void SetCharacterHelmetStyle(scriptInstance_t inst, scr_entref_t *entref) {
  locked_call("SetCharacterHelmetStyle", set_helmet_style_orig, inst, entref);
}
} // namespace

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_character_slots");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client()) {
      return;
    }
    const game::DvarFlags no_flags{};
    game::register_dvar_int(
        "xerces_char_lock", 0, 0, INT32_MAX, no_flags,
        "Xerces' Zombies character slots: the players (bit per client) whose "
        "character other scripts may not change.");
    game::register_dvar_int(
        "xerces_char_apply", 0, 0, 1, no_flags,
        "Xerces' Zombies character slots: 1 while Xerces' own script sets a "
        "player's character.");
    const game::DvarFlags saved{.archive = 1};
    game::register_dvar_int(
        "xerces_char_saved", 0, 0, 4096, saved,
        "Xerces' Zombies character slots: the character you last picked "
        "(index * 16 + outfit + 1, 0 for none). Put on again when you host a "
        "match.");
    using namespace game::scr::builtin::table;
    set_body_type_orig = player_methods->SetCharacterBodyType.actionFunc;
    player_methods->SetCharacterBodyType.actionFunc = SetCharacterBodyType;
    set_body_style_orig = player_methods->SetCharacterBodyStyle.actionFunc;
    player_methods->SetCharacterBodyStyle.actionFunc = SetCharacterBodyStyle;
    set_helmet_style_orig = player_methods->SetCharacterHelmetStyle.actionFunc;
    player_methods->SetCharacterHelmetStyle.actionFunc = SetCharacterHelmetStyle;

    wchar_t buffer[8];
    if (GetEnvironmentVariableW(L"XERCES_CHAR_TRACE", buffer, 8) == 0 ||
        buffer[0] != L'1') {
      return;
    }
    tracing = true;
    set_model_orig =
        game::scr::builtin::table::gscr::builtin_methods->SetModel.actionFunc;
    game::scr::builtin::table::gscr::builtin_methods->SetModel.actionFunc =
        SetModel;
    attach_orig = common_methods->Attach.actionFunc;
    common_methods->Attach.actionFunc = Attach;
    detach_all_orig = common_methods->DetachAll.actionFunc;
    common_methods->DetachAll.actionFunc = DetachAll;
    set_player_character_orig = player_methods->SetPlayerCharacter.actionFunc;
    player_methods->SetPlayerCharacter.actionFunc = SetPlayerCharacter;
  }
};
} // namespace xerces_character_slots

REGISTER_COMPONENT(xerces_character_slots::component)
