#include <std_include.hpp>

// xerces_launch_dvars: the launcher's settings win over the game's saved
// config (DESIGN.md 7.3, Settings). The launcher passes settings such as the
// FPS limit and the field of view as "+set <dvar> <value>"; the engine applies
// them at startup, but the client then runs boiii_players\user\config.cfg
// (dvars.cpp, read_archive_dvars), whose saved archive dvars would overwrite
// them. Right after that file has run (the dvars_loaded pipeline), this sets
// the same pairs again. Only for a game the launcher started, and never for
// fs_game or fs_extramods, which belong to the plan and the engine.

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>

#include <shellapi.h>

#include "runtime.hpp"

namespace xerces_launch_dvars {
namespace {
std::string narrow(const wchar_t *text) {
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string out(size > 0 ? std::size_t(size - 1) : 0, '\0');
  if (size > 1) {
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr,
                        nullptr);
  }
  return out;
}

bool reserved(std::string name) {
  std::ranges::transform(name, name.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return name == "fs_game" || name == "fs_extramods" || name.starts_with("0x");
}

// A value that cannot end its quotes or start another command.
bool plain(std::string_view text) {
  return !text.empty() && std::ranges::none_of(text, [](char c) {
    return c == '"' || c == ';' || static_cast<unsigned char>(c) < 0x20;
  });
}

// "set <dvar> \"<value>\"" for every launcher +set pair, as one buffer.
std::string commands() {
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return {};
  }
  std::string out;
  for (int i = 1; i + 2 < argc; ++i) {
    if (_wcsicmp(argv[i], L"+set") != 0) {
      continue;
    }
    const std::string name = narrow(argv[i + 1]);
    const std::string value = narrow(argv[i + 2]);
    const bool name_ok = !name.empty() && std::ranges::all_of(name, [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    });
    if (name_ok && !reserved(name) && plain(value)) {
      out += "set " + name + " \"" + value + "\"\n";
    }
    i += 2;
  }
  LocalFree(argv);
  return out;
}
} // namespace

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_launch_dvars");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client() ||
        !xerces_runtime::started_by_launcher()) {
      return;
    }
    const std::string text = commands();
    if (text.empty()) {
      return;
    }
    // Runs only when config.cfg exists, which is when it could override.
    scheduler::once(
        [text] {
          game::cbuf::Cbuf_ExecuteBuffer(
              game::LOCAL_CLIENT_0, game::ControllerIndex_t::CONTROLLER_INDEX_0,
              text.c_str());
          xerces_runtime::log_info("xerces.dvars.reapplied",
                                   "launcher settings set again after config.cfg");
        },
        scheduler::pipeline::dvars_loaded);
  }
};
} // namespace xerces_launch_dvars

REGISTER_COMPONENT(xerces_launch_dvars::component)
