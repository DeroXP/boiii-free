#include <std_include.hpp>

// The XERCES_BENCH frame probe and perf.swapchain (docs/DESIGN.md 12.2). See
// perf_probe.hpp.

#include "flip_model.hpp"
#include "perf.hpp"
#include "perf_probe.hpp"
#include "runtime.hpp"

#include <component/command.hpp>
#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

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

// 2^18 Presents is 18 minutes at 240 FPS; 2^17 main frames.
constexpr unsigned frame_ring_log2 = 18;
constexpr unsigned main_ring_log2 = 17;
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
  std::vector<std::pair<std::string, std::optional<std::string>>> dvars;
};

struct finished_run {
  run_state run;
  std::int64_t t1 = 0;
  std::uint64_t frame_end = 0;
  std::uint64_t main_end = 0;
  std::uint32_t chain_switches = 0;
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
                                 bench::run_meta_json(meta));

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

// --------------------------------------------------- runs (main thread)
bool begin_run(const std::string &requested, const std::string &kind) {
  auto *ring = frames.load(std::memory_order_acquire);
  if (run.active || !ring) {
    return false;
  }
  run = {};
  run.active = true;
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

bool end_run() {
  auto *ring = frames.load(std::memory_order_acquire);
  if (!run.active || !ring) {
    return false;
  }
  if (run.kind.starts_with("auto-")) {
    // The automatic run ended, on time or by xerces_bench_end: step_auto
    // stops, so its end time cannot cut a manual run started after it.
    autos.done = true;
  }
  auto done = std::make_shared<finished_run>();
  done->t1 = probe_clock();
  done->frame_end = ring->count();
  done->main_end = mains->count();
  done->chain_switches = chain_switches.load();
  done->run = std::move(run);
  run = {};
  scheduler::once([done] { write_run(*done); }, scheduler::async);
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

  frequency = qpc_frequency();
  mains = new xerces_core::frame_ring<std::int64_t>(main_ring_log2);
  frames.store(new xerces_core::frame_ring<frame_sample>(frame_ring_log2),
               std::memory_order_release);
  probe_detail::armed.store(true, std::memory_order_release);

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
};
} // namespace xerces_perf

REGISTER_COMPONENT(xerces_perf::probe_component)
