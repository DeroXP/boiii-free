#include <std_include.hpp>

// xerces_join_after_menu: the join Servers starts (docs/DESIGN.md 11.10) runs
// once the main menu is up. Up to 0.1.0-beta.5 the launcher passed
// "+connect <address>", which the engine runs as soon as it starts. On a
// tester's PC (2026-11-07) that crashed the game three times in a row, about
// a minute and a half in: a null read in the engine's
// Storage_SerializeTransferData, under CL_CheckForResend, as the handshake
// sent the player's stats, which the game had not loaded yet. A "connect"
// typed at the main menu joined the same host. The launcher now passes
// "+set xerces_join <address>"; this reads the address from the command line
// (as mod_stack.cpp reads fs_extramods), waits for the main menu (the check
// the startup timing uses, gpu::on_main_menu), and a few seconds more, then
// runs "connect <address>" as the console does, once.

#include "runtime.hpp"

#include <component/gpu.hpp>
#include <component/scheduler.hpp>

#include <game/game.hpp>
#include <game/utils.hpp>

#include <loader/component_loader.hpp>

#include <utils/string.hpp>

#include <xerces_core/launch.hpp>

namespace xerces_join_after_menu {
namespace {
using namespace std::chrono_literals;

constexpr const char *dvar_name = "xerces_join";
// From the first frame the menu shows to the join.
constexpr std::chrono::milliseconds settle = 3s;
// No main menu by then: the join is dropped (the player can still type
// "connect" in the console).
constexpr std::chrono::minutes give_up = 10min;

std::string address{}; // from the command line; "" for a launch without a join
std::chrono::steady_clock::time_point started{};
std::optional<std::chrono::steady_clock::time_point> menu_since{};

// The value after the last "+set xerces_join" on the command line, or "".
std::string command_line_address() {
  int32_t count = 0;
  LPWSTR *const argv = CommandLineToArgvW(GetCommandLineW(), &count);
  std::string result;
  for (int32_t i = 0; argv && i + 2 < count; ++i) {
    if (_wcsicmp(argv[i], L"+set") == 0 &&
        _wcsicmp(argv[i + 1], L"xerces_join") == 0) {
      result = utils::string::convert(argv[i + 2]);
    }
  }
  LocalFree(argv);
  return result;
}

// The main thread, every 100 ms, until the join has run or is dropped.
bool poll() {
  const auto now = std::chrono::steady_clock::now();
  if (now - started > give_up) {
    xerces_runtime::log_warn("xerces.join.dropped",
                             "no main menu in 10 minutes: the join the "
                             "launcher asked for is dropped");
    return scheduler::cond_end;
  }
  if (!gpu::on_main_menu()) {
    menu_since.reset();
    return scheduler::cond_continue;
  }
  if (!menu_since) {
    menu_since = now;
    return scheduler::cond_continue;
  }
  if (now - *menu_since < settle) {
    return scheduler::cond_continue;
  }

  // The launcher checked it, but the value goes into a console command.
  if (!xerces_core::valid_server_address(address)) {
    xerces_runtime::log_warn("xerces.join.invalid",
                             "the join the launcher asked for is not a server "
                             "address; nothing joined");
    return scheduler::cond_end;
  }
  // The address stays out of the log (DESIGN.md 15.1).
  xerces_runtime::log_info("xerces.join.connect",
                           "main menu up: joining the server the launcher "
                           "named");
  const std::string command = "connect " + address + "\n";
  game::cbuf::Cbuf_AddText(game::LOCAL_CLIENT_0, command.c_str());
  return scheduler::cond_end;
}
} // namespace

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_join_after_menu");

public:
  void post_unpack() override {
    if (!game::is_client()) {
      return;
    }
    address = command_line_address();
    // On the main thread, as the other components register theirs: the
    // engine's dvars are not set up yet in post_unpack (registering here
    // crashed at start, 2026-11-07).
    scheduler::once(
        [] {
          game::register_dvar_string(
              dvar_name, address.c_str(), game::dvarFlags_e::NONE,
              "The server the launcher's Join asked for; joined once the "
              "main menu is up.");
        },
        scheduler::main);
    if (address.empty()) {
      return;
    }
    started = std::chrono::steady_clock::now();
    scheduler::schedule(poll, scheduler::main, 100ms);
  }
};
} // namespace xerces_join_after_menu

REGISTER_COMPONENT(xerces_join_after_menu::component)
