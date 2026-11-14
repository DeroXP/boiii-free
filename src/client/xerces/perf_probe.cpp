#include <std_include.hpp>

// The XERCES_BENCH frame probe and perf.swapchain (docs/DESIGN.md 12.2). See
// perf_probe.hpp.

#include "flip_model.hpp"
#include "perf.hpp"
#include "perf_probe.hpp"
#include "runtime.hpp"

#include <psapi.h>

#include <component/command.hpp>
#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>
#include <utils/thread.hpp>

#include <xerces_core/frame_ring.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/log.hpp>
#include <xerces_core/perf_bench.hpp>

namespace xerces_perf {
namespace probe_detail {
std::atomic_bool armed{false};
} // namespace probe_detail

namespace {
namespace bench = xerces_core::bench;
namespace json = xerces_core::json;
using bench::frame_sample;

// 2^19 Presents and main frames: the launcher's ten-minute recording at up to
// 870 FPS. The main ring held 2^17, under ten minutes above 218 FPS, and the
// start of a run at the player's own 240 was lost (review, 2026-11-13). Set
// aside only when recording.
constexpr unsigned frame_ring_log2 = 19;
constexpr unsigned main_ring_log2 = 19;
// A map load that never reaches the level is given up after this.
constexpr double map_load_limit_ms = 600'000;

// Set aside once when the probe is armed and never freed: the render thread
// can still present while the game shuts down.
std::atomic<xerces_core::frame_ring<frame_sample> *> frames{nullptr};
xerces_core::frame_ring<std::int64_t> *mains = nullptr;
std::int64_t frequency = 1; // set before `frames`, read after it
std::filesystem::path out_dir;
std::optional<bench::auto_run> auto_spec;

std::atomic<std::uint32_t> present_thread{0};
std::atomic<std::uintptr_t> game_window{0}; // the swap chain's window
std::atomic<std::uint32_t> chain_switches{0};
std::atomic_bool window_unfocused{false};

// Written once on the render thread before swapchain_known (release).
bench::swapchain_info swapchain_seen{};
std::atomic_bool swapchain_known{false};

// ------------------------------------------- the render thread's own state
IDXGISwapChain *game_chain = nullptr;
IDXGISwapChain *other_chain = nullptr; // last chain found not to be the game's
HWND game_window_render = nullptr;

std::uint32_t clamp32(std::int64_t value) {
  return value <= 0                         ? 0
         : value > std::int64_t(UINT32_MAX) ? UINT32_MAX
                                            : static_cast<std::uint32_t>(value);
}

// A swap chain other than the one first seen: the game's if it presents to
// the game's window (the game made a new chain), else ignored from then on.
bool adopt(IDXGISwapChain *chain) {
  DXGI_SWAP_CHAIN_DESC desc{};
  if (game_window_render && SUCCEEDED(chain->GetDesc(&desc)) &&
      desc.OutputWindow == game_window_render) {
    game_chain = chain;
    chain_switches.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  other_chain = chain;
  return false;
}

std::string decimal(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.3f", value);
  return buffer;
}

// ---------------------------------------------- the main thread's own state
bool in_level_active() {
  return game::com::Com_IsInGame() && !game::com::Com_IsRunningUILevel() &&
         game::cg::clientUIActives->actives[0].connectionState ==
             game::connstate_t::ACTIVE;
}

std::string current_map() {
  for (const char *name : {"mapname", "ui_mapname"}) {
    if (auto value = game::get_dvar_string(name); value && !value->empty()) {
      return json::valid_utf8(*value, 64);
    }
  }
  return {};
}

// What the meta file records about the engine's settings; a dvar this build
// of the engine does not have is written as null.
std::vector<std::pair<std::string, std::optional<std::string>>> read_dvars() {
  std::vector<std::pair<std::string, std::optional<std::string>>> out;
  for (const char *name :
       {"com_maxfps", "r_vsync", "r_fullscreen", "r_mode", "mapname", "fs_game",
        "fs_extramods", "sv_running", "cl_paused"}) {
    std::optional<std::string> value;
    if (const auto dvar = game::get_dvar(name); dvar.nonnull()) {
      if (const char *text = game::Dvar_DisplayableValue(dvar)) {
        value = text;
      }
    }
    out.emplace_back(name, std::move(value));
  }
  return out;
}

// Console output only once the menu is up: early prints crashed the game
// during startup (CLAUDE.md).
void say(const std::string &text) {
  if (menu_ready_qpc() != 0) {
    printf("%s", text.c_str());
  }
}

void log_async(xerces_core::log_level level, std::string event,
               std::string message, std::string data) {
  scheduler::once([=] { xerces_runtime::log(level, event, message, data); },
                  scheduler::async);
}

struct run_state {
  bool active = false;
  std::string name;
  std::string kind;
  std::string map;
  std::string started_utc;
  std::int64_t t0 = 0;
  std::uint64_t frame_begin = 0;
  std::uint64_t main_begin = 0;
  std::uint32_t focus_losses = 0;
  std::uint32_t switches_at_begin = 0;
  std::uint32_t stalls_dropped_at_begin = 0;
  std::vector<std::pair<std::string, std::optional<std::string>>> dvars;
};

struct finished_run {
  run_state run;
  std::int64_t t1 = 0;
  std::uint64_t frame_end = 0;
  std::uint64_t main_end = 0;
  std::uint32_t chain_switches = 0;
  std::uint32_t stalls_dropped_end = 0;
};

struct map_load {
  bool pending = false;
  bool left_level = false; // saw a frame not active in a level since
  std::string map;
  std::int64_t t0 = 0;
};

struct auto_state {
  bool started = false;
  bool done = false;
  std::int64_t start_at = 0;
  std::int64_t end_at = 0;
  std::int64_t left_level_at = 0; // first frame outside the level, 0 = inside
};

run_state run;    // main thread only
map_load load;    // main thread only
auto_state autos; // main thread only
bool was_focused = true;

std::int64_t seconds_to_ticks(double seconds) {
  return static_cast<std::int64_t>(seconds * double(frequency));
}

std::string stalls_csv(const run_state &r, std::int64_t t1); // below

// ------------------------------------------------- writing (async thread)
std::filesystem::path unique_base(const std::string &name) {
  std::error_code ec;
  for (int n = 1; n < 1000; ++n) {
    const std::string stem = n == 1 ? name : name + "-" + std::to_string(n);
    const auto base = out_dir / stem;
    if (!std::filesystem::exists(base.string() + ".meta.json", ec) &&
        !std::filesystem::exists(base.string() + ".frames.csv", ec)) {
      return base;
    }
  }
  return out_dir / (name + "-" + std::to_string(GetTickCount64()));
}

void write_run(const finished_run &done) {
  const run_state &r = done.run;
  const bench::run_clock clock{r.t0, frequency};

  // The frame before the run gives the first row its dt.
  std::vector<frame_sample> copied;
  std::optional<std::int64_t> previous;
  std::vector<frame_sample> in_run;
  std::uint64_t frames_lost = 0;
  if (auto *ring = frames.load(std::memory_order_acquire)) {
    const std::uint64_t from = r.frame_begin > 0 ? r.frame_begin - 1 : 0;
    const std::uint64_t first = ring->copy(from, done.frame_end, copied);
    frames_lost = first > r.frame_begin ? first - r.frame_begin : 0;
    for (const frame_sample &sample : copied) {
      if (sample.qpc < r.t0) {
        previous = sample.qpc;
      } else if (sample.qpc < done.t1) {
        in_run.push_back(sample);
      }
    }
  }

  std::vector<std::int64_t> main_copied;
  std::optional<std::int64_t> main_previous;
  std::vector<std::int64_t> main_in_run;
  std::uint64_t main_lost = 0;
  {
    const std::uint64_t from = r.main_begin > 0 ? r.main_begin - 1 : 0;
    const std::uint64_t first = mains->copy(from, done.main_end, main_copied);
    main_lost = first > r.main_begin ? first - r.main_begin : 0;
    for (const std::int64_t stamp : main_copied) {
      if (stamp < r.t0) {
        main_previous = stamp;
      } else if (stamp < done.t1) {
        main_in_run.push_back(stamp);
      }
    }
  }

  bench::run_meta meta;
  meta.run = r.name;
  meta.kind = r.kind;
  meta.started_utc = r.started_utc;
  meta.map = r.map;
  meta.qpc_frequency = frequency;
  meta.t0_qpc = r.t0;
  meta.t1_qpc = done.t1;
  meta.frames = in_run.size();
  meta.frames_lost = frames_lost;
  meta.main_frames = main_in_run.size();
  meta.main_lost = main_lost;
  meta.stalls_dropped = done.stalls_dropped_end - r.stalls_dropped_at_begin;
  meta.pid = GetCurrentProcessId();
  meta.present_thread = present_thread.load();
  meta.main_thread = main_thread_id();
  meta.focus_losses = r.focus_losses;
  meta.chain_switches = done.chain_switches - r.switches_at_begin;
  if (swapchain_known.load(std::memory_order_acquire)) {
    meta.swapchain = swapchain_seen;
  }
  meta.dvars = r.dvars;

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  const auto base = unique_base(r.name);
  const std::string stem = base.filename().string();
  const bool ok =
      xerces_runtime::write_file(
          base.string() + ".frames.csv",
          bench::frames_csv(in_run, previous, clock, frames_lost)) &&
      xerces_runtime::write_file(
          base.string() + ".main.csv",
          bench::main_csv(main_in_run, main_previous, clock, main_lost)) &&
      xerces_runtime::write_file(base.string() + ".meta.json",
                                 bench::run_meta_json(meta)) &&
      xerces_runtime::write_file(base.string() + ".stalls.csv",
                                 stalls_csv(r, done.t1));

  const bench::frames_summary summary =
      bench::summarize(in_run, previous, clock, done.t1);
  const std::string data =
      "{\"run\":" + json::quote(stem) + ",\"kind\":" + json::quote(r.kind) +
      ",\"dir\":" + json::quote(out_dir.string()) +
      ",\"written\":" + (ok ? "true" : "false") +
      ",\"frames\":" + std::to_string(summary.frames) +
      ",\"frames_lost\":" + std::to_string(frames_lost) +
      ",\"main_frames\":" + std::to_string(main_in_run.size()) +
      ",\"duration_ms\":" + decimal(summary.duration_ms) +
      ",\"mean_fps\":" + decimal(summary.mean_fps) +
      ",\"max_dt_ms\":" + decimal(summary.max_dt_ms) + "}";
  if (ok) {
    xerces_runtime::log_info("perf.bench.written",
                             stem + ": " + std::to_string(summary.frames) +
                                 " frames, " + decimal(summary.mean_fps) +
                                 " FPS mean",
                             data);
  } else {
    xerces_runtime::log_error(
        "perf.bench.failed",
        "Could not write the run's files to " + out_dir.string(), data);
  }
}

// ------------------------------------------ the stall sampler (own thread)
// While the main thread is inside one long frame, this thread now and then
// stops it for a moment and notes where it is (its call stack as raw
// addresses). The run's <run>.stalls.csv then says which engine code the long
// frames were in. Nothing here allocates while the main thread is stopped:
// the samples go into a fixed array, and the addresses are turned into
// module+offset only when a run is written.
constexpr std::size_t stall_stack_depth = 14;
constexpr std::size_t stall_sample_max = 20000;
double stall_threshold_ms = 40.0; // a frame this long is sampled; XERCES_BENCH_STALL_MS
constexpr double stall_interval_ms = 8.0;   // between samples of one frame

struct stall_sample {
  std::int64_t qpc = 0;
  std::int64_t stalled_ticks = 0; // the frame's age when sampled
  std::uint32_t depth = 0;
  std::uint64_t pc[stall_stack_depth]{};
};

std::atomic<std::int64_t> last_main_frame{0};
std::atomic_bool sampling{false}; // a run is going (mirror of run.active)
stall_sample *stall_samples = nullptr;         // stall_sample_max entries
// Written by the sampler, and set back to 0 by begin_run while no run is
// sampled and none waits to be written
std::atomic<std::uint32_t> stall_count{0};
// Samples a full array could not take: in the run's meta.json, so a
// stalls.csv that stops part-way does not read as "the stutter stopped"
std::atomic<std::uint32_t> stalls_dropped{0};
std::atomic<int> writes_pending{0}; // end_run's writes not done yet

// The loaded modules' function tables, noted by the sampler while the main
// thread runs, never while it is stopped. RtlLookupFunctionEntry takes
// ntdll's function-table lock, which the stopped main thread may hold (in a
// DLL's load or unload): the sampler would then wait for ever and the game
// stay stopped (a review, 2026-11-13). The lookup below takes no lock. Only
// the sampler's thread touches these.
struct unwind_module {
  DWORD64 base = 0;
  DWORD64 size = 0;
  const RUNTIME_FUNCTION *table = nullptr;
  DWORD count = 0;
};
std::array<unwind_module, 512> unwind_modules{};
std::size_t unwind_module_count = 0;

// Reads the module's headers; the entry is stored only once all are read.
// Called under a guard of its own (a module may be unloaded between the list
// and the read), so one bad module costs only itself.
__declspec(noinline) bool add_unwind_module(const DWORD64 base) {
  if (unwind_module_count >= unwind_modules.size()) {
    return false;
  }
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return false;
  }
  const auto *nt =
      reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    return false;
  }
  const IMAGE_DATA_DIRECTORY exceptions =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  const DWORD64 size = nt->OptionalHeader.SizeOfImage;
  if (exceptions.VirtualAddress == 0 || exceptions.Size == 0 ||
      static_cast<DWORD64>(exceptions.VirtualAddress) + exceptions.Size >
          size) {
    return false;
  }
  unwind_modules[unwind_module_count] = {
      base, size,
      reinterpret_cast<const RUNTIME_FUNCTION *>(base +
                                                 exceptions.VirtualAddress),
      static_cast<DWORD>(exceptions.Size / sizeof(RUNTIME_FUNCTION))};
  ++unwind_module_count;
  return true;
}

void add_unwind_module_guarded(const DWORD64 base) {
  __try {
    add_unwind_module(base);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// The game's image (loaded by loader::load_binary) goes first, from its base:
// most of the main thread's frames are in it. It is in Windows' list too,
// which only adds a second entry that is never reached.
__declspec(noinline) void read_unwind_modules() {
  unwind_module_count = 0;
  add_unwind_module_guarded(game::get_engine_base());
  HMODULE handles[512]{};
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles),
                          &needed)) {
    return;
  }
  const std::size_t count =
      std::min<std::size_t>(needed / sizeof(HMODULE), std::size(handles));
  for (std::size_t i = 0; i < count; ++i) {
    add_unwind_module_guarded(reinterpret_cast<DWORD64>(handles[i]));
  }
}

void note_unwind_modules() {
  __try {
    read_unwind_modules();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// The function entry for pc from the noted tables (a binary search, no lock),
// and the module it is in
__declspec(noinline) const RUNTIME_FUNCTION *
find_function_entry(const DWORD64 pc, const unwind_module *&owner) {
  for (std::size_t m = 0; m < unwind_module_count; ++m) {
    const unwind_module &module = unwind_modules[m];
    if (pc < module.base || pc >= module.base + module.size) {
      continue;
    }
    const auto rva = static_cast<DWORD>(pc - module.base);
    std::size_t low = 0;
    std::size_t high = module.count;
    while (low < high) {
      const std::size_t mid = (low + high) / 2;
      const RUNTIME_FUNCTION &entry = module.table[mid];
      if (rva < entry.BeginAddress) {
        high = mid;
      } else if (rva >= entry.EndAddress) {
        low = mid + 1;
      } else {
        const RUNTIME_FUNCTION *found = &entry;
        // Unwind data with bit 0 set is the image offset (plus one) of the
        // primary entry, which RtlLookupFunctionEntry follows
        // (RUNTIME_FUNCTION_INDIRECT): so does this.
        if ((found->UnwindData & 1) != 0) {
          const DWORD64 primary = found->UnwindData - 1;
          if (primary + sizeof(RUNTIME_FUNCTION) > module.size) {
            return nullptr;
          }
          found =
              reinterpret_cast<const RUNTIME_FUNCTION *>(module.base + primary);
        }
        owner = &module;
        return found;
      }
    }
    return nullptr;
  }
  return nullptr;
}

// The stopped thread's stack: its base and its committed limit (which moves
// as the stack grows), from its TEB
struct stack_range {
  DWORD64 low = 0;
  DWORD64 high = 0;
};

__declspec(noinline) bool read_stack_range(const DWORD64 teb,
                                           stack_range &out) {
  const auto *tib = reinterpret_cast<const NT_TIB *>(teb);
  out.low = reinterpret_cast<DWORD64>(tib->StackLimit);
  out.high = reinterpret_cast<DWORD64>(tib->StackBase);
  return out.low != 0 && out.low < out.high;
}

// The thread's TEB (ThreadBasicInformation), 0 when it cannot be read
DWORD64 thread_teb(const HANDLE thread) {
  struct basic_information {
    LONG exit_status;
    PVOID teb;
    PVOID process_id;
    PVOID thread_id;
    ULONG_PTR affinity;
    LONG priority;
    LONG base_priority;
  };
  using query_fn = LONG(WINAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  static const auto query = reinterpret_cast<query_fn>(GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
  basic_information info{};
  if (!query || query(thread, 0, &info, sizeof(info), nullptr) < 0) {
    return 0;
  }
  return reinterpret_cast<DWORD64>(info.teb);
}

// Whether RtlVirtualUnwind can unwind this frame reading only the stopped
// thread's stack and the module's unwind data: the stack pointer inside the
// stack, the unwind data (and its chain) inside the module, and the frame
// register inside the stack once the prologue has set it (the saved registers
// are read from there). A fault would be handled on the sampler's thread, and
// Windows' handling looks up function tables and handlers under locks the
// stopped main thread may hold (a review, 2026-11-14): so the walk stops
// instead of faulting.
__declspec(noinline) bool unwindable(const CONTEXT &context,
                                     const RUNTIME_FUNCTION &function,
                                     const unwind_module &module,
                                     const stack_range &stack) {
  if (context.Rsp < stack.low || context.Rsp + 8 > stack.high ||
      (context.Rsp & 7) != 0) {
    return false;
  }
  const DWORD64 registers[16] = {
      context.Rax, context.Rcx, context.Rdx, context.Rbx,
      context.Rsp, context.Rbp, context.Rsi, context.Rdi,
      context.R8,  context.R9,  context.R10, context.R11,
      context.R12, context.R13, context.R14, context.R15};
  const DWORD64 offset = context.Rip - (module.base + function.BeginAddress);
  const RUNTIME_FUNCTION *entry = &function;
  for (int link = 0; link < 8; ++link) {
    const DWORD64 unwind = entry->UnwindData;
    if (unwind == 0 || unwind + 4 > module.size) {
      return false;
    }
    const auto *info =
        reinterpret_cast<const std::uint8_t *>(module.base + unwind);
    // Header (4 bytes), then the codes, 2 bytes each, an even count of them
    const DWORD64 codes_end = unwind + 4 + ((info[2] + 1u) & ~1u) * 2u;
    if (codes_end > module.size) {
      return false;
    }
    // A chained entry's primary is past its prologue
    if (const unsigned reg = info[3] & 0x0F;
        reg != 0 && (link > 0 || offset >= info[1])) {
      const DWORD64 frame =
          registers[reg] - static_cast<DWORD64>(info[3] >> 4) * 16;
      if (frame < stack.low || frame >= stack.high) {
        return false;
      }
    }
    if (((info[0] >> 3) & UNW_FLAG_CHAININFO) == 0) {
      return true;
    }
    if (codes_end + sizeof(RUNTIME_FUNCTION) > module.size) {
      return false;
    }
    entry = reinterpret_cast<const RUNTIME_FUNCTION *>(module.base + codes_end);
  }
  return false;
}

// Unwinds into out.pc while each frame checks out (unwindable); the guard is
// only a last resort. Without the thread's stack bounds only the pc it stopped
// at is kept. Every read is in a call: clang's __try guards calls only.
void walk_stack(CONTEXT &context, const DWORD64 teb, stall_sample &out) {
  __try {
    stack_range stack{};
    const bool bounded = teb != 0 && read_stack_range(teb, stack);
    while (out.depth < stall_stack_depth && context.Rip) {
      out.pc[out.depth++] = context.Rip;
      const unwind_module *module = nullptr;
      const RUNTIME_FUNCTION *function =
          bounded ? find_function_entry(context.Rip, module) : nullptr;
      if (!function || !unwindable(context, *function, *module, stack)) {
        break;
      }
      const DWORD64 before = context.Rsp;
      PVOID handler_data = nullptr;
      DWORD64 establisher_frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, module->base, context.Rip,
                       const_cast<PRUNTIME_FUNCTION>(function), &context,
                       &handler_data, &establisher_frame, nullptr);
      if (context.Rsp <= before) {
        break; // a frame that does not move up the stack: garbage
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void sample_main_thread(HANDLE thread, DWORD64 teb, std::int64_t now,
                        std::int64_t age) {
  const std::uint32_t slot = stall_count.load(std::memory_order_relaxed);
  if (slot >= stall_sample_max) {
    stalls_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  stall_sample &out = stall_samples[slot];
  out = {};
  CONTEXT context{};
  // The unwinder reads the integer registers too (a frame pointer, the saved
  // ones): a control-only context made it fault (beta.16).
  context.ContextFlags = CONTEXT_FULL;
  if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
    return;
  }
  if (GetThreadContext(thread, &context)) {
    walk_stack(context, teb, out);
  }
  ResumeThread(thread);
  if (out.depth > 0) {
    out.qpc = now;
    out.stalled_ticks = age;
    stall_count.store(slot + 1, std::memory_order_release);
  }
}

void stall_sampler() {
  HANDLE thread = nullptr;
  DWORD64 teb = 0;
  std::uint32_t thread_id = 0;
  std::int64_t last_sample = 0;
  ULONGLONG modules_noted = 0;
  for (;;) {
    Sleep(4);
    // The function tables, every 2 s, while the main thread runs
    if (const ULONGLONG tick = GetTickCount64(); tick - modules_noted >= 2000) {
      note_unwind_modules();
      modules_noted = tick;
    }
    const std::int64_t last = last_main_frame.load(std::memory_order_relaxed);
    if (last == 0) {
      continue; // no main frame yet (start-up, loading)
    }
    if (const std::uint32_t id = main_thread_id(); id != thread_id) {
      if (thread) {
        CloseHandle(thread);
      }
      thread_id = id;
      thread = id ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_LIMITED_INFORMATION,
                               FALSE, id)
                  : nullptr;
      teb = thread ? thread_teb(thread) : 0;
    }
    if (!thread) {
      continue;
    }
    const std::int64_t now = probe_clock();
    const double age_ms = bench::ticks_to_ms(now - last, frequency);
    // A map load or a lost window is one huge "frame": not sampled.
    if (age_ms < stall_threshold_ms || age_ms > 5000.0 ||
        !sampling.load(std::memory_order_relaxed)) {
      continue;
    }
    if (bench::ticks_to_ms(now - last_sample, frequency) < stall_interval_ms) {
      continue;
    }
    last_sample = now;
    sample_main_thread(thread, teb, now, now - last);
  }
}

// "module+0xoffset" for an address; the module's file name only.
std::string module_offset(std::uint64_t address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module) ||
      !module) {
    char text[32];
    std::snprintf(text, sizeof(text), "?+0x%llx",
                  static_cast<unsigned long long>(address));
    return text;
  }
  wchar_t path[MAX_PATH];
  const DWORD size = GetModuleFileNameW(module, path, MAX_PATH);
  std::wstring name(path, size);
  if (const auto slash = name.find_last_of(L"\\/"); slash != std::wstring::npos) {
    name = name.substr(slash + 1);
  }
  char text[64];
  std::snprintf(text, sizeof(text), "+0x%llx",
                static_cast<unsigned long long>(
                    address - reinterpret_cast<std::uint64_t>(module)));
  std::string out;
  for (const wchar_t c : name) {
    out.push_back(c < 128 ? static_cast<char>(c) : '?');
  }
  return out + text;
}

// One row a sample: t_ms from the run start, how old the frame was, then the
// stack, innermost first, ';'-separated.
std::string stalls_csv(const run_state &r, std::int64_t t1) {
  std::string csv = "t_ms,frame_age_ms,stack\n";
  if (!stall_samples) {
    return csv;
  }
  const std::uint32_t count = stall_count.load(std::memory_order_acquire);
  for (std::uint32_t i = 0; i < count; ++i) {
    const stall_sample &s = stall_samples[i];
    if (s.qpc < r.t0 || s.qpc >= t1) {
      continue;
    }
    csv += decimal(bench::ticks_to_ms(s.qpc - r.t0, frequency)) + "," +
           decimal(bench::ticks_to_ms(s.stalled_ticks, frequency)) + ",";
    for (std::uint32_t d = 0; d < s.depth; ++d) {
      csv += (d ? ";" : "") + module_offset(s.pc[d]);
    }
    csv += "\n";
  }
  return csv;
}

// --------------------------------------------------- runs (main thread)
bool begin_run(const std::string &requested, const std::string &kind) {
  auto *ring = frames.load(std::memory_order_acquire);
  if (run.active || !ring) {
    return false;
  }
  run = {};
  run.active = true;
  // Each run has the whole sample array: one game's second run started with
  // what the first had left (review, 2026-11-13)
  if (writes_pending.load(std::memory_order_acquire) == 0) {
    stall_count.store(0, std::memory_order_release);
  }
  run.stalls_dropped_at_begin = stalls_dropped.load(std::memory_order_relaxed);
  sampling.store(true, std::memory_order_relaxed);
  run.kind = kind;
  run.t0 = probe_clock();
  run.frame_begin = ring->count();
  run.main_begin = mains->count();
  run.switches_at_begin = chain_switches.load();
  run.started_utc = xerces_core::json_log::now_utc();
  SYSTEMTIME now{};
  GetLocalTime(&now);
  char stamp[64];
  std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u-%s", now.wYear,
                now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                kind.c_str());
  run.name = bench::run_name(requested, stamp);
  run.map = current_map();
  run.dvars = read_dvars();
  log_async(xerces_core::log_level::info, "perf.bench.begin", run.name,
            "{\"run\":" + json::quote(run.name) + ",\"kind\":" +
                json::quote(kind) + ",\"map\":" + json::quote(run.map) +
                ",\"qpc\":" + std::to_string(run.t0) + "}");
  return true;
}

// write_now: written on this thread before returning (the game is closing,
// and the async step would never run)
bool end_run(const bool write_now = false) {
  auto *ring = frames.load(std::memory_order_acquire);
  if (!run.active || !ring) {
    return false;
  }
  if (run.kind.starts_with("auto-")) {
    // The automatic run ended, on time or by xerces_bench_end: step_auto
    // stops, so its end time cannot cut a manual run started after it.
    autos.done = true;
  }
  sampling.store(false, std::memory_order_relaxed);
  auto done = std::make_shared<finished_run>();
  done->t1 = probe_clock();
  done->frame_end = ring->count();
  done->main_end = mains->count();
  done->chain_switches = chain_switches.load();
  done->stalls_dropped_end = stalls_dropped.load(std::memory_order_relaxed);
  done->run = std::move(run);
  run = {};
  if (write_now) {
    write_run(*done);
    return true;
  }
  writes_pending.fetch_add(1, std::memory_order_acq_rel);
  scheduler::once(
      [done] {
        write_run(*done);
        writes_pending.fetch_sub(1, std::memory_order_acq_rel);
      },
      scheduler::async);
  return true;
}

void finish_load(std::int64_t now, bool timed_out) {
  const double ms = bench::ticks_to_ms(now - load.t0, frequency);
  const std::string data =
      "{\"map\":" + json::quote(load.map) + ",\"ms\":" + decimal(ms) +
      ",\"qpc_begin\":" + std::to_string(load.t0) +
      ",\"qpc_end\":" + std::to_string(now) +
      ",\"timed_out\":" + (timed_out ? "true" : "false") + "}";
  if (timed_out) {
    log_async(xerces_core::log_level::warn, "perf.load",
              load.map + " did not load in " + decimal(ms / 1000.0) + " s",
              data);
  } else {
    log_async(xerces_core::log_level::info, "perf.load",
              load.map + " " + decimal(ms / 1000.0) + " s", data);
    say("[Xerces] " + load.map + " loaded in " + decimal(ms / 1000.0) + " s\n");
  }
  load = {};
}

void step_auto(std::int64_t now) {
  if (!autos.started) {
    if (autos.start_at == 0) {
      if (auto_spec->when == bench::auto_run::trigger::menu) {
        if (const std::int64_t menu = menu_ready_qpc(); menu != 0) {
          autos.start_at = menu + seconds_to_ticks(auto_spec->start_s);
        }
      } else if (in_level_active()) {
        autos.start_at = now + seconds_to_ticks(auto_spec->start_s);
      }
    }
    if (autos.start_at != 0 && now >= autos.start_at) {
      const bool menu = auto_spec->when == bench::auto_run::trigger::menu;
      if (begin_run({}, menu ? "auto-menu" : "auto-ingame")) {
        autos.started = true;
        autos.end_at = now + seconds_to_ticks(auto_spec->length_s);
      } else {
        autos.done = true; // a run was already going
      }
    }
  } else if (now >= autos.end_at) {
    end_run(); // the automatic run: one ended by hand set autos.done already
    autos.done = true;
  } else if (run.active && run.kind == "auto-ingame") {
    // Leaving the level ends the in-game run there and writes it: otherwise
    // a game that hangs or is closed on the way back to the menu loses the
    // whole recording (the files are written only when a run ends).
    if (in_level_active()) {
      autos.left_level_at = 0;
    } else if (autos.left_level_at == 0) {
      autos.left_level_at = now;
    } else if (now - autos.left_level_at >= seconds_to_ticks(2.0)) {
      end_run();
      autos.done = true;
    }
  }
}

// Every main frame, probe armed only.
void main_frame() {
  const std::int64_t now = probe_clock();
  mains->push(now);
  last_main_frame.store(now, std::memory_order_relaxed);

  const auto window =
      reinterpret_cast<HWND>(game_window.load(std::memory_order_relaxed));
  const bool focused = !window || GetForegroundWindow() == window;
  window_unfocused.store(!focused, std::memory_order_relaxed);
  if (run.active && was_focused && !focused) {
    ++run.focus_losses;
  }
  was_focused = focused;

  if (load.pending) {
    if (!in_level_active()) {
      load.left_level = true;
    } else if (load.left_level) {
      finish_load(now, false);
    }
    if (load.pending &&
        bench::ticks_to_ms(now - load.t0, frequency) > map_load_limit_ms) {
      finish_load(now, true);
    }
  }
  if (auto_spec && !autos.done) {
    step_auto(now);
  }
}

bool valid_map_name(const std::string &name) {
  return !name.empty() && name.size() <= 64 &&
         std::ranges::all_of(name, [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_';
         });
}

void add_commands() {
  command::add("xerces_bench_begin", [](const command::params &params) {
    const std::string name = params.size() > 1 ? params.join(1) : "";
    if (begin_run(name, "manual")) {
      say("[Xerces] bench run " + run.name + " started\n");
    } else {
      say("[Xerces] a bench run is already going (xerces_bench_end)\n");
    }
  });
  command::add("xerces_bench_end", [] {
    const std::string name = run.name;
    if (end_run()) {
      say("[Xerces] bench run " + name + " ended; writing it\n");
    } else {
      say("[Xerces] no bench run is going\n");
    }
  });
  command::add("xerces_bench_map", [](const command::params &params) {
    const std::string map = params.size() > 1 ? params.get(1) : "";
    if (!valid_map_name(map)) {
      say("[Xerces] usage: xerces_bench_map <map>\n");
      return;
    }
    load = {};
    load.pending = true;
    load.map = map;
    load.t0 = probe_clock();
    game::cbuf::Cbuf_AddText(game::LOCAL_CLIENT_0,
                             ("map " + map + "\n").c_str());
  });
}

std::optional<std::wstring> env(const wchar_t *name) {
  wchar_t buffer[32768];
  const DWORD size = GetEnvironmentVariableW(name, buffer, 32768);
  if (size == 0 || size >= 32768) {
    return std::nullopt;
  }
  return std::wstring(buffer, size);
}

// Where bench output must never go.
std::vector<std::filesystem::path> forbidden_dirs() {
  std::vector<std::filesystem::path> out;
  for (const wchar_t *name : {L"LOCALAPPDATA", L"APPDATA"}) {
    if (const auto dir = env(name)) {
      const std::filesystem::path path(*dir);
      out.push_back(path);
      out.push_back(path.parent_path()); // the whole AppData folder
    }
  }
  if (const auto profile = env(L"USERPROFILE")) {
    out.push_back(std::filesystem::path(*profile) / "AppData");
  }
  const std::filesystem::path &game_dir = game::get_game_path();
  out.push_back(game_dir);
  // steamapps\common\<game> -> steamapps\workshop
  out.push_back(game_dir.parent_path().parent_path() / "workshop");
  return out;
}

void arm() {
  if (env(L"XERCES_BENCH").value_or(L"") != L"1") {
    return;
  }
  std::filesystem::path dir = xerces_runtime::home() / "bench";
  if (const auto set = env(L"XERCES_BENCH_OUT"); set && !set->empty()) {
    dir = *set;
  }
  if (!bench::output_allowed(dir, forbidden_dirs())) {
    log_async(xerces_core::log_level::warn, "perf.bench.refused",
              "XERCES_BENCH is set but the output folder is under AppData, "
              "the game folder or the Workshop folder, or not absolute: the "
              "probe stays off",
              "{\"dir\":" + json::quote(dir.string()) + "}");
    return;
  }
  out_dir = dir.lexically_normal();

  std::string auto_text;
  if (const auto text = env(L"XERCES_BENCH_AUTO"); text && !text->empty()) {
    auto_text.assign(text->begin(), text->end()); // ASCII by its grammar
    auto_spec = bench::parse_auto_run(auto_text);
    if (!auto_spec) {
      log_async(xerces_core::log_level::warn, "perf.bench.auto_invalid",
                "XERCES_BENCH_AUTO is not menu:<start_s>:<len_s> or "
                "ingame:<start_s>:<len_s>; no automatic run",
                "{\"value\":" + json::quote(json::valid_utf8(auto_text, 64)) +
                    "}");
    }
  }

  if (const auto text = env(L"XERCES_BENCH_STALL_MS")) {
    // For testing the sampler: a low value samples nearly every frame.
    const double value = _wtof(text->c_str());
    if (value >= 1.0 && value <= 1000.0) {
      stall_threshold_ms = value;
    }
  }
  frequency = qpc_frequency();
  mains = new xerces_core::frame_ring<std::int64_t>(main_ring_log2);
  frames.store(new xerces_core::frame_ring<frame_sample>(frame_ring_log2),
               std::memory_order_release);
  probe_detail::armed.store(true, std::memory_order_release);

  stall_samples = new stall_sample[stall_sample_max];
  utils::thread::create_named_thread("Xerces Stall Sampler", stall_sampler)
      .detach();
  scheduler::loop(main_frame, scheduler::main);
  add_commands();
  log_async(xerces_core::log_level::info, "perf.bench.armed",
            "Frame probe on; runs go to " + out_dir.string(),
            "{\"dir\":" + json::quote(out_dir.string()) + ",\"auto\":" +
                (auto_spec ? json::quote(auto_text) : "null") + "}");
}
} // namespace

void on_first_present(IDXGISwapChain *chain, UINT sync_interval, UINT flags) {
  bench::swapchain_info info;
  DXGI_SWAP_CHAIN_DESC desc{};
  if (SUCCEEDED(chain->GetDesc(&desc))) {
    info.has_desc = true;
    info.width = desc.BufferDesc.Width;
    info.height = desc.BufferDesc.Height;
    info.refresh_numerator = desc.BufferDesc.RefreshRate.Numerator;
    info.refresh_denominator = desc.BufferDesc.RefreshRate.Denominator;
    info.format = static_cast<std::uint32_t>(desc.BufferDesc.Format);
    info.buffer_count = desc.BufferCount;
    info.swap_effect = static_cast<std::uint32_t>(desc.SwapEffect);
    info.flags = desc.Flags;
    info.windowed = desc.Windowed != FALSE;
    game_window_render = desc.OutputWindow;
    game_window.store(reinterpret_cast<std::uintptr_t>(desc.OutputWindow),
                      std::memory_order_relaxed);
  }
  info.sync_interval = sync_interval;
  info.present_flags = flags;
  IDXGISwapChain1 *chain1 = nullptr;
  if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1)))) {
    info.swapchain1 = true;
    chain1->Release();
  }
  IDXGISwapChain2 *chain2 = nullptr;
  if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain2)))) {
    info.swapchain2 = true;
    chain2->Release();
  }
  // -xerces-flip-model: the engine's own request and what became of it.
  xerces_flip::describe(info);
  game_chain = chain;
  present_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
  swapchain_seen = info;
  swapchain_known.store(true, std::memory_order_release);

  scheduler::once(
      [info] {
        xerces_runtime::log_info(
            "perf.swapchain",
            std::string(bench::swap_effect_name(info.swap_effect)) + ", " +
                std::to_string(info.buffer_count) + " buffers, " +
                std::to_string(info.width) + "x" + std::to_string(info.height) +
                (info.windowed ? ", windowed" : ", fullscreen") +
                (info.flip_model == "off" ? std::string()
                                          : ", flip model: " + info.flip_model),
            bench::swapchain_json(info));
      },
      scheduler::async);
}

void probe_present(IDXGISwapChain *chain, const probe_entry &entry,
                   std::int64_t before, std::int64_t after, HRESULT result,
                   UINT sync_interval, UINT present_flags, bool overlay_drew) {
  auto *ring = frames.load(std::memory_order_acquire);
  if (!ring) {
    return;
  }
  std::uint8_t flags = 0;
  if (chain != game_chain) {
    if (chain == other_chain || !adopt(chain)) {
      return;
    }
    flags |= bench::frame_flag::new_chain;
  }
  if (overlay_drew) {
    flags |= bench::frame_flag::overlay_drew;
  }
  if (sync_interval != 0) {
    flags |= bench::frame_flag::sync_interval;
  }
  if (result != S_OK) {
    flags |= bench::frame_flag::present_status;
  }
  if ((present_flags & DXGI_PRESENT_ALLOW_TEARING) != 0) {
    flags |= bench::frame_flag::tearing;
  }
  if (window_unfocused.load(std::memory_order_relaxed)) {
    flags |= bench::frame_flag::unfocused;
  }
  frame_sample sample;
  sample.qpc = entry.qpc;
  sample.present_ticks = clamp32(after - before);
  const std::int64_t overlay_us = (before - entry.qpc) * 1'000'000 / frequency;
  sample.overlay_us = static_cast<std::uint16_t>(
      overlay_us < 0 ? 0 : (overlay_us > 0xFFFF ? 0xFFFF : overlay_us));
  sample.cpu = static_cast<std::uint8_t>(entry.cpu > 0xFF ? 0xFF : entry.cpu);
  sample.flags = flags;
  ring->push(sample);
}

void end_run_for_quit() { end_run(true); }

std::uint32_t present_thread_id() {
  return present_thread.load(std::memory_order_relaxed);
}

class probe_component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_perf_probe");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client()) {
      return;
    }
    arm();
  }

  // A run going when the game closes (the player quit from inside the level)
  // is written now: it was written only by end_run's async step, which an
  // exit never reaches, and its samples are in memory only (review,
  // 2026-11-13).
  void pre_destroy() override { end_run(true); }
};
} // namespace xerces_perf

REGISTER_COMPONENT(xerces_perf::probe_component)
