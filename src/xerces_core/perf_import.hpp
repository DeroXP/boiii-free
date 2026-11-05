#pragma once

// Reading what the benchmark measures (perf plan H1): frame-time CSVs (the
// in-game probe's and PresentMon 2.x's), the game console's load lines, and
// the JSON log events the load and launcher benchmarks read. Headless, and
// no file access here: xerces_cli reads the files, these parse the text.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core::perf {
// --- Frame times -------------------------------------------------------
enum class frames_format { probe, presentmon };
const char *to_string(frames_format format);

// One run's frames. The per-frame extras are empty or as long as dt_ms
// (NaN where a row had no value).
struct frame_series {
  frames_format format = frames_format::probe;
  std::string dt_column;     // where the frame times came from
  std::string time_column;   // where the times came from; empty: summed dt
  std::vector<double> t_ms;  // each frame's time, ms after the first frame
  std::vector<double> dt_ms; // each frame's time
  std::vector<double> cpu_busy_ms; // PresentMon (MsCPUBusy or CPUBusy)
  std::vector<double> gpu_busy_ms; // PresentMon (MsGPUBusy or GPUBusy)
  std::vector<double> present_ms;  // probe: time inside Present
  std::vector<double> overlay_us;  // probe: the overlay's share of it
  std::map<int, std::size_t> cpus; // probe: frames per CPU number
  std::size_t flagged = 0;         // probe: frames whose flags are not 0
  std::string application;         // PresentMon: the process kept
  std::string process_id;
  std::size_t rows = 0;          // data rows of the process kept
  std::size_t skipped = 0;       // of those, rows with no usable frame time
  std::size_t other_rows = 0;    // PresentMon: other processes, swap chains
  std::size_t not_displayed = 0; // PresentMon: frames never shown
  std::map<std::string, std::size_t> present_modes; // PresentMon
};

struct frames_result {
  std::optional<frame_series> series;
  std::string error;
};

// The probe's CSV (i,t_ms,dt_ms,present_ms,overlay_us,cpu,flags; columns
// found by name) or PresentMon's (1.x, 2.0 and 2.3+ column names: frame
// times from MsBetweenPresents, else MsBetweenAppStart, else FrameTime),
// told apart by the header. A PresentMon capture of several processes or
// swap chains keeps the one with the most rows.
frames_result parse_frames_csv(std::string_view text);

// The frames from `warmup_s` seconds after the first frame on.
frame_series drop_warmup(const frame_series &series, double warmup_s);

// --- The game console --------------------------------------------------
enum class console_kind {
  fs_startup,     // ----- FS_Startup -----
  zone_loaded,    // FastFileLoad: Setting <zone> to XZONE_LOADED in <n>ms.
  mod_load_start, // ModLoad start: <name>
  mod_load_done,  // ModLoad done: <name>
  supplemented,   // [ Workshop ] Supplemented <n> usermaps|mods ...
  command,        // ]<command>, the console's echo of a typed command
};

struct console_event {
  console_kind kind = console_kind::command;
  std::size_t line = 0;  // 1-based
  std::string name;      // the zone, ModLoad's name, usermaps|mods, command
  double ms = 0;         // zone_loaded
  std::size_t count = 0; // supplemented
};

struct console_log {
  std::vector<console_event> events;
  std::size_t lines = 0;
  // The text starts with the engine's "Command line:" line, so it holds the
  // whole session (a console copied late has lost its start).
  bool from_start = false;
  std::string fs_game;                   // from the command line
  std::vector<std::string> fs_extramods; // likewise, in order
};
console_log parse_console(std::string_view text);

// What a loaded zone is to a load's time.
enum class zone_group { map, main_mod, extra_mod, frontend, stock };
const char *to_string(zone_group group);
// `map` is the map being loaded (stock or custom; empty when none); the
// front end's zones are core_frontend and its language and patch zones.
zone_group group_of_zone(std::string_view zone, std::string_view map,
                         std::span<const std::string> extra_ids);

struct zone_load {
  std::string zone;
  double ms = 0;
  zone_group group = zone_group::stock;
  std::size_t line = 0;
};

struct mod_load_span {
  std::string name; // "usermaps", a mod id
  std::size_t zones = 0;
  double zones_ms = 0;
  bool done = false; // its "ModLoad done" line was seen
};

// A stretch of the console: the start up to the first typed command, then
// one per command. A "map"/"devmap" command's stretch is its map load (up to
// the next command, so a match's own loads count too).
struct console_section {
  std::string kind;    // "startup", "map" or "command"
  std::string command; // the command that opened it; empty for startup
  std::string map;     // a map section's map
  std::size_t first_line = 0;
  std::size_t last_line = 0;
  std::vector<zone_load> zones;
  double zones_ms = 0; // sum of the zones' own times (they may overlap)
  std::map<zone_group, double> ms_by_group;
  std::map<zone_group, std::size_t> zones_by_group;
  std::vector<mod_load_span> mod_loads;
  std::size_t fs_startups = 0;
  std::vector<std::pair<std::string, std::size_t>> supplemented;
};
std::vector<console_section> console_sections(const console_log &log);

// A map section's load: its zones up to and including the last zone of the
// map itself (the engine loads the map's own zone last). What follows (the
// drop back to the front end, zones a match loads) is counted apart. A
// section whose map zone never loaded (a crash, an error) is all load.
struct map_load_zones {
  bool map_zone_loaded = false;
  std::size_t zones = 0;
  double zones_ms = 0;
  std::map<zone_group, double> ms_by_group;
  std::map<zone_group, std::size_t> zones_by_group;
  std::size_t after_zones = 0;
  double after_ms = 0;
};
map_load_zones map_load_part(const console_section &section);

// --- JSON log events ---------------------------------------------------
struct log_event {
  std::string event;
  std::string msg;
  std::string role;
  std::string session;
  std::string level;
  std::string ts;
  std::optional<std::int64_t> ts_ms; // ms since 1970-01-01 UTC
  // The data object's top-level members: numbers, and strings and booleans
  // ("true"/"false") as text.
  std::map<std::string, double> numbers;
  std::map<std::string, std::string> strings;

  std::optional<double> number(const std::string &key) const;
  std::string text(const std::string &key) const;
};

// "2026-11-05T18:18:31.655Z" (the logs' form), also with more or fewer
// fraction digits, an offset (+01:00), or no zone (taken as UTC).
std::optional<std::int64_t> parse_timestamp_ms(std::string_view text);

// One JSONL line; nullopt when it is not a log event (no "event").
std::optional<log_event> parse_log_event(std::string_view line);
// Every event of a log, in order; other lines and a byte-order mark are
// skipped.
std::vector<log_event> parse_log_events(std::string_view text);

// launcher.startup.step's mark without its time: "window created at 14.7
// ms" is "window created".
std::string startup_mark(std::string_view msg);
} // namespace xerces_core::perf
