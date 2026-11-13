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
//   launching again while the engine starts, frontend, loading_map (a level
//   is loading: the client is not yet active in it), in_game and clean_exit.
//   A launch that never reaches clean_exit crashed, and the launcher offers
//   safe mode next time. Only the launch the launcher recorded touches the
//   journal (the launch id, runtime.hpp).
// - clean_exit is written when the engine's quit command runs (wrapped
//   below): the console's close forces the exit 3 s later, before any
//   shutdown code, and leaving a match takes longer than that. A game that
//   ends through its own ExitProcess without a quit and with a code other
//   than 0 crashed (seen with two custom maps: 0xFFFFFFFF mid-load); that
//   exit and every engine error the player sees go into the game log with
//   the console's last lines.

#include "loader.hpp"

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

// The console lines that go into the log with a failure.
constexpr std::size_t console_lines_logged = 60;

std::string console_json() {
  std::string out = "[";
  bool first = true;
  for (const auto &line :
       xerces_runtime::console_last_lines(console_lines_logged)) {
    if (!first) {
      out += ',';
    }
    first = false;
    // Game text may hold CP-1252 bytes; one invalid byte would make the log
    // drop the event's whole data.
    out += xerces_core::json::quote(xerces_core::json::valid_utf8(line, 600));
  }
  return out + "]";
}

// ---------------------------------------------------------------- quit
game::cmd::xcommand_t engine_quit = nullptr;

// Only notes the quit: clean_exit is written once the exit happens, by the
// shutdown (pre_destroy) or by the console's forced exit 3 s later
// (runtime.hpp, journal_quit_forced), never after the crash handler ran, so a
// crash while the game shuts down still counts as a crash.
void quit_command() {
  xerces_runtime::note_quit_requested();
  if (engine_quit) {
    engine_quit();
  }
}

// Points the engine's "quit" entry at quit_command (the engine's function
// still runs). Data in the command list, no code bytes.
bool wrap_quit_command() {
  for (auto *command = *game::cmd::cmd_functions; command;
       command = command->next) {
    if (command->name && _stricmp(command->name, "quit") == 0) {
      if (command->function != &quit_command) {
        engine_quit = command->function;
        command->function = &quit_command;
      }
      return true;
    }
  }
  return false;
}

[[noreturn]] void log_terminate() {
  std::string what = "unknown exception";
  if (const std::exception_ptr current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception &e) {
      what = std::string(typeid(e).name()) + ": " + e.what();
    } catch (...) {
      what = "a non-standard exception";
    }
  } else {
    what = "std::terminate without an exception";
  }
  xerces_runtime::note_crash();
  xerces_runtime::log_error(
      "loader.terminate",
      "The game ended on an uncaught C++ exception: " +
          xerces_core::json::valid_utf8(what, 300),
      "{\"exception\":" +
          xerces_core::json::quote(xerces_core::json::valid_utf8(what, 1000)) +
          ",\"phase\":" +
          xerces_core::json::quote(
              xerces_core::to_string(xerces_runtime::journal_phase())) +
          ",\"console\":" + console_json() + "}");
  std::abort();
}

std::atomic<bool> terminate_handler_on{false};

// In a level, is the local client playing it yet, or still loading it?
launch_phase level_phase() {
  const auto state = game::cg::clientUIActives->actives[0].connectionState;
  return state == game::connstate_t::ACTIVE ? launch_phase::in_game
                                            : launch_phase::loading_map;
}

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

void set_terminate_handler_on_this_thread() {
  if (terminate_handler_on.load(std::memory_order_acquire)) {
    std::set_terminate(log_terminate);
  }
}

std::string current_map() {
  for (const char *name : {"mapname", "ui_mapname"}) {
    if (auto value = game::get_dvar_string(name); value && !value->empty()) {
      return xerces_core::json::valid_utf8(*value, 64);
    }
  }
  return {};
}

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_loader");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client()) {
      return;
    }
    // An uncaught C++ exception ends the process through abort's fail-fast
    // (0xC0000409), past the crash handler and the exit hook: say which
    // exception it was first, with the console's last lines.
    static const std::terminate_handler previous =
        std::set_terminate(log_terminate);
    (void)previous;
    terminate_handler_on.store(true, std::memory_order_release);
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
          // Every engine command exists by the first frame.
          if (!wrap_quit_command()) {
            xerces_runtime::log_warn(
                "loader.quit.unwrapped",
                "The engine's quit command was not found; quitting relies on "
                "the exit code alone");
          }
        },
        scheduler::main);

    // Loading a level, playing it, or back in the menus, checked once a
    // second.
    // Also for a game that does not write the journal (restarted from the
    // crash dialog, started without the launcher): its logs, crash notes and
    // freeze watchdog go by the phase (xerces/runtime.cpp).
    scheduler::loop(
        [] {
          const bool in_level =
              game::com::Com_IsInGame() && !game::com::Com_IsRunningUILevel();
          const launch_phase now = xerces_runtime::journal_phase();
          if (in_level) {
            const launch_phase phase = level_phase();
            if (phase != now) {
              set_journal_phase(phase);
              if (phase == launch_phase::in_game) {
                xerces_runtime::log_info(
                    "loader.map.ready", current_map(),
                    "{\"map\":" + xerces_core::json::quote(current_map()) +
                        "}");
              }
            }
          } else if (now == launch_phase::in_game ||
                     now == launch_phase::loading_map) {
            set_journal_phase(launch_phase::frontend);
          }
        },
        scheduler::main, 1s);
  }

  void pre_destroy() override {
    if (!game::is_client() || !game::is_new_client()) {
      return;
    }
    const bool quit = xerces_runtime::quit_requested();
    const auto code = xerces_runtime::exit_code();
    if (!xerces_runtime::crash_seen() &&
        xerces_core::game_exit_is_clean(quit, code)) {
      set_journal_phase(launch_phase::clean_exit);
      return;
    }
    // The engine ended the game without a quit: the journal keeps its phase,
    // so the launcher counts the launch as a crash.
    char hex[16];
    std::snprintf(hex, sizeof(hex), "0x%08X", code.value_or(0));
    xerces_runtime::log_error(
        "loader.exit.unexpected",
        std::string("The game ended without quitting (exit code ") + hex +
            ") while " +
            xerces_core::to_string(xerces_runtime::journal_phase()),
        "{\"code\":" + std::to_string(code.value_or(0)) + ",\"phase\":" +
            xerces_core::json::quote(
                xerces_core::to_string(xerces_runtime::journal_phase())) +
            ",\"map\":" + xerces_core::json::quote(current_map()) +
            ",\"console\":" + console_json() + "}");
  }
};

void engine_error(std::uint32_t code, std::string_view message,
                  std::string_view source) {
  using game::errorParm;
  const auto parm = static_cast<errorParm>(code);
  // Restarts and disconnects are how the engine leaves a match normally.
  const auto quiet = errorParm::SOFTRESTART | errorParm::SOFTRESTART_KEEPDW |
                     errorParm::SOFTRESTART_SILENT | errorParm::DISCONNECT |
                     errorParm::SERVERDISCONNECT;
  if ((parm & quiet) != errorParm::NONE || !game::is_client() ||
      !game::is_new_client()) {
    return;
  }
  const std::string text = xerces_core::json::valid_utf8(message, 4000);
  // The first line that says something ("****" banners carry nothing).
  std::string headline;
  std::size_t start = 0;
  while (start < text.size() && headline.empty()) {
    const auto end = text.find('\n', start);
    std::string line = text.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (line.find_first_not_of("* \t\r") != std::string::npos) {
      headline = line;
    }
    start = end == std::string::npos ? text.size() : end + 1;
  }
  headline = xerces_core::json::valid_utf8(headline, 300);
  xerces_runtime::log_error(
      "loader.engine.error", headline.empty() ? "engine error" : headline,
      "{\"source\":" + xerces_core::json::quote(source) +
          ",\"code\":" + std::to_string(code) + ",\"map\":" +
          xerces_core::json::quote(current_map()) + ",\"phase\":" +
          xerces_core::json::quote(
              xerces_core::to_string(xerces_runtime::journal_phase())) +
          ",\"message\":" + xerces_core::json::quote(text) +
          ",\"console\":" + console_json() + "}");
}
} // namespace xerces_loader

REGISTER_COMPONENT(xerces_loader::component)
