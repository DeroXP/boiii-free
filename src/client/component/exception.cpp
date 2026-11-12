#include <std_include.hpp>

#include <cstdint>

#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include "console.hpp"
#include "error_help.hpp"
#include "exception.hpp"
#include "mod_stack.hpp"
#include "scheduler.hpp"
#include "script.hpp"
#include "script_error.hpp"

#include <errhandlingapi.h>
#include <tlhelp32.h>
#include <utils/compression.hpp>
#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>
#include <utils/thread.hpp>

#include <exception/minidump.hpp>
#include <xerces/db_pool.hpp>
#include <xerces/loader.hpp>
#include <xerces/runtime.hpp>
#include "upnp.hpp"
#include <xerces_core/crash_report.hpp>
#include <xerces_core/journal.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/launcher_home.hpp>
#include <xerces_core/log.hpp>
#include <xerces_core/out_of_memory.hpp>

// In case of clangd compilation
#if __has_include("version.hpp")
#include "version.hpp"
#else
#ifndef VERSION
#define VERSION "0"
#endif
#ifndef SHORTVERSION
#define SHORTVERSION "0"
#endif
#endif
#ifndef XERCES_VERSION_LABEL
#define XERCES_VERSION_LABEL "Xerces"
#endif

#include <CommCtrl.h>
#include <dbghelp.h>
#include <psapi.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

// The image this code is part of (boiii.exe), as the linker names it.
EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace exception {

namespace {
void exception_log(bool err, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list measure;
  va_copy(measure, args);
  std::string buffer(std::max(vsnprintf(nullptr, 0, fmt, measure), 0), '\0');
  va_end(measure);
  vsnprintf(buffer.data(), buffer.size() + 1, fmt, args);
  va_end(args);

  std::FILE *io = err ? stderr : stdout;
  fprintf(io, "%s\n", buffer.c_str());
  fflush(io);

  if (err) {
    console::print("^1" + buffer + "\n");
  }

  game::trace("{}{}", err ? "[Error] " : "", buffer.c_str());
}
static uint32_t main_thread_id{};
static std::once_flag sym_init_flag{};
// Xerces: dbghelp serves one thread at a time, and since 2026-11-12 two may
// ask it: a crash's handler and the freeze report's thread. Whoever does not
// get it within two seconds goes on without names rather than wait on a
// thread that may never come back.
static std::recursive_timed_mutex symbols_mutex{};

void ensure_symbols_initialized() {
  std::call_once(sym_init_flag, [] {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  });
}

struct resolved_frame {
  std::string module_name;
  std::string function_name;
  std::string file_name;
  uint64_t address = 0;
  uint64_t module_base = 0;
  uint64_t rva = 0;
  uint32_t line_number = 0;
};

// The function's name and its source line, where symbols know them.
void add_symbols(resolved_frame &frame) {
  std::unique_lock lock(symbols_mutex, std::defer_lock);
  if (!lock.try_lock_for(2s)) {
    return;
  }
  ensure_symbols_initialized();

  // Try to resolve function name from PDB symbols
  alignas(SYMBOL_INFO) char sym_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
  SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(sym_buffer);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = MAX_SYM_NAME;

  uint64_t displacement = 0;
  if (SymFromAddr(GetCurrentProcess(), frame.address, &displacement, sym)) {
    frame.function_name = sym->Name;
    if (displacement > 0)
      frame.function_name += utils::string::va("+0x%llX", displacement);
  }

  // Try to resolve source file and line
  IMAGEHLP_LINE64 line_info{};
  line_info.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
  unsigned long line_displacement = 0;
  if (SymGetLineFromAddr64(GetCurrentProcess(), frame.address,
                           &line_displacement, &line_info)) {
    frame.file_name = line_info.FileName;
    frame.line_number = line_info.LineNumber;
  }
}

resolved_frame resolve_address(void *addr) {
  resolved_frame frame{};
  frame.address = reinterpret_cast<uint64_t>(addr);

  const utils::nt::library mod = utils::nt::library::get_by_address(addr);
  if (mod) {
    frame.module_name = mod.get_name();
    frame.module_base = reinterpret_cast<uint64_t>(mod.get_ptr());
    frame.rva = frame.address - frame.module_base;

    if (frame.module_name == "BlackOps3.exe")
      frame.rva += 0x140000000;
  } else {
    frame.module_name = "unknown";
    frame.rva = frame.address;
  }

  add_symbols(frame);
  return frame;
}

std::string format_frame(const resolved_frame &f, size_t index) {
  std::string entry = utils::string::va("\t[%zu] %s + 0x%llX", index,
                                        f.module_name.c_str(), f.rva);

  if (!f.function_name.empty())
    entry += utils::string::va("  (%s)", f.function_name.c_str());

  if (!f.file_name.empty() && f.line_number > 0)
    entry +=
        utils::string::va("  [%s:%lu]", f.file_name.c_str(), f.line_number);

  return entry;
}

std::vector<resolved_frame>
capture_stackwalk(const LPEXCEPTION_POINTERS exceptioninfo,
                  int32_t max_frames = 48) {
  std::vector<resolved_frame> frames;

  if (!exceptioninfo || !exceptioninfo->ContextRecord)
    return frames;

  std::unique_lock lock(symbols_mutex, std::defer_lock);
  if (!lock.try_lock_for(2s)) {
    return frames;
  }
  ensure_symbols_initialized();

  CONTEXT ctx = *exceptioninfo->ContextRecord;

  STACKFRAME64 stack_frame{};
  stack_frame.AddrPC.Offset = ctx.Rip;
  stack_frame.AddrPC.Mode = AddrModeFlat;
  stack_frame.AddrFrame.Offset = ctx.Rbp;
  stack_frame.AddrFrame.Mode = AddrModeFlat;
  stack_frame.AddrStack.Offset = ctx.Rsp;
  stack_frame.AddrStack.Mode = AddrModeFlat;

  const HANDLE process = GetCurrentProcess();
  const HANDLE thread = GetCurrentThread();

  for (int32_t i = 0; i < max_frames; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &stack_frame,
                     &ctx, nullptr, SymFunctionTableAccess64,
                     SymGetModuleBase64, nullptr))
      break;

    if (stack_frame.AddrPC.Offset == 0)
      break;

    frames.push_back(
        resolve_address(reinterpret_cast<void *>(stack_frame.AddrPC.Offset)));
  }

  return frames;
}

std::string get_crash_module_info(void *address) {
  const resolved_frame frame = resolve_address(address);
  return std::format("{}+0x{:X}{}", frame.module_name, frame.rva,
                     frame.function_name.empty()
                         ? ""
                         : std::format(" ({})", frame.function_name.c_str()));
}

utils::hook::detour mini_dump_write_dump_hook;

// Xerces: MiniDumpWriteDump stops every other thread of this process while
// it works, the freeze watchdog among them, and then needs the heap and the
// loader's lock. If a stopped thread holds one of those the dump never comes
// back, and with the watchdog stopped nothing would close the game. A thread
// that is left out of a dump is left running, so every dump leaves the
// watchdog out: it sleeps in a loop and tells nothing. dump_watchdog_stops
// is how often the watchdog was stopped when the last dump wrote its first
// thread (0: it kept running), for the freeze report to say.
std::atomic<DWORD> watchdog_thread_id{0};
std::atomic<int> dump_watchdog_stops{-1};

BOOL CALLBACK dump_callback(PVOID, const PMINIDUMP_CALLBACK_INPUT input,
                            PMINIDUMP_CALLBACK_OUTPUT output) {
  if (!input || !output) {
    return FALSE;
  }
  // The answers dbghelp's own documentation gives for a callback that
  // changes nothing; any other kind of call gets FALSE, "not handled".
  switch (input->CallbackType) {
  case IncludeThreadCallback:
    return input->IncludeThread.ThreadId != watchdog_thread_id.load();
  case ThreadCallback:
  case ThreadExCallback:
    if (dump_watchdog_stops.load() < 0) {
      int stops = 0;
      if (const HANDLE watchdog = OpenThread(THREAD_SUSPEND_RESUME, FALSE,
                                             watchdog_thread_id.load())) {
        const DWORD before = SuspendThread(watchdog);
        if (before != static_cast<DWORD>(-1)) {
          ResumeThread(watchdog);
          stops = static_cast<int>(before);
        }
        CloseHandle(watchdog);
      }
      dump_watchdog_stops = stops;
    }
    return TRUE;
  case IncludeModuleCallback:
  case ModuleCallback:
    return TRUE;
  case IncludeVmRegionCallback:
    // every region, and go on to the next
    output->Continue = TRUE;
    return TRUE;
  default:
    return FALSE;
  }
}

qboolean WINAPI mini_dump_write_dump_stub(
    const HANDLE h_process, const uint32_t process_id, const HANDLE h_file,
    const MINIDUMP_TYPE dump_type,
    const PMINIDUMP_EXCEPTION_INFORMATION exception_param,
    const PMINIDUMP_USER_STREAM_INFORMATION user_stream_param,
    const PMINIDUMP_CALLBACK_INFORMATION callback_param) {
  wchar_t filename[MAX_PATH];
  if (GetFinalPathNameByHandleW(h_file, filename, std::size(filename),
                                VOLUME_NAME_DOS)) {
    std::wstring path = filename;
    if (path.find(L"\\\\?\\") == 0) {
      path = path.substr(4);
    }

    // Xerces: a dump that brings no callback of its own gets the one that
    // leaves the watchdog running.
    MINIDUMP_CALLBACK_INFORMATION own_callback{dump_callback, nullptr};
    const PMINIDUMP_CALLBACK_INFORMATION callback =
        callback_param ? callback_param : &own_callback;
    dump_watchdog_stops = -1;
    const std::filesystem::path p(path);
    const std::filesystem::path minidumps_path =
        game::get_appdata_path() / "minidumps";
    std::error_code error;
    if (p.extension() == L".dmp" &&
        !std::filesystem::equivalent(p.parent_path(), minidumps_path, error)) {
      std::filesystem::create_directories(minidumps_path);

      const std::filesystem::path new_path = minidumps_path / p.filename();
      const HANDLE new_handle =
          CreateFileW(new_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

      if (new_handle != INVALID_HANDLE_VALUE) {
        const qboolean result = mini_dump_write_dump_hook.invoke<qboolean>(
            h_process, process_id, new_handle, dump_type, exception_param,
            user_stream_param, callback);
        CloseHandle(new_handle);
        return result;
      }
    }
    return mini_dump_write_dump_hook.invoke<qboolean>(
        h_process, process_id, h_file, dump_type, exception_param,
        user_stream_param, callback);
  }

  return mini_dump_write_dump_hook.invoke<qboolean>(
      h_process, process_id, h_file, dump_type, exception_param,
      user_stream_param, callback_param);
}

thread_local struct {
  uint32_t code = 0;
  void *address = nullptr;
  uintptr_t access = 0;
  uintptr_t target = 0;
  void *caller = nullptr;
  std::string culprit;
  std::string scripts;
} exception_data{};

bool is_game_thread();

// Runs fn; false when it faulted. For reads of game state by a report: after
// a crash, or from the freeze watchdog's side, that state may be half
// written. What fn had built stays as it was (no destructors run), which is
// a leak in a process that is about to end.
bool run_guarded_raw(void (*fn)(void *), void *arg) {
  __try {
    fn(arg);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

template <typename F> bool run_guarded(F &&fn) {
  return run_guarded_raw(
      [](void *arg) { (*static_cast<std::remove_reference_t<F> *>(arg))(); },
      &fn);
}

constexpr std::array<uint32_t, game::scr::SCRIPTINSTANCE_MAX>
    SCRIPT_VARIABLE_COUNT{game::scr::var::SCRIPTVARIABLE_POOL_SIZE.server,
                          0xFDE8};

struct script_state {
  const char *running = nullptr;
  bool executing = false;
  bool stack_corrupt = false;
  bool frames_corrupt = false;
  bool variables_corrupt = false;
};

bool read_script_state(const game::scr::scriptInstance_t inst,
                       script_state &state) {
  __try {
    volatile auto &vm = game::scr::vm::gScrVmPub->instance[inst];
    auto *stack = const_cast<game::scr::var::ScrVarValue_t *>(vm.stack);
    auto *frames =
        const_cast<game::scr::vm::function_frame_t *>(vm.function_frame_start);
    state.executing = vm.function_count > 0;
    state.stack_corrupt =
        vm.top < stack || vm.top >= stack + std::size(vm.stack);
    state.frames_corrupt =
        vm.function_frame < frames ||
        vm.function_frame >= frames + std::size(vm.function_frame_start);

    const volatile game::scr::var::ScrVar_t *variables =
        game::scr::vm::gScrVarGlob->instance[inst].scriptVariables;
    for (uint32_t i = 0; variables && i < SCRIPT_VARIABLE_COUNT[inst]; ++i) {
      if (static_cast<uint32_t>(variables[i].value.type) >=
          static_cast<uint32_t>(game::scr::var::ScrVarType::COUNT)) {
        state.variables_corrupt = true;
        break;
      }
    }

    const uint8_t *pos = game::scr::vm::gFs->instance[inst].pos;
    for (uint32_t i = 0;
         state.executing && i < game::scr::gObjFileInfoCount->instance[inst];
         ++i) {
      const game::scr::GSC_OBJ *obj =
          game::scr::gObjFileInfo->instance[inst][i].activeVersion;
      const uint8_t *code =
          reinterpret_cast<const uint8_t *>(obj) + obj->cseg_offset;
      if (pos >= code && pos < code + obj->cseg_size) {
        state.running = obj->get_name();
        break;
      }
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

const char *thread_role() {
  if (is_game_thread()) {
    return "main";
  }
  if (game::sys::Sys_IsServerThread.get() && game::sys::Sys_IsServerThread()) {
    return "server";
  }
  return "auxiliary";
}

std::string describe_scripts() {
  const std::string_view role = thread_role();
  const bool server_thread =
      role == "server" || (game::is_server() && role == "main");

  std::string report;
  for (const game::scr::scriptInstance_t inst :
       {game::scr::SCRIPTINSTANCE_SERVER, game::scr::SCRIPTINSTANCE_CLIENT}) {
    script_state state{};
    // Through run_guarded: with clang a __try block catches a fault inside a
    // function it calls, not one its own instructions raise, and the reads
    // in read_script_state are its own.
    bool read = false;
    if (!run_guarded(
            [&read, &state, inst] { read = read_script_state(inst, state); }) ||
        !read) {
      continue;
    }

    const bool client = inst == game::scr::SCRIPTINSTANCE_CLIENT;
    const char *name = client ? "CSC" : "GSC";
    const bool crashing_thread =
        client ? role == "main" && !game::is_server() : server_thread;
    if (crashing_thread && state.executing) {
      report += std::format(
          "\nThe {} script{} was running when the game crashed, so a script "
          "(often from a mod or custom map) may have triggered it.",
          name,
          state.running ? std::format(" '{}'", state.running) : std::string{});
    }
    if (state.variables_corrupt || state.stack_corrupt ||
        state.frames_corrupt) {
      report += std::format("\nThe {} script {} corrupted.", name,
                            state.variables_corrupt ? "variables were"
                            : state.stack_corrupt   ? "VM stack was"
                                                    : "VM call frames were");
    }
  }
  return report;
}

bool is_game_thread() { return main_thread_id == GetCurrentThreadId(); }

// Xerces: the name of the text report the crash handler wrote (crash-<time>
// .txt in the minidumps folder), for the dialog.
std::string crash_report_file;

// Xerces: a time limit on the crash handler. It reads the game's state on a
// thread that just crashed, and after a crash the freeze watchdog stands
// down, so a handler stuck on a lock the crashed code held would leave a
// game that neither plays nor closes. While this is not 0 the handler (or the
// fatal error's report) is at work since that tick; the watchdog ends the
// game with crash_handler_code when it has been at it for
// CRASH_HANDLER_LIMIT_MS. The dump is written first, so it is there by then.
// The dialog clears it: that one waits for the player.
constexpr uint64_t CRASH_HANDLER_LIMIT_MS = 120'000;
std::atomic<uint64_t> crash_handler_started_ms{0};
std::atomic<uint32_t> crash_handler_code{0};

void show_mouse_cursor() {
  while (ShowCursor(TRUE) < 0)
    ;
}

const char *get_exception_string(uint32_t exception);

std::string describe_crash() {
  if (exception_data.code == EXCEPTION_ACCESS_VIOLATION) {
    if (exception_data.caller) {
      return std::format("called an invalid function pointer 0x{:X}{} from {}",
                         exception_data.target,
                         exception_data.target < 0x10000 ? " (null pointer)"
                                                         : "",
                         get_crash_module_info(exception_data.caller));
    }
    return std::format("invalid memory access: tried to {} 0x{:X}{} in {}",
                       exception_data.access == 1 ? "write to" : "read",
                       exception_data.target,
                       exception_data.target < 0x10000 ? " (null pointer)" : "",
                       get_crash_module_info(exception_data.address));
  }
  return std::format("{} in {}", get_exception_string(exception_data.code),
                     get_crash_module_info(exception_data.address));
}

std::string crash_reason() {
  std::string reason = std::format("The game crashed: {}.{}", describe_crash(),
                                   exception_data.scripts);
  if (!exception_data.culprit.empty()) {
    reason += "\n\n" + exception_data.culprit;
  } else if (exception_data.scripts.empty()) {
    reason += "\n\nThe cause could not be determined from the crash itself. "
              "If it keeps happening: verify the game files in Steam, update "
              "your graphics driver, disable overlays (Steam, Discord, MSI "
              "Afterburner/RTSS) and remove CPU/GPU/RAM overclocks.";
  }
  return error_help::strip_colors(reason);
}

bool ask_restart(const std::string &text,
                 const wchar_t *headline = L"The game crashed.") {
  constexpr int32_t RESTART_BUTTON = 100;
  constexpr int32_t CLOSE_BUTTON = 101;
  const TASKDIALOG_BUTTON buttons[] = {
      {RESTART_BUTTON, L"Restart game\nClose the game and start it again."},
      {CLOSE_BUTTON, L"Close game"},
  };
  constexpr std::string_view repeated_headline = "The game crashed: ";
  std::string body = text;
  if (body.starts_with(repeated_headline)) {
    body.erase(0, repeated_headline.size());
    body[0] = static_cast<char>(std::toupper(body[0]));
  }
  const std::wstring content = utils::string::convert(body);

  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_SIZE_TO_CONTENT;
  config.pszWindowTitle = L"Xerces";
  config.pszMainIcon = TD_ERROR_ICON;
  config.pszMainInstruction = headline;
  config.pszContent = content.c_str();
  config.pButtons = buttons;
  config.cButtons = static_cast<UINT>(std::size(buttons));
  config.nDefaultButton = RESTART_BUTTON;

  int32_t pressed = CLOSE_BUTTON;
  return SUCCEEDED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr)) &&
         pressed == RESTART_BUTTON;
}

// Xerces: whether "Send crash reports" is on (launcher.json, read when the
// crash happens, so a change in the open launcher counts). Off when the file
// cannot be read: the person may have turned it off in the file that broke.
bool crash_reports_on() {
  return xerces_runtime::started_by_launcher() &&
         xerces_core::crash_reports_enabled(xerces_runtime::read_file(
             xerces_runtime::home() / "config" / "launcher.json"));
}

// Xerces: what happens to this crash's report (docs/CRASH_REPORTS.md), for
// the dialog: the launcher sends it when the game closes or when Xerces
// opens next or, with the setting off, how to report it by hand.
std::string crash_report_note(bool dump = true) {
  const bool by_launcher = xerces_runtime::started_by_launcher();
  return xerces_core::crash_report_note(
      by_launcher, by_launcher && crash_reports_on(),
      by_launcher && xerces_core::launcher_running(xerces_runtime::home()),
      dump);
}

void display_error_dialog() {
  const std::string minidumps_out =
      (game::get_appdata_path() / "minidumps").string();
  const std::string error_str = std::format(
      "{}\n\nDetails: {} (0x{:08X}) at {:p} on the {} thread.\n\n"
      "Crash report{} and dump saved in:\n{}\n\n{}",
      crash_reason(), get_exception_string(exception_data.code),
      exception_data.code, game::derelocate(exception_data.address),
      thread_role(),
      crash_report_file.empty() ? std::string{}
                                : std::format(" ({})", crash_report_file),
      minidumps_out, crash_report_note());

  crash_handler_started_ms = 0;
  utils::thread::suspend_other_threads();
  show_mouse_cursor();

  if (game::quiet_crash() || game::is_headless()) {
    game::show_error(error_str, "Xerces");
    utils::thread::terminate_other_threads(exception_data.code);
  } else if (ask_restart(error_str)) {
    restart_game();
  } else {
    ShellExecuteA(nullptr, "open", minidumps_out.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
  }

  TerminateProcess(GetCurrentProcess(), exception_data.code);
}

void reset_state() {
  if (game::is_server()) {
    if (!is_game_thread()) {
      display_error_dialog();
    }

    game::com::Com_Printf(
        game::consoleChannel_e::CHANNEL_DONT_FILTER,
        game::consoleLabel_e::DEFAULT, "Server crash: %s (0x%08X) at %s\n",
        get_exception_string(exception_data.code), exception_data.code,
        get_crash_module_info(exception_data.address).c_str());
    script_error::notify_players("The server crashed.");
    game::com::Com_Error(game::errorParm::DROP, "Server crashed");
  } else {

    display_error_dialog();
  }
}

size_t get_reset_state_stub() {
  static void *stub = utils::hook::assemble([](utils::hook::assembler &a) {
    a.get().sub(rsp, 0x10);
    a.get().or_(rsp, 0x8);
    a.jmp(reset_state);
  });

  return reinterpret_cast<size_t>(stub);
}

std::string get_timestamp() {
  tm ltime{};
  char timestamp[MAX_PATH] = {0};
  const __time64_t time = _time64(nullptr);

  _localtime64_s(&ltime, &time);
  strftime(timestamp, sizeof(timestamp) - 1, "%Y-%m-%d-%H-%M-%S", &ltime);

  return timestamp;
}

const char *get_exception_string(const uint32_t exception) {
#define EXCEPTION_CASE(code)                                                   \
  case EXCEPTION_##code:                                                       \
    return "EXCEPTION_" #code
  switch (exception) {
    EXCEPTION_CASE(ACCESS_VIOLATION);
    EXCEPTION_CASE(DATATYPE_MISALIGNMENT);
    EXCEPTION_CASE(BREAKPOINT);
    EXCEPTION_CASE(SINGLE_STEP);
    EXCEPTION_CASE(ARRAY_BOUNDS_EXCEEDED);
    EXCEPTION_CASE(FLT_DENORMAL_OPERAND);
    EXCEPTION_CASE(FLT_DIVIDE_BY_ZERO);
    EXCEPTION_CASE(FLT_INEXACT_RESULT);
    EXCEPTION_CASE(FLT_INVALID_OPERATION);
    EXCEPTION_CASE(FLT_OVERFLOW);
    EXCEPTION_CASE(FLT_STACK_CHECK);
    EXCEPTION_CASE(FLT_UNDERFLOW);
    EXCEPTION_CASE(INT_DIVIDE_BY_ZERO);
    EXCEPTION_CASE(INT_OVERFLOW);
    EXCEPTION_CASE(PRIV_INSTRUCTION);
    EXCEPTION_CASE(IN_PAGE_ERROR);
    EXCEPTION_CASE(ILLEGAL_INSTRUCTION);
    EXCEPTION_CASE(NONCONTINUABLE_EXCEPTION);
    EXCEPTION_CASE(STACK_OVERFLOW);
    EXCEPTION_CASE(INVALID_DISPOSITION);
    EXCEPTION_CASE(GUARD_PAGE);
    EXCEPTION_CASE(INVALID_HANDLE);
  default:
    return "UNKNOWN";
  }
#undef EXCEPTION_CASE
}

std::string get_memory_registers(const LPEXCEPTION_POINTERS exceptioninfo) {
  if (!exceptioninfo || !exceptioninfo->ContextRecord) {
    return {};
  }

  const PCONTEXT ctx = exceptioninfo->ContextRecord;
  std::string info{"registers:\r\n{\r\n"};

  const std::function<void(const char *key, const uint64_t value)> reg =
      [&info](const char *key, const uint64_t value) {
        info.append(utils::string::va("\t%s = 0x%llX\r\n", key, value));
      };

  reg("rax", ctx->Rax);
  reg("rbx", ctx->Rbx);
  reg("rcx", ctx->Rcx);
  reg("rdx", ctx->Rdx);
  reg("rsp", ctx->Rsp);
  reg("rbp", ctx->Rbp);
  reg("rsi", ctx->Rsi);
  reg("rdi", ctx->Rdi);
  reg("r8", ctx->R8);
  reg("r9", ctx->R9);
  reg("r10", ctx->R10);
  reg("r11", ctx->R11);
  reg("r12", ctx->R12);
  reg("r13", ctx->R13);
  reg("r14", ctx->R14);
  reg("r15", ctx->R15);
  reg("rip", ctx->Rip);

  info.append("}");
  return info;
}

std::string get_callstack_summary(const LPEXCEPTION_POINTERS exceptioninfo,
                                  int32_t trace_depth = 48) {
  std::string info{"callstack:\r\n{\r\n"};

  std::vector<resolved_frame> frames =
      capture_stackwalk(exceptioninfo, trace_depth);

  if (frames.empty()) {
    // Fallback to RtlCaptureStackBackTrace if StackWalk64 fails
    void *backtrace_stack[32]{};
    const uint16_t count =
        RtlCaptureStackBackTrace(0, 32, backtrace_stack, nullptr);
    for (uint16_t i = 0; i < count; ++i) {
      resolved_frame f = resolve_address(backtrace_stack[i]);
      info.append(format_frame(f, i));
      info.append("\r\n");
    }
  } else {
    for (size_t i = 0; i < frames.size(); ++i) {
      info.append(format_frame(frames[i], i));
      info.append("\r\n");
    }
  }

  info.append("}");
  return info;
}

// ------------------------------------------------------------------------
// Xerces: the text report (docs/CRASH_REPORTS.md, "The text report"). A
// crash, a freeze and a fatal error each leave one in the minidumps folder,
// next to the dump, and the same text without the console's lines goes into
// the crash report the launcher sends. It opens with what happened and what
// the game was doing (version, how long it ran, the map, the mods in load
// order, the scripts running), so most reports can be read without opening
// the dump.

std::atomic<uint32_t> frame_count{0};
std::atomic<DWORD> frame_thread_id{0};
// The threads that run the server's frames and the renderer's, noted by each
// as it runs (post_unpack): a report names threads by what they do.
std::atomic<DWORD> server_thread_id{0};
std::atomic<DWORD> render_thread_id{0};
const std::chrono::steady_clock::time_point process_start =
    std::chrono::steady_clock::now();

std::string duration_text(const std::chrono::steady_clock::duration time) {
  const long long total =
      std::chrono::duration_cast<std::chrono::seconds>(time).count();
  if (total >= 3600) {
    return std::format("{} h {:02} min", total / 3600, total % 3600 / 60);
  }
  if (total >= 60) {
    return std::format("{} min {:02} s", total / 60, total % 60);
  }
  return std::format("{} s", total);
}

// When this client was built (its PE header's time stamp): it tells apart
// two builds that carry the same version label, as a developer's do.
std::string build_time() {
  const auto *base = reinterpret_cast<const uint8_t *>(&__ImageBase);
  const auto *nt =
      reinterpret_cast<const IMAGE_NT_HEADERS *>(base + __ImageBase.e_lfanew);
  const __time64_t stamp = nt->FileHeader.TimeDateStamp;
  tm utc{};
  char text[32]{};
  if (_gmtime64_s(&utc, &stamp) != 0 || utc.tm_year < 120 ||
      utc.tm_year > 200 ||
      strftime(text, sizeof(text), "%Y-%m-%d %H:%M UTC", &utc) == 0) {
    return {};
  }
  return text;
}

std::string os_version() {
  RTL_OSVERSIONINFOW version_info{};
  version_info.dwOSVersionInfoSize = sizeof(version_info);

  // Clang/GCC warnings with -Weverything
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored                                               \
    "-Wcast-function-type" // warning: cast between incompatible function types
                           // (for loader)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored                                                 \
    "-Wpragmas" // warning: unknown option after '#pragma GCC diagnostic' kind
#pragma GCC diagnostic ignored                                                 \
    "-Wcast-function-type" // warning: cast between incompatible function types
                           // (for loader)
#endif
  typedef fastcallPtr_t<NTSTATUS(PRTL_OSVERSIONINFOW)> RtlGetVersionFunc;
  const RtlGetVersionFunc rtl_get_version = reinterpret_cast<RtlGetVersionFunc>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

  if (rtl_get_version && NT_SUCCESS(rtl_get_version(&version_info))) {
    return utils::string::va("%u.%u.%u", version_info.dwMajorVersion,
                             version_info.dwMinorVersion,
                             version_info.dwBuildNumber);
  }
  return "unavailable";
}

// How much memory is left: the PC's, and what the game itself holds. A game
// that ran out, or nearly, shows here without a debugger.
std::string memory_text() {
  std::string text;
  const auto gib = [](const uint64_t bytes) {
    return std::format("{:.1f}",
                       static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
  };
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status)) {
    text += std::format("Memory: RAM {} of {} GB free ({} % in use), commit {} "
                        "of {} GB free\r\n",
                        gib(status.ullAvailPhys), gib(status.ullTotalPhys),
                        status.dwMemoryLoad, gib(status.ullAvailPageFile),
                        gib(status.ullTotalPageFile));
  }
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(
          GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
          sizeof(counters))) {
    text += std::format(
        "Game memory: {} GB in RAM (most so far {}), {} GB "
        "committed (most so far {})\r\n",
        gib(counters.WorkingSetSize), gib(counters.PeakWorkingSetSize),
        gib(counters.PrivateUsage), gib(counters.PeakPagefileUsage));
  }
  std::string pools;
  run_guarded([&pools] { pools = xerces_db_pool::pools_json(); });
  if (pools.size() > 2) {
    text += "Zone pools: " + pools + "\r\n";
  }
  return text;
}

// The settings that change what the game does, as it had them. The game
// answers for its dvars on its main thread only (a freeze report's thread got
// nothing, 2026-11-12), so the main thread notes them every few seconds and a
// report from another thread uses what was noted.
std::mutex settings_mutex;
std::string settings_seen;

std::string read_settings() {
  static constexpr std::array names{
      "xerces_upscaler",
      "xerces_upscalerSharpness",
      "xerces_zm_aimAssist",
      "xerces_zm_noHipSpread",
      "xerces_zm_roundHud",
      "xerces_zm_selfRevives",
      "xerces_zm_roundSkips",
      "xerces_zm_round999",
      "xerces_zm_smartZombies",
      "xerces_zm_randomLoadout",
      "xerces_zm_startRound",
      "xerces_zm_startPoints",
      "xerces_zm_wheel",
      "xerces_zm_coinFlip",
      "xerces_char_saved",
      "com_maxfps",
      "r_vsync",
      "r_fullscreen",
      "r_mode",
      "cg_fov_default",
      "cg_fovScale",
      "com_maxclients",
      "sv_running",
      "cl_paused",
  };
  std::string text;
  run_guarded([&text] {
    for (const char *name : names) {
      if (const auto dvar = game::get_dvar(name); dvar.nonnull()) {
        if (const char *value = game::Dvar_DisplayableValue(dvar)) {
          text += std::format("{}{}={}", text.empty() ? "" : ", ", name,
                              xerces_core::json::valid_utf8(value, 80));
        }
      }
    }
  });
  return text.empty() ? std::string{} : "Settings: " + text + "\r\n";
}

void note_settings() {
  std::string text = read_settings();
  const std::lock_guard lock(settings_mutex);
  settings_seen = std::move(text);
}

std::string settings_text() {
  if (is_game_thread()) {
    return read_settings();
  }
  const std::lock_guard lock(settings_mutex);
  return settings_seen;
}

// The scripts one VM is running, innermost first, as "function (file line
// 12)": not "file:12", which the scrubbing of a report that is sent reads as
// a host and a port. Empty when it runs none or its state cannot be read.
std::vector<std::string> script_stack(const game::scr::scriptInstance_t inst) {
  std::vector<std::string> lines;
  run_guarded([&lines, inst] {
    const auto depth = game::scr::vm::gScrVmPub->instance[inst].function_count;
    // more frames than the VM has room for: its state is not to be walked
    if (depth <= 0 || static_cast<std::size_t>(depth) >
                          std::size(game::scr::vm::gScrVmPub->instance[inst]
                                        .function_frame_start)) {
      return;
    }
    for (const script::script_frame &frame : script::get_script_frames(inst)) {
      if (frame.file.empty() || lines.size() >= 24) {
        continue;
      }
      const std::string where =
          frame.line < 0 ? frame.file
                         : std::format("{} line {}", frame.file, frame.line);
      lines.push_back(xerces_core::json::valid_utf8(
          frame.function.empty()
              ? where
              : std::format("{} ({})", frame.function, where),
          300));
    }
  });
  return lines;
}

const char *script_vm_name(const game::scr::scriptInstance_t inst) {
  return inst == game::scr::SCRIPTINSTANCE_CLIENT ? "CSC" : "GSC";
}

// Both VMs' running scripts, for the report's body.
std::string scripts_text() {
  std::string text;
  for (const game::scr::scriptInstance_t inst :
       {game::scr::SCRIPTINSTANCE_SERVER, game::scr::SCRIPTINSTANCE_CLIENT}) {
    const std::vector<std::string> lines = script_stack(inst);
    if (lines.empty()) {
      continue;
    }
    text += std::format("{} scripts running (innermost first):\r\n",
                        script_vm_name(inst));
    for (const std::string &line : lines) {
      text += "\t" + line + "\r\n";
    }
    text += "\r\n";
  }
  return text;
}

// One line naming the script that runs: the server's when it runs one (a
// freeze or a crash is far more often a GSC script's), else the client's.
std::string running_script() {
  for (const game::scr::scriptInstance_t inst :
       {game::scr::SCRIPTINSTANCE_SERVER, game::scr::SCRIPTINSTANCE_CLIENT}) {
    const std::vector<std::string> lines = script_stack(inst);
    if (!lines.empty()) {
      return std::format("{} {}", script_vm_name(inst), lines.front());
    }
  }
  return {};
}

// What the game was doing: these lines open every report.
std::string session_text() {
  std::string text;
  const auto line = [&text](const std::string_view key,
                            const std::string &value) {
    if (!value.empty()) {
      text += std::format("{}: {}\r\n", key, value);
    }
  };
  line("Version", XERCES_VERSION_LABEL);
  line("Built", build_time());
  line("Time", get_timestamp() + " (local)");
  line("Windows", os_version());
  line("Running for",
       duration_text(std::chrono::steady_clock::now() - process_start));
  line("Phase", xerces_runtime::journal_phase_for_crash());

  std::string mode, map, mods;
  run_guarded([&mode, &map] {
    using game::eModes;
    mode = game::com::Com_SessionMode_IsMode(eModes::ZOMBIES)       ? "zm"
           : game::com::Com_SessionMode_IsMode(eModes::MULTIPLAYER) ? "mp"
           : game::com::Com_SessionMode_IsMode(eModes::CAMPAIGN)    ? "cp"
                                                                    : "";
    map = xerces_core::out_of_memory_map(mod_stack::active_usermap(),
                                         xerces_loader::current_map());
  });
  run_guarded([&mods] {
    // The main mod first, then the others as they are stacked
    std::vector<std::string> ids = mod_stack::get();
    if (const std::string main_mod = mod_stack::main_id();
        !main_mod.empty() && std::ranges::find(ids, main_mod) == ids.end()) {
      ids.insert(ids.begin(), main_mod);
    }
    for (const std::string &id : ids) {
      const std::string title =
          error_help::strip_colors(mod_stack::workshop_title(id));
      mods += std::format("{}{}{}", mods.empty() ? "" : "; ", id,
                          title.empty() ? std::string{}
                                        : std::format(" ({})", title));
    }
  });
  line("Mode", mode);
  line("Map", map.empty() ? std::string{"(none: the menus)"} : map);
  line("Mods in load order", mods.empty() ? std::string{"(none)"} : mods);
  std::string flags;
  for (const std::string &flag :
       xerces_core::launch_flag_names(xerces_runtime::command_line_args())) {
    flags += (flags.empty() ? "" : " ") + flag;
  }
  line("Flags", flags);
  // The renderer's work runs on the main thread in most of the game; it is
  // named only when it has a thread of its own.
  line("Threads",
       std::format("main {}, server {}{} (0: not seen yet)",
                   frame_thread_id.load(), server_thread_id.load(),
                   render_thread_id.load() != frame_thread_id.load()
                       ? std::format(", renderer {}", render_thread_id.load())
                       : std::string{}));
  return xerces_core::json::valid_utf8(text, 8000);
}

// The console's last lines, for the report on this PC. The report that is
// sent has them already (its console tail), so they are kept apart.
constexpr std::size_t LOCAL_CONSOLE_LINES = 80;
std::string console_text() {
  const std::vector<std::string> lines =
      xerces_runtime::console_last_lines(LOCAL_CONSOLE_LINES);
  if (lines.empty()) {
    return {};
  }
  std::string text =
      std::format("\r\nConsole (its last {} lines):\r\n", lines.size());
  for (const std::string &line : lines) {
    text += "\t" + error_help::strip_colors(line) + "\r\n";
  }
  return text;
}

// The bytes of the instruction that faulted and the ones after it: enough
// to tell which instruction it was when the address is in no module (a
// hook's stub, memory that was freed).
std::string code_bytes(const void *address) {
  uint8_t bytes[16]{};
  if (!address || !run_guarded([&bytes, address] {
        std::memcpy(bytes, address, sizeof(bytes));
      })) {
    return "(not readable)";
  }
  std::string text;
  for (const uint8_t byte : bytes) {
    text += std::format("{:02X} ", byte);
  }
  return text;
}

bool write_report_file(const std::filesystem::path &file,
                       const std::string &text) {
  return utils::io::write_file(file, text, false);
}

std::string generate_crash_info(const LPEXCEPTION_POINTERS exceptioninfo) {
  std::string info{};
  const std::function<void(const std::string &text)> line =
      [&info](const std::string &text) {
        info.append(text);
        info.append("\r\n");
      };

  const resolved_frame crash_frame =
      resolve_address(exceptioninfo->ExceptionRecord->ExceptionAddress);

  line("Xerces Crash Report");
  line(std::string{});
  line(std::format("What happened: {} on the {} thread.", describe_crash(),
                   thread_role()));
  if (!exception_data.culprit.empty())
    line("Diagnosis: " + error_help::strip_colors(exception_data.culprit));
  if (!exception_data.scripts.empty())
    line("Scripts:" + exception_data.scripts);
  line(std::string{});
  info += session_text();
  line(std::string{});
  line(utils::string::va(
      "Exception: 0x%08X (%s)", exceptioninfo->ExceptionRecord->ExceptionCode,
      get_exception_string(exceptioninfo->ExceptionRecord->ExceptionCode)));
  line(utils::string::va("Address: 0x%llX", crash_frame.address));
  line(utils::string::va("Module: %s + 0x%llX", crash_frame.module_name.c_str(),
                         crash_frame.rva));
  if (!crash_frame.function_name.empty())
    line("Function: " + crash_frame.function_name);
  if (!crash_frame.file_name.empty() && crash_frame.line_number > 0)
    line(utils::string::va("Source: %s:%lu", crash_frame.file_name.c_str(),
                           crash_frame.line_number));
  line(utils::string::va("Base: 0x%llX", game::get_base()));
  line(utils::string::va("Thread ID: %lu (%s)", GetCurrentThreadId(),
                         thread_role()));

  if (exceptioninfo->ExceptionRecord->ExceptionCode ==
      EXCEPTION_ACCESS_VIOLATION) {
    const char *op =
        exceptioninfo->ExceptionRecord->ExceptionInformation[0] == 1
            ? "write to"
            : "read from";
    uintptr_t target = exceptioninfo->ExceptionRecord->ExceptionInformation[1];
    line(utils::string::va(
        "Access Violation: Attempted to %s 0x%012llX%s", op, target,
        target < 0x10000 ? " (NULL pointer dereference)" : ""));
  }
  line("Code at the crash address: " +
       code_bytes(exceptioninfo->ExceptionRecord->ExceptionAddress));
  line(std::string{});
  info += scripts_text();
  line(get_callstack_summary(exceptioninfo));
  const std::string registers = get_memory_registers(exceptioninfo);
  if (!registers.empty()) {
    line(std::string{});
    line(registers);
  }
  line(std::string{});
  info += memory_text();
  info += settings_text();

  return info;
}

// stamp: the time in the early dump's name, so the dump, the text report
// and the zip of one crash carry the same name.
void write_minidump(const LPEXCEPTION_POINTERS exceptioninfo,
                    const std::string &stamp, const std::string &info) {
  const std::string crash_name =
      (game::get_appdata_path() / "minidumps" /
       utils::string::va("xerces-crash-%s.zip", stamp.data()))
          .string();

  utils::compression::zip::archive zip_file{};
  zip_file.add("crash.dmp", create_minidump(exceptioninfo));
  zip_file.add("info.txt", info);
  if (!zip_file.write(crash_name, "Xerces Crash Dump")) {
    utils::io::remove_file(crash_name);
  }
}

// Empty string used as fallback for null localization pointers
char ui_localize_fallback[4] = "";

long WINAPI crash_fix_exception_handler(PEXCEPTION_POINTERS exception_info) {
  const PEXCEPTION_RECORD record = exception_info->ExceptionRecord;
  PCONTEXT context = exception_info->ContextRecord;

  long result = EXCEPTION_CONTINUE_SEARCH;

  switch (record->ExceptionCode) {
  case EXCEPTION_ACCESS_VIOLATION:
  case STATUS_ILLEGAL_INSTRUCTION: {
    const uintptr_t addr =
        reinterpret_cast<uintptr_t>(record->ExceptionAddress);
    const uintptr_t base = game::get_base();
    const uintptr_t offset = addr - base;
#ifndef NDEBUG
    const char *patch_name = nullptr;
#endif

    if (game::is_legacy_client()) {
      switch (offset) {
      // Killcam animation crash - invalid anim data access
      case 0x234B9BD:
#ifndef NDEBUG
        patch_name = "Killcam animation (invalid anim data)";
#endif
        context->Rax = 0;
        context->Rip = base + 0x234D14B;
        goto continue_execution;

      // CG_ZBarrierAttachWeapon - null weapon pointer in zombie barriers
      case 0x464FEF:
#ifndef NDEBUG
        patch_name = "ZBarrier weapon attach (null weapon)";
#endif
        context->Rax = 0;
        context->Rip = base + 0x4651A2;
        goto continue_execution;

      // asmsetanimationrate - bad entity reference
      case 0x15E4B5A:
#ifndef NDEBUG
        patch_name = "asmsetanimationrate (bad entity ref)";
#endif
        context->Rip = base + 0x15E4B83;
        goto continue_execution;

      // Orphaned thread crash
      case 0x12EE4CC:
#ifndef NDEBUG
        patch_name = "Orphaned thread";
#endif
        context->Rip = base + 0x12EE5C8;
        goto continue_execution;

      // Character index out-of-bounds crash
      case 0x234210C:
#ifndef NDEBUG
        patch_name = "Character index out-of-bounds";
#endif
        context->Rip = base + 0x2342136;
        goto continue_execution;

      // HKS internal crash
      case 0x1CAB4F1:
#ifndef NDEBUG
        patch_name = "HKS/Lua internal error";
#endif
        context->Rip = base + 0x1CAB69E;
        goto continue_execution;

      // Null localization string crashes
      case 0x2279323:
#ifndef NDEBUG
        patch_name = "Null localization string (UI)";
#endif
        context->Rdx = reinterpret_cast<uintptr_t>(ui_localize_fallback);
        goto continue_execution;

      case 0x2278B96:
#ifndef NDEBUG
        patch_name = "Null localization string (UI)";
#endif
        context->Rsi = reinterpret_cast<uintptr_t>(ui_localize_fallback);
        goto continue_execution;

      case 0x228ED56:
#ifndef NDEBUG
        patch_name = "Null localization string (UI)";
#endif
        context->Rcx = reinterpret_cast<uintptr_t>(ui_localize_fallback);
        goto continue_execution;

      // Unknown UI crash
      case 0x1EAAA27:
#ifndef NDEBUG
        patch_name = "UI crash (unknown)";
#endif
        context->Rip = base + 0x1EAABB3;
        goto continue_execution;

      // Non-existent clientfield crashes (CSC side)
      case 0xC15B80:
      case 0xC15C50:
      case 0xC18CF5:
#ifndef NDEBUG
        patch_name = "Non-existent clientfield (CSC)";
#endif
        context->Rcx = 1; // CSC instance
        context->Rdx =
            reinterpret_cast<uintptr_t>("Clientfield does not exist");
        context->R8 = 0;
        context->Rip = base + 0x12EA430; // Scr_Error
        goto continue_execution;

      // Non-existent clientfield crashes (GSC side)
      case 0x1A6BD1B:
      case 0x1A6BE2E:
      case 0x1A6BF2E:
      case 0x1A6BFCD:
      case 0x1A6C246:
      case 0x1A6C356:
      case 0x1A6C40D:
      case 0x1A6C697:
      case 0x1A6C894:
#ifndef NDEBUG
        patch_name = "Non-existent clientfield (GSC)";
#endif
        context->Rcx = 0; // GSC instance
        context->Rdx =
            reinterpret_cast<uintptr_t>("Clientfield does not exist");
        context->R8 = 0;
        context->Rip = base + 0x12EA430; // Scr_Error
        goto continue_execution;

      // Non-existent clientfield (additional crash sites)
      case 0x133EC1:
      case 0x133EEB:
#ifndef NDEBUG
        patch_name = "Non-existent clientfield (additional)";
#endif
        context->Rip = base + 0x133F12;
        goto continue_execution;

      case 0x133F31:
#ifndef NDEBUG
        patch_name = "Non-existent clientfield (additional)";
#endif
        context->Rip = base + 0x133F42;
        goto continue_execution;

      // Random crash on Zetsubou No Shima
      case 0x13591D3:
#ifndef NDEBUG
        patch_name = "Zetsubou No Shima map bug";
#endif
        context->Rip = base + 0x13591DA;
        goto continue_execution;
      default: {
        break;
      }
      continue_execution:
        {
          result = EXCEPTION_CONTINUE_EXECUTION;
          break;
        }
      }
    }

#ifndef NDEBUG
    if (patch_name) {
      exception_log(true, "^3[Exception] Known crash patched: %s (base+0x%llX)",
                    patch_name, offset);
    }
#endif
    break;
  }
  default: {
    break;
  }
  }
  return result;
}

std::string find_culprit(const std::vector<resolved_frame> &frames) {
  if (exception_data.code == EXCEPTION_IN_PAGE_ERROR) {
    return "Windows could not read part of the game from the disk.\nFix: "
           "Check the drive for errors (or reconnect it if it is external), "
           "then verify the game files in Steam.";
  }

  const utils::nt::library crashed =
      utils::nt::library::get_by_address(exception_data.address);
  if (crashed.get_ptr() == utils::nt::library::get_by_address(
                               reinterpret_cast<const void *>(&find_culprit))
                               .get_ptr()) {
    return "This is a bug in the Xerces game client (boiii.exe), not a "
           "problem with your PC or game files.";
  }

  std::vector<const void *> addresses{exception_data.address};
  for (const resolved_frame &frame : frames) {
    addresses.push_back(reinterpret_cast<const void *>(frame.address));
  }
  for (const void *address : addresses) {
    const utils::nt::library module =
        utils::nt::library::get_by_address(address);
    if (module) {
      if (std::string help = error_help::explain_module(module.get_path());
          !help.empty()) {
        return help;
      }
    }
  }
  return {};
}

// Xerces: the crash report's part (docs/CRASH_REPORTS.md). When the launcher
// started this game (XERCES_HOME is set) and "Send crash reports" is on, a
// stacks-only dump goes to <home>\crash\pending\<report_id>.dmp first, then
// a note with the code, the module and offset, the thread and the frames
// (module+offset, as the console report prints them) next to it. The
// launcher sends both when the game has closed. Upstream's dump and zip in
// the minidumps folder stay as they are, for local use. Returns the report
// id; empty when nothing was written.
// Xerces: at most three reports from one game. A server drops the map after
// a crash and plays on, so one fault it meets again and again would
// otherwise send a report each time (and the server limits each PC anyway).
std::atomic<int> report_slots_taken{0};

bool take_report_slot() { return report_slots_taken.fetch_add(1) < 3; }

// A report that was taken back (a stall, not a freeze) gives its slot back:
// three long stalls used to leave a later crash without a report.
void return_report_slot() { --report_slots_taken; }

std::string begin_pending_report(const LPEXCEPTION_POINTERS exceptioninfo) {
  if (!crash_reports_on() || !take_report_slot()) {
    return {};
  }
  const std::string id = xerces_core::new_launch_id();
  std::error_code ec;
  const std::filesystem::path dir =
      xerces_runtime::home() / "crash" / "pending";
  std::filesystem::create_directories(dir, ec);
  // Written as .dmp.tmp and renamed: the MiniDumpWriteDump hook above sends
  // every other .dmp to the minidumps folder.
  const std::filesystem::path temporary = dir / (id + ".dmp.tmp");
  if (write_stacks_minidump_file(exceptioninfo, temporary.c_str())) {
    std::filesystem::rename(temporary, dir / (id + ".dmp"), ec);
  }
  return id;
}

std::string frame_text(const resolved_frame &frame) {
  return xerces_core::crash_frame(
      frame.module_name == "unknown" ? "" : frame.module_name, frame.rva);
}

// The note's common part: the thread, the journal's phase, the console's
// last lines (the names the game knows already replaced), the flags, the
// game log's name, the journal, and those names for the report to hide.
// last_line: one more line after the console's (a freeze's own).
void write_pending_note(xerces_core::pending_crash note,
                        const std::string &last_line = {}) {
  note.created_utc = xerces_core::json_log::now_utc();
  if (note.thread.empty()) {
    note.thread = thread_role();
  }
  note.phase = xerces_runtime::journal_phase_for_crash();
  note.console =
      xerces_runtime::console_last_lines(xerces_core::crash_console_lines);
  if (!last_line.empty()) {
    note.console.push_back(last_line);
  }
  note.flags =
      xerces_core::launch_flag_names(xerces_runtime::command_line_args());
  note.game_log = xerces_runtime::log_file_name();
  note.journal = xerces_runtime::read_file(xerces_runtime::home() / "logs" /
                                           "journal.json");
  note.names = xerces_runtime::player_names();
  xerces_runtime::write_file(xerces_runtime::home() / "crash" / "pending" /
                                 (note.report_id + ".json"),
                             xerces_core::pending_crash_to_json(note));
}

// One line of at most `cap` bytes: line breaks become spaces.
std::string one_line(std::string text, const std::size_t cap = 250) {
  for (char &c : text) {
    if (c == '\r' || c == '\n' || c == '\t') {
      c = ' ';
    }
  }
  if (text.size() > cap) {
    // Cut at a word: half a player's name at the end would not be seen as
    // that name by the scrubbing of what is sent.
    text.resize(cap);
    if (const std::size_t space = text.rfind(' ');
        space != std::string::npos && space > cap / 2) {
      text.resize(space);
    }
  }
  return xerces_core::json::valid_utf8(text, cap);
}

// details: the text report (generate_crash_info), which the report carries
// as it is; reason, summary and script say in a line each what the report is
// about, so a list of reports can be read without opening one.
void finish_pending_report(const std::string &id, const resolved_frame &crash,
                           const std::vector<resolved_frame> &frames,
                           const std::string &details) {
  if (id.empty()) {
    return;
  }
  xerces_core::pending_crash note;
  note.report_id = id;
  note.code = exception_data.code;
  note.reason = "exception";
  note.summary = one_line(
      std::format("{} on the {} thread", describe_crash(), thread_role()));
  note.script = one_line(running_script());
  note.details = details;
  if (crash.module_name != "unknown") {
    note.module = crash.module_name;
    note.offset = crash.rva;
  }
  for (const resolved_frame &frame : frames) {
    if (note.frames.size() == xerces_core::crash_frame_cap) {
      break;
    }
    note.frames.push_back(frame_text(frame));
  }
  write_pending_note(std::move(note));
}

// Xerces: a fatal engine error (Com_Error FATAL, Sys_Error) ends the game
// though no exception ran. Its note has no dump (kind unknown_exit); the
// module and offset are the first frame outside the client, the code that
// raised the error. Without it "Restart game", which ends this process with
// code 0, would read as a clean exit and nothing would be reported.
void write_fatal_report(const std::string &message,
                        const std::string &details) {
  if (!crash_reports_on() || !take_report_slot()) {
    return;
  }
  xerces_core::pending_crash note;
  note.report_id = xerces_core::new_launch_id();
  note.kind = xerces_core::crash_kind::unknown_exit;
  note.reason = "fatal_error";
  note.summary = one_line(message);
  note.script = one_line(running_script());
  note.details = details;
  void *stack[xerces_core::crash_frame_cap]{};
  const USHORT count = RtlCaptureStackBackTrace(
      1, static_cast<DWORD>(std::size(stack)), stack, nullptr);
  const auto client = reinterpret_cast<uint64_t>(
      utils::nt::library::get_by_address(
          reinterpret_cast<const void *>(&write_fatal_report))
          .get_ptr());
  for (USHORT i = 0; i < count; ++i) {
    const resolved_frame frame = resolve_address(stack[i]);
    note.frames.push_back(frame_text(frame));
    if (!note.offset && frame.module_name != "unknown" &&
        frame.module_base != client) {
      note.module = frame.module_name;
      note.offset = frame.rva;
    }
  }
  // The error's own words as the console's last line: the report's console
  // tail then has them whatever the console itself printed.
  write_pending_note(std::move(note),
                     "[Fatal error] " + one_line(message, 1000));
}

// The text report of a fatal engine error: the error, what the game was
// doing, the scripts running and where the error was raised.
// stack: where show_fatal_error was called from, taken there so the report's
// own calls are not in it.
std::string fatal_text(const std::string &message, void *const *stack,
                       const USHORT count) {
  std::string info = "Xerces Fatal Error Report\r\n\r\n";
  info +=
      std::format("What happened: the game stopped itself with an error "
                  "on the {} thread.\r\nError: {}\r\n\r\n",
                  thread_role(), xerces_core::json::valid_utf8(message, 4000));
  info += session_text() + "\r\n" + scripts_text();
  info += "callstack:\r\n{\r\n";
  for (USHORT i = 0; i < count; ++i) {
    info += format_frame(resolve_address(stack[i]), i) + "\r\n";
  }
  info += "}\r\n\r\n" + memory_text() + settings_text();
  return info;
}

bool is_harmless_error(const LPEXCEPTION_POINTERS exceptioninfo) {
  const uint32_t code = exceptioninfo->ExceptionRecord->ExceptionCode;
  return code == STATUS_INTEGER_OVERFLOW || code == STATUS_FLOAT_OVERFLOW ||
         code == STATUS_SINGLE_STEP;
}

std::atomic<bool> crashed{false};

// Xerces: a fault on another thread once the game has called ExitProcess
// after a quit (or with code 0). The engine has shut itself down; main.cpp's
// exit hook is running the components' shutdown on the exiting thread. The
// game's own runtime has closed its file table by then, and a game thread
// that still opens a file reads through a null pointer there (2026-11-12:
// BlackOps3.exe+0x2BDC8C6 under the game's fopen, while the hook waited for
// the command pipe's thread; "once in 12 quits" in the first betas' notes).
// That was written up as a crash, and the next start of the launcher offered
// safe mode for a game that had quit. The launch ends here instead as the
// exit it was, the way the console's forced exit ends one (console.cpp): the
// journal gets clean_exit, a lobby's router port is given back, and the
// process ends with the code the game gave. No dump, no report, no dialog.
//
// Not covered, and still a crash: a fault on the exiting thread itself (a
// fault in that shutdown), anything before the game calls ExitProcess (a
// crash while the engine shuts itself down, including the short stretch in
// which its exit() has closed its tables and has not called ExitProcess yet:
// nothing marks it), and any exit that is not a clean one. The rule is
// xerces_core::late_fault_is_part_of_exit.

// Ends the process after 3 s whatever happens: the steps below take locks
// (the journal's, the log's) and wait for the router, and the thread that
// faulted might be the one holding a lock.
DWORD WINAPI late_fault_failsafe(const LPVOID code) {
  Sleep(3000);
  TerminateProcess(GetCurrentProcess(),
                   static_cast<UINT>(reinterpret_cast<std::uintptr_t>(code)));
  return 0;
}

[[noreturn]] void end_exit_after_late_fault(
    const LPEXCEPTION_POINTERS exceptioninfo) {
  const auto code = xerces_runtime::exit_code().value_or(0);
  // A fault in what follows comes back here on the same thread: the second
  // time there is nothing left to try. (Per thread: two game threads can
  // fault close together, and the second must not cut the first one short.)
  static thread_local bool entered = false;
  if (entered) {
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) {
      Sleep(INFINITE);
    }
  }
  entered = true;
  if (const HANDLE failsafe = CreateThread(
          nullptr, 0, late_fault_failsafe,
          reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(code)), 0,
          nullptr)) {
    CloseHandle(failsafe);
  }
  // First what matters to the next start of the launcher
  xerces_runtime::journal_exit_after_late_fault();
  const auto address = reinterpret_cast<std::uintptr_t>(
      exceptioninfo->ExceptionRecord->ExceptionAddress);
  // Which module, without the symbol engine: a name and an offset are enough
  // to find the place in a dump of the game or in the client's own symbols
  char where[160] = {};
  HMODULE module = nullptr;
  char path[MAX_PATH] = {};
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &module) &&
      module && GetModuleFileNameA(module, path, MAX_PATH) != 0) {
    const char *name = std::strrchr(path, '\\');
    std::snprintf(where, sizeof(where), "%s+0x%llX", name ? name + 1 : path,
                  static_cast<unsigned long long>(
                      address - reinterpret_cast<std::uintptr_t>(module)));
  } else {
    std::snprintf(where, sizeof(where), "0x%llX",
                  static_cast<unsigned long long>(address));
  }
  char data[320] = {};
  std::snprintf(data, sizeof(data),
                "{\"at\":\"%s\",\"exception\":\"0x%08lX\",\"thread\":%lu,"
                "\"exit_code\":%lu}",
                where, exceptioninfo->ExceptionRecord->ExceptionCode,
                GetCurrentThreadId(), static_cast<unsigned long>(code));
  xerces_runtime::log_warn(
      "loader.exit.late_fault",
      std::string("A thread faulted at ") + where +
          " while the game was closing; the launch ends as the exit it was",
      data);
  // Waits for a removal the exiting thread may already have begun
  upnp::close_port_now();
  TerminateProcess(GetCurrentProcess(), code);
  for (;;) {
    Sleep(INFINITE);
  }
}

long WINAPI exception_filter(const LPEXCEPTION_POINTERS exceptioninfo) {
  if (is_harmless_error(exceptioninfo)) {
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (xerces_runtime::late_fault_is_part_of_exit()) {
    end_exit_after_late_fault(exceptioninfo);
  }
  crashed = true;
  crash_handler_code = exceptioninfo->ExceptionRecord->ExceptionCode;
  crash_handler_started_ms = GetTickCount64();
  // Xerces' journal: no exit after this counts as clean (xerces/runtime.hpp).
  xerces_runtime::note_crash();

  // Xerces: a dump first, straight to a file. On 2026-11-05 two custom maps
  // with DS4C crashed while loading and the game ended (code 0xFFFFFFFF)
  // during the report below, before the zip with the dump was written.
  const std::string stamp = get_timestamp();
  {
    std::error_code ec;
    const std::filesystem::path dir = game::get_appdata_path() / "minidumps";
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path early = dir / ("crash-" + stamp + ".dmp");
    exception_log(true, "Writing a crash dump to %s",
                  write_minidump_file(exceptioninfo, early.c_str())
                      ? early.string().c_str()
                      : "(failed)");
  }
  const std::string report_id = begin_pending_report(exceptioninfo);

  const resolved_frame crash_frame =
      resolve_address(exceptioninfo->ExceptionRecord->ExceptionAddress);
  const char *exception_name =
      get_exception_string(exceptioninfo->ExceptionRecord->ExceptionCode);

  // Detailed console crash report
  exception_log(true, "========== CRASH DETECTED ==========");
  exception_log(true, "  Exception:  %s (0x%08lX)", exception_name,
                exceptioninfo->ExceptionRecord->ExceptionCode);
  exception_log(true, "  Module:     %s + 0x%llX",
                crash_frame.module_name.c_str(), crash_frame.rva);
  if (!crash_frame.function_name.empty())
    exception_log(true, "  Function:   %s", crash_frame.function_name.c_str());
  if (!crash_frame.file_name.empty() && crash_frame.line_number > 0)
    exception_log(true, "  Source:     %s:%u", crash_frame.file_name.c_str(),
                  crash_frame.line_number);
  exception_log(true, "  Address:    0x%llX", crash_frame.address);
  exception_log(true, "  Thread:     %lu (%s)", GetCurrentThreadId(),
                thread_role());

  if (exceptioninfo->ExceptionRecord->ExceptionCode ==
      EXCEPTION_ACCESS_VIOLATION) {
    const char *op =
        exceptioninfo->ExceptionRecord->ExceptionInformation[0] == 1
            ? "write to"
            : "read from";
    const uintptr_t target =
        exceptioninfo->ExceptionRecord->ExceptionInformation[1];
    exception_log(true, "  Details:    Attempted to %s 0x%012llX%s", op, target,
                  target < 0x10000 ? " (NULL pointer dereference)" : "");
  }

  // Print condensed callstack to console. Xerces: 32 frames are walked for
  // the crash report; the console shows the first 16, as before.
  std::vector<resolved_frame> frames = capture_stackwalk(
      exceptioninfo, static_cast<int32_t>(xerces_core::crash_frame_cap));
  if (!frames.empty()) {
    exception_log(true, "  Callstack:");
    for (size_t i = 0; i < frames.size() && i < 16; ++i) {
      const resolved_frame *f = &frames[i];

      if (!f->function_name.empty()) {
        exception_log(true, "    [%zu] 0x%llX - %s!%s", i, f->address,
                      f->module_name.c_str(), f->function_name.c_str());
      } else {
        exception_log(true, "    [%zu] 0x%llX - %s + 0x%llX", i, f->address,
                      f->module_name.c_str(), f->rva);
      }
      fflush(stderr);
    }
  }
  exception_log(true, "=====================================");

  exception_data.code = exceptioninfo->ExceptionRecord->ExceptionCode;
  exception_data.address = exceptioninfo->ExceptionRecord->ExceptionAddress;
  exception_data.access =
      exceptioninfo->ExceptionRecord->ExceptionInformation[0];
  exception_data.target =
      exceptioninfo->ExceptionRecord->ExceptionInformation[1];
  exception_data.caller =
      exception_data.code == EXCEPTION_ACCESS_VIOLATION &&
              exception_data.access == 8
          ? *reinterpret_cast<void **>(exceptioninfo->ContextRecord->Rsp)
          : nullptr;
  // Xerces: the note for the launcher first, without the text, so that it is
  // there whatever the rest of this meets; it is written again with the text
  // below.
  finish_pending_report(report_id, crash_frame, frames, {});
  exception_data.culprit = find_culprit(frames);
  exception_data.scripts = describe_scripts();

  // Xerces: the text report. On its own next to the dump first (the zip
  // below was lost to a second fault once, see above), then in the crash
  // report the launcher sends and in the zip. A fault while it is put
  // together costs the text, never the report.
  std::string report;
  if (!run_guarded([&report, exceptioninfo] {
        report = generate_crash_info(exceptioninfo);
      })) {
    report =
        "Xerces Crash Report\r\n\r\n(the report could not be put together: "
        "reading the game's state faulted)\r\n";
  }
  std::string local_report = report;
  run_guarded([&local_report] { local_report += console_text(); });
  {
    const std::filesystem::path file =
        game::get_appdata_path() / "minidumps" / ("crash-" + stamp + ".txt");
    if (write_report_file(file, local_report)) {
      crash_report_file = file.filename().string();
      exception_log(true, "Crash report written to %s", file.string().c_str());
    }
  }
  finish_pending_report(report_id, crash_frame, frames, report);

  write_minidump(exceptioninfo, stamp, local_report);

  // Xerces: Windows cannot resume a C++ exception (0xE06D7363) or any other
  // that it marks noncontinuable. Asked to, it raises
  // STATUS_NONCONTINUABLE_EXCEPTION, which came back here: on 2026-11-06 one
  // crash wrote 13 dumps and 13 crash reports, 3 s apart, until the game
  // ended (docs/TESTING.md, "OneDrive crash"). Such a crash gets the dialog
  // here instead, as reset_state would show it; a server ends, as it cannot
  // drop the map from here.
  if (exceptioninfo->ExceptionRecord->ExceptionFlags &
      EXCEPTION_NONCONTINUABLE) {
    display_error_dialog(); // ends the process
  }
  crash_handler_started_ms = 0;
  exceptioninfo->ContextRecord->Rip = get_reset_state_stub();

  return EXCEPTION_CONTINUE_EXECUTION;
}

constexpr std::chrono::seconds FREEZE_TIME = 30s;

// Xerces: a freeze is handled as a crash is (2026-11-08, after two freezes
// left only the main thread's stack and a game that had to be ended by hand).
// At FREEZE_TIME without a frame: a text report with every thread's stack
// (the main, server and renderer threads first and in full, then the threads
// that are running, then the ones that wait, alike ones together), the
// scripts running and what the game was doing, and a dump of every thread,
// both next to the crash dumps in the minidumps folder; and, when the
// launcher started the game with crash reports on, a crash report for it to
// send (kind unknown_exit, reason "freeze", with that text). If frames come
// back, the report is taken back (a long stall). If they do not, the game is
// closed, as a crash ends it: FREEZE_CLOSE_TIME after the last frame in a
// match or the menus, FREEZE_CLOSE_LOADING while the journal says it is
// loading (a load can hold the main thread for a while on a large stack).
//
// The report is written by a thread of its own (2026-11-12): whatever it
// waits for, the watchdog still closes the game on time. Its first version
// goes to disk before it asks Windows for anything a frozen game may hold
// (the loader's lock, the symbol engine): the stacks by module and offset,
// from the list of modules the watchdog keeps while the game runs well.
constexpr std::chrono::seconds FREEZE_CLOSE_TIME = 60s;
constexpr std::chrono::seconds FREEZE_CLOSE_LOADING = 180s;
// STATUS_POSSIBLE_DEADLOCK: the launcher reads any exit that is not clean as
// a crash and sends the report the game left.
constexpr UINT FREEZE_EXIT_CODE = 0xC0000194;

// Shared by the watchdog and the report's thread. The watchdog's way to the
// end of a frozen game takes no lock and no memory (2026-11-12): the heap's
// lock may be the very thing the frozen thread holds. So the report's thread
// is started when the game starts and waits for freeze_report_event, whether
// one is being written is an atomic, and whether the game is loading is
// noted while frames still come.
HANDLE freeze_report_event = nullptr;
std::atomic<bool> freeze_report_running{false};
std::atomic<bool> loading_noted{false};
// Frames came back while the report was still being written, after this many
// seconds: the report takes itself back when it is done. -1: they did not.
std::atomic<long long> freeze_withdraw{-1};
// The id of the freeze's crash report; under freeze_mutex, which the
// watchdog takes only once frames have come back.
std::mutex freeze_mutex;
std::string freeze_report_id;
// The report's thread only.
std::filesystem::path freeze_dump;
// The thread the report holds stopped for its stack walk, and since when:
// the watchdog starts it again if the walk does not come back.
std::atomic<DWORD> walk_thread_id{0};
std::atomic<uint64_t> walk_started_ms{0};

using thread_describe_fn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
thread_describe_fn thread_describe = nullptr;

bool write_dump_file(const std::filesystem::path &path) {
  const HANDLE file =
      CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  // No exception: every thread as it stands.
  const bool written =
      MiniDumpWriteDump(
          GetCurrentProcess(), GetCurrentProcessId(), file,
          static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo),
          nullptr, nullptr, nullptr) != FALSE;
  CloseHandle(file);
  if (!written) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  return written;
}

// The loaded modules, noted by the watchdog every half minute while frames
// come: a frozen game's report names addresses from this list.
struct module_range {
  uint64_t base = 0;
  uint64_t size = 0;
  std::string name;
};
std::mutex modules_mutex;
std::vector<module_range> modules_seen;

void note_modules() {
  HMODULE handles[1024]{};
  DWORD needed = 0;
  const HANDLE process = GetCurrentProcess();
  if (!EnumProcessModules(process, handles, sizeof(handles), &needed)) {
    return;
  }
  std::vector<module_range> list;
  const std::size_t count =
      std::min<std::size_t>(needed / sizeof(HMODULE), std::size(handles));
  for (std::size_t i = 0; i < count; ++i) {
    MODULEINFO info{};
    if (GetModuleInformation(process, handles[i], &info, sizeof(info))) {
      list.push_back({reinterpret_cast<uint64_t>(info.lpBaseOfDll),
                      info.SizeOfImage,
                      utils::nt::library(handles[i]).get_name()});
    }
  }
  const std::lock_guard lock(modules_mutex);
  modules_seen = std::move(list);
}

// A frame by module and offset from that list, as resolve_address names
// them, without its function.
resolved_frame frame_from_modules(const DWORD64 address) {
  resolved_frame frame{};
  frame.address = address;
  frame.module_name = "unknown";
  frame.rva = address;
  const std::lock_guard lock(modules_mutex);
  for (const module_range &module : modules_seen) {
    if (address >= module.base && address < module.base + module.size) {
      frame.module_name = module.name;
      frame.module_base = module.base;
      frame.rva = address - module.base;
      if (frame.module_name == "BlackOps3.exe") {
        frame.rva += 0x140000000;
      }
      break;
    }
  }
  return frame;
}

struct thread_stack {
  DWORD id = 0;
  std::size_t count = 0;
  std::array<DWORD64, 32> frames{};
};

// The return addresses up a stopped thread's stack. Code without unwind
// data is a leaf function by the ABI, its return address on top of the
// stack, or code a hook made: one such step is taken, a second in a row
// would be a guess. A stack that cannot be read ends the walk.
// One value off a stopped thread's stack; false when it cannot be read. In a
// function of its own, never inlined: with clang a __try block catches a
// fault inside a function it calls, not one its own instructions raise.
__declspec(noinline) void read_stack_value(const DWORD64 address,
                                           DWORD64 *value) {
  *value = *reinterpret_cast<const volatile DWORD64 *>(address);
}

std::size_t walk_stack(CONTEXT &context, DWORD64 *frames,
                       const std::size_t cap) {
  std::size_t count = 0;
  __try {
    bool leaf_before = false;
    while (count < cap && context.Rip) {
      frames[count++] = context.Rip;
      DWORD64 image_base = 0;
      const PRUNTIME_FUNCTION function =
          RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
      if (!function) {
        if (leaf_before || context.Rsp == 0) {
          break;
        }
        leaf_before = true;
        DWORD64 return_address = 0;
        read_stack_value(context.Rsp, &return_address);
        context.Rip = return_address;
        context.Rsp += 8;
        continue;
      }
      leaf_before = false;
      PVOID handler_data = nullptr;
      DWORD64 establisher_frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function,
                       &context, &handler_data, &establisher_frame, nullptr);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return count;
}

// Every thread's stack but this one's. A thread is stopped only for its own
// walk, and nothing here allocates or takes a lock of the game's while it
// is (the list has its room already).
std::vector<thread_stack> capture_threads() {
  std::vector<thread_stack> stacks;
  stacks.reserve(512);
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return stacks;
  }
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  const DWORD self = GetCurrentThreadId();
  const DWORD process = GetCurrentProcessId();
  for (BOOL more = Thread32First(snapshot, &entry);
       more && stacks.size() < stacks.capacity();
       more = Thread32Next(snapshot, &entry)) {
    // Never the watchdog: it must stay able to end the game
    if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self ||
        entry.th32ThreadID == watchdog_thread_id.load()) {
      continue;
    }
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                     FALSE, entry.th32ThreadID);
    if (!thread) {
      continue;
    }
    thread_stack &stack = stacks.emplace_back();
    stack.id = entry.th32ThreadID;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    walk_started_ms = GetTickCount64();
    walk_thread_id = stack.id;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
      if (GetThreadContext(thread, &context)) {
        stack.count =
            walk_stack(context, stack.frames.data(), stack.frames.size());
      }
      ResumeThread(thread);
    }
    walk_thread_id = 0;
    CloseHandle(thread);
  }
  CloseHandle(snapshot);
  return stacks;
}

// The watchdog's side of that: a walk that has not come back in two seconds
// (it waits for something the stopped thread holds) gets its thread started
// again, so the report cannot be what freezes a game that only stalled.
void release_walked_thread() {
  DWORD id = walk_thread_id.load();
  if (id == 0 || GetTickCount64() - walk_started_ms.load() < 2000) {
    return;
  }
  if (const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, id)) {
    ResumeThread(thread);
    CloseHandle(thread);
  }
  // only if it is still that walk: the next one may have begun meanwhile
  walk_thread_id.compare_exchange_strong(id, 0);
}

// A thread as a report sees it: what it is for, its name, its stack.
struct seen_thread {
  DWORD id = 0;
  std::string title;
  std::vector<resolved_frame> frames;
};

std::string thread_title(const DWORD id) {
  const char *role = id == frame_thread_id.load()    ? "main"
                     : id == server_thread_id.load() ? "server"
                     : id == render_thread_id.load() ? "renderer"
                                                     : nullptr;
  if (id == GetCurrentThreadId()) {
    role = "this report's";
  }
  std::string name;
  if (thread_describe) {
    if (const HANDLE handle =
            OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id)) {
      PWSTR description = nullptr;
      if (SUCCEEDED(thread_describe(handle, &description)) && description) {
        name = utils::string::convert(description);
        LocalFree(description);
      }
      CloseHandle(handle);
    }
  }
  std::string title = std::format("Thread {}", id);
  if (role) {
    title += std::format(" ({})", role);
  }
  if (!name.empty()) {
    title += std::format(" \"{}\"", name);
  }
  return title;
}

bool system_module(const std::string &name) {
  static constexpr std::array<std::string_view, 6> names{
      "ntdll.dll",    "win32u.dll", "kernelbase.dll",
      "kernel32.dll", "user32.dll", "ucrtbase.dll"};
  const std::string lower = utils::string::to_lower(name);
  return std::ranges::find(names, lower) != names.end();
}

// Whether the thread is inside a system call: waiting for something, as an
// idle thread is and a thread stuck on a lock is too.
bool in_system_call(const std::vector<resolved_frame> &frames) {
  if (frames.empty()) {
    return false;
  }
  const std::string lower = utils::string::to_lower(frames[0].module_name);
  return lower == "ntdll.dll" || lower == "win32u.dll";
}

std::string frame_place(const resolved_frame &frame) {
  std::string text = frame_text(frame);
  if (!frame.function_name.empty()) {
    text += std::format(" ({})", frame.function_name);
  }
  return text;
}

// Where a thread is, in words. brief: for the one-line summary, by module
// and offset only.
std::string thread_place(const std::vector<resolved_frame> &frames,
                         const bool brief = false) {
  if (frames.empty()) {
    return "its stack could not be read";
  }
  const auto place = [brief](const resolved_frame &frame) {
    return brief ? frame_text(frame) : frame_place(frame);
  };
  if (!in_system_call(frames)) {
    return "running in " + place(frames[0]);
  }
  std::string text = brief ? "waiting" : "waiting in a system call";
  if (!brief && !frames[0].function_name.empty()) {
    text += std::format(" ({})", frames[0].function_name);
  }
  for (const resolved_frame &frame : frames) {
    if (!system_module(frame.module_name)) {
      return text + (brief ? " under " : ", called from ") + place(frame);
    }
  }
  return text;
}

std::string stack_text(const seen_thread &thread, const std::size_t most) {
  std::string text =
      std::format("{}: {}\r\n", thread.title, thread_place(thread.frames));
  for (std::size_t i = 0; i < thread.frames.size() && i < most; ++i) {
    text += format_frame(thread.frames[i], i) + "\r\n";
  }
  if (thread.frames.size() > most) {
    text +=
        std::format("\t... {} more frames\r\n", thread.frames.size() - most);
  }
  return text + "\r\n";
}

struct freeze_text_result {
  std::string core;    // the report, without the console's lines
  std::string summary; // one line
};

constexpr std::size_t WAITING_FRAMES = 8;

// full: with the scripts running and what the game was doing, which read
// the game's own state; the first version leaves them out.
freeze_text_result freeze_text(const std::vector<seen_thread> &threads,
                               const bool full) {
  const auto find = [&threads](const DWORD id) -> const seen_thread * {
    const auto found = std::ranges::find(threads, id, &seen_thread::id);
    return id != 0 && found != threads.end() ? &*found : nullptr;
  };
  const std::array<std::pair<const char *, const seen_thread *>, 3> roles{{
      {"main", find(frame_thread_id.load())},
      {"server", find(server_thread_id.load())},
      {"renderer", render_thread_id.load() != frame_thread_id.load() &&
                           render_thread_id.load() != server_thread_id.load()
                       ? find(render_thread_id.load())
                       : nullptr},
  }};

  freeze_text_result out;
  out.core = "Xerces Freeze Report\r\n\r\n";
  out.core += std::format("What happened: the game has not finished a frame "
                          "for {} seconds, so it is frozen.\r\n",
                          FREEZE_TIME.count());
  out.summary = "Freeze:";
  for (const auto &[role, thread] : roles) {
    if (!thread) {
      continue;
    }
    const std::string place = thread_place(thread->frames);
    out.core += std::format("The {} thread is {}.\r\n", role, place);
    if (std::string_view(role) != "renderer") {
      out.summary +=
          std::format("{} {} thread {}", out.summary.ends_with(':') ? "" : ";",
                      role, thread_place(thread->frames, true));
    }
  }
  if (full) {
    if (const std::string script = running_script(); !script.empty()) {
      out.core += "Script running: " + script + "\r\n";
      out.summary += "; script " + script;
    }
  }
  out.core += std::format(
      "The game closes {} seconds after its last frame ({} while it is "
      "loading) unless frames come back.\r\n\r\n",
      FREEZE_CLOSE_TIME.count(), FREEZE_CLOSE_LOADING.count());
  if (full) {
    out.core += session_text() + "\r\n" + scripts_text();
  }

  // The threads that matter most, in full
  std::vector<DWORD> printed;
  for (const auto &[role, thread] : roles) {
    if (thread) {
      out.core += stack_text(*thread, thread->frames.size());
      printed.push_back(thread->id);
    }
  }
  // then every other thread that is running,
  for (const seen_thread &thread : threads) {
    if (std::ranges::find(printed, thread.id) == printed.end() &&
        !thread.frames.empty() && !in_system_call(thread.frames)) {
      out.core += stack_text(thread, thread.frames.size());
      printed.push_back(thread.id);
    }
  }
  // then the ones that wait, alike ones together (a pool's workers all wait
  // at one place).
  std::vector<std::pair<std::string, std::vector<const seen_thread *>>> groups;
  for (const seen_thread &thread : threads) {
    if (std::ranges::find(printed, thread.id) != printed.end()) {
      continue;
    }
    std::string key;
    for (std::size_t i = 0; i < thread.frames.size() && i < WAITING_FRAMES;
         ++i) {
      key += std::format("{:X};", thread.frames[i].address);
    }
    const auto group = std::ranges::find(
        groups, key,
        &std::pair<std::string, std::vector<const seen_thread *>>::first);
    if (group == groups.end()) {
      groups.push_back({key, {&thread}});
    } else {
      group->second.push_back(&thread);
    }
  }
  if (!groups.empty()) {
    out.core +=
        "Threads that wait (their full stacks are in the dump):\r\n\r\n";
  }
  for (const auto &[key, members] : groups) {
    if (members.size() == 1) {
      out.core += stack_text(*members.front(), WAITING_FRAMES);
      continue;
    }
    std::string ids;
    for (const seen_thread *member : members) {
      ids += std::format("{}{}", ids.empty() ? "" : ", ", member->id);
    }
    seen_thread shown = *members.front();
    shown.title =
        std::format("{} threads at the same place ({})", members.size(), ids);
    out.core += stack_text(shown, WAITING_FRAMES);
  }
  if (full) {
    out.core += memory_text() + settings_text();
  }
  out.summary = one_line(out.summary, 250);
  return out;
}

// The id for a freeze's crash report, for the launcher to send; empty when
// none is to be written (reports off, or this game has written its three).
std::string new_freeze_report() {
  if (!crash_reports_on() || !take_report_slot()) {
    return {};
  }
  std::error_code ec;
  std::filesystem::create_directories(
      xerces_runtime::home() / "crash" / "pending", ec);
  return xerces_core::new_launch_id();
}

// Writes the report's note, or writes it again with more in it.
void write_freeze_note(const std::string &id,
                       const std::vector<resolved_frame> &frames,
                       const freeze_text_result &text) {
  if (id.empty()) {
    return;
  }
  xerces_core::pending_crash note;
  note.report_id = id;
  note.kind = xerces_core::crash_kind::unknown_exit;
  note.thread = "main";
  note.reason = "freeze";
  note.summary = text.summary;
  note.script = one_line(running_script());
  note.details = text.core;
  for (const resolved_frame &frame : frames) {
    if (note.frames.size() == xerces_core::crash_frame_cap) {
      break;
    }
    note.frames.push_back(frame_text(frame));
    // The first frame in the game itself, as a crash's address
    if (!note.offset && frame.module_name != "unknown" &&
        !system_module(frame.module_name)) {
      note.module = frame.module_name;
      note.offset = frame.rva;
    }
  }
  write_pending_note(std::move(note),
                     std::format("[Freeze] The game has not finished a frame "
                                 "for {} seconds, so it is frozen. {}",
                                 FREEZE_TIME.count(), text.summary));
}

// The dump already written, copied next to the note as .dmp.tmp and renamed:
// the MiniDumpWriteDump hook above sends every other .dmp to the minidumps
// folder.
void attach_freeze_dump(const std::string &id) {
  if (id.empty() || freeze_dump.empty()) {
    return;
  }
  std::error_code ec;
  const std::filesystem::path dir =
      xerces_runtime::home() / "crash" / "pending";
  const std::filesystem::path temporary = dir / (id + ".dmp.tmp");
  if (std::filesystem::copy_file(
          freeze_dump, temporary,
          std::filesystem::copy_options::overwrite_existing, ec)) {
    std::filesystem::rename(temporary, dir / (id + ".dmp"), ec);
  }
}

// Frames came back: it was a long stall, not a freeze, and its report is not
// to be sent. No lock is held here.
void withdraw_freeze_report(const std::string &id,
                            const std::chrono::seconds stalled) {
  if (id.empty()) {
    return;
  }
  const std::filesystem::path dir =
      xerces_runtime::home() / "crash" / "pending";
  std::error_code ec;
  std::filesystem::remove(dir / (id + ".json"), ec);
  std::filesystem::remove(dir / (id + ".dmp"), ec);
  return_report_slot();
  exception_log(false,
                "[Freeze] Frames came back after %lld seconds; the freeze "
                "report was taken back",
                static_cast<long long>(stalled.count()));
}

struct freeze_files {
  std::string id;      // the crash report's, empty without one
  std::string summary; // for the console
  std::filesystem::path text_file;
};

freeze_files write_freeze_files() {
  freeze_files out;
  const std::vector<thread_stack> stacks = capture_threads();
  std::vector<seen_thread> threads;
  threads.reserve(stacks.size());
  for (const thread_stack &stack : stacks) {
    seen_thread &thread = threads.emplace_back();
    thread.id = stack.id;
    thread.title = thread_title(stack.id);
    for (std::size_t i = 0; i < stack.count; ++i) {
      thread.frames.push_back(frame_from_modules(stack.frames[i]));
    }
  }
  const auto main_thread =
      std::ranges::find(threads, frame_thread_id.load(), &seen_thread::id);
  const auto main_frames = [&threads, &main_thread] {
    return main_thread != threads.end() ? main_thread->frames
                                        : std::vector<resolved_frame>{};
  };

  const std::string stamp = get_timestamp();
  const std::filesystem::path minidumps =
      game::get_appdata_path() / "minidumps";
  std::error_code ec;
  std::filesystem::create_directories(minidumps, ec);
  out.text_file = minidumps / ("freeze-" + stamp + ".txt");

  // First what asks nothing of Windows that a frozen game may hold: the
  // stacks by module and offset, on disk and in the launcher's note before
  // anything else is tried.
  const freeze_text_result first = freeze_text(threads, false);
  out.summary = first.summary;
  write_report_file(out.text_file, first.core);
  exception_log(false, "[Freeze] %s", first.summary.c_str());
  out.id = new_freeze_report();
  write_freeze_note(out.id, main_frames(), first);

  // Then the dump, which stops every thread but this one and the watchdog.
  freeze_dump = minidumps / ("freeze-" + stamp + ".dmp");
  if (!write_dump_file(freeze_dump)) {
    freeze_dump.clear();
  }
  attach_freeze_dump(out.id);

  // Then the names (functions, source lines), the scripts running and what
  // the game was doing: the same file and the same note again with them.
  for (seen_thread &thread : threads) {
    for (resolved_frame &frame : thread.frames) {
      add_symbols(frame);
    }
  }
  freeze_text_result full = freeze_text(threads, true);
  if (const int stops = dump_watchdog_stops.load(); stops > 0) {
    full.core += std::format("The dump stopped the watchdog ({}).\r\n", stops);
  }
  out.summary = full.summary;
  write_report_file(out.text_file, full.core + console_text());
  write_freeze_note(out.id, main_frames(), full);
  return out;
}

// The report is done, or gave up: the watchdog may ask for the next one, and
// frames that came back meanwhile take this one back.
void finish_freeze_report(const std::string &id) {
  std::string taken_back;
  long long stalled = -1;
  {
    const std::lock_guard lock(freeze_mutex);
    freeze_report_id = id;
    freeze_report_running = false;
    stalled = freeze_withdraw.exchange(-1);
    if (stalled >= 0) {
      taken_back = std::exchange(freeze_report_id, {});
    }
  }
  withdraw_freeze_report(taken_back, std::chrono::seconds(stalled));
}

// The report's thread: made when the game starts, so that asking for a
// report is setting an event.
void freeze_report_thread() {
  for (;;) {
    WaitForSingleObject(freeze_report_event, INFINITE);
    freeze_files files;
    try {
      files = write_freeze_files();
    } catch (const std::exception &error) {
      exception_log(false, "[Freeze] The freeze report failed: %s",
                    error.what());
    }
    finish_freeze_report(files.id);
    // The console's own lock may be what the main thread holds: its line
    // last, after everything that matters.
    if (!files.text_file.empty()) {
      console::print(std::format("^1[Freeze] The game has not finished a "
                                 "frame for {} seconds. {}\n^1[Freeze] "
                                 "Report: {}\n",
                                 FREEZE_TIME.count(), files.summary,
                                 files.text_file.string()));
    }
  }
}

// The watchdog's side. No lock and no memory: see freeze_report_event.
void start_freeze_report() {
  if (!freeze_report_event || freeze_report_running.exchange(true)) {
    return; // an earlier freeze's report is still being written
  }
  freeze_withdraw = -1;
  SetEvent(freeze_report_event);
}

// The game runs again, so locks and memory are safe to take.
void frames_came_back(const std::chrono::seconds stalled) {
  std::string taken_back;
  {
    const std::lock_guard lock(freeze_mutex);
    if (freeze_report_running) {
      freeze_withdraw = stalled.count();
    } else {
      taken_back = std::exchange(freeze_report_id, {});
    }
  }
  withdraw_freeze_report(taken_back, stalled);
  console::print(std::format("^3[Freeze] Frames came back after {} seconds: "
                             "it was a long stall, not a freeze. Its report "
                             "stays in the minidumps folder and is not sent.\n",
                             stalled.count()));
}

DWORD WINAPI log_freeze_close(LPVOID seconds) {
  exception_log(false,
                "[Freeze] No frame for %lld seconds: the game is closed, as "
                "after a crash",
                static_cast<long long>(reinterpret_cast<intptr_t>(seconds)));
  xerces_runtime::log_error("exception.freeze.closed",
                            "the game froze and was closed");
  return 0;
}

[[noreturn]] void close_frozen_game(std::chrono::seconds stalled) {
  crashed = true;
  // The lines for the logs are written by a thread of their own, waited for
  // one second: the log's lock or the heap's may be what the frozen game
  // holds, and nothing may stand between the watchdog and the end.
  if (const HANDLE writer = CreateThread(
          nullptr, 0, log_freeze_close,
          reinterpret_cast<LPVOID>(static_cast<intptr_t>(stalled.count())), 0,
          nullptr)) {
    WaitForSingleObject(writer, 1000);
  }
  TerminateProcess(GetCurrentProcess(), FREEZE_EXIT_CODE);
  std::abort();
}

bool loading_now() {
  const std::string phase = xerces_runtime::journal_phase_for_crash();
  return phase != "in_game" && phase != "frontend";
}

void watch_for_freezes() {
  watchdog_thread_id = GetCurrentThreadId();
  thread_describe = reinterpret_cast<thread_describe_fn>(GetProcAddress(
      GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  uint32_t last_frame = 0;
  auto last_progress = std::chrono::steady_clock::now();
  bool reported = false;
  uint32_t ticks = 0;
  for (;;) {
    std::this_thread::sleep_for(1s);
    release_walked_thread();
    if (const uint64_t started = crash_handler_started_ms.load();
        started != 0 && GetTickCount64() - started > CRASH_HANDLER_LIMIT_MS) {
      // Nothing before this: whatever the handler is stuck on may be what a
      // log line would wait for too.
      TerminateProcess(GetCurrentProcess(), crash_handler_code.load());
    }
    const uint32_t frame = frame_count.load();
    if (frame != last_frame || frame == 0) {
      if (reported) {
        frames_came_back(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - last_progress));
      }
      if (ticks++ % 30 == 0) {
        note_modules();
      }
      // Noted while the game runs: asking when it is frozen takes memory
      loading_noted = loading_now();
      last_frame = frame;
      last_progress = std::chrono::steady_clock::now();
      reported = false;
    } else if (!crashed) {
      const auto stalled = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - last_progress);
      if (!reported && stalled >= FREEZE_TIME) {
        reported = true;
        start_freeze_report();
      } else if (reported &&
                 stalled >= (loading_noted.load() ? FREEZE_CLOSE_LOADING
                                                  : FREEZE_CLOSE_TIME)) {
        close_frozen_game(stalled);
      }
    }
  }
}

void WINAPI set_unhandled_exception_filter_stub(LPTOP_LEVEL_EXCEPTION_FILTER) {
  // Don't register anything here...
}
} // namespace

[[noreturn]] void restart_game() {
  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info{};
  if (CreateProcessW(nullptr, GetCommandLineW(), nullptr, nullptr, FALSE, 0,
                     nullptr, nullptr, &startup_info, &process_info)) {
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
  }
  TerminateProcess(GetCurrentProcess(), exception_data.code);
  __assume(false);
}

void show_fatal_error(const std::string &message) {
  // Xerces: before the other threads are suspended, while their locks can
  // still be released. A text report and a dump of every thread stay on this
  // PC, in the minidumps folder (an engine error wrote neither before
  // 2026-11-12, so one could only be read from the console's last lines).
  crash_handler_code = 1;
  crash_handler_started_ms = GetTickCount64();
  const std::string plain = error_help::strip_colors(message);
  void *stack[xerces_core::crash_frame_cap]{};
  const USHORT frames = RtlCaptureStackBackTrace(
      1, static_cast<DWORD>(std::size(stack)), stack, nullptr);
  std::string report;
  if (!run_guarded([&report, &plain, &stack, frames] {
        report = fatal_text(plain, stack, frames);
      })) {
    report = "Xerces Fatal Error Report\r\n\r\nError: " + plain + "\r\n";
  }
  {
    const std::filesystem::path dir = game::get_appdata_path() / "minidumps";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string stamp = get_timestamp();
    std::string local = report;
    run_guarded([&local] { local += console_text(); });
    write_report_file(dir / ("fatal-" + stamp + ".txt"), local);
    // The note for the launcher before the dump: the dump asks the most of
    // a process that is already in trouble.
    write_fatal_report(plain, report);
    write_dump_file(dir / ("fatal-" + stamp + ".dmp"));
  }
  crash_handler_started_ms = 0;
  if (!game::quiet_crash() && !game::is_headless()) {
    utils::thread::suspend_other_threads();
    show_mouse_cursor();
    if (ask_restart(error_help::strip_colors(message) + "\n\n" +
                        crash_report_note(false),
                    L"The game hit an error and has to close.")) {
      restart_game();
    }
  }
  TerminateProcess(GetCurrentProcess(), 1);
}

struct component final : generic_component {
  DEFINE_COMPONENT_NAME("exception");

  component() {
    main_thread_id = GetCurrentThreadId();
    SetUnhandledExceptionFilter(exception_filter);

    std::filesystem::create_directories(game::get_appdata_path() / "minidumps");
  }

  void post_load() override {
    const utils::nt::library ntdll("ntdll.dll");
    using SetFilterFunc =
        fastcallPtr_t<void(LPTOP_LEVEL_EXCEPTION_FILTER filter)>;
    SetFilterFunc set_filter =
        ntdll.get_proc<SetFilterFunc>("RtlSetUnhandledExceptionFilter");

    set_filter(exception_filter);
    utils::hook::jump(set_filter, set_unhandled_exception_filter_stub);

    const utils::nt::library dbghelp = utils::nt::library::load("dbghelp.dll");
    if (dbghelp) {
      mini_dump_write_dump_hook.create(
          dbghelp.get_proc<decltype(mini_dump_write_dump_stub) *>(
              "MiniDumpWriteDump"),
          mini_dump_write_dump_stub);
    }

    AddVectoredExceptionHandler(1, crash_fix_exception_handler);
  }

  void post_unpack() override {
    scheduler::loop(
        [] {
          frame_thread_id = GetCurrentThreadId();
          ++frame_count;
        },
        scheduler::pipeline::main, 1s);
    // Xerces: which threads run the server and the renderer, for the reports
    scheduler::loop([] { server_thread_id = GetCurrentThreadId(); },
                    scheduler::pipeline::server, 1s);
    scheduler::loop([] { render_thread_id = GetCurrentThreadId(); },
                    scheduler::pipeline::renderer, 1s);
    scheduler::loop(note_settings, scheduler::pipeline::main, 5s);
    // Xerces: the freeze report's thread exists from the start, waiting, so
    // that the watchdog asks for a report without making a thread
    freeze_report_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (freeze_report_event) {
      utils::thread::create_named_thread("Freeze Report", freeze_report_thread)
          .detach();
    }
    utils::thread::create_named_thread("Freeze Watchdog", watch_for_freezes)
        .detach();

    scheduler::once(
        [] {
          game::cbuf::Cbuf_AddText(
              game::LOCAL_CLIENT_0,
              utils::string::va(
                  "dumpdir \"%s\"\n",
                  (game::get_appdata_path() / "minidumps").string().c_str()));
        },
        scheduler::pipeline::main);
  }
};
} // namespace exception

REGISTER_COMPONENT(exception::component)
