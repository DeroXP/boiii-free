#pragma once

// The launcher's own crash handler (2026-11-14). Xerces.exe had none: a crash
// of the launcher left nothing but Windows' own record. Now an unhandled
// exception on any of its threads, or an uncaught C++ exception, writes
// <home>\crash\launcher-<time>.txt (what happened, Xerces' version, the stack
// by module and offset) and launcher-<time>.dmp (the threads and their
// stacks), with nothing asked of the heap at that point: the text is built in
// fixed buffers and dbghelp is loaded at the start. Both show in Diagnostics'
// crash list. The next start logs each new one (launcher.crash.previous) and
// says so on screen. They are not sent: the crash contract (CRASH_REPORTS.md)
// is the game's.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace xerces_launcher {
// crash_dir: <home>\crash; version: Xerces' version for the report
void install_crash_guard(const std::filesystem::path &crash_dir,
                         const std::string &version);

// The launcher-<time>.txt files in crash_dir not seen by an earlier start
// (their names are kept in launcher-seen.txt there), each with its "What
// happened" line; they are marked as seen.
struct previous_crash {
  std::string file;
  std::string what;
};
std::vector<previous_crash>
take_previous_crashes(const std::filesystem::path &crash_dir);

} // namespace xerces_launcher
