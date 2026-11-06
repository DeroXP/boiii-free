#include <std_include.hpp>

#include <loader/component_loader.hpp>

// In case of clangd compilation
#if __has_include("version.hpp")
#include "version.hpp" // IWYU pragma: keep
#else
#ifndef VERSION
#define VERSION "0"
#endif
#ifndef SHORTVERSION
#define SHORTVERSION "0"
#endif
#endif
#ifndef XERCES_VERSION_LABEL
#define XERCES_VERSION_LABEL "Xerces"
#endif

#include "scheduler.hpp"

#include <utils/flags.hpp>
#include <utils/hook.hpp>

namespace branding {
namespace {
void draw_branding() {
  if (!game::com::Com_IsInGame()) {
    constexpr float X = 4.0;
    constexpr float Y = 0.0;
    constexpr float SCALE = 0.45f;
    game::vec4_t color = {.r = 0.666f, .g = 0.666f, .b = 0.666f, .a = 0.666f};

    static const uint32_t *font = reinterpret_cast<const uint32_t *>(
        game::select(0x1432D2918, 0x1433519B0, 0x1410E1BE0));

    // Xerces: its own version and commit (premake5.lua, xerces_version.h).
    game::render::R_AddCmdDrawText(
        XERCES_VERSION_LABEL, std::numeric_limits<int>::max(), font, X,
        Y + static_cast<float>(font[2]) * SCALE, SCALE, SCALE, 0.0f, &color,
        game::itemTextStyle::NORMAL);
  }
}

const char *get_ingame_console_prefix_stub() { return "EZZ> "; }
} // namespace

struct component final : client_component {
  DEFINE_COMPONENT_NAME("branding");

  void post_unpack() override {
    if (!utils::flags::has_flag("nobranding")) {

      scheduler::loop(draw_branding, scheduler::renderer);

      // Change window title prefix. Xerces: the name only. The engine's own
      // text there (its "Call of Duty - ship" title) holds 21 characters, so
      // a version and commit would overwrite the strings after it.
      constexpr char window_prefix[] = "Xerces";
      static_assert(sizeof(window_prefix) <= 22);
      utils::hook::copy_string(game::select(0x142FC02D8, 0x14303F3D8, 0x0),
                               window_prefix);

      // Change ingame console prefix
      utils::hook::call(game::con::Con_DrawInput.offset(0x90),
                        get_ingame_console_prefix_stub);
    }
  }
};
} // namespace branding

REGISTER_COMPONENT(branding::component)
