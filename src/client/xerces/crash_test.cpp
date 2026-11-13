#include <std_include.hpp>

// xerces_crash_test (docs/CRASH_REPORTS.md, docs/TESTING.md): a console
// command that crashes the game on purpose, to check crash reports from the
// crash dialog to the backend. It exists only when the game starts with
// -xerces-crash-test (Settings > Extra launch flags), so no normal session
// has it.
//   xerces_crash_test            writes through a null pointer: the crash
//                                handler runs (kind "exception")
//   xerces_crash_test fastfail   __fastfail: Windows ends the game at once
//                                with 0xC0000409 and no dump ("fail_fast")
//   xerces_crash_test fatal      a fatal engine error (Com_Error): the
//                                fatal-error dialog, a note without a dump
//                                ("unknown_exit")
//   xerces_crash_test freeze     the main thread never finishes its frame:
//                                the freeze report after 30 s, the game
//                                closed after 60 s ("unknown_exit", reason
//                                "freeze")
//   xerces_crash_test stall      the main thread sleeps 40 s and goes on: a
//                                freeze report that is taken back
//   xerces_crash_test throw      a thread of its own throws a C++ exception
//                                nobody catches: the game log must name it
//                                (loader.terminate) before the game ends
//   xerces_crash_test heap       the main thread takes the heap's lock and
//                                never gives it back: no report can be
//                                written, and the watchdog must still close
//                                the game after 60 s
//   xerces_crash_test script     the next script call that sets a player's
//                                character never returns (pick one in
//                                CHARACTERS): the server's thread is stuck
//                                inside a script, and the freeze report
//                                must name that script
//   xerces_crash_test guard      reads and writes a bad address through
//                                xerces_safe: the console says whether the
//                                fault was caught; the game plays on
//   xerces_crash_test latefault  a thread that writes through a null pointer
//                                as soon as the game, quitting, has called
//                                ExitProcess (quit after it): no dump and no
//                                report, the journal says clean_exit, the
//                                log has loader.exit.late_fault
//   xerces_crash_test exitfault  the thread that runs the shutdown writes
//                                through a null pointer in it (quit after
//                                it): that is a crash like any other, with
//                                its dump and report, and no clean_exit

#include <component/command.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>

#include <xerces_core/launch.hpp>

#include "character_slots.hpp"
#include "runtime.hpp"
#include "safe_memory.hpp"

namespace xerces_crash_test {
namespace {
constexpr std::string_view flag = "-xerces-crash-test";

// xerces_crash_test exitfault: this component's shutdown faults
std::atomic_bool fault_in_shutdown{false};

[[noreturn]] void crash(const std::string &kind) {
  xerces_runtime::log_warn("xerces.crash_test",
                           "Crashing on purpose (xerces_crash_test " + kind +
                               ")");
  if (kind == "fastfail") {
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
  }
  if (kind == "fatal") {
    // Never returns: the dialog ends the process (exception.cpp).
    game::com::Com_Error(game::errorParm::FATAL,
                         "xerces_crash_test: a fatal error on purpose");
  }
  if (kind == "freeze") {
    // The freeze watchdog ends the process (exception.cpp).
    for (;;) {
      Sleep(100);
    }
  }
  if (kind == "heap") {
    // The same, holding what every allocation needs.
    HeapLock(GetProcessHeap());
    for (;;) {
      Sleep(100);
    }
  }
  volatile int *nowhere = nullptr;
  *nowhere = 0x58455243; // "XERC"
  __assume(false);
}
} // namespace

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_crash_test");

public:
  void post_unpack() override {
    const auto args = xerces_runtime::command_line_args();
    if (!xerces_core::has_launch_flag(args, flag) &&
        !xerces_core::has_launch_flag(args, "-" + std::string(flag))) {
      return;
    }
    command::add("xerces_crash_test", [](const command::params &params) {
      const std::string kind = params.size() > 1 ? params.get(1) : "access";
      if (kind == "guard") {
        // An address that passes for one (above 0x10000) and is not mapped
        auto *nowhere = reinterpret_cast<void *>(uintptr_t{0x10000});
        uint64_t value = 0;
        char text[16]{};
        const bool read = xerces_safe::read(nowhere, &value, sizeof(value));
        const bool wrote = xerces_safe::write(nowhere, &value, sizeof(value));
        const size_t length =
            xerces_safe::read_text(nowhere, text, sizeof(text));
        game::com::Com_Printf(
            game::consoleChannel_e::CHANNEL_DONT_FILTER,
            game::consoleLabel_e::DEFAULT,
            "xerces_safe on a bad address: read %d, write %d, text %d (all 0: "
            "the faults were caught)\n",
            read ? 1 : 0, wrote ? 1 : 0, static_cast<int>(length));
        return;
      }
      if (kind == "script") {
        xerces_runtime::log_warn(
            "xerces.crash_test",
            "The next character change hangs the server (xerces_crash_test "
            "script)");
        xerces_character_slots::test_hang_in_script();
        return;
      }
      if (kind == "latefault") {
        xerces_runtime::log_warn(
            "xerces.crash_test",
            "A thread will fault once the game is quitting "
            "(xerces_crash_test latefault)");
        std::thread([] {
          while (!xerces_runtime::exit_code().has_value()) {
            Sleep(1);
          }
          volatile int *nowhere = nullptr;
          *nowhere = 0x58455243; // "XERC"
        }).detach();
        return;
      }
      if (kind == "exitfault") {
        xerces_runtime::log_warn(
            "xerces.crash_test",
            "The shutdown will fault once the game is quitting "
            "(xerces_crash_test exitfault)");
        fault_in_shutdown = true;
        return;
      }
      if (kind == "throw") {
        xerces_runtime::log_warn(
            "xerces.crash_test",
            "A thread of its own throws a C++ exception nobody catches "
            "(xerces_crash_test throw)");
        std::thread([] {
          throw std::runtime_error(
              "xerces_crash_test throw: uncaught on a thread of its own");
        }).detach();
        return;
      }
      if (kind == "stall") {
        xerces_runtime::log_warn(
            "xerces.crash_test",
            "Stalling on purpose (xerces_crash_test stall)");
        Sleep(40000);
        return;
      }
      crash(kind);
    });
    xerces_runtime::log_info(
        "xerces.crash_test.ready",
        "xerces_crash_test is available (-xerces-crash-test)");
  }

  void pre_destroy() override {
    if (fault_in_shutdown) {
      volatile int *nowhere = nullptr;
      *nowhere = 0x58455243; // "XERC"
    }
  }
};
} // namespace xerces_crash_test

REGISTER_COMPONENT(xerces_crash_test::component)
