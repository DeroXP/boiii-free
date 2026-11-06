#include <std_include.hpp>

// xerces_voice_default: voice chat starts as push-to-talk in a Xerces install
// (docs/DESIGN.md 10.5; the rule is xerces_core/voice_default.hpp). The game
// keeps the choice in the player's profile, which only the game's Lua can
// change, and reads the profile after this runs. So this decides from
// config.cfg as the client starts, and
// data/ui_scripts/xerces_voice/__init__.lua sets the profile setting when the
// front end loads, then turns the archived dvar on so it is never set again.
//
// The decision reads the file here rather than at the dvars_loaded step,
// because that step runs only when config.cfg exists, and a new install has
// none. The client has not written the file yet: it saves only after loading
// it (dvars.cpp), and it runs with the game folder as its working folder.

#include <component/dvars.hpp>
#include <component/scheduler.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>
#include <utils/io.hpp>

#include <xerces_core/voice_default.hpp>

#include "runtime.hpp"

namespace xerces_voice_default {
namespace {
const std::string applied_name(xerces_core::voice_default_dvar);
const std::string due_name(xerces_core::voice_due_dvar);
game::EngineDependentDvarMut applied_dvar;
} // namespace

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_voice_default");

public:
  void post_unpack() override {
    std::string saved_config; // stays empty when there is no file
    utils::io::read_file(dvars::CONFIG_FILE_PATH, &saved_config);
    const bool due = xerces_core::voice_push_to_talk_due(saved_config);

    // Registered before config.cfg runs, so its saved line sets this dvar.
    applied_dvar = game::register_dvar_bool(
        applied_name.c_str(), false, game::dvarFlags_e::ARCHIVE,
        "On once Xerces has started voice chat as push-to-talk on this "
        "install; the game's Push to talk option is the player's from then "
        "on.");
    game::register_dvar_bool(
        due_name.c_str(), due, game::dvarFlags_e::NONE,
        "On while Xerces still has to set push-to-talk this session (read by "
        "the front end's Lua).");

    if (!due) {
      xerces_runtime::log_info(
          "xerces.voice.kept",
          "voice chat keeps the player's own push-to-talk setting");
      return;
    }
    xerces_runtime::log_info(
        "xerces.voice.default_due",
        "voice chat will start as push-to-talk (set when the front end loads)");

    // The Lua turns the archived dvar on once the profile says push-to-talk.
    scheduler::schedule(
        [] {
          if (!applied_dvar || !applied_dvar.get_bool()) {
            return scheduler::cond_continue;
          }
          xerces_runtime::log_info(
              "xerces.voice.push_to_talk",
              "voice chat set to push-to-talk; the game's option keeps the "
              "player's choice from now on");
          return scheduler::cond_end;
        },
        scheduler::main, 1s);
  }
};
} // namespace xerces_voice_default

REGISTER_COMPONENT(xerces_voice_default::component)
