#pragma once

// The pure parts of the game's performance instrumentation (docs/DESIGN.md
// 12.2): the frame probe's sample and CSV files, the bench run's meta file,
// XERCES_BENCH_AUTO, where output may go, and the JSON of the perf.* log
// events. The game role (src/client/xerces/perf*.cpp) fills these in; nothing
// here touches the game.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xerces_core::bench {

// One Present of the game's swap chain, as the probe records it.
struct frame_sample {
  std::int64_t qpc = 0;            // QueryPerformanceCounter at Present's entry
  std::uint32_t present_ticks = 0; // QPC ticks inside the original Present
  std::uint16_t overlay_us = 0;    // entry to calling the original (saturates)
  std::uint8_t cpu = 0;            // logical processor at entry
  std::uint8_t flags = 0;          // frame_flag bits
};
static_assert(sizeof(frame_sample) == 16);

namespace frame_flag {
inline constexpr std::uint8_t overlay_drew = 1;   // the overlay drew ImGui
inline constexpr std::uint8_t sync_interval = 2;  // Present's sync interval > 0
inline constexpr std::uint8_t present_status = 4; // Present returned non-S_OK
inline constexpr std::uint8_t unfocused = 8; // the game window was not in front
inline constexpr std::uint8_t new_chain = 16; // first frame of a new swap chain
// Present had DXGI_PRESENT_ALLOW_TEARING (-xerces-flip-model, flip_model.hpp)
inline constexpr std::uint8_t tearing = 32;
} // namespace frame_flag

// Times in a run's CSV files are milliseconds from t0_qpc.
struct run_clock {
  std::int64_t t0_qpc = 0;
  std::int64_t frequency = 1; // QueryPerformanceFrequency
};

double ticks_to_ms(std::int64_t ticks, std::int64_t frequency);

// <run>.frames.csv: i,t_ms,dt_ms,present_ms,overlay_us,cpu,flags. `i` counts
// from first_index (frames lost at the start of a run keep their numbers).
// dt_ms is the time since the previous Present's entry; for the first row it
// uses previous_qpc, or is empty when that is unknown.
std::string frames_csv(std::span<const frame_sample> frames,
                       std::optional<std::int64_t> previous_qpc,
                       const run_clock &clock, std::uint64_t first_index = 0);

// <run>.main.csv: i,t_ms,dt_ms, one row per main-thread frame.
std::string main_csv(std::span<const std::int64_t> stamps,
                     std::optional<std::int64_t> previous_qpc,
                     const run_clock &clock, std::uint64_t first_index = 0);

// XERCES_BENCH_AUTO: "menu:<start_s>:<len_s>" starts a run start_s seconds
// after the main menu is ready; "ingame:<start_s>:<len_s>" after the client
// is first active in a level. Seconds may have decimals; a run lasts at most
// an hour.
struct auto_run {
  enum class trigger { menu, ingame };
  trigger when = trigger::menu;
  double start_s = 0;
  double length_s = 0;
};
std::optional<auto_run> parse_auto_run(std::string_view text);

// A run's file name: letters, digits, '-', '_' and '.', at most 64
// characters, never starting with '.'; `fallback` when nothing is left.
std::string run_name(std::string_view requested, std::string_view fallback);

// Bench output must never land under AppData, the game folder or the
// Workshop folder: `dir` must be absolute and inside none of `forbidden`
// (compared case-insensitively, by whole path components). Empty entries in
// `forbidden` are ignored.
bool output_allowed(const std::filesystem::path &dir,
                    const std::vector<std::filesystem::path> &forbidden);

// The game's swap chain as the first Present saw it (perf.swapchain).
struct swapchain_info {
  bool has_desc = false;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t refresh_numerator = 0;
  std::uint32_t refresh_denominator = 0;
  std::uint32_t format = 0;       // DXGI_FORMAT
  std::uint32_t buffer_count = 0; // DXGI_SWAP_CHAIN_DESC::BufferCount
  std::uint32_t swap_effect = 0;  // DXGI_SWAP_EFFECT
  std::uint32_t flags = 0;        // DXGI_SWAP_CHAIN_FLAG bits
  bool windowed = false;
  std::uint32_t sync_interval = 0; // Present's arguments, first call
  std::uint32_t present_flags = 0;
  bool swapchain1 = false; // QueryInterface answers IDXGISwapChain1
  bool swapchain2 = false; // ... and IDXGISwapChain2 (waitable chains)
  // -xerces-flip-model (xerces_core/flip_model.hpp): "off" without the flag,
  // else what became of the chain (flip_model::to_string). The effective
  // swap effect is swap_effect above (GetDesc); the engine's own request is
  // known when the CreateSwapChain detour saw the chain made.
  std::string flip_model = "off";
  std::optional<std::uint32_t> engine_swap_effect;
  std::optional<std::uint32_t> engine_flags;
};
std::string_view swap_effect_name(std::uint32_t swap_effect);
std::string swapchain_json(const swapchain_info &info);

// <run>.meta.json.
struct run_meta {
  std::string run;
  std::string kind; // "manual", "auto-menu", "auto-ingame"
  std::string started_utc;
  std::string map; // the level loaded at the start ("" in the menus)
  std::int64_t qpc_frequency = 1;
  std::int64_t t0_qpc = 0;
  std::int64_t t1_qpc = 0;
  std::uint64_t frames = 0;
  std::uint64_t frames_lost = 0; // overwritten in the ring before the copy
  std::uint64_t main_frames = 0;
  std::uint64_t main_lost = 0;
  std::uint32_t pid = 0;
  std::uint32_t present_thread = 0;
  std::uint32_t main_thread = 0;
  std::uint32_t focus_losses = 0;
  std::uint32_t chain_switches = 0;
  std::optional<swapchain_info> swapchain;
  // Dvars as the run began; nullopt when the engine has no such dvar.
  std::vector<std::pair<std::string, std::optional<std::string>>> dvars;
};
std::string run_meta_json(const run_meta &meta);

// A short summary of a run's frames for the log line that says it was
// written (the full statistics come from the CSV offline).
struct frames_summary {
  std::uint64_t frames = 0;
  double duration_ms = 0;
  double mean_fps = 0;
  double max_dt_ms = 0;
};
frames_summary summarize(std::span<const frame_sample> frames,
                         std::optional<std::int64_t> previous_qpc,
                         const run_clock &clock, std::int64_t t1_qpc);

// --------------------------------------------------------- perf.* events
// Milliseconds since the process was created, or nullopt when not seen.
struct startup_marks {
  std::optional<double> first_detour_ms;
  std::optional<double> last_detour_ms;
  std::optional<double> ipc_start_ms;
  std::optional<double> frontend_ms;
  std::optional<double> menu_ready_ms;
  std::uint32_t detours = 0; // created by the first main frame
  bool menu_timed_out = false;
  // Thread ids, so a perf.io record's bare id can be placed: the thread
  // post_unpack ran on and the main frame's (the same thread when the game
  // runs its main loop on the thread that started it); 0 when not known.
  std::uint32_t unpack_thread_id = 0;
  std::uint32_t main_thread_id = 0;
};
std::string startup_json(const startup_marks &marks);

struct hook_point {
  double at_ms = 0; // since the process was created
  double create_us = 0;
  double enable_us = 0;
};
struct hook_totals {
  std::uint32_t created = 0;
  std::uint32_t enables = 0;
  double create_ms = 0;
  double enable_ms = 0;
  double max_enable_ms = 0;
  std::uint32_t created_later = 0; // after the first main frame, so far
  std::vector<hook_point> timeline;
};
std::string hooks_json(const hook_totals &totals);
} // namespace xerces_core::bench
