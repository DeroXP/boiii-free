#pragma once

// What every Xerces part of the game role shares (docs/DESIGN.md 3.5, 5.6,
// 15.1): <home> (XERCES_HOME, set by the launcher, else %LOCALAPPDATA%\Xerces),
// the game role's JSON log, <home>\logs\<date>-<time>-game-<pid>.jsonl, and
// the launch journal, <home>\logs\journal.json.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <xerces_core/journal.hpp>
#include <xerces_core/log.hpp>

namespace xerces_runtime {
std::filesystem::path home();
// The launcher sets XERCES_HOME for the game it starts (the command pipe is
// served then). The journal is decided by the launch id instead (below),
// because a game restarted from the crash dialog inherits XERCES_HOME too.
bool started_by_launcher();

void log(xerces_core::log_level level, std::string_view event,
         std::string_view message, std::string_view data_json = {});
inline void log_info(std::string_view event, std::string_view message,
                     std::string_view data_json = {}) {
  log(xerces_core::log_level::info, event, message, data_json);
}
inline void log_warn(std::string_view event, std::string_view message,
                     std::string_view data_json = {}) {
  log(xerces_core::log_level::warn, event, message, data_json);
}
inline void log_error(std::string_view event, std::string_view message,
                      std::string_view data_json = {}) {
  log(xerces_core::log_level::error, event, message, data_json);
}

// Writes a file through a temporary file, so a crash never leaves half of
// it behind.
bool write_file(const std::filesystem::path &path, std::string_view text);
std::string read_file(const std::filesystem::path &path);

// The value after a flag on the game's own command line ("-xerces-plan
// <id>"), looking only before the first "+" command.
std::string flag_value(std::string_view flag);

// ----------------------------------------------------------- the journal
// The launcher writes the journal as "launching" with a one-time launch id
// and passes the same id in XERCES_LAUNCH_ID. This process takes the id out
// of its environment as it starts, before main() and so before the crash
// dialog exists, so a game restarted from that dialog never inherits it. The
// first journal call here takes the journal over only if the id matches
// (xerces_core::journal_claimable), then clears the id from the journal,
// which tells the launcher the game took over. After that a phase is written
// only while the journal is still this launch's: a newer launch or
// clean_exit ends it.
bool journal_active();
xerces_core::launch_phase journal_phase();
void set_journal_phase(xerces_core::launch_phase phase);

// The signature lookup (DESIGN.md 3.3) is the journal's "symbols" step. The
// object returned moves the journal back to "launching" (the engine is still
// starting) when the lookup ends, until the first main-loop frame writes
// "frontend". It does nothing if the journal is already past "launching".
class journal_step {
public:
  journal_step(xerces_core::launch_phase phase,
               xerces_core::launch_phase after);
  ~journal_step();
  journal_step(const journal_step &) = delete;
  journal_step &operator=(const journal_step &) = delete;

private:
  bool entered_ = false;
  xerces_core::launch_phase after_;
};
[[nodiscard]] journal_step journal_symbols_step();

// ---------------------------------------------------------------- the exit
// The engine's quit command ran (loader.cpp wraps it): the player chose to
// leave, so the launch ended cleanly even if the exit that follows is forced
// (the console's close forces it after 3 s, code 1).
void note_quit_requested();
bool quit_requested();
// The crash handler ran (exception.cpp): no exit after it counts as clean,
// even one that follows a quit (a crash while the game shuts down).
void note_crash();
bool crash_seen();
// The console's close forces the exit 3 s after it runs "quit" (console.cpp),
// before any shutdown code: the journal gets clean_exit there, when a quit
// ran and no crash did. A normal exit writes it in the loader's shutdown.
void journal_quit_forced();
// main.cpp's exit hook passes the code the game gave ExitProcess, just before
// the components' shutdown runs; nullopt when the process ends another way.
void note_exit_code(std::uint32_t code);
std::optional<std::uint32_t> exit_code();

// ------------------------------------------------------- the console tail
// console.cpp passes everything the console shows; the last lines go into the
// game log when a map fails or the game ends without quitting (DESIGN.md
// 5.10), because the console window dies with the process. Safe to call from
// any thread and during shutdown.
void console_append(std::string_view text);
std::vector<std::string> console_last_lines(std::size_t count);
} // namespace xerces_runtime
