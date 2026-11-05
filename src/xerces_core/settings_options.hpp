#pragma once

// The headless half of the launcher's Settings screen (docs/DESIGN.md 7.3, 14):
// the player name and the client's own properties file, the launch flags the
// screen manages, and the game options it passes to the game as dvars. No UI
// and no game headers, so the tests can cover all of it.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
// ------------------------------------------------------------- player name
// The client reads the player name once, when it starts
// (src/client/component/name.cpp, load_player_name), from "playerName" in
// <game>\boiii_players\properties.json (src/common/utils/properties.cpp; the
// game runs with the game folder as its working directory). It keeps only
// printable ASCII; upstream's launcher (savePlayerName in
// src/client/launcher/launcher.cpp) also trims the name and cuts it to 16
// characters before it stores it. Xerces stores it the same way.
inline constexpr std::size_t player_name_max = 16;

struct player_name_result {
  std::string name;       // what gets stored
  bool dropped = false;   // characters outside printable ASCII were removed
  bool shortened = false; // cut to player_name_max
};
player_name_result clean_player_name(std::string_view typed);

// <game>\boiii_players\properties.json
std::filesystem::path player_properties_file(const std::filesystem::path &game_dir);

// The "playerName" member of the properties text, when it is a string.
std::optional<std::string> read_player_name(std::string_view properties_text);

// Why the name could not go into the game's file. The launcher shows its own
// (translated) sentence for each; the English message is for the log.
enum class name_failure {
  none,
  busy,           // the client held its properties lock for 3 seconds
  unreadable,     // the file could not be read
  not_json,       // the file is not valid JSON
  not_object,     // valid JSON, but not an object
  write_failed,   // the new copy could not be written next to it
  replace_failed, // the new copy could not take the file's place
};

// The properties text with "playerName" set to name, or removed when name is
// empty (the client then falls back to the Steam name). Every other member is
// kept as it was. Text that is not a JSON object is refused, so a file Xerces
// does not understand is never overwritten.
struct properties_edit {
  bool ok = false;
  std::string text;
  std::string error;
  name_failure failure = name_failure::none;
};
properties_edit with_player_name(std::string_view properties_text,
                                 std::string_view name);

enum class name_apply {
  written,   // the file now holds the name
  unchanged, // it already did
  no_file,   // the game has not made boiii_players\properties.json yet
  failed,    // could not read, parse or write it (message says why)
};
struct name_apply_result {
  name_apply status = name_apply::failed;
  name_failure failure = name_failure::none; // set when status is failed
  std::string message; // English, for the log
};
// Puts the name into the game folder's properties file while holding the
// client's own lock (the named mutex "boiii-properties-lock"), so a running
// client never reads a half-written file. The file is never created: the
// client copies an older copy from %LOCALAPPDATA%\boiii\user only when
// boiii_players\properties.json is missing, and Xerces does not read that
// folder (it also holds the identity key).
name_apply_result apply_player_name(const std::filesystem::path &game_dir,
                                    std::string_view name);

// Whether the game's file holds something other than the saved name, so
// that Enter in the name field writes it there again. in_file is the file's
// "playerName" (nullopt when it has none). An empty saved name means "use
// your Steam name", which the file only gives without a name of its own:
// the client stores the name it uses at every start (name.cpp,
// load_player_name) and on its "name" command, so a name left there by
// either, or by upstream's launcher, differs from an empty one.
bool game_name_differs(std::string_view saved,
                       const std::optional<std::string> &in_file);

// A name that could not go into the game's file yet (no file, the file
// busy, no game folder) when the game folder or the game changes:
// - write: the game folder was set or found again; the screen said the name
//   goes in once there is one.
// - ask: the game ran (it started or exited). It may have written a name of
//   its own, the one it uses at every start or one the player gave it in
//   the game, and Xerces cannot tell them apart, so it does not overwrite
//   it: the screen shows what the file says, and Enter writes.
// - wait: there is still no file.
enum class pending_name_step { write, ask, wait };
pending_name_step pending_name_after(bool folder_changed, bool file_exists);

// ------------------------------------------------------------ launch flags
// The rule build_launch_command applies: '-' then letters, digits, '-' or
// '_', at most 64 characters.
bool valid_launch_flag(std::string_view flag);

struct flags_parse {
  std::vector<std::string> flags;    // valid, in order, no duplicates
  std::vector<std::string> invalid;  // tokens that are not flags
  std::vector<std::string> reserved; // set by Xerces itself (-launch, ...)
  bool too_many = false;             // more than max_launch_flags
};
inline constexpr std::size_t max_launch_flags = 32;
// Splits on spaces and checks every token.
flags_parse parse_launch_flags(std::string_view text);
std::string join_flags(const std::vector<std::string> &flags);

// The client's flag parser accepts "-name" and "--name" (src/common/utils/
// flags.cpp), and short forms for some flags ("-nc" is -nocinematics); all
// count.
bool has_flag(const std::vector<std::string> &flags, std::string_view name);
// Removes every spelling of the flag, then adds "-name" when on.
std::vector<std::string> with_flag(std::vector<std::string> flags,
                                   std::string_view name, bool on);

// Display mode through the client's own flags, which it applies after its
// saved config has loaded (src/client/component/startup_flags.cpp):
// "borderless" (r_fullscreen 2), "windowed" (r_fullscreen 0), or "game" (no
// flag, the game's own setting). The client checks borderless first.
std::string display_mode(const std::vector<std::string> &flags);
std::vector<std::string> with_display_mode(std::vector<std::string> flags,
                                           std::string_view mode);

// -nointro and -nocinematics are not only flags: the client turns its own
// archived dvars boiii_skipIntro and boiii_skipCinematics on when it sees
// them (src/client/component/settings.cpp) and never back off, and it saves
// archived dvars into <game>\boiii_players\user\config.cfg
// (src/client/component/dvars.cpp). So once the game has started with the
// flag, it keeps skipping without it until the player turns the switch off
// in the game's Quick Settings. The launcher reads that file (never writes
// it) to say so.
inline constexpr std::string_view dvar_skip_intro = "boiii_skipIntro";
inline constexpr std::string_view dvar_skip_cinematics = "boiii_skipCinematics";

// <game>\boiii_players\user\config.cfg
std::filesystem::path game_config_file(const std::filesystem::path &game_dir);

// The value config.cfg gives a dvar: the last "set <name> <value>" line (the
// client writes set name "value"; seta and sets count too, names in any
// case), or nullopt when no line sets it.
std::optional<std::string> read_saved_dvar(std::string_view config_text,
                                           std::string_view name);
// A saved boolean dvar: true unless the value is 0 or false; nullopt when the
// file does not set it.
std::optional<bool> saved_dvar_on(std::string_view config_text,
                                  std::string_view name);

// -------------------------------------------------------------- game options
// Options passed as "+set <dvar> <value>", each confirmed in the client:
// com_maxfps (client_patches.cpp, the in-game graphics and quick settings
// menus), cg_fov_default (the graphics menu's FOV slider, 65 to 120), r_vsync
// (game/symbols/dvar, the menus) and com_drawFPS_PC (the menus' FPS counter).
inline constexpr std::string_view dvar_fps_limit = "com_maxfps";
inline constexpr std::string_view dvar_fov = "cg_fov_default";
inline constexpr std::string_view dvar_vsync = "r_vsync";
inline constexpr std::string_view dvar_draw_fps = "com_drawFPS_PC";

inline constexpr int fps_limit_min = 0; // 0: no limit
inline constexpr int fps_limit_max = 1000;
inline constexpr int fov_min = 65;
inline constexpr int fov_max = 120;

struct number_input {
  enum class kind { empty, ok, invalid } state = kind::empty;
  int value = 0;
};
// Whole numbers only (spaces around are fine); invalid when out of range.
number_input parse_number_input(std::string_view text, int min, int max);

// The FPS limit slider's stops: 0 is "the game's setting", then these limits,
// then the last stop is 0 ("no limit") so the slider reads low to high.
const std::vector<int> &fps_limit_stops();
int fps_slider_max();
// The slider stop for a value (nullopt: the game's setting); a limit between
// stops goes to the nearest one.
int fps_slider_index(std::optional<int> limit);
std::optional<int> fps_slider_value(int index);
} // namespace xerces_core
