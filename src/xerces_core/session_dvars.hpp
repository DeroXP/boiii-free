#pragma once

// The launcher's game options for one game session (docs/DESIGN.md 7.3,
// Settings: "Game's setting leaves the game's own choice alone").
//
// The launcher passes options such as the FPS limit and the field of view as
// "+set <dvar> <value>", and the client sets them again right after its saved
// config, <game>\boiii_players\user\config.cfg, has run
// (src/client/xerces/launch_dvars.cpp). The client also saves every archived
// dvar back into that file, 10 s after loading it and after each change
// (src/client/component/dvars.cpp, write_archive_dvars). Saved as they run,
// the launcher's values would become the game's own, and "Game's setting"
// would then bring back the last value Xerces passed instead of the
// player's.
//
// This table keeps them out of the file: a dvar the launcher set is saved as
// config.cfg had it, or left out when the file had no line for it (so the
// game's default stays), until the game sets it to another value, which is
// the player's choice from then on. No game headers, so tests cover it.

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace xerces_core {
// Two dvar values as the game compares them: numbers by value ("110" and
// "110.000000", as the engine shows a float), anything else by its text,
// letters in any case.
bool same_dvar_value(std::string_view a, std::string_view b);

class session_dvars {
public:
  // The launcher set name to value for this session; saved is the value
  // config.cfg gives it (read_saved_dvar), nullopt when the file has no
  // line for it. Names in any case; a second call for a name replaces the
  // first.
  void add(std::string_view name, std::string_view value,
           std::optional<std::string> saved);
  bool empty() const { return entries_.empty(); }
  bool has(std::string_view name) const;

  // The game set the dvar to value after config.cfg had run. Any value other
  // than the launcher's is the player's (the game's menu, the console): the
  // dvar is saved as it runs from then on. True when that dropped it.
  bool changed(std::string_view name, std::string_view value);

  // What to save for an archived dvar running at running: running itself for
  // a dvar the launcher did not set, or one the player has changed (also
  // when running no longer matches the launcher's value, a change this table
  // did not hear of); config.cfg's value for one the launcher set; nullopt
  // to leave its line out, when config.cfg had none.
  std::optional<std::string> to_save(std::string_view name,
                                     std::string_view running);

private:
  struct entry {
    std::string value;
    std::optional<std::string> saved;
  };
  std::map<std::string, entry, std::less<>> entries_; // lower-case names
};
} // namespace xerces_core
