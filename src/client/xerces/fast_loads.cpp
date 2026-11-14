#include <std_include.hpp>

// -xerces-fast-loads (opt-in; docs/research/engine-re.md, "A map's load"):
//
// 1. The loading screen draws every frame. The engine's screen update
//    (SCR_UpdateScreen, 0x1413E1570) draws a frame only once 16 ms have passed
//    since the last while the local client's connection is not active (33 ms
//    in its last two states before that), and the texture streamer asks for
//    its next reads from that screen update: the images that stream in behind
//    the loading screen come at that pace. Both waits are set to 0 (the two
//    instructions, mov ebx, 16 at 0x1413E15A4 and mov ecx, 33 at 0x1413E15A9,
//    are checked first; anything else there is left alone and logged).
//    Custom maps loaded 3 to 12 s sooner with it (docs/TESTING.md).
// 2. A match launched from a lobby with no other player in it does not wait
//    for the others' acknowledgements: lobbyLaunch_waitForClientAckDelay
//    (2,000 ms) and lobbyLaunch_gameLaunchDelay (700 ms) are 0 for that
//    launch, and back to what they were for a launch with others in the
//    lobby (they give a friend's game time to hear of the launch). About 1.9 s
//    before the level starts loading.

#include "fast_loads.hpp"

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include <utils/hook.hpp>

#include <xerces_core/launch.hpp>

#include "runtime.hpp"

namespace xerces_fast_loads {
namespace {
constexpr std::string_view flag = "-xerces-fast-loads";
constexpr std::uintptr_t wait_loading = 0x1413E15A4; // mov ebx, 0x10
constexpr std::uintptr_t wait_late = 0x1413E15A9;    // mov ecx, 0x21

constexpr const char *ack_delay = "lobbyLaunch_waitForClientAckDelay";
constexpr const char *launch_delay = "lobbyLaunch_gameLaunchDelay";

bool enabled = false;
// The lobby's own values, read before each change of ours (main thread only)
std::optional<std::int32_t> ack_delay_before;
std::optional<std::int32_t> launch_delay_before;
// Our zeros are in the dvars now (put back by restore_waits)
bool zeroed = false;

bool code_is(const std::uintptr_t address, const std::uint8_t opcode,
             const std::uint32_t value) {
  const auto *code = reinterpret_cast<const std::uint8_t *>(address);
  std::uint32_t immediate = 0;
  std::memcpy(&immediate, code + 1, sizeof(immediate));
  return code[0] == opcode && immediate == value;
}

bool draw_every_frame() {
  const std::uintptr_t loading = game::relocate(wait_loading);
  const std::uintptr_t late = game::relocate(wait_late);
  if (!code_is(loading, 0xBB, 0x10) || !code_is(late, 0xB9, 0x21)) {
    xerces_runtime::log_warn("xerces.fast_loads.screen_off",
                             "The screen update's waits are not the expected "
                             "code; left as they are");
    return false;
  }
  utils::hook::set<std::uint32_t>(loading + 1, 0);
  utils::hook::set<std::uint32_t>(late + 1, 0);
  return true;
}

// The lobby's own waits back once the launch is under way: a later launch
// as a friend's guest reads lobbyLaunch_gameLaunchDelay too (a review,
// 2026-11-14). Main thread.
void restore_waits() {
  if (!zeroed || !ack_delay_before || !launch_delay_before) {
    return;
  }
  game::set_dvar_int(ack_delay, *ack_delay_before, false);
  game::set_dvar_int(launch_delay, *launch_delay_before, false);
  zeroed = false;
}
} // namespace

void before_lobby_launch() {
  if (!enabled || game::is_legacy_client()) {
    return;
  }
  // What the lobby has now, unless our zeros are still in (a mod or the
  // console may have changed them since)
  if (!zeroed) {
    ack_delay_before = game::get_dvar_int(ack_delay);
    launch_delay_before = game::get_dvar_int(launch_delay);
    if (!ack_delay_before || !launch_delay_before) {
      enabled = false; // not this game's dvars: leave the launch alone
      return;
    }
  }
  // No other player in the game lobby nor in the party (private) lobby,
  // whose members may not have moved to the game lobby yet
  const bool alone = game::lobby::LobbyHost_GetClientCount(
                         game::lobby::LobbyType::GAME,
                         game::lobby::LobbyClientType::REMOTE) == 0 &&
                     game::lobby::LobbyHost_GetClientCount(
                         game::lobby::LobbyType::PRIVATE,
                         game::lobby::LobbyClientType::REMOTE) == 0;
  game::set_dvar_int(ack_delay, alone ? 0 : *ack_delay_before, false);
  game::set_dvar_int(launch_delay, alone ? 0 : *launch_delay_before, false);
  zeroed = alone;
  if (alone) {
    // The launch reads them within its first seconds (the level starts
    // loading about 3.6 s after the command)
    scheduler::once(restore_waits, scheduler::main, 15s);
  }
  xerces_runtime::log_info(
      "xerces.fast_loads.launch",
      alone ? "No other player in the lobby: the launch does not wait for "
              "acknowledgements"
            : "Other players in the lobby: the launch waits for them as usual");
}

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_fast_loads");

public:
  void post_unpack() override {
    if (!game::is_new_client()) {
      return;
    }
    const auto args = xerces_runtime::command_line_args();
    if (!xerces_core::has_launch_flag(args, flag) &&
        !xerces_core::has_launch_flag(args, "-" + std::string(flag))) {
      return;
    }
    enabled = true;
    xerces_runtime::log_info(
        "xerces.fast_loads.on",
        draw_every_frame()
            ? "The loading screen draws every frame and a launch alone does "
              "not wait for others (-xerces-fast-loads)"
            : "A launch alone does not wait for others; the loading screen "
              "is as it was (-xerces-fast-loads)");
  }
};
} // namespace xerces_fast_loads

REGISTER_COMPONENT(xerces_fast_loads::component)
