#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics.hpp"
#include "planner.hpp"

namespace xerces_core {
// How the launcher starts the game role (DESIGN.md 5.4):
//   <client> -launch [flags] -xerces-plan <plan_id>
//            +set fs_game <main> +set fs_extramods "<rest, highest first>"
// Flags come first: the engine joins everything after "+set <dvar>" up to the
// next "+" into the value, so a flag after it would become part of a dvar.
struct launch_options {
  bool nointro = true;
  bool safe = false;                    // upstream's -safe (windowed, no vsync)
  bool mods_off = false;                // safe mode: no fs_game, no extra mods
  std::vector<std::string> extra_flags; // each "-name", letters, digits, - _
  // Extra "+set <dvar> <value>" pairs from settings, after the mod dvars.
  std::vector<std::pair<std::string, std::string>> dvars;
  // "+set xerces_join <host:port>" last: join a self-hosted server once the
  // main menu is up (DESIGN.md 11.10).
  std::optional<std::string> connect;
};

// A server address the command line can carry: host (letters, digits, '.',
// '-') or IPv4, optionally ":port" (1 to 65535), or [IPv6]:port.
bool valid_server_address(std::string_view address);

struct launch_command {
  std::vector<std::string> args; // without the program
  std::string command_line;      // args quoted for CreateProcessW, UTF-8
  diagnostics diags;
  bool ok() const { return !diags.has_errors(); }
};

// resolved is null for a vanilla launch. A plan with errors is refused, and so
// is any value the engine's command line cannot carry safely: a '+' starts a
// new command, a '"' ends a quoted value, a ';' separates commands in the
// engine's command buffer and "//" starts a comment (values are not quoted
// instead, because how the engine reads quotes in "+set" is not verified).
// Settings never set fs_game or fs_extramods, in any letter case, and dvar
// names written as a hash ("0x...") are refused.
launch_command build_launch_command(const plan *resolved,
                                    const launch_options &options);

// One argument quoted the way CommandLineToArgvW and the CRT read it back.
std::string quote_argument(std::string_view arg);

// What the game role checks a launch against (DESIGN.md 5.4 step 3): the
// plan file's id and the fs_game and fs_extramods values its stack gives, read
// back from plan_to_json output.
struct plan_mod_dvars {
  std::string plan_id;
  std::string fs_game;
  std::string fs_extramods;
};
std::optional<plan_mod_dvars> read_plan_mod_dvars(std::string_view plan_json);

// A plan id without its "sha256:" tag: the -xerces-plan value and the name of
// config\plans\<id>.json (':' cannot be in a Windows file name).
std::string plan_file_id(std::string_view plan_id);

// The value after a flag ("-xerces-plan <id>"), looking only before the first
// "+" command, as the client reads its own command line.
std::optional<std::string> flag_value(const std::vector<std::string> &args,
                                      std::string_view flag);
// Whether a flag without a value ("-xerces-db-pool") is there, again only
// before the first "+" command.
bool has_launch_flag(const std::vector<std::string> &args,
                     std::string_view flag);
} // namespace xerces_core
