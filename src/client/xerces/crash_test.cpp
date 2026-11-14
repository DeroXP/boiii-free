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
//   xerces_crash_test hitch [ms] no crash: one frame of ms (300; 50 to 3000),
//                                half busy, half asleep, for a frame-time
//                                run's stalls.csv (the stall sampler)
//   xerces_crash_test throw      a thread of its own throws a C++ exception
//                                nobody catches: the game log must name it
//                                (loader.terminate) before the game ends
//   xerces_crash_test handlerfault  a crash, and the crash handler faults
//                                itself after its first report: one dump
//                                and one report, not one for each time
//                                the handler is entered again
//   xerces_crash_test dumpfault  a crash, and the handler's in-memory dump
//                                faults: caught, the report stands
//   xerces_crash_test cmdlist    no crash: every console command in the
//                                engine's list, with its handler (the game's
//                                address at base 0x140000000, or the module
//                                that added it), to <home>\commands.txt
//   xerces_crash_test threads    no crash: the game's thread table, the job
//                                system's worker records and every thread
//                                with its name, CPU time, cycles and where it
//                                started, to <home>\threads.txt (what names
//                                the threads in reports; two of these some
//                                seconds apart give each thread's CPU share)
//   xerces_crash_test windowhang the game's Window thread is held in a
//                                message hook for good: Present never comes,
//                                and the freeze report's "What the frame
//                                waits for" must lead to it
//   xerces_crash_test joinrule0  no crash: the map-first rule a join of a host
//                                with map scripts first off leaves (0); the
//                                next match this game launches itself must
//                                drop it (mod_stack.cpp, lobbyLaunchGame)
//   xerces_crash_test workshopget <id>  no crash: asks Steam for a Workshop
//                                item as the in-game Workshop menu does (the
//                                helper process, its lines, its end); the
//                                game log says workshop.steam.installed or
//                                .failed
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
#include <component/mod_stack.hpp>
#include <component/workshop/workshop.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>

#include <xerces_core/crash_report.hpp>
#include <xerces_core/launch.hpp>

#include <tlhelp32.h>

#include "character_slots.hpp"
#include "runtime.hpp"
#include "safe_memory.hpp"

namespace xerces_crash_test {
namespace {
constexpr std::string_view flag = "-xerces-crash-test";

// xerces_crash_test exitfault: this component's shutdown faults
std::atomic_bool fault_in_shutdown{false};
std::atomic<int> handler_fault_at{0};

// xerces_crash_test windowhang: the message the hook holds the game's
// Window thread on
constexpr UINT window_hang_message = WM_APP + 0x77;

LRESULT CALLBACK window_hang_hook(const int code, const WPARAM wparam,
                                  const LPARAM lparam) {
  const auto *message = reinterpret_cast<const MSG *>(lparam);
  if (code == HC_ACTION && message && message->message == window_hang_message) {
    // The Window thread never comes back: Present is not taken
    for (;;) {
      Sleep(100);
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

BOOL CALLBACK first_visible_window(const HWND window, const LPARAM out) {
  if (IsWindowVisible(window)) {
    *reinterpret_cast<HWND *>(out) = window;
    return FALSE;
  }
  return TRUE;
}

// One long frame (not a freeze): half of it busy in this function, half
// asleep in the kernel. A frame-time run's stall sampler (perf_probe.cpp)
// samples the main thread every 8 ms meanwhile, so its stalls.csv shows
// whether the walk gets from here (and from ntdll) through the client's and
// the game's frames.
__declspec(noinline) void hitch(const int ms) {
  LARGE_INTEGER frequency{};
  LARGE_INTEGER start{};
  LARGE_INTEGER now{};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&start);
  const LONGLONG busy = frequency.QuadPart * (ms / 2) / 1000;
  volatile std::uint64_t spins = 0;
  do {
    spins = spins + 1;
    QueryPerformanceCounter(&now);
  } while (now.QuadPart - start.QuadPart < busy);
  Sleep(static_cast<DWORD>(ms - ms / 2));
}

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
      if (kind == "cmdlist") {
        const std::uintptr_t base = game::get_engine_base();
        std::string out;
        int count = 0;
        for (auto *command = *game::cmd::cmd_functions; command;
             command = command->next) {
          if (!command->name) {
            continue;
          }
          const auto handler =
              reinterpret_cast<std::uintptr_t>(command->function);
          char line[320];
          HMODULE module = nullptr;
          char path[MAX_PATH] = {};
          if (handler >= base && handler < base + 0x1D75BC00) {
            std::snprintf(line, sizeof(line), "%s 0x%llX\n", command->name,
                          static_cast<unsigned long long>(handler - base +
                                                          0x140000000ull));
          } else if (GetModuleHandleExA(
                         GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(handler), &module) &&
                     GetModuleFileNameA(module, path, MAX_PATH) != 0) {
            const char *name = std::strrchr(path, '\\');
            std::snprintf(
                line, sizeof(line), "%s %s+0x%llX\n", command->name,
                name ? name + 1 : path,
                static_cast<unsigned long long>(
                    handler - reinterpret_cast<std::uintptr_t>(module)));
          } else {
            std::snprintf(line, sizeof(line), "%s ?\n", command->name);
          }
          out += line;
          ++count;
        }
        const auto file = xerces_runtime::home() / "commands.txt";
        xerces_runtime::write_file(file, out);
        xerces_runtime::log_info("xerces.crash_test",
                                 std::to_string(count) +
                                     " console commands written to " +
                                     file.string());
        return;
      }
      if (kind == "threads") {
        // The game's thread table, the job system's worker records and every
        // thread of the process with where it started: what names the
        // threads in reports, checked against what each thread is. The
        // addresses are the current client's.
        if (!game::is_new_client()) {
          return;
        }
        const std::uintptr_t base = game::get_engine_base();
        const auto at = [base](const std::uintptr_t address) {
          return base + (address - 0x140000000ull);
        };
        // Where the game's image is, for tools that place addresses from
        // outside (tools/dev/re/sample_thread.py --engine-base)
        std::string out = std::format("engine base 0x{:X}\n", base);
        out += "table (IDs 0x15753F660, functions 0x15753F720):\n";
        std::uint64_t ids[xerces_core::engine_thread_contexts]{};
        std::uint64_t functions[xerces_core::engine_thread_contexts]{};
        xerces_safe::read(reinterpret_cast<const void *>(at(0x15753F660)), ids,
                          sizeof(ids));
        xerces_safe::read(reinterpret_cast<const void *>(at(0x15753F720)),
                          functions, sizeof(functions));
        for (std::size_t i = 0; i < std::size(ids); ++i) {
          out += std::format("  {:2} {:>8} {:<14} 0x{:X}\n", i, ids[i],
                             xerces_core::engine_thread_name(i),
                             functions[i] >= base &&
                                     functions[i] < base + 0x1D75BC00
                                 ? functions[i] - base + 0x140000000ull
                                 : functions[i]);
        }
        std::uintptr_t jobs = 0;
        xerces_safe::read(reinterpret_cast<const void *>(at(0x1435F5790)),
                          &jobs, sizeof(jobs));
        out += std::format("job system (0x1435F5790): 0x{:X}\n", jobs);
        for (int i = 0; jobs && i < 12; ++i) {
          std::uint32_t id = 0;
          xerces_safe::read(
              reinterpret_cast<const void *>(jobs + 0x70 + i * 0x68), &id,
              sizeof(id));
          out += std::format("  worker {:2} +0x{:X}: {}\n", i, 0x70 + i * 0x68,
                             id);
        }
        // The processors the job workers were given (0x1422BC380), and the
        // process's own
        std::uint32_t job_mask = 0;
        xerces_safe::read(reinterpret_cast<const void *>(at(0x14325238C)),
                          &job_mask, sizeof(job_mask));
        DWORD_PTR process_mask = 0;
        DWORD_PTR system_mask = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &process_mask,
                               &system_mask);
        out += std::format(
            "job processor mask (0x14325238C): 0x{:X}; process affinity 0x{:X} "
            "of 0x{:X}; {} logical processors\n",
            job_mask, process_mask, system_mask,
            GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        // CPU time from the clock tick overcharges a thread that wakes often
        // for a moment; its cycles do not. The cycle counter's rate, to turn
        // them into time, and when this was taken.
        LARGE_INTEGER frequency{}, before{}, after{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&before);
        const std::uint64_t tsc_before = __rdtsc();
        Sleep(100);
        const std::uint64_t tsc_after = __rdtsc();
        QueryPerformanceCounter(&after);
        const double seconds =
            static_cast<double>(after.QuadPart - before.QuadPart) /
            static_cast<double>(frequency.QuadPart);
        out +=
            std::format("cycle rate {:.0f} Hz; taken at {:.3f} s\n",
                        static_cast<double>(tsc_after - tsc_before) / seconds,
                        static_cast<double>(after.QuadPart) /
                            static_cast<double>(frequency.QuadPart));
        using query_fn = LONG(WINAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        const auto query = reinterpret_cast<query_fn>(GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        out += "threads:\n";
        if (const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            snap != INVALID_HANDLE_VALUE) {
          THREADENTRY32 entry{};
          entry.dwSize = sizeof(entry);
          for (BOOL more = Thread32First(snap, &entry); more;
               more = Thread32Next(snap, &entry)) {
            if (entry.th32OwnerProcessID != GetCurrentProcessId()) {
              continue;
            }
            std::uintptr_t start = 0;
            std::uint64_t cpu_ms = 0;
            std::uint64_t cycles = 0;
            std::string described;
            if (const HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION,
                                                 FALSE, entry.th32ThreadID)) {
              if (query) {
                // ThreadQuerySetWin32StartAddress
                query(thread, 9, &start, sizeof(start), nullptr);
              }
              FILETIME created{}, ended{}, kernel{}, user{};
              if (GetThreadTimes(thread, &created, &ended, &kernel, &user)) {
                const auto ticks = [](const FILETIME &t) {
                  return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) |
                         t.dwLowDateTime;
                };
                cpu_ms = (ticks(kernel) + ticks(user)) / 10000;
              }
              ULONG64 thread_cycles = 0;
              if (QueryThreadCycleTime(thread, &thread_cycles)) {
                cycles = thread_cycles;
              }
              // Through GetProcAddress, as exception.cpp: Windows before 10
              // (1607) has no GetThreadDescription, and an import of it
              // would keep the game from starting there
              using describe_fn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
              static const auto describe = reinterpret_cast<describe_fn>(
                  GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                 "GetThreadDescription"));
              PWSTR description = nullptr;
              if (describe && SUCCEEDED(describe(thread, &description)) &&
                  description) {
                for (const wchar_t *c = description; *c; ++c) {
                  described += *c < 0x80 ? static_cast<char>(*c) : '?';
                }
                LocalFree(description);
              }
              CloseHandle(thread);
            }
            HMODULE module = nullptr;
            char path[MAX_PATH] = {};
            std::string where = std::format("0x{:X}", start);
            if (start >= base && start < base + 0x1D75BC00) {
              where = std::format("BlackOps3.exe 0x{:X}",
                                  start - base + 0x140000000ull);
            } else if (start &&
                       GetModuleHandleExA(
                           GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(start), &module) &&
                       GetModuleFileNameA(module, path, MAX_PATH) != 0) {
              const char *name = std::strrchr(path, '\\');
              where =
                  std::format("{}+0x{:X}", name ? name + 1 : path,
                              start - reinterpret_cast<std::uintptr_t>(module));
            }
            const std::size_t context = xerces_core::engine_thread_context(
                ids, functions, xerces_core::engine_thread_contexts,
                entry.th32ThreadID);
            const std::string_view table_name =
                xerces_core::engine_thread_name(context);
            out += std::format(
                "  {:>6} {:<28} cpu {} ms {} cycles {}{}\n", entry.th32ThreadID,
                described.empty() ? std::string(table_name) : described, cpu_ms,
                cycles, where,
                !table_name.empty() && !described.empty() &&
                        !described.ends_with(table_name)
                    ? std::format(" (table: {})", table_name)
                    : std::string{});
          }
          CloseHandle(snap);
        }
        const auto file = xerces_runtime::home() / "threads.txt";
        xerces_runtime::write_file(file, out);
        xerces_runtime::log_info("xerces.crash_test",
                                 "the game's threads written to " +
                                     file.string());
        return;
      }
      if (kind == "windowhang") {
        // The game's Window thread (the engine's table, slot 21) is held in
        // a message hook: the render backend waits for its Present, the
        // main thread for the backend, and the freeze report must say so
        if (!game::is_new_client()) {
          return;
        }
        const std::uint64_t *ids = game::sys::s_threadContextIds.get();
        const std::uint64_t *functions =
            game::sys::s_threadContextFunctions.get();
        const DWORD window_thread =
            ids && functions && functions[21] ? static_cast<DWORD>(ids[21]) : 0;
        HWND window = nullptr;
        if (window_thread) {
          EnumThreadWindows(window_thread, first_visible_window,
                            reinterpret_cast<LPARAM>(&window));
        }
        if (!window || !SetWindowsHookExW(WH_GETMESSAGE, window_hang_hook,
                                          nullptr, window_thread)) {
          xerces_runtime::log_warn("xerces.crash_test",
                                   "windowhang: the game's Window thread or "
                                   "its window was not found");
          return;
        }
        xerces_runtime::log_warn(
            "xerces.crash_test",
            "The game's Window thread will hang in a message hook "
            "(xerces_crash_test windowhang)");
        PostMessageW(window, window_hang_message, 0, 0);
        return;
      }
      if (kind == "joinrule0") {
        mod_stack::follow_host_map_scripts_first("0");
        xerces_runtime::log_warn("xerces.crash_test",
                                 "the host rule a join leaves, set to 0");
        return;
      }
      if (kind == "workshopget") {
        if (params.size() > 2) {
          xerces_runtime::log_warn(
              "xerces.crash_test",
              std::string("Asking Steam for Workshop item ") + params.get(2) +
                  " (xerces_crash_test workshopget)");
          workshop::start_download(params.get(2), game::ZoneType::MOD);
        }
        return;
      }
      if (kind == "handlerfault" || kind == "dumpfault") {
        xerces_runtime::log_warn(
            "xerces.crash_test",
            "Crashing, and the crash handler will fault itself "
            "(xerces_crash_test " +
                kind + ")");
        handler_fault_at = kind == "handlerfault" ? 1 : 2;
        crash("access");
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
      if (kind == "hitch") {
        const int ms = params.size() > 2
                           ? std::clamp(std::atoi(params.get(2)), 50, 3000)
                           : 300;
        xerces_runtime::log_warn(
            "xerces.crash_test",
            "A frame of " + std::to_string(ms) +
                " ms on purpose (xerces_crash_test hitch)");
        hitch(ms);
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

int handler_fault() { return handler_fault_at.load(); }
} // namespace xerces_crash_test

REGISTER_COMPONENT(xerces_crash_test::component)
