#include <std_include.hpp>

// xerces_out_of_memory (docs/DESIGN.md 5.6, 5.7): the engine's out-of-memory
// exit as a fatal error the player can read.
//
// The engine's handler, Sys_OutOfMemErrorInternal(file, line) (signature
// sys.out_of_mem_error_internal, feature oom_report), runs when one of its
// fixed memory pools is full, and when the C runtime's malloc fails in the
// engine's allocation wrappers. On 2026-11-05 it was the 2 GiB pool holding
// blocks 5 and 6 of every loaded fastfile, when a large custom map loaded
// with a large mod. The handler shows its own box only when a dvar is set (it
// is not), then calls exit(-1) on the thread that hit the limit: the C
// runtime destroyed static objects while the main thread still drew loading
// frames, and the main thread crashed on one. The player saw the game vanish.
//
// The detour replaces the whole handler and, like it, never returns (DESIGN.md
// 5.7: the error stays fatal). It logs loader.out_of_memory with the zone
// still loading, the map, the mods, the journal phase and the console's
// last lines; marks the crash for the journal and leaves its phase, so the
// launcher counts a crash and nothing writes clean_exit; suspends the other
// threads; tells the player what happened (not with -headless or
// -quiet-crash); and ends the process with TerminateProcess, so no static
// destructor runs under the main thread. The exit code stays 0xFFFFFFFF, the
// engine's exit(-1).

#include <component/exception.hpp>
#include <intrin.h>

#include <component/mod_stack.hpp>
#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>
#include <utils/hook.hpp>
#include <utils/thread.hpp>

#include <xerces_core/db_pool.hpp>
#include <xerces_core/journal.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/out_of_memory.hpp>
#include <xerces_core/sigruntime.hpp>

#include "db_pool.hpp"
#include "loader.hpp"
#include "runtime.hpp"
#include "signatures.hpp"

namespace xerces_out_of_memory {
namespace {
constexpr UINT exit_code = 0xFFFFFFFF; // the engine's exit(-1)
// The console tail holds 16 KiB: search all of it for the zone, log the end.
constexpr std::size_t console_lines_searched = 1000;
constexpr std::size_t console_lines_logged = 60;

// What MinHook moves into its trampoline: push rdi; sub rsp, 40h. The
// signature matched them when it resolved; they are checked again right
// before the hook is written (DESIGN.md 4.3), so a patch another component
// made in between is never relocated.
constexpr std::array<std::uint8_t, 6> expected_entry = {0x40, 0x57, 0x48,
                                                        0x83, 0xEC, 0x40};

utils::hook::detour handler_hook;

// The thread that is reporting; 0 until one is.
std::atomic<DWORD> reporter{0};

[[noreturn]] void end_process() {
  TerminateProcess(GetCurrentProcess(), exit_code);
  for (;;) {
    Sleep(INFINITE);
  }
}

std::wstring wide(const std::string &text) {
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
  if (size > 0) {
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size);
  }
  return out;
}

std::string hex(std::uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%llX",
                static_cast<unsigned long long>(value));
  return text;
}

std::string dvar(const char *name) {
  return xerces_core::json::valid_utf8(game::get_dvar_string(name).value_or(""),
                                       2000);
}

// The mods' names from this launch's plan file (by its id, hex only, so the
// id can never leave the plans folder); empty without a plan.
std::map<std::string, std::string> plan_mod_names() {
  const std::string id = xerces_runtime::flag_value("-xerces-plan");
  const auto valid = std::ranges::all_of(id, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
  if (id.empty() || id.size() > 128 || !valid) {
    return {};
  }
  return xerces_core::read_plan_mod_names(xerces_runtime::read_file(
      xerces_runtime::home() / "config" / "plans" / (id + ".json")));
}

// Where in the game the handler was called from, as an RVA ("" outside it):
// which pool or allocation wrapper ran out. Logged only: Xerces code uses no
// offsets, so nothing here decides by it.
std::string caller_rva(const void *return_address) {
  const auto base = game::get_engine_base();
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  const auto *nt =
      reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
  const auto at = reinterpret_cast<std::uintptr_t>(return_address);
  if (at < base || at >= base + nt->OptionalHeader.SizeOfImage) {
    return {};
  }
  return hex(at - base);
}

// The calling thread's registers as the handler was entered (the first thing
// the stub does): the engine's pool allocator still holds the pool's
// descriptor in rbx when its size check fails.
thread_local CONTEXT caller_registers{};

bool readable(const void *start, const std::size_t size) {
  const auto *at = static_cast<const std::uint8_t *>(start);
  const auto *end = at + size;
  while (at < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(at, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
      return false;
    }
    at = static_cast<const std::uint8_t *>(info.BaseAddress) + info.RegionSize;
  }
  return true;
}

// How many bytes the pool was short, when the handler was called from the
// engine's pool allocator: its size check stores that (sub rax, rcx; mov
// [rip+x], rax), then clears edx, loads rcx and calls the handler. The store
// is found from the return address and every byte of that sequence is
// checked first; any other caller gives nothing.
std::optional<std::int64_t> pool_shortfall(const void *return_address) {
  const auto *ret = static_cast<const std::uint8_t *>(return_address);
  const std::uint8_t *code = ret - 24;
  if (!readable(code, 24)) {
    return std::nullopt;
  }
  const bool matches =
      code[0] == 0x48 && code[1] == 0x2B && code[2] == 0xC1 && // sub rax, rcx
      code[3] == 0x48 && code[4] == 0x89 && code[5] == 0x05 && // mov [rip+x]
      code[10] == 0x33 && code[11] == 0xD2 &&                  // xor edx, edx
      code[12] == 0x48 && code[13] == 0x8D && code[14] == 0x0D && // lea rcx
      code[19] == 0xE8;                                           // call
  if (!matches) {
    return std::nullopt;
  }
  std::int32_t displacement = 0;
  std::memcpy(&displacement, code + 6, sizeof(displacement));
  const std::uint8_t *stored = code + 10 + displacement;
  if (!readable(stored, sizeof(std::int64_t))) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  std::memcpy(&value, stored, sizeof(value));
  return value;
}

// Logs loader.out_of_memory; returns what the player is told.
xerces_core::out_of_memory_message report(const void *return_address) {
  const auto lines = xerces_runtime::console_last_lines(console_lines_searched);
  const std::string zone = xerces_core::last_loading_zone(lines);
  const std::string fs_game = dvar("fs_game");
  const std::string fs_extramods = dvar("fs_extramods");
  std::map<std::string, std::string> names;
  try {
    names = plan_mod_names();
  } catch (...) {
    // Folders instead of names.
  }
  const std::vector<std::string> mods =
      xerces_core::loaded_mods(fs_game, fs_extramods, names);
  xerces_core::out_of_memory_message text =
      xerces_core::out_of_memory_text(zone, mods);

  namespace json = xerces_core::json;
  std::string console = "[";
  const std::size_t first = lines.size() > console_lines_logged
                                ? lines.size() - console_lines_logged
                                : 0;
  for (std::size_t i = first; i < lines.size(); ++i) {
    console += (i == first ? "" : ",");
    // Game text may hold CP-1252 bytes; one invalid byte would make the log
    // drop the event's whole data.
    console += json::quote(json::valid_utf8(lines[i], 600));
  }
  console += "]";
  std::string mod_array = "[";
  for (std::size_t i = 0; i < mods.size(); ++i) {
    mod_array += (i == 0 ? "" : ",") + json::quote(mods[i]);
  }
  mod_array += "]";

  // mapname still says core_frontend while a level loads (2026-11-05), so
  // the custom map being loaded comes first.
  const std::string map_name = xerces_core::out_of_memory_map(
      mod_stack::active_usermap(), xerces_loader::current_map());

  // With the larger zone memory: how full the DB main pool was (its committed
  // pages, as the once-a-second watch reads them), the most it held and its
  // size, so a report shows whether this pool was the one that filled up.
  std::string pool;
  if (const auto usage = xerces_db_pool::main_usage()) {
    constexpr std::uint64_t page = xerces_core::db_pool::page_size;
    pool =
        ",\"pool_mib\":" + std::to_string(usage->pages * page >> 20) +
        ",\"pool_peak_mib\":" + std::to_string(usage->peak_pages * page >> 20) +
        ",\"pool_size_mib\":" + std::to_string(usage->size >> 20);
    // Every pool in use: the engine's check is the same for all of them.
    pool += ",\"pools\":" + xerces_db_pool::pools_json();
    // The pool whose descriptor rbx held: the one that was full (on
    // 2026-11-07 the streamed-texture pool, with the main pool at 2753 of
    // 4096 MiB).
    if (const auto index =
            xerces_db_pool::pool_of_descriptor(caller_registers.Rbx)) {
      for (const auto &state : xerces_db_pool::pools()) {
        if (state.index == *index) {
          pool += ",\"failed_pool\":" + xerces_db_pool::pool_json(state);
        }
      }
    }
  }
  if (const auto missing = pool_shortfall(return_address)) {
    pool += ",\"shortfall\":" + std::to_string(*missing) +
            ",\"shortfall_mib\":" + std::to_string(*missing >> 20);
  }

  xerces_runtime::log_error(
      "loader.out_of_memory", text.summary,
      "{\"zone\":" + json::quote(zone) + ",\"map\":" + json::quote(map_name) +
          ",\"fs_game\":" + json::quote(fs_game) + ",\"fs_extramods\":" +
          json::quote(fs_extramods) + ",\"mods\":" + mod_array + ",\"phase\":" +
          json::quote(xerces_core::to_string(xerces_runtime::journal_phase())) +
          ",\"caller\":" + json::quote(caller_rva(return_address)) +
          ",\"exit_code\":" + std::to_string(exit_code) + pool +
          ",\"console\":" + console + "}");
  return text;
}

// The engine's Sys_OutOfMemErrorInternal(const char *file, int line). Its
// callers pass "" and 0 in this build, and the engine's body never reads
// them, so neither is used here.
[[noreturn]] void out_of_mem_error_stub(const char * /*file*/, int /*line*/) {
  RtlCaptureContext(&caller_registers);
  const void *return_address = _ReturnAddress();
  const DWORD self = GetCurrentThreadId();
  DWORD expected = 0;
  if (!reporter.compare_exchange_strong(expected, self)) {
    if (expected == self) {
      end_process(); // ran out again while reporting: end now
    }
    for (;;) {
      Sleep(INFINITE); // the first thread reports and ends the process
    }
  }

  // First, so nothing writes clean_exit from here on; the journal keeps its
  // phase (loading_map, usually) and the launcher counts a crash.
  xerces_runtime::note_crash();

  // Nothing may leave this function: an exception would unwind into the
  // engine's frames. If the report fails (ordinary memory gone too, say), the
  // player gets the short text, which blames nothing: the engine's malloc
  // wrappers end here too.
  constexpr const char *short_title = "Black Ops III ran out of memory";
  constexpr const char *short_body =
      "The game ran out of memory. The details may be in Xerces' game log.";
  // Read before the other threads stop: install() made both flags' first
  // reads (argparse, allocations), so these only read the cached values.
  bool quiet = false;
  try {
    quiet = game::quiet_crash() || game::is_headless();
  } catch (...) {
    // Show the box.
  }
  xerces_core::out_of_memory_message text;
  std::wstring title;
  std::wstring body;
  try {
    text = report(return_address);
    title = wide(text.title);
    body = wide(text.body);
  } catch (...) {
    text = {};
    title.clear();
    body.clear();
  }

  // The other threads are left running. They used to be suspended here, but a
  // thread stopped inside an allocation, a DLL load or a printf holds a lock
  // the message box needs, and then there is no box and the game stays frozen
  // for good (the freeze watchdog is one of the stopped threads). A second
  // thread that runs out of memory meanwhile waits behind the reporter flag,
  // and the process ends right after the box.
  if (quiet) {
    std::fprintf(stderr, "%s\n%s\n",
                 text.title.empty() ? short_title : text.title.c_str(),
                 text.body.empty() ? short_body : text.body.c_str());
    std::fflush(stderr);
  } else {
    while (ShowCursor(TRUE) < 0) {
    }
    // The box waits for the player; no freeze meanwhile
    const exception::modal_box waiting;
    if (title.empty() || body.empty()) {
      MessageBoxA(nullptr, short_body, short_title,
                  MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    } else {
      MessageBoxW(nullptr, body.c_str(), title.c_str(),
                  MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    }
  }
  end_process();
}

void install() {
  namespace ids = xerces_core::oom_report_signatures;
  if (!xerces_signatures::feature_enabled("oom_report")) {
    xerces_runtime::log_warn(
        "xerces.feature.off",
        xerces_signatures::feature_message(
            "oom_report", "The out-of-memory report",
            "The game still runs; if it runs out of memory it closes without "
            "saying why."),
        R"({"feature":"oom_report"})");
    return;
  }
  // feature_enabled requires the id resolved (signatures.cpp); looked up again
  // so an address is never read from an empty optional.
  const std::optional<std::uintptr_t> address =
      xerces_signatures::address(ids::out_of_mem_error);
  if (!address) {
    xerces_runtime::log_error(
        "xerces.feature.off",
        "The out-of-memory report is off: the signature registry has no "
        "resolved address for sys.out_of_mem_error_internal.",
        R"({"feature":"oom_report","signature":"sys.out_of_mem_error_internal"})");
    return;
  }
  if (std::memcmp(reinterpret_cast<const void *>(*address),
                  expected_entry.data(), expected_entry.size()) != 0) {
    xerces_runtime::log_error(
        "xerces.feature.off",
        "The out-of-memory report is off: the engine's handler does not start "
        "with the instructions the hook moves. The game still runs; if it "
        "runs out of memory it closes without saying why.",
        R"({"feature":"oom_report","signature":"sys.out_of_mem_error_internal"})");
    return;
  }
  try {
    // The flags' first reads parse the command line (argparse, allocations,
    // exceptions it catches): done here, so the handler only reads the cached
    // values, before it stops the other threads.
    static_cast<void>(game::quiet_crash());
    static_cast<void>(game::is_headless());
    handler_hook.create(*address,
                        reinterpret_cast<void *>(&out_of_mem_error_stub));
  } catch (const std::exception &e) {
    xerces_runtime::log_error("xerces.feature.off",
                              std::string("The out-of-memory report is off: ") +
                                  e.what(),
                              R"({"feature":"oom_report"})");
    return;
  }
  xerces_runtime::log_info("xerces.feature.on",
                           "Out-of-memory report installed from signatures",
                           R"({"feature":"oom_report"})");
}
} // namespace

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_out_of_memory");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client()) {
      return;
    }
    install();
  }
};
} // namespace xerces_out_of_memory

REGISTER_COMPONENT(xerces_out_of_memory::component)
