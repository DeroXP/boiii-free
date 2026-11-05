#include <std_include.hpp>

// xerces_launch_dvars: the launcher's settings win over the game's saved
// config (DESIGN.md 7.3, Settings). The launcher passes settings such as the
// FPS limit and the field of view as "+set <dvar> <value>"; the engine applies
// them at startup, but the client then runs boiii_players\user\config.cfg
// (dvars.cpp, read_archive_dvars), whose saved archive dvars would overwrite
// them. Right after that file has run (the dvars_loaded pipeline), this sets
// the same pairs again. Only for a game the launcher started, and never for
// fs_game or fs_extramods, which belong to the plan and the engine.
//
// They are for this session only: the client saves every archived dvar back
// into config.cfg (dvars.cpp, write_archive_dvars), and a launcher value
// saved there would become the game's own, so "Game's setting" in the
// launcher would bring back the last value Xerces passed. The table below
// (xerces_core/session_dvars) has those lines saved as config.cfg had them,
// until the player changes the dvar in the game (launch_dvars.hpp).

#include "launch_dvars.hpp"

#include <component/dvars.hpp>
#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>
#include <utils/io.hpp>

#include <atomic>
#include <mutex>

#include <shellapi.h>

#include <xerces_core/session_dvars.hpp>
#include <xerces_core/settings_options.hpp>

#include "runtime.hpp"

namespace xerces_launch_dvars {
namespace {
// Set once in post_unpack, before the game sets any dvar, and only read
// after that.
std::atomic_bool tracking{false};
std::mutex table_mutex;
xerces_core::session_dvars table;

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

// Every launcher +set pair this component sets again, in order.
std::vector<std::pair<std::string, std::string>> pairs() {
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return {};
  }
  std::vector<std::pair<std::string, std::string>> out;
  for (int i = 1; i + 2 < argc; ++i) {
    if (_wcsicmp(argv[i], L"+set") != 0) {
      continue;
    }
    std::string name = narrow(argv[i + 1]);
    std::string value = narrow(argv[i + 2]);
    const bool name_ok = !name.empty() && std::ranges::all_of(name, [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    });
    if (name_ok && !reserved(name) && plain(value)) {
      out.emplace_back(std::move(name), std::move(value));
    }
    i += 2;
  }
  LocalFree(argv);
  return out;
}

// "set <dvar> \"<value>\"" for every pair, as one buffer.
std::string
commands(const std::vector<std::pair<std::string, std::string>> &set) {
  std::string out;
  for (const auto &[name, value] : set) {
    out += "set " + name + " \"" + value + "\"\n";
  }
  return out;
}
} // namespace

void archive_set(game::EngineDependentDvar dvar) {
  if (!tracking) {
    return;
  }
  const char *name = dvar.debugName();
  if (!name) {
    return;
  }
  std::lock_guard lock(table_mutex);
  if (!table.has(name)) {
    return;
  }
  const char *value = game::Dvar_DisplayableValue(dvar);
  if (value && table.changed(name, value)) {
    xerces_runtime::log_info("xerces.dvars.changed_in_game",
                             std::string(name) +
                                 " changed in the game; config.cfg keeps it");
  }
}

std::optional<std::string> value_to_save(const char *name,
                                         const char *running) {
  if (!tracking || !name || !running) {
    return std::string(running ? running : "");
  }
  std::lock_guard lock(table_mutex);
  return table.to_save(name, running);
}

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_launch_dvars");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client() ||
        !xerces_runtime::started_by_launcher()) {
      return;
    }
    const auto set = pairs();
    if (set.empty()) {
      return;
    }
    // What config.cfg gives each one now: the client has not written the
    // file yet (it saves only after loading it), and runs with the game
    // folder as its working folder.
    std::string saved_config; // stays empty when there is no file
    utils::io::read_file(dvars::CONFIG_FILE_PATH, &saved_config);
    {
      std::lock_guard lock(table_mutex);
      for (const auto &[name, value] : set) {
        table.add(name, value,
                  xerces_core::read_saved_dvar(saved_config, name));
      }
    }
    tracking = true;
    xerces_runtime::log_info("xerces.dvars.session",
                             std::to_string(set.size()) +
                                 " launcher settings kept out of config.cfg");

    // Runs only when config.cfg exists, which is when it could override.
    scheduler::once(
        [text = commands(set)] {
          game::cbuf::Cbuf_ExecuteBuffer(
              game::LOCAL_CLIENT_0, game::ControllerIndex_t::CONTROLLER_INDEX_0,
              text.c_str());
          xerces_runtime::log_info(
              "xerces.dvars.reapplied",
              "launcher settings set again after config.cfg");
        },
        scheduler::pipeline::dvars_loaded);
  }
};
} // namespace xerces_launch_dvars

REGISTER_COMPONENT(xerces_launch_dvars::component)
