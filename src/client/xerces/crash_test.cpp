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

#include <component/command.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>

#include <xerces_core/launch.hpp>

#include "runtime.hpp"

namespace xerces_crash_test {
namespace {
constexpr std::string_view flag = "-xerces-crash-test";

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
      crash(params.size() > 1 ? params.get(1) : "access");
    });
    xerces_runtime::log_info(
        "xerces.crash_test.ready",
        "xerces_crash_test is available (-xerces-crash-test)");
  }
};
} // namespace xerces_crash_test

REGISTER_COMPONENT(xerces_crash_test::component)
