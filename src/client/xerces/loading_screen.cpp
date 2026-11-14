#include <std_include.hpp>

// -xerces-fast-loading-screen (opt-in; docs/research/engine-re.md, "A map's
// load"). The engine's screen update (SCR_UpdateScreen, 0x1413E1570) draws a
// frame only once 16 ms have passed since the last while the local client's
// connection is not active (33 ms in its last two states before that): the
// loading screen runs at about 57 frames a second while the main loop runs
// at the frame cap. The texture streamer asks for its next reads from that
// screen update, so the last seconds of a load, when the level's images
// stream in behind the loading screen, go at that pace (about 115 MB a
// second from Windows' cache, where the same streamer reads several hundred
// in a level). With the flag both waits are 0: the loading screen draws
// every frame the main loop makes, and the streamer asks as often.
//
// The two instructions are checked before they are changed (mov ebx, 16 at
// 0x1413E15A4 and mov ecx, 33 at 0x1413E15A9); anything else there is left
// alone and logged.

#include <game/game.hpp>
#include <loader/component_loader.hpp>

#include <utils/hook.hpp>

#include <xerces_core/launch.hpp>

#include "runtime.hpp"

namespace xerces_loading_screen {
namespace {
constexpr std::string_view flag = "-xerces-fast-loading-screen";
constexpr std::uintptr_t wait_loading = 0x1413E15A4; // mov ebx, 0x10
constexpr std::uintptr_t wait_late = 0x1413E15A9;    // mov ecx, 0x21

bool code_is(const std::uintptr_t address, const std::uint8_t opcode,
             const std::uint32_t value) {
  const auto *code = reinterpret_cast<const std::uint8_t *>(address);
  std::uint32_t immediate = 0;
  std::memcpy(&immediate, code + 1, sizeof(immediate));
  return code[0] == opcode && immediate == value;
}
} // namespace

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_loading_screen");

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
    const std::uintptr_t loading = game::relocate(wait_loading);
    const std::uintptr_t late = game::relocate(wait_late);
    if (!code_is(loading, 0xBB, 0x10) || !code_is(late, 0xB9, 0x21)) {
      xerces_runtime::log_warn("xerces.loading_screen.off",
                               "The screen update's waits are not the "
                               "expected code; left as they are");
      return;
    }
    utils::hook::set<std::uint32_t>(loading + 1, 0);
    utils::hook::set<std::uint32_t>(late + 1, 0);
    xerces_runtime::log_info("xerces.loading_screen.on",
                             "The loading screen draws every frame "
                             "(-xerces-fast-loading-screen)");
  }
};
} // namespace xerces_loading_screen

REGISTER_COMPONENT(xerces_loading_screen::component)
