#include <std_include.hpp>

// xerces_loader (docs/DESIGN.md 5.4 step 3, 5.6): the game role's side of a
// launch from Xerces.exe.
//
// - The plan: the launcher writes <home>\config\plans\<id>.json and passes
//   "-xerces-plan <id>" with fs_game and fs_extramods on the command line.
//   This checks the dvars the engine ended up with against the plan and logs
//   loader.plan.applied (or loader.plan.mismatch) with the plan id.
// - The journal: <home>\logs\journal.json, written "launching" by the
//   launcher, moves through symbols (the signature lookup, signatures.cpp),
//   launching again while the engine starts, frontend, in_game and
//   clean_exit. A launch that never reaches clean_exit crashed, and the
//   launcher offers safe mode next time. Only the launch the launcher
//   recorded touches the journal (the launch id, runtime.hpp).

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include <xerces_core/journal.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/launch.hpp>

#include "runtime.hpp"
#include "signatures.hpp"

namespace xerces_loader {
namespace {
using xerces_core::launch_phase;
using xerces_runtime::set_journal_phase;

// The plan file the launcher wrote for this launch, by its id (hex only, so
// the id can never leave the plans folder).
std::optional<xerces_core::plan_mod_dvars> read_plan(const std::string &id) {
  const auto valid = std::ranges::all_of(id, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
  if (id.empty() || id.size() > 128 || !valid) {
    return std::nullopt;
  }
  return xerces_core::read_plan_mod_dvars(xerces_runtime::read_file(
      xerces_runtime::home() / "config" / "plans" / (id + ".json")));
}

void verify_plan(const std::string &id) {
  const auto plan = read_plan(id);
  if (!plan) {
    xerces_runtime::log_error("loader.plan.missing",
                              "The plan file for -xerces-plan " + id +
                                  " is missing or broken.");
    return;
  }
  const std::string fs_game(game::get_dvar_string("fs_game").value_or(""));
  const std::string fs_extramods(
      game::get_dvar_string("fs_extramods").value_or(""));
  const std::string data =
      "{\"plan_id\":" + xerces_core::json::quote(plan->plan_id) +
      ",\"fs_game\":" + xerces_core::json::quote(fs_game) +
      ",\"fs_extramods\":" + xerces_core::json::quote(fs_extramods) +
      ",\"expected_fs_game\":" + xerces_core::json::quote(plan->fs_game) +
      ",\"expected_fs_extramods\":" +
      xerces_core::json::quote(plan->fs_extramods) + ",\"multi_mod\":" +
      (xerces_signatures::feature_enabled("multi_mod") ? "true" : "false") +
      "}";
  if (fs_game == plan->fs_game && fs_extramods == plan->fs_extramods) {
    xerces_runtime::log_info("loader.plan.applied",
                             "The engine has the plan's mods", data);
  } else {
    xerces_runtime::log_error("loader.plan.mismatch",
                              "The engine's fs_game or fs_extramods differ "
                              "from the plan",
                              data);
  }
}
} // namespace

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_loader");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client()) {
      return;
    }
    // The signature lookup (the journal's "symbols" step) has usually run
    // already, in mod_stack's post_unpack; this makes sure it has.
    xerces_signatures::feature_enabled("multi_mod");
    const bool journal = xerces_runtime::journal_active();

    const std::string plan_id = xerces_runtime::flag_value("-xerces-plan");
    xerces_runtime::log_info(
        "loader.start",
        plan_id.empty() ? "no plan (vanilla or safe mode)" : plan_id,
        std::string("{\"started_by_launcher\":") +
            (xerces_runtime::started_by_launcher() ? "true" : "false") +
            ",\"journal\":" + (journal ? "true" : "false") + "}");

    // The first frame of the main loop: the engine has parsed the command line
    // (fs_game, fs_extramods) and the front end is starting.
    scheduler::once(
        [plan_id] {
          set_journal_phase(launch_phase::frontend);
          if (!plan_id.empty()) {
            verify_plan(plan_id);
          }
        },
        scheduler::main);

    // In a match or back in the menus, checked once a second.
    scheduler::loop(
        [] {
          if (!xerces_runtime::journal_active()) {
            return;
          }
          const bool in_game = game::com::Com_IsInGame() &&
                               !game::com::Com_IsRunningUILevel();
          if (in_game) {
            set_journal_phase(launch_phase::in_game);
          } else if (xerces_runtime::journal_phase() == launch_phase::in_game) {
            set_journal_phase(launch_phase::frontend);
          }
        },
        scheduler::main, 1s);
  }

  void pre_destroy() override {
    if (game::is_client() && game::is_new_client()) {
      set_journal_phase(launch_phase::clean_exit);
    }
  }
};
} // namespace xerces_loader

REGISTER_COMPONENT(xerces_loader::component)
