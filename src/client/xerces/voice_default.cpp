#include <std_include.hpp>

// xerces_voice_default: voice chat starts as push-to-talk on a new game
// profile (docs/DESIGN.md 10.5; the rule is xerces_core/voice_default.hpp).
// The game keeps the choice in the player's profile, which only the game's
// Lua can change, and reads the profile after this runs. So this decides as
// the client starts, from whether the game has made a profile yet, and
// data/ui_scripts/xerces_voice/__init__.lua sets the profile setting at the
// first front end of the session.
//
// The decision is made here rather than at the dvars_loaded step, because
// that step runs only when config.cfg exists, and a new install has none. It
// does not use config.cfg at all: the profile that holds the player's choice
// is what says whether there is a choice to keep. The client runs with the
// game folder as its working folder (players_dir is relative, like
// dvars::CONFIG_FILE_PATH), and the game reads or makes its profile only
// after this runs.

#include <component/scheduler.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include <xerces_core/voice_default.hpp>

#include "runtime.hpp"

namespace xerces_voice_default {
namespace {
const std::string state_name(xerces_core::voice_state_dvar);
// Where the client sends the game's players folder (component/path.cpp).
const std::filesystem::path players_dir("boiii_players");
game::EngineDependentDvarMut state_dvar;

int value(xerces_core::voice_state state) { return static_cast<int>(state); }
} // namespace

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_voice_default");

public:
  void post_unpack() override {
    using xerces_core::game_profile;
    using xerces_core::voice_state;

    const game_profile profile = xerces_core::find_game_profile(players_dir);
    const voice_state state = xerces_core::voice_start_state(profile);

    state_dvar = game::register_dvar_int(
        state_name.c_str(), value(state), value(voice_state::keep),
        value(voice_state::not_set), game::dvarFlags_e::NONE,
        "Xerces' push-to-talk default this session (read by the front end's "
        "Lua): 0 the profile keeps its setting, 1 still to set, 2 set, 3 "
        "tried and not set.");

    if (state != voice_state::due) {
      xerces_runtime::log_info(
          "xerces.voice.kept",
          profile == game_profile::found
              ? "voice chat keeps the game profile's own push-to-talk setting"
              : "voice chat keeps the game's setting: boiii_players could not "
                "be read");
      return;
    }
    xerces_runtime::log_info("xerces.voice.default_due",
                             "no game profile yet: voice chat will start as "
                             "push-to-talk (set at the first front end)");

    // The Lua moves the state on once, at the first front end.
    scheduler::schedule(
        [] {
          const int now = state_dvar ? state_dvar.get_int() : -1;
          if (now == value(voice_state::set)) {
            xerces_runtime::log_info(
                "xerces.voice.push_to_talk",
                "voice chat set to push-to-talk; the game's option is the "
                "player's from now on");
            return scheduler::cond_end;
          }
          if (now == value(voice_state::not_set)) {
            xerces_runtime::log_warn(
                "xerces.voice.not_set",
                "voice chat could not be set to push-to-talk; the game's own "
                "setting stays, and Xerces does not try again this session");
            return scheduler::cond_end;
          }
          return scheduler::cond_continue;
        },
        scheduler::main, 1s);
  }
};
} // namespace xerces_voice_default

REGISTER_COMPONENT(xerces_voice_default::component)
