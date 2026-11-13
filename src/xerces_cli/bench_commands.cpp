// xerces_cli bench: the offline half of the benchmark (DESIGN.md 12.2, 12.3;
// perf plan H2). Everything here reads files that already exist: a home's
// logs, map-check folders, frame-time CSVs. Nothing starts the game, and
// nothing is written. `bench run` drives the game and is not built yet.
//
// - bench loads: game starts (the launcher's launch.started to the first
//   main frame, the journal's "frontend") and map loads (map-check's
//   stopwatch, the game log's loading_map to loader.map.ready, the console's
//   zone times by zone group).
// - bench launcher: Xerces.exe's startup marks and Workshop scans.
// - bench analyze: one frames CSV per run (the probe's <run>.frames.csv or
//   PresentMon's), of one source.
// - bench compare: two sets of runs, each run one sample.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <xerces_core/json.hpp>
#include <xerces_core/launcher_home.hpp>
#include <xerces_core/log_tail.hpp>
#include <xerces_core/perf_import.hpp>
#include <xerces_core/perf_stats.hpp>

#include "cli.hpp"

namespace xerces_cli {
namespace {
using namespace xerces_core::perf;
namespace json = xerces_core::json;
using json_value = rapidjson::Value;
using json_alloc = json::allocator;

// ------------------------------------------------------------- helpers
std::string fixed(double value, int decimals) {
  if (!std::isfinite(value)) {
    return "-";
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  return buffer;
}

std::string pad(std::string text, std::size_t width) {
  if (text.size() < width) {
    text.append(width - text.size(), ' ');
  }
  return text;
}

// Rounded to 4 decimals; null when not a number.
json_value num(double value) {
  if (!std::isfinite(value)) {
    return json_value(rapidjson::kNullType);
  }
  return json_value(std::round(value * 10000.0) / 10000.0);
}

json_value num(std::optional<double> value) {
  return value ? num(*value) : json_value(rapidjson::kNullType);
}

json_value count(std::size_t value) {
  return json_value(static_cast<std::uint64_t>(value));
}

void put(json_value &object, std::string_view key, json_value value,
         json_alloc &alloc) {
  json::set(object, key, std::move(value), alloc);
}

void put(json_value &object, std::string_view key, std::string_view text,
         json_alloc &alloc) {
  json::set(object, key, text, alloc);
}

json_value stats_json(const sample_stats &stats, json_alloc &alloc) {
  json_value out(rapidjson::kObjectType);
  put(out, "n", count(stats.n), alloc);
  put(out, "mean", num(stats.n ? stats.mean : NAN), alloc);
  put(out, "sd", num(stats.n >= 2 ? stats.sd : NAN), alloc);
  put(out, "median", num(stats.n ? stats.median : NAN), alloc);
  put(out, "min", num(stats.n ? stats.min : NAN), alloc);
  put(out, "max", num(stats.n ? stats.max : NAN), alloc);
  json_value ci(rapidjson::kNullType);
  if (stats.ci_low && stats.ci_high) {
    ci.SetArray();
    ci.PushBack(num(*stats.ci_low), alloc);
    ci.PushBack(num(*stats.ci_high), alloc);
  }
  put(out, "ci95", std::move(ci), alloc);
  return out;
}

// n, mean, sd, median, p90, min, max and the 95% CI of the mean.
json_value spread_json(const std::vector<double> &values, json_alloc &alloc) {
  json_value out = stats_json(describe_sample(values), alloc);
  put(out, "p90", num(percentile(values, 90)), alloc);
  return out;
}

std::string spread_text(const std::vector<double> &values, int decimals,
                        const char *unit) {
  if (values.empty()) {
    return "n 0";
  }
  const sample_stats s = describe_sample(values);
  std::string out = "n " + std::to_string(s.n) + "  median " +
                    fixed(s.median, decimals) + unit + "  p90 " +
                    fixed(percentile(values, 90), decimals) + unit + "  mean " +
                    fixed(s.mean, decimals) + unit;
  if (s.n >= 2) {
    out += "  sd " + fixed(s.sd, decimals) + "  95% CI " +
           fixed(*s.ci_low, decimals) + "-" + fixed(*s.ci_high, decimals) +
           unit;
  }
  out += "  [" + fixed(s.min, decimals) + "-" + fixed(s.max, decimals) + "]";
  return out;
}

std::optional<std::string> read_file(const fs::path &path) {
  return xerces_core::read_text_file(path);
}

std::string file_name(const fs::path &path) { return utf8(path.filename()); }

std::vector<fs::path> files_in(const fs::path &dir,
                               std::string_view extension) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == extension) {
      out.push_back(entry.path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<fs::path> folders_in(const fs::path &dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_directory(ec)) {
      out.push_back(entry.path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::optional<double> parse_double(const std::string &text) {
  try {
    std::size_t used = 0;
    const double value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value)) {
      return std::nullopt;
    }
    return value;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

std::optional<double> number_option(const arguments &args,
                                    const std::string &name, double fallback,
                                    bool &ok) {
  const auto given = args.get(name);
  if (!given) {
    return fallback;
  }
  const auto value = parse_double(*given);
  if (!value || *value < 0) {
    std::cerr << name << " takes a number of 0 or more, not \"" << *given
              << "\".\n";
    ok = false;
    return std::nullopt;
  }
  return value;
}

// The logs folders the command reads: every --logs, <home>\logs for
// --home, and the default home's when neither is given.
std::vector<fs::path> logs_folders(const arguments &args, bool default_home) {
  std::vector<fs::path> out;
  for (const auto &dir : args.all("--logs")) {
    out.emplace_back(dir);
  }
  if (const auto home = args.get("--home")) {
    out.push_back(xerces_core::path_from_utf8(*home) / "logs");
  } else if (out.empty() && default_home) {
    out.push_back(xerces_core::find_home(std::nullopt) / "logs");
  }
  return out;
}

bool check_folders(const std::vector<fs::path> &folders) {
  bool ok = true;
  for (const auto &dir : folders) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
      std::cerr << utf8(dir) << " is not a folder.\n";
      ok = false;
    }
  }
  return ok;
}

json_value paths_json(const std::vector<fs::path> &paths, json_alloc &alloc) {
  json_value out(rapidjson::kArrayType);
  for (const auto &path : paths) {
    out.PushBack(json::string_value(utf8(path), alloc), alloc);
  }
  return out;
}

int print_json(const rapidjson::Document &doc) {
  std::cout << json::pretty(doc) << "\n";
  return exit_ok;
}

// ------------------------------------------------------- bench loads
struct log_file {
  fs::path path;
  std::uintmax_t size = 0;
  std::optional<fs::path> run_dir; // the map-check folder it was found in
};

// A map-check record (results.jsonl), one per run.
struct map_check_record {
  std::string folder; // the run folder's name
  std::string map;
  std::string result;
  std::string detail;
  std::vector<std::string> stack;
  std::optional<double> menu_s;
  std::optional<double> load_s;
  std::string pid;
};

struct launch_record {
  std::int64_t ts_ms = 0;
  std::string source; // launcher or cli
  std::string session;
};

struct map_load {
  std::optional<std::int64_t> loading_ms; // loader.phase loading_map
  std::optional<std::int64_t> ready_ms;   // in_game / loader.map.ready
  std::string map;
  std::string ended; // why it ended without being ready
};

std::string phase_of(const log_event &event) {
  const std::string phase = event.text("phase");
  return phase.empty() ? event.msg : phase;
}

// The label after "<date>-<time>-<map>-" in a map-check folder's name
// ("vanilla", "stack3", a mod id).
std::string run_label(const std::string &folder, const std::string &map) {
  const std::size_t at = map.empty() ? std::string::npos : folder.find(map);
  if (at != std::string::npos && at + map.size() < folder.size() &&
      folder[at + map.size()] == '-') {
    return folder.substr(at + map.size() + 1);
  }
  return {};
}

std::vector<map_check_record> read_map_checks(const fs::path &root) {
  std::vector<map_check_record> out;
  const auto text = read_file(root / "results.jsonl");
  if (!text) {
    return out;
  }
  std::string_view rest(*text);
  if (rest.starts_with("\xEF\xBB\xBF")) {
    rest.remove_prefix(3);
  }
  while (!rest.empty()) {
    const std::size_t end = std::min(rest.find('\n'), rest.size());
    const std::string_view line = rest.substr(0, end);
    rest.remove_prefix(std::min(end + 1, rest.size()));
    const auto parsed = json::parse(line);
    if (!parsed.ok || !parsed.document.IsObject()) {
      continue;
    }
    const auto &root_value = parsed.document;
    map_check_record record;
    record.map = json::get_string(root_value, "map").value_or("");
    record.result = json::get_string(root_value, "result").value_or("");
    record.detail = json::get_string(root_value, "detail").value_or("");
    if (const auto console = json::get_string(root_value, "console")) {
      record.folder = utf8(xerces_core::path_from_utf8(*console).parent_path().filename());
    }
    if (const auto *stack = json::member(root_value, "stack");
        stack && stack->IsArray()) {
      for (const auto &item : stack->GetArray()) {
        if (item.IsString()) {
          record.stack.emplace_back(item.GetString());
        }
      }
    }
    const auto number = [&](const char *key) -> std::optional<double> {
      const auto *value = json::member(root_value, key);
      if (value && value->IsNumber()) {
        return value->GetDouble();
      }
      return std::nullopt;
    };
    record.menu_s = number("menu_seconds");
    record.load_s = number("load_seconds");
    if (const auto pid = number("pid")) {
      record.pid = std::to_string(static_cast<long long>(*pid));
    }
    out.push_back(std::move(record));
  }
  return out;
}

int cmd_bench_loads(const arguments &args) {
  if (!accept_only(args, 2, {"--home", "--logs", "--console"}, {"--json"})) {
    return exit_error;
  }
  const bool as_json = args.has("--json");
  // Without --logs, --home or --console: the default home's logs.
  const std::vector<fs::path> logs =
      logs_folders(args, args.all("--console").empty());
  std::vector<fs::path> consoles;
  for (const auto &dir : args.all("--console")) {
    consoles.emplace_back(dir);
  }
  if (!check_folders(logs) || !check_folders(consoles)) {
    return exit_error;
  }

  // Game logs by file name: a run's log is both in the home's logs and in
  // its map-check folder; the larger copy is the more complete one.
  std::map<std::string, log_file> games;
  std::vector<fs::path> others; // launcher and CLI logs
  const auto add_game = [&](const fs::path &path,
                            const std::optional<fs::path> &run_dir) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    auto &entry = games[file_name(path)];
    if (entry.path.empty() || (!ec && size > entry.size)) {
      entry.path = path;
      entry.size = ec ? 0 : size;
    }
    if (run_dir) {
      entry.run_dir = run_dir;
    }
  };
  for (const auto &dir : logs) {
    for (const auto &file : files_in(dir, ".jsonl")) {
      const std::string role = xerces_core::log_role(file);
      if (role == "game") {
        add_game(file, std::nullopt);
      } else if (role == "launcher" || role == "cli") {
        others.push_back(file);
      }
    }
  }
  std::map<std::string, map_check_record> records_by_folder;
  std::map<std::string, map_check_record> records_by_pid;
  for (const auto &root : consoles) {
    std::vector<fs::path> runs = folders_in(root);
    runs.push_back(root); // a run folder given directly
    for (const auto &run : runs) {
      std::error_code ec;
      if (!fs::exists(run / "console.txt", ec)) {
        bool has_game_log = false;
        for (const auto &file : files_in(run, ".jsonl")) {
          has_game_log |= xerces_core::log_role(file) == "game";
        }
        if (!has_game_log) {
          continue;
        }
      }
      for (const auto &file : files_in(run, ".jsonl")) {
        if (xerces_core::log_role(file) == "game") {
          add_game(file, run);
        }
      }
    }
    for (auto &record : read_map_checks(root)) {
      if (!record.pid.empty()) {
        records_by_pid[record.pid] = record;
      }
      if (!record.folder.empty()) {
        records_by_folder[record.folder] = std::move(record);
      }
    }
  }

  // The launches: the launcher's (or CLI's) launch.started, by game pid.
  std::multimap<std::string, launch_record> launches;
  for (const auto &file : others) {
    const auto text = read_file(file);
    if (!text) {
      continue;
    }
    for (const auto &event : parse_log_events(*text)) {
      if ((event.event == "launcher.launch.started" ||
           event.event == "cli.launch.started") &&
          event.ts_ms && event.number("pid")) {
        launches.emplace(
            std::to_string(static_cast<long long>(*event.number("pid"))),
            launch_record{*event.ts_ms,
                          event.event.substr(0, 3) == "cli" ? "cli"
                                                            : "launcher",
                          event.session});
      }
    }
  }

  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  put(doc, "schema", "xerces.bench.loads/1", alloc);
  json_value sources(rapidjson::kObjectType);
  put(sources, "logs", paths_json(logs, alloc), alloc);
  put(sources, "console", paths_json(consoles, alloc), alloc);
  put(doc, "sources", std::move(sources), alloc);
  put(doc, "definitions",
      "game_start_to_frontend_s: launch.started (Xerces.exe's "
      "launcher.launch.started or xerces_cli's cli.launch.started, matched "
      "by pid; launched_by says which, and the startup summary keeps them "
      "apart) to the game's first loader.phase frontend (the first main "
      "frame). first_log_to_frontend_s: the game log's first line (Xerces' "
      "first code, about 14 s after the process starts) to the same. "
      "journal new: the game log has loader.phase loading_map (the engine "
      "is in the level) or loader.map.ready (in_game, the player is in it); "
      "journal old (builds before 2026-11-05 11:25 local): in_game with no "
      "loading_map, as in_game came as soon as the engine was in the level; "
      "journal unknown: the log never reached a level (the game ended "
      "first), so it cannot tell. map_command_to_ready_s (new journals) and "
      "map_command_to_level_s come from map-check's stopwatch. It starts "
      "after console-command.ps1's 1.5 s echo wait, so about 1.5 s after "
      "the game ran the map command, and stops when map-check saw the "
      "journal's in_game: up to about 1.5 s later (polled once a second), "
      "more with -SkipCinematic (Space held 1.5 s every 6 s from 15 s on), "
      "and the game writes each phase up to 1 s late (checked once a "
      "second). Both figures are therefore off by roughly -1.5 to +2.5 s "
      "(low when the polls are quick): compare them with other stopwatch "
      "figures only, not with a load the game times itself. "
      "map_command_to_ready_s includes any intro or wait before the player "
      "is in. loading_map_to_ready_s: the game log's loading_map to "
      "loader.map.ready, both checked once a second (+-1 s). "
      "map_command_to_level_s: the command to the engine being in the "
      "level: the stopwatch for old journals, the stopwatch minus "
      "loading_map_to_ready_s for new ones. The logs do not time the "
      "command itself yet. Zone times are the console's own FastFileLoad "
      "times, summed by zone group (zones may overlap), counted up to the "
      "map's own zone; zones after it (a drop back to the menu) are "
      "after_map_zone.",
      alloc);

  json_value launches_json(rapidjson::kArrayType);
  json_value loads_json(rapidjson::kArrayType);
  // By who launched it (launcher: Xerces.exe, cli: xerces_cli), then by
  // variant.
  std::map<std::string, std::map<std::string, std::vector<double>>>
      to_frontend_by_launcher;
  std::map<std::string, std::vector<double>> first_log_by_variant;
  std::map<std::string, std::vector<double>> startup_zone_ms; // by group
  std::size_t startup_consoles = 0;

  struct load_row {
    std::string map;
    std::string variant;
    std::string journal; // new, old or unknown
    std::optional<double> command_to_ready_s;
    std::optional<double> loading_to_ready_s;
    std::optional<double> command_to_level_s;
    std::optional<double> zones_ms;
    std::map<std::string, double> zone_groups;
    double frontend_reload_ms = 0;
    bool ready = false;
  };
  std::vector<load_row> rows;

  for (const auto &[name, file] : games) {
    const auto text = read_file(file.path);
    if (!text) {
      continue;
    }
    const auto events = parse_log_events(*text);
    if (events.empty() || !events.front().ts_ms) {
      continue;
    }
    const std::string session = events.front().session;
    const std::int64_t first_ms = *events.front().ts_ms;
    std::map<std::string, std::int64_t> marks;
    const auto mark = [&](const std::string &key, const log_event &event) {
      if (event.ts_ms && !marks.contains(key)) {
        marks[key] = *event.ts_ms;
      }
    };
    std::string plan_id;
    std::string fs_game;
    std::string fs_extramods;
    std::vector<map_load> loads;
    bool pending = false;
    // Journals before loading_map existed (builds before 2026-11-05 11:25
    // local) said in_game as soon as the engine was in the level. A log only
    // shows which kind it is once it reaches a level: one that ended before
    // (a crash right after the map command) is "unknown".
    bool marks_loading = false;
    bool reached_level = false;
    for (const auto &event : events) {
      const bool phase = event.event == "loader.phase";
      marks_loading |= event.event == "loader.map.ready" ||
                       (phase && phase_of(event) == "loading_map");
      reached_level |= phase && phase_of(event) == "in_game";
    }
    const std::string journal = marks_loading   ? "new"
                                : reached_level ? "old"
                                                : "unknown";
    const bool new_journal = journal == "new";
    for (const auto &event : events) {
      if (event.event == "loader.phase") {
        const std::string phase = phase_of(event);
        if (plan_id.empty()) {
          plan_id = event.text("plan_id");
        }
        mark(phase, event);
        if (phase == "loading_map" && !pending) {
          loads.push_back({event.ts_ms, std::nullopt, {}, {}});
          pending = true;
        } else if (phase == "in_game") {
          if (!pending) {
            loads.push_back({std::nullopt, event.ts_ms, {}, {}});
          } else {
            loads.back().ready_ms = event.ts_ms;
          }
          pending = false;
        } else if (phase == "frontend" && pending) {
          loads.back().ended = "back in the front end before in_game";
          pending = false;
        }
      } else if (event.event == "loader.map.ready") {
        const std::string map =
            event.text("map").empty() ? event.msg : event.text("map");
        if (!loads.empty() && loads.back().ready_ms &&
            loads.back().map.empty()) {
          loads.back().map = map;
        } else if (pending) {
          loads.back().ready_ms = event.ts_ms;
          loads.back().map = map;
          pending = false;
        }
      } else if (event.event == "loader.exit.unexpected" && pending) {
        loads.back().ended = "the game ended while loading";
        pending = false;
      } else if (event.event == "xerces.signatures.resolved") {
        mark("signatures", event);
      } else if (event.event == "xerces.feature.on") {
        mark("feature_on", event);
      } else if (event.event == "ipc.start") {
        mark("ipc_start", event);
      } else if (event.event == "loader.start") {
        mark("loader_start", event);
      } else if (event.event == "loader.plan.applied" ||
                 event.event == "loader.plan.mismatch") {
        fs_game = event.text("fs_game");
        fs_extramods = event.text("fs_extramods");
      }
    }
    if (pending) {
      loads.back().ended = "the log ends while loading";
    }

    // The launch that started this game: same pid, before its first line.
    std::optional<launch_record> launch;
    for (auto [it, end] = launches.equal_range(session); it != end; ++it) {
      if (it->second.ts_ms <= first_ms &&
          first_ms - it->second.ts_ms < 120000 &&
          (!launch || it->second.ts_ms > launch->ts_ms)) {
        launch = it->second;
      }
    }

    std::optional<map_check_record> record;
    std::string folder;
    if (file.run_dir) {
      folder = file_name(*file.run_dir);
      if (const auto it = records_by_folder.find(folder);
          it != records_by_folder.end()) {
        record = it->second;
      }
    }
    if (!record) {
      if (const auto it = records_by_pid.find(session);
          it != records_by_pid.end()) {
        record = it->second;
      }
    }
    std::string variant =
        record ? run_label(record->folder.empty() ? folder : record->folder,
                           record->map)
               : std::string();
    if (variant.empty()) {
      variant = plan_id.empty() ? "vanilla" : "modded";
    }

    // The console, when the run folder kept one.
    std::optional<console_log> console;
    if (file.run_dir) {
      if (const auto console_text = read_file(*file.run_dir / "console.txt")) {
        console = parse_console(*console_text);
      }
    }
    std::vector<console_section> sections;
    if (console) {
      sections = console_sections(*console);
    }

    json_value launch_json(rapidjson::kObjectType);
    put(launch_json, "session", session, alloc);
    put(launch_json, "log", utf8(file.path), alloc);
    put(launch_json, "variant", variant, alloc);
    put(launch_json, "plan_id", plan_id, alloc);
    put(launch_json, "fs_game", fs_game, alloc);
    put(launch_json, "fs_extramods", fs_extramods, alloc);
    if (!folder.empty()) {
      put(launch_json, "map_check", folder, alloc);
    }
    put(launch_json, "launched_by", launch ? launch->source : "", alloc);
    std::optional<double> to_frontend;
    if (launch && marks.contains("frontend")) {
      to_frontend = (marks["frontend"] - launch->ts_ms) / 1000.0;
      auto &by_variant = to_frontend_by_launcher[launch->source];
      by_variant[variant].push_back(*to_frontend);
      by_variant["all"].push_back(*to_frontend);
    }
    put(launch_json, "game_start_to_frontend_s", num(to_frontend), alloc);
    std::optional<double> first_to_frontend;
    if (marks.contains("frontend")) {
      first_to_frontend = (marks["frontend"] - first_ms) / 1000.0;
      first_log_by_variant[variant].push_back(*first_to_frontend);
      first_log_by_variant["all"].push_back(*first_to_frontend);
    }
    put(launch_json, "first_log_to_frontend_s", num(first_to_frontend), alloc);
    json_value marks_json(rapidjson::kObjectType);
    for (const char *key : {"symbols", "signatures", "launching", "feature_on",
                            "ipc_start", "loader_start", "frontend"}) {
      if (marks.contains(key)) {
        const std::int64_t origin = launch ? launch->ts_ms : first_ms;
        put(marks_json, key, num((marks[key] - origin) / 1000.0), alloc);
      }
    }
    put(launch_json, "marks_s", std::move(marks_json), alloc);
    put(launch_json, "marks_from",
        launch ? "launcher.launch.started" : "the game log's first line",
        alloc);
    if (record && record->menu_s) {
      put(launch_json, "map_check_menu_s", num(*record->menu_s), alloc);
    }
    if (!sections.empty() && console->from_start) {
      json_value zones(rapidjson::kObjectType);
      const auto &start = sections.front();
      for (const auto &[group, ms] : start.ms_by_group) {
        put(zones, to_string(group), num(ms), alloc);
        startup_zone_ms[to_string(group)].push_back(ms);
      }
      put(zones, "total", num(start.zones_ms), alloc);
      startup_zone_ms["total"].push_back(start.zones_ms);
      ++startup_consoles;
      put(launch_json, "startup_zones_ms", std::move(zones), alloc);
    }
    launches_json.PushBack(std::move(launch_json), alloc);

    // Map loads: the k-th map command in the console goes with the k-th
    // load in the log.
    std::vector<const console_section *> map_sections;
    for (const auto &section : sections) {
      if (section.kind == "map") {
        map_sections.push_back(&section);
      }
    }
    // A map-check run that failed while loading still counts as a load when
    // neither the log nor a console shows it (the game ended at once).
    const bool failed_load =
        record && !record->map.empty() && record->result.ends_with("_load");
    const std::size_t count_loads =
        std::max({loads.size(), map_sections.size(),
                  std::size_t{failed_load ? 1u : 0u}});
    for (std::size_t k = 0; k < count_loads; ++k) {
      const map_load *load = k < loads.size() ? &loads[k] : nullptr;
      const console_section *section =
          k < map_sections.size() ? map_sections[k] : nullptr;
      load_row row;
      row.variant = variant;
      row.journal = journal;
      row.map = load && !load->map.empty() ? load->map
                : section                  ? section->map
                : record                   ? record->map
                                           : std::string();
      row.ready = load && load->ready_ms.has_value();
      if (load && load->loading_ms && load->ready_ms) {
        row.loading_to_ready_s = (*load->ready_ms - *load->loading_ms) / 1000.0;
      }
      if (k == 0 && record && record->load_s && row.ready) {
        if (new_journal) {
          row.command_to_ready_s = *record->load_s;
          if (row.loading_to_ready_s) {
            row.command_to_level_s = *record->load_s - *row.loading_to_ready_s;
          }
        } else {
          row.command_to_level_s = *record->load_s;
        }
      }
      json_value item(rapidjson::kObjectType);
      put(item, "session", session, alloc);
      put(item, "map", row.map, alloc);
      put(item, "variant", variant, alloc);
      if (!folder.empty()) {
        put(item, "map_check", folder, alloc);
      }
      put(item, "journal", journal, alloc);
      put(item, "ready", json_value(row.ready), alloc);
      if (load && !load->ended.empty()) {
        put(item, "ended", load->ended, alloc);
      } else if (!load) {
        put(item, "ended",
            k == 0 && record && !section
                ? "map-check: " + record->detail
                : std::string("no load in the game log"),
            alloc);
      }
      if (k == 0 && record) {
        put(item, "map_check_result", record->result, alloc);
      }
      put(item, "map_command_to_ready_s", num(row.command_to_ready_s), alloc);
      put(item, "loading_map_to_ready_s", num(row.loading_to_ready_s), alloc);
      put(item, "map_command_to_level_s", num(row.command_to_level_s), alloc);
      if (section) {
        const map_load_zones part = map_load_part(*section);
        row.zones_ms = part.zones_ms;
        json_value zones(rapidjson::kObjectType);
        put(zones, "count", count(part.zones), alloc);
        put(zones, "total_ms", num(part.zones_ms), alloc);
        put(zones, "map_zone_loaded", json_value(part.map_zone_loaded), alloc);
        json_value groups(rapidjson::kObjectType);
        for (const auto &[group, ms] : part.ms_by_group) {
          json_value g(rapidjson::kObjectType);
          put(g, "ms", num(ms), alloc);
          put(g, "zones", count(part.zones_by_group.at(group)), alloc);
          put(groups, to_string(group), std::move(g), alloc);
          row.zone_groups[to_string(group)] = ms;
        }
        put(zones, "by_group", std::move(groups), alloc);
        put(item, "zones", std::move(zones), alloc);
        json_value after(rapidjson::kObjectType);
        put(after, "zones", count(part.after_zones), alloc);
        put(after, "ms", num(part.after_ms), alloc);
        put(item, "after_map_zone", std::move(after), alloc);
        if (const auto it = part.ms_by_group.find(zone_group::frontend);
            it != part.ms_by_group.end()) {
          row.frontend_reload_ms = it->second;
        }
        put(item, "frontend_reload_ms", num(row.frontend_reload_ms), alloc);
        json_value mod_loads(rapidjson::kArrayType);
        for (const auto &span : section->mod_loads) {
          json_value m(rapidjson::kObjectType);
          put(m, "name", span.name, alloc);
          put(m, "zones", count(span.zones), alloc);
          put(m, "zones_ms", num(span.zones_ms), alloc);
          put(m, "done", json_value(span.done), alloc);
          mod_loads.PushBack(std::move(m), alloc);
        }
        put(item, "mod_loads", std::move(mod_loads), alloc);
        put(item, "fs_startups", count(section->fs_startups), alloc);
      }
      loads_json.PushBack(std::move(item), alloc);
      rows.push_back(std::move(row));
    }
  }

  // Summaries.
  json_value startup(rapidjson::kObjectType);
  json_value by_launcher(rapidjson::kObjectType);
  for (const auto &[launcher, by_variant] : to_frontend_by_launcher) {
    json_value variants(rapidjson::kObjectType);
    for (const auto &[variant, values] : by_variant) {
      put(variants, variant, spread_json(values, alloc), alloc);
    }
    put(by_launcher, launcher, std::move(variants), alloc);
  }
  put(startup, "game_start_to_frontend_s", std::move(by_launcher), alloc);
  json_value first_by_variant(rapidjson::kObjectType);
  for (const auto &[variant, values] : first_log_by_variant) {
    put(first_by_variant, variant, spread_json(values, alloc), alloc);
  }
  put(startup, "first_log_to_frontend_s", std::move(first_by_variant), alloc);
  json_value zones_summary(rapidjson::kObjectType);
  for (const auto &[group, values] : startup_zone_ms) {
    put(zones_summary, group, spread_json(values, alloc), alloc);
  }
  put(startup, "startup_zones_ms", std::move(zones_summary), alloc);
  put(startup, "consoles_from_start", count(startup_consoles), alloc);

  struct group_values {
    std::size_t runs = 0;
    std::size_t ready = 0;
    std::size_t old_journal = 0;
    std::size_t unknown_journal = 0;
    std::vector<double> command_to_ready;
    std::vector<double> loading_to_ready;
    std::vector<double> command_to_level;
    std::vector<double> zones_ms;
    std::map<std::string, std::vector<double>> by_group;
    std::vector<double> frontend_reload;
  };
  std::map<std::pair<std::string, std::string>, group_values> groups;
  for (const auto &row : rows) {
    auto &g = groups[{row.map, row.variant}];
    ++g.runs;
    g.ready += row.ready ? 1 : 0;
    g.old_journal += row.journal == "old" ? 1 : 0;
    g.unknown_journal += row.journal == "unknown" ? 1 : 0;
    if (row.command_to_level_s) {
      g.command_to_level.push_back(*row.command_to_level_s);
    }
    if (row.command_to_ready_s) {
      g.command_to_ready.push_back(*row.command_to_ready_s);
    }
    if (row.loading_to_ready_s) {
      g.loading_to_ready.push_back(*row.loading_to_ready_s);
    }
    if (row.zones_ms && row.ready) {
      g.zones_ms.push_back(*row.zones_ms);
      for (const char *name :
           {"map", "main_mod", "extra_mod", "frontend", "stock"}) {
        const auto it = row.zone_groups.find(name);
        g.by_group[name].push_back(it == row.zone_groups.end() ? 0
                                                               : it->second);
      }
    }
    if (row.frontend_reload_ms > 0) {
      g.frontend_reload.push_back(row.frontend_reload_ms);
    }
  }
  json_value map_summary(rapidjson::kArrayType);
  for (const auto &[key, g] : groups) {
    json_value item(rapidjson::kObjectType);
    put(item, "map", key.first, alloc);
    put(item, "variant", key.second, alloc);
    put(item, "runs", count(g.runs), alloc);
    put(item, "ready", count(g.ready), alloc);
    put(item, "old_journal", count(g.old_journal), alloc);
    put(item, "unknown_journal", count(g.unknown_journal), alloc);
    put(item, "map_command_to_level_s", spread_json(g.command_to_level, alloc),
        alloc);
    put(item, "map_command_to_ready_s", spread_json(g.command_to_ready, alloc),
        alloc);
    put(item, "loading_map_to_ready_s", spread_json(g.loading_to_ready, alloc),
        alloc);
    put(item, "zones_ms", spread_json(g.zones_ms, alloc), alloc);
    json_value by_group(rapidjson::kObjectType);
    for (const auto &[name, values] : g.by_group) {
      put(by_group, name, num(percentile(values, 50)), alloc);
    }
    put(item, "zones_ms_median_by_group", std::move(by_group), alloc);
    put(item, "frontend_reloads", count(g.frontend_reload.size()), alloc);
    map_summary.PushBack(std::move(item), alloc);
  }

  put(doc, "startup", std::move(startup), alloc);
  put(doc, "map_summary", std::move(map_summary), alloc);
  put(doc, "launches", std::move(launches_json), alloc);
  put(doc, "map_loads", std::move(loads_json), alloc);
  if (as_json) {
    return print_json(doc);
  }

  std::cout << "Game starts: " << games.size() << " game logs\n";
  if (to_frontend_by_launcher.empty()) {
    std::cout << "  launch.started to the first main frame: none, no launcher "
                 "or CLI log names these games' pids\n";
  }
  for (const auto &[launcher, by_variant] : to_frontend_by_launcher) {
    std::cout << "  launch.started to the first main frame (journal "
                 "frontend), started by "
              << (launcher == "cli" ? "xerces_cli" : "Xerces.exe") << ":\n";
    for (const auto &[variant, values] : by_variant) {
      std::cout << "    " << pad(variant, 12) << spread_text(values, 2, " s")
                << "\n";
    }
  }
  std::cout << "  the game log's first line to the first main frame:\n";
  for (const auto &[variant, values] : first_log_by_variant) {
    std::cout << "    " << pad(variant, 12) << spread_text(values, 2, " s")
              << "\n";
  }
  if (startup_consoles) {
    std::cout << "  startup zone times, " << startup_consoles
              << " consoles that have the whole start (sum of FastFileLoad "
                 "times):\n";
    for (const auto &[group, values] : startup_zone_ms) {
      std::cout << "    " << pad(group, 12) << spread_text(values, 0, " ms")
                << "\n";
    }
  }
  std::cout << "\nMap loads: " << rows.size()
            << " (medians [min-max] in seconds. \"to level\" and \"to "
               "player\": map-check's stopwatch from the console map command "
               "to the engine in the level and to the player in it; it "
               "starts 1.5 s after the command and stops 0 to 2.5 s after the "
               "event, so each is off by about -1.5 to +2.5 s. "
               "\"level->player\": the game log, +-1 s)\n";
  if (!rows.empty()) {
    std::cout << "  " << pad("map", 18) << pad("variant", 12) << pad("ok", 6)
              << pad("to level", 18) << pad("to player", 20)
              << pad("level->player", 18)
              << "zones ms (map/main/extra/frontend/stock)\n";
  }
  const auto median_range = [](const std::vector<double> &values) {
    if (values.empty()) {
      return std::string("-");
    }
    const sample_stats s = describe_sample(values);
    return fixed(s.median, 1) +
           (s.n > 1 ? " [" + fixed(s.min, 1) + "-" + fixed(s.max, 1) + "]"
                    : std::string());
  };
  for (const auto &[key, g] : groups) {
    const std::string level = median_range(g.command_to_level);
    const std::string command = median_range(g.command_to_ready);
    const std::string log_s = median_range(g.loading_to_ready);
    std::string zones = "-";
    if (!g.zones_ms.empty()) {
      zones.clear();
      for (const char *name :
           {"map", "main_mod", "extra_mod", "frontend", "stock"}) {
        zones += (zones.empty() ? "" : "/") +
                 fixed(percentile(g.by_group.at(name), 50), 0);
      }
    }
    std::cout << "  " << pad(key.first, 18) << pad(key.second, 12)
              << pad(std::to_string(g.ready) + "/" + std::to_string(g.runs), 6)
              << pad(level, 18) << pad(command, 20) << pad(log_s, 18) << zones
              << "\n";
  }
  bool reloads = false;
  for (const auto &[key, g] : groups) {
    for (const double ms : g.frontend_reload) {
      if (!reloads) {
        std::cout << "\nFront-end zones reloaded inside a map load:\n";
        reloads = true;
      }
      std::cout << "  " << key.first << " (" << key.second
                << "): " << fixed(ms, 0) << " ms\n";
    }
  }
  return exit_ok;
}

// ----------------------------------------------------- bench launcher
int cmd_bench_launcher(const arguments &args) {
  if (!accept_only(args, 2, {"--home", "--logs"}, {"--json"})) {
    return exit_error;
  }
  const std::vector<fs::path> logs = logs_folders(args, true);
  if (!check_folders(logs)) {
    return exit_error;
  }
  std::map<std::string, fs::path> files; // by name: the same log twice is one
  for (const auto &dir : logs) {
    for (const auto &file : files_in(dir, ".jsonl")) {
      if (xerces_core::log_role(file) == "launcher") {
        files.emplace(file_name(file), file);
      }
    }
  }
  std::vector<std::string> mark_order;
  std::map<std::string, std::vector<double>> marks;
  std::vector<double> to_detected;
  std::vector<double> to_scanned;
  struct scan {
    std::string session;
    double seconds = 0;
    double bytes = 0;
    double items = 0;
    double hits = 0;
    double misses = 0;
    bool startup = false; // the session's first scan
  };
  std::vector<scan> scans;
  std::size_t sessions = 0;
  std::size_t dry = 0;
  std::size_t later_detections = 0;
  for (const auto &[name, path] : files) {
    const auto text = read_file(path);
    if (!text) {
      continue;
    }
    const auto events = parse_log_events(*text);
    std::optional<std::int64_t> start;
    bool counted = false;
    // Rescan, a new game folder and the other screens' refreshes log both
    // events again later in the session; only the first of each is startup.
    bool detected = false;
    bool scanned = false;
    for (const auto &event : events) {
      if (event.event == "launcher.start") {
        start = event.ts_ms;
        detected = scanned = false;
        if (!counted) {
          ++sessions;
          counted = true;
        }
        dry += event.text("dry_launch") == "true" ? 1 : 0;
      } else if (event.event == "launcher.startup.step") {
        const auto ms = event.number("ms");
        if (!ms) {
          continue;
        }
        const std::string mark = startup_mark(event.msg);
        if (!marks.contains(mark)) {
          mark_order.push_back(mark);
        }
        marks[mark].push_back(*ms);
      } else if (event.event == "launcher.game.detected") {
        if (detected) {
          ++later_detections;
        } else if (start && event.ts_ms) {
          to_detected.push_back(static_cast<double>(*event.ts_ms - *start));
        }
        detected = true;
      } else if (event.event == "launcher.catalog.scanned") {
        if (!scanned && start && event.ts_ms) {
          to_scanned.push_back(static_cast<double>(*event.ts_ms - *start));
        }
        scans.push_back({event.session, event.number("seconds").value_or(NAN),
                         event.number("bytes_hashed").value_or(0),
                         event.number("items").value_or(0),
                         event.number("cache_hits").value_or(0),
                         event.number("cache_misses").value_or(0), !scanned});
        scanned = true;
      }
    }
  }
  const std::size_t later_scans = static_cast<std::size_t>(std::count_if(
      scans.begin(), scans.end(), [](const scan &s) { return !s.startup; }));
  std::vector<double> warm;
  std::vector<double> cold;
  for (const auto &s : scans) {
    (s.misses > 0 || s.bytes > 0 ? cold : warm).push_back(s.seconds);
  }

  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  put(doc, "schema", "xerces.bench.launcher/1", alloc);
  json_value sources(rapidjson::kObjectType);
  put(sources, "logs", paths_json(logs, alloc), alloc);
  put(doc, "sources", std::move(sources), alloc);
  put(doc, "sessions", count(sessions), alloc);
  put(doc, "dry_launch_sessions", count(dry), alloc);
  put(doc, "definitions",
      "marks: launcher.startup.step times as the launcher logs them (ms "
      "from its start). to_game_detected_ms and to_catalog_scanned_ms: from "
      "launcher.start to the session's first such event, by the log's "
      "timestamps; later ones (Rescan, a new game folder) are counted in "
      "later_detections and later_scans and are not startup samples. A scan "
      "is cold when it hashed files (cache misses), warm when the hash cache "
      "had every file; warm_s and cold_s take every scan, startup or later "
      "(scans.list says which).",
      alloc);
  put(doc, "later_detections", count(later_detections), alloc);
  put(doc, "later_scans", count(later_scans), alloc);
  json_value marks_json(rapidjson::kArrayType);
  for (const auto &mark : mark_order) {
    json_value item = spread_json(marks[mark], alloc);
    json::set(item, "mark", mark, alloc);
    marks_json.PushBack(std::move(item), alloc);
  }
  put(doc, "marks_ms", std::move(marks_json), alloc);
  put(doc, "to_game_detected_ms", spread_json(to_detected, alloc), alloc);
  put(doc, "to_catalog_scanned_ms", spread_json(to_scanned, alloc), alloc);
  json_value scans_json(rapidjson::kObjectType);
  put(scans_json, "warm_s", spread_json(warm, alloc), alloc);
  put(scans_json, "cold_s", spread_json(cold, alloc), alloc);
  json_value list(rapidjson::kArrayType);
  for (const auto &s : scans) {
    json_value item(rapidjson::kObjectType);
    put(item, "session", s.session, alloc);
    put(item, "startup", json_value(s.startup), alloc);
    put(item, "seconds", num(s.seconds), alloc);
    put(item, "bytes_hashed", num(s.bytes), alloc);
    put(item, "items", num(s.items), alloc);
    put(item, "cache_hits", num(s.hits), alloc);
    put(item, "cache_misses", num(s.misses), alloc);
    put(item, "gb_per_s",
        num(s.bytes > 0 && s.seconds > 0 ? s.bytes / 1e9 / s.seconds : NAN),
        alloc);
    list.PushBack(std::move(item), alloc);
  }
  put(scans_json, "list", std::move(list), alloc);
  put(doc, "scans", std::move(scans_json), alloc);
  if (args.has("--json")) {
    return print_json(doc);
  }

  std::cout << "Launcher starts: " << sessions << " sessions (" << dry
            << " dry launches) in " << files.size() << " logs\n";
  std::cout << "  startup marks (ms from the launcher's start):\n";
  for (const auto &mark : mark_order) {
    std::cout << "    " << pad(mark, 34) << spread_text(marks[mark], 1, "")
              << "\n";
  }
  std::cout << "    " << pad("start to game detected", 34)
            << spread_text(to_detected, 0, "") << "\n";
  std::cout << "    " << pad("start to catalog scanned", 34)
            << spread_text(to_scanned, 0, "") << "\n";
  if (later_detections || later_scans) {
    std::cout << "    (each session's first of these only; " << later_detections
              << " later detections and " << later_scans
              << " later scans are not startup samples)\n";
  }
  std::cout << "  Workshop scans: warm (hash cache had every file) "
            << spread_text(warm, 2, " s") << "\n";
  std::cout << "                  cold (hashed files) "
            << spread_text(cold, 1, " s") << "\n";
  for (const auto &s : scans) {
    if (s.bytes > 0) {
      std::cout << "    session " << s.session << ": "
                << fixed(s.bytes / 1e9, 1) << " GB in " << fixed(s.seconds, 1)
                << " s (" << fixed(s.bytes / 1e9 / s.seconds, 2) << " GB/s), "
                << fixed(s.misses, 0) << " files hashed\n";
    }
  }
  return exit_ok;
}

// ------------------------------------------- bench analyze / compare
struct run {
  fs::path path;
  frame_series series;
  frame_summary summary;
};

// A capture before the warm-up is dropped.
struct capture {
  fs::path path;
  frame_series series;
};

bool name_ends_with(const fs::path &path, std::string_view suffix) {
  std::string name = file_name(path);
  for (char &c : name) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return name.ends_with(suffix);
}

std::optional<frame_series> read_frames(const fs::path &path,
                                        std::string &error) {
  const auto text = read_file(path);
  if (!text) {
    error = "cannot be read";
    return std::nullopt;
  }
  auto parsed = parse_frames_csv(*text);
  if (!parsed.series) {
    error = parsed.error;
    return std::nullopt;
  }
  return std::move(parsed.series);
}

// The captures a path names, each one run:
// - a file: that CSV, the probe's or PresentMon's. <run>.main.csv is the
//   probe's main-thread cadence, not frame times, and is refused.
// - a folder: its runs and those of its subfolders, one level down (the
//   plan's layout: bench\<run>\ with <run>.frames.csv, <run>.main.csv and
//   PresentMon's presentmon.csv). Each *.frames.csv is a probe run, each
//   other CSV with PresentMon's columns a PresentMon run; *.main.csv and
//   other CSVs are left out.
bool collect_captures(const fs::path &path, std::vector<capture> &out) {
  std::error_code ec;
  if (!fs::is_directory(path, ec)) {
    if (name_ends_with(path, ".main.csv")) {
      std::cerr << utf8(path)
                << " is the probe's main-thread cadence, not frame times; "
                   "give its .frames.csv.\n";
      return false;
    }
    std::string error;
    auto series = read_frames(path, error);
    if (!series) {
      std::cerr << utf8(path) << ": " << error << "\n";
      return false;
    }
    out.push_back({path, std::move(*series)});
    return true;
  }
  std::vector<fs::path> folders{path};
  for (auto &sub : folders_in(path)) {
    folders.push_back(std::move(sub));
  }
  for (const auto &folder : folders) {
    for (const auto &file : files_in(folder, ".csv")) {
      if (name_ends_with(file, ".main.csv")) {
        continue;
      }
      const bool probe_name = name_ends_with(file, ".frames.csv");
      std::string error;
      auto series = read_frames(file, error);
      if (probe_name && !series) {
        std::cerr << utf8(file) << ": " << error << "\n";
        return false;
      }
      if (!series ||
          (!probe_name && series->format != frames_format::presentmon)) {
        std::cerr << "Left out " << utf8(file)
                  << ": neither a *.frames.csv nor a PresentMon CSV.\n";
        continue;
      }
      out.push_back({file, std::move(*series)});
    }
  }
  return true;
}

// --source probe|presentmon; false (with the reason printed) when it is
// something else.
bool source_option(const arguments &args,
                   std::optional<frames_format> &source) {
  const auto given = args.get("--source");
  if (!given) {
    return true;
  }
  if (*given == "probe" || *given == "presentmon") {
    source =
        *given == "probe" ? frames_format::probe : frames_format::presentmon;
    return true;
  }
  std::cerr << "--source takes probe or presentmon, not \"" << *given
            << "\".\n";
  return false;
}

// The probe and PresentMon time the same frames, so a set of runs takes one
// of them: --source picks it, and without it a set holding both is refused
// (a run folder with both would count as two runs). Keeps the captures of
// that source and returns it; nullopt (the reason printed) when refused.
std::optional<frames_format>
pick_source(const std::vector<std::vector<capture> *> &sides,
            std::optional<frames_format> source) {
  std::size_t counts[2] = {0, 0};
  for (const auto *side : sides) {
    for (const auto &c : *side) {
      ++counts[c.series.format == frames_format::probe ? 0 : 1];
    }
  }
  if (!source) {
    if (counts[0] && counts[1]) {
      std::cerr << "Found " << counts[0]
                << " probe captures (*.frames.csv) and " << counts[1]
                << " PresentMon captures. They time the same frames, so a "
                   "run folder holding both would count twice: pick one with "
                   "--source probe or --source presentmon.\n";
      return std::nullopt;
    }
    source = counts[0] ? frames_format::probe : frames_format::presentmon;
  }
  const std::size_t other = counts[*source == frames_format::probe ? 1 : 0];
  if (other) {
    std::cerr << "Left out " << other << " "
              << (*source == frames_format::probe ? "PresentMon" : "probe")
              << " captures (--source " << to_string(*source) << ").\n";
  }
  for (auto *side : sides) {
    std::erase_if(*side,
                  [&](const capture &c) { return c.series.format != *source; });
  }
  return source;
}

std::optional<run> load_run(capture &&c, double warmup_s) {
  run out;
  out.path = std::move(c.path);
  out.series = drop_warmup(c.series, warmup_s);
  if (out.series.dt_ms.empty()) {
    std::cerr << utf8(out.path) << ": no frames left after the " << warmup_s
              << " s warm-up.\n";
    return std::nullopt;
  }
  out.summary = summarize_frames(out.series.dt_ms);
  return out;
}

double mean_finite(const std::vector<double> &values) {
  double sum = 0;
  std::size_t n = 0;
  for (const double v : values) {
    if (std::isfinite(v)) {
      sum += v;
      ++n;
    }
  }
  return n ? sum / static_cast<double>(n) : NAN;
}

double percentile_finite(const std::vector<double> &values, double p) {
  std::vector<double> kept;
  for (const double v : values) {
    if (std::isfinite(v)) {
      kept.push_back(v);
    }
  }
  return percentile(std::move(kept), p);
}

json_value low_json(const low_pair &low, json_alloc &alloc) {
  json_value out(rapidjson::kObjectType);
  put(out, "mean_of_worst_fps", num(low.mean_of_worst_fps), alloc);
  put(out, "percentile_fps", num(low.percentile_fps), alloc);
  put(out, "worst_frames", count(low.worst_frames), alloc);
  return out;
}

constexpr const char *frames_definitions =
    "Runs: each *.frames.csv (the probe's) or PresentMon CSV is one run, of "
    "one source (--source when a folder holds both); *.main.csv is not a "
    "run. Percentiles: linear between closest ranks (type 7). mean_ms = "
    "total time / frames. Lows both ways: 1000 / mean of the worst x% of "
    "frames, and 1000 / the (100-x)th percentile. Hitch: longer than 2x the "
    "rolling median of 31 frames and than that median + 4 ms. stutter_pct: "
    "time in hitch frames / total time. drift: second half's mean frame "
    "time against the first's, by time; a warning above 2%. With "
    "--warmup-s, every count (rows, rows_skipped, not_displayed, "
    "present_modes, flagged_frames, frames_per_cpu) is over the frames "
    "kept; presentmon.other_rows is the whole capture.";

json_value run_json(const run &r, double warmup_s, json_alloc &alloc) {
  const frame_summary &s = r.summary;
  const frame_series &f = r.series;
  json_value out(rapidjson::kObjectType);
  put(out, "file", utf8(r.path), alloc);
  put(out, "format", to_string(f.format), alloc);
  put(out, "frame_time_column", f.dt_column, alloc);
  put(out, "time_column", f.time_column, alloc);
  put(out, "warmup_s", num(warmup_s), alloc);
  put(out, "warmup_rows_dropped", count(f.warmup_rows), alloc);
  put(out, "warmup_frames_dropped", count(f.warmup_frames), alloc);
  put(out, "rows", count(f.rows), alloc);
  put(out, "rows_skipped", count(f.skipped), alloc);
  put(out, "frames", count(s.frames), alloc);
  put(out, "seconds", num(s.total_ms / 1000.0), alloc);
  put(out, "mean_ms", num(s.mean_ms), alloc);
  put(out, "avg_fps", num(s.avg_fps), alloc);
  put(out, "sd_ms", num(s.sd_ms), alloc);
  put(out, "min_ms", num(s.min_ms), alloc);
  put(out, "median_ms", num(s.median_ms), alloc);
  put(out, "p95_ms", num(s.p95_ms), alloc);
  put(out, "p99_ms", num(s.p99_ms), alloc);
  put(out, "p999_ms", num(s.p999_ms), alloc);
  put(out, "max_ms", num(s.max_ms), alloc);
  put(out, "low_1", low_json(s.low_1, alloc), alloc);
  put(out, "low_01", low_json(s.low_01, alloc), alloc);
  json_value hitches(rapidjson::kObjectType);
  put(hitches, "count", count(s.hitches), alloc);
  put(hitches, "per_minute",
      num(s.total_ms > 0 ? s.hitches / (s.total_ms / 60000.0) : NAN), alloc);
  put(hitches, "over_50ms", count(s.over_50ms), alloc);
  put(hitches, "over_100ms", count(s.over_100ms), alloc);
  put(hitches, "time_ms", num(s.hitch_ms), alloc);
  json_value list(rapidjson::kArrayType);
  for (const auto &h : s.hitch_list) {
    json_value item(rapidjson::kObjectType);
    put(item, "frame", count(h.index), alloc);
    put(item, "t_ms", num(h.index < f.t_ms.size() ? f.t_ms[h.index] : NAN),
        alloc);
    put(item, "dt_ms", num(h.dt_ms), alloc);
    put(item, "rolling_median_ms", num(h.rolling_median_ms), alloc);
    list.PushBack(std::move(item), alloc);
  }
  put(hitches, "longest", std::move(list), alloc);
  put(out, "hitches", std::move(hitches), alloc);
  put(out, "stutter_pct", num(s.stutter_pct), alloc);
  json_value drift(rapidjson::kObjectType);
  put(drift, "first_half_mean_ms", num(s.first_half_mean_ms), alloc);
  put(drift, "second_half_mean_ms", num(s.second_half_mean_ms), alloc);
  put(drift, "pct", num(s.drift_pct), alloc);
  put(drift, "warning", json_value(s.drift_warning), alloc);
  put(out, "drift", std::move(drift), alloc);
  if (f.format == frames_format::presentmon) {
    json_value pm(rapidjson::kObjectType);
    put(pm, "application", f.application, alloc);
    put(pm, "process_id", f.process_id, alloc);
    put(pm, "other_rows", count(f.other_rows), alloc);
    put(pm, "not_displayed", count(f.not_displayed), alloc);
    put(pm, "cpu_busy_mean_ms", num(mean_finite(f.cpu_busy_ms)), alloc);
    put(pm, "gpu_busy_mean_ms", num(mean_finite(f.gpu_busy_ms)), alloc);
    put(pm, "cpu_busy_pct_of_frame",
        num(100.0 * mean_finite(f.cpu_busy_ms) / s.mean_ms), alloc);
    put(pm, "gpu_busy_pct_of_frame",
        num(100.0 * mean_finite(f.gpu_busy_ms) / s.mean_ms), alloc);
    json_value modes(rapidjson::kObjectType);
    for (const auto &[mode, n] : f.present_modes) {
      put(modes, mode.empty() ? "(none)" : mode, count(n), alloc);
    }
    put(pm, "present_modes", std::move(modes), alloc);
    put(out, "presentmon", std::move(pm), alloc);
  } else {
    json_value probe(rapidjson::kObjectType);
    put(probe, "present_mean_ms", num(mean_finite(f.present_ms)), alloc);
    put(probe, "present_p99_ms", num(percentile_finite(f.present_ms, 99)),
        alloc);
    put(probe, "overlay_mean_us", num(mean_finite(f.overlay_us)), alloc);
    put(probe, "overlay_p99_us", num(percentile_finite(f.overlay_us, 99)),
        alloc);
    put(probe, "flagged_frames", count(f.flagged), alloc);
    json_value cpus(rapidjson::kObjectType);
    for (const auto &[cpu, n] : f.cpus) {
      put(cpus, std::to_string(cpu), count(n), alloc);
    }
    put(probe, "frames_per_cpu", std::move(cpus), alloc);
    put(out, "probe", std::move(probe), alloc);
  }
  return out;
}

void print_run(const run &r) {
  const frame_summary &s = r.summary;
  const frame_series &f = r.series;
  std::cout << utf8(r.path) << "  (" << to_string(f.format) << ", "
            << f.dt_column << ")\n";
  if (f.warmup_frames || f.warmup_rows) {
    std::cout << "  warm-up left out: " << f.warmup_frames << " frames ("
              << f.warmup_rows << " rows); the counts below are after it\n";
  }
  std::cout << "  " << s.frames << " frames in " << fixed(s.total_ms / 1000, 1)
            << " s: mean " << fixed(s.mean_ms, 3) << " ms ("
            << fixed(s.avg_fps, 1) << " FPS), median " << fixed(s.median_ms, 3)
            << ", p95 " << fixed(s.p95_ms, 3) << ", p99 " << fixed(s.p99_ms, 3)
            << ", p99.9 " << fixed(s.p999_ms, 3) << ", max "
            << fixed(s.max_ms, 3) << " ms\n";
  std::cout << "  1% low: " << fixed(s.low_1.mean_of_worst_fps, 1)
            << " FPS (mean of the worst " << s.low_1.worst_frames
            << " frames), " << fixed(s.low_1.percentile_fps, 1)
            << " FPS (1000/p99)\n";
  std::cout << "  0.1% low: " << fixed(s.low_01.mean_of_worst_fps, 1)
            << " FPS (mean of the worst " << s.low_01.worst_frames
            << " frames), " << fixed(s.low_01.percentile_fps, 1)
            << " FPS (1000/p99.9)\n";
  std::cout << "  hitches: " << s.hitches << " (" << s.over_50ms
            << " over 50 ms, " << s.over_100ms << " over 100 ms), stutter "
            << fixed(s.stutter_pct, 2) << "% of the time; drift "
            << fixed(s.drift_pct, 2) << "% between halves"
            << (s.drift_warning ? " (WARNING: over 2%, not steady)" : "")
            << "\n";
  if (f.format == frames_format::presentmon && !f.cpu_busy_ms.empty()) {
    std::cout << "  CPU busy " << fixed(mean_finite(f.cpu_busy_ms), 3)
              << " ms, GPU busy " << fixed(mean_finite(f.gpu_busy_ms), 3)
              << " ms on average (" << f.application << ", pid " << f.process_id
              << ")\n";
  }
}

struct metric_def {
  const char *name;
  const char *unit;
  double (*get)(const frame_summary &);
};

const std::vector<metric_def> &metric_defs() {
  static const std::vector<metric_def> defs = {
      {"mean_ms", "ms", [](const frame_summary &s) { return s.mean_ms; }},
      {"avg_fps", "fps", [](const frame_summary &s) { return s.avg_fps; }},
      {"median_ms", "ms", [](const frame_summary &s) { return s.median_ms; }},
      {"p95_ms", "ms", [](const frame_summary &s) { return s.p95_ms; }},
      {"p99_ms", "ms", [](const frame_summary &s) { return s.p99_ms; }},
      {"p999_ms", "ms", [](const frame_summary &s) { return s.p999_ms; }},
      {"low_1_mean_of_worst_fps", "fps",
       [](const frame_summary &s) { return s.low_1.mean_of_worst_fps; }},
      {"low_1_percentile_fps", "fps",
       [](const frame_summary &s) { return s.low_1.percentile_fps; }},
      {"low_01_mean_of_worst_fps", "fps",
       [](const frame_summary &s) { return s.low_01.mean_of_worst_fps; }},
      {"low_01_percentile_fps", "fps",
       [](const frame_summary &s) { return s.low_01.percentile_fps; }},
      {"stutter_pct", "%",
       [](const frame_summary &s) { return s.stutter_pct; }},
      {"hitches_per_minute", "1/min",
       [](const frame_summary &s) {
         return s.total_ms > 0 ? s.hitches / (s.total_ms / 60000.0) : 0.0;
       }},
  };
  return defs;
}

std::vector<double> metric_values(const std::vector<run> &runs,
                                  const metric_def &def) {
  std::vector<double> out;
  for (const auto &r : runs) {
    out.push_back(def.get(r.summary));
  }
  return out;
}

int cmd_bench_analyze(const arguments &args) {
  if (args.words.size() < 3) {
    std::cerr << "Give one or more frames CSVs (the probe's or PresentMon's), "
                 "or folders of them.\n";
    return exit_error;
  }
  if (!accept_only(args, args.words.size(), {"--warmup-s", "--source"},
                   {"--json"})) {
    return exit_error;
  }
  bool ok = true;
  const double warmup = number_option(args, "--warmup-s", 0, ok).value_or(0);
  std::optional<frames_format> source;
  if (!ok || !source_option(args, source)) {
    return exit_error;
  }
  std::vector<capture> captures;
  for (std::size_t i = 2; i < args.words.size(); ++i) {
    if (!collect_captures(xerces_core::path_from_utf8(args.words[i]), captures)) {
      return exit_error;
    }
  }
  if (captures.empty()) {
    std::cerr << "No frames CSVs found (*.frames.csv or PresentMon CSVs).\n";
    return exit_error;
  }
  const auto picked = pick_source({&captures}, source);
  if (!picked) {
    return exit_error;
  }
  std::vector<run> runs;
  for (auto &c : captures) {
    auto loaded = load_run(std::move(c), warmup);
    if (!loaded) {
      return exit_error;
    }
    runs.push_back(std::move(*loaded));
  }
  if (runs.empty()) {
    std::cerr << "No " << to_string(*picked) << " frames CSVs found.\n";
    return exit_error;
  }
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  put(doc, "schema", "xerces.bench.analyze/1", alloc);
  put(doc, "source", to_string(*picked), alloc);
  put(doc, "definitions", frames_definitions, alloc);
  json_value runs_json(rapidjson::kArrayType);
  for (const auto &r : runs) {
    runs_json.PushBack(run_json(r, warmup, alloc), alloc);
  }
  put(doc, "runs", std::move(runs_json), alloc);
  if (runs.size() >= 2) {
    json_value across(rapidjson::kObjectType);
    for (const auto &def : metric_defs()) {
      put(across, def.name,
          stats_json(describe_sample(metric_values(runs, def)), alloc), alloc);
    }
    put(doc, "across_runs", std::move(across), alloc);
  }
  if (args.has("--json")) {
    return print_json(doc);
  }
  for (const auto &r : runs) {
    print_run(r);
  }
  if (runs.size() >= 2) {
    std::cout << "\nAcross " << runs.size()
              << " runs (each run one sample; mean and 95% CI):\n";
    for (const auto &def : metric_defs()) {
      const sample_stats s = describe_sample(metric_values(runs, def));
      std::cout << "  " << pad(def.name, 26) << fixed(s.mean, 3) << " "
                << def.unit << "  [" << fixed(*s.ci_low, 3) << ", "
                << fixed(*s.ci_high, 3) << "]\n";
    }
  }
  return exit_ok;
}

int cmd_bench_compare(const arguments &args) {
  if (!accept_only(args, 2,
                   {"--a", "--b", "--margin-mean", "--margin-p99", "--warmup-s",
                    "--source"},
                   {"--json"})) {
    return exit_error;
  }
  const auto a_path = args.get("--a");
  const auto b_path = args.get("--b");
  if (!a_path || !b_path) {
    std::cerr << "bench compare needs --a <folder|csv> and --b <folder|csv>: "
                 "A is the reference (upstream, or the build before), B the "
                 "candidate.\n";
    return exit_error;
  }
  bool ok = true;
  const double margin_mean =
      number_option(args, "--margin-mean", 1.0, ok).value_or(1.0);
  const double margin_p99 =
      number_option(args, "--margin-p99", 3.0, ok).value_or(3.0);
  const double warmup = number_option(args, "--warmup-s", 0, ok).value_or(0);
  std::optional<frames_format> source;
  if (!ok || !source_option(args, source)) {
    return exit_error;
  }
  const std::string side_paths[2] = {*a_path, *b_path};
  std::vector<capture> captures[2];
  for (int side = 0; side < 2; ++side) {
    if (!collect_captures(xerces_core::path_from_utf8(side_paths[side]), captures[side])) {
      return exit_error;
    }
    if (captures[side].empty()) {
      std::cerr << side_paths[side] << " has no frames CSVs.\n";
      return exit_error;
    }
  }
  const auto picked = pick_source({&captures[0], &captures[1]}, source);
  if (!picked) {
    return exit_error;
  }
  std::vector<run> sides[2];
  for (int side = 0; side < 2; ++side) {
    for (auto &c : captures[side]) {
      auto loaded = load_run(std::move(c), warmup);
      if (!loaded) {
        return exit_error;
      }
      sides[side].push_back(std::move(*loaded));
    }
    if (sides[side].empty()) {
      std::cerr << side_paths[side] << " has no " << to_string(*picked)
                << " frames CSVs.\n";
      return exit_error;
    }
  }
  const auto &a = sides[0];
  const auto &b = sides[1];

  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  put(doc, "schema", "xerces.bench.compare/1", alloc);
  put(doc, "source", to_string(*picked), alloc);
  put(doc, "definitions",
      "Each run (one *.frames.csv or one PresentMon CSV, of one source) is "
      "one sample; bench analyze's definitions give each run's numbers. "
      "ci95: t-based 95% interval of the mean. "
      "welch: mean(B) - mean(A) with its 95% interval and two-sided p. tost: "
      "B is equivalent to A when the 90% interval of the difference lies "
      "inside +-margin (percent of A's mean), alpha 0.05 each side. "
      "pooled_lows: every run's frames pooled, resampled by whole runs "
      "(cluster bootstrap, fixed seed); descriptive, not a test.",
      alloc);
  for (int side = 0; side < 2; ++side) {
    json_value block(rapidjson::kObjectType);
    put(block, "path", side_paths[side], alloc);
    json_value runs_json(rapidjson::kArrayType);
    for (const auto &r : sides[side]) {
      runs_json.PushBack(run_json(r, warmup, alloc), alloc);
    }
    put(block, "runs", std::move(runs_json), alloc);
    put(doc, side == 0 ? "a" : "b", std::move(block), alloc);
  }

  struct row {
    const metric_def *def;
    sample_stats a;
    sample_stats b;
    welch_result w;
    std::optional<tost_result> t;
  };
  std::vector<row> table;
  json_value metrics(rapidjson::kArrayType);
  for (const auto &def : metric_defs()) {
    const auto va = metric_values(a, def);
    const auto vb = metric_values(b, def);
    row r{&def, describe_sample(va), describe_sample(vb), welch(va, vb), {}};
    const std::string name = def.name;
    if (name == "mean_ms") {
      r.t = tost(va, vb, margin_mean);
    } else if (name == "p99_ms") {
      r.t = tost(va, vb, margin_p99);
    }
    json_value item(rapidjson::kObjectType);
    put(item, "metric", def.name, alloc);
    put(item, "unit", def.unit, alloc);
    put(item, "a", stats_json(r.a, alloc), alloc);
    put(item, "b", stats_json(r.b, alloc), alloc);
    json_value w(rapidjson::kNullType);
    if (r.w.ok) {
      w.SetObject();
      put(w, "diff", num(r.w.diff), alloc);
      put(w, "diff_pct", num(r.w.diff_pct), alloc);
      json_value ci(rapidjson::kArrayType);
      ci.PushBack(num(r.w.ci_low), alloc);
      ci.PushBack(num(r.w.ci_high), alloc);
      put(w, "ci95", std::move(ci), alloc);
      put(w, "df", num(r.w.df), alloc);
      put(w, "p", num(r.w.p_value), alloc);
    }
    put(item, "welch", std::move(w), alloc);
    if (r.t) {
      json_value t(rapidjson::kObjectType);
      put(t, "margin_pct", num(r.t->margin_pct), alloc);
      if (r.t->ok) {
        put(t, "margin", num(r.t->margin), alloc);
        json_value ci(rapidjson::kArrayType);
        ci.PushBack(num(r.t->ci_low), alloc);
        ci.PushBack(num(r.t->ci_high), alloc);
        put(t, "ci90", std::move(ci), alloc);
        put(t, "p_lower", num(r.t->p_lower), alloc);
        put(t, "p_upper", num(r.t->p_upper), alloc);
        put(t, "equivalent", json_value(r.t->equivalent), alloc);
      } else {
        put(t, "equivalent", json_value(rapidjson::kNullType), alloc);
        put(t, "reason", "needs at least 2 runs a side", alloc);
      }
      put(item, "tost", std::move(t), alloc);
    }
    metrics.PushBack(std::move(item), alloc);
    table.push_back(std::move(r));
  }
  put(doc, "metrics", std::move(metrics), alloc);

  std::vector<std::vector<double>> frames_a;
  std::vector<std::vector<double>> frames_b;
  for (const auto &r : a) {
    frames_a.push_back(r.series.dt_ms);
  }
  for (const auto &r : b) {
    frames_b.push_back(r.series.dt_ms);
  }
  const pooled_bootstrap pooled = bootstrap_pooled_lows(frames_a, frames_b);
  json_value pooled_json(rapidjson::kObjectType);
  put(pooled_json, "descriptive_only", json_value(true), alloc);
  put(pooled_json, "iterations", count(pooled.iterations), alloc);
  put(pooled_json, "seed", json_value(pooled.seed), alloc);
  json_value pooled_metrics(rapidjson::kArrayType);
  for (const auto &m : pooled.metrics) {
    json_value item(rapidjson::kObjectType);
    put(item, "metric", m.name, alloc);
    const auto interval = [&](const bootstrap_interval &i) {
      json_value v(rapidjson::kObjectType);
      put(v, "estimate", num(i.estimate), alloc);
      json_value range(rapidjson::kArrayType);
      range.PushBack(num(i.low), alloc);
      range.PushBack(num(i.high), alloc);
      put(v, "interval95", std::move(range), alloc);
      return v;
    };
    put(item, "a", interval(m.a), alloc);
    put(item, "b", interval(m.b), alloc);
    put(item, "diff", interval(m.diff), alloc);
    pooled_metrics.PushBack(std::move(item), alloc);
  }
  put(pooled_json, "metrics", std::move(pooled_metrics), alloc);
  put(doc, "pooled_lows", std::move(pooled_json), alloc);

  const auto verdict = [&](const row &r) -> std::string {
    if (!r.t || !r.t->ok) {
      return "not tested (needs at least 2 runs a side)";
    }
    return r.t->equivalent
               ? "equivalent within +-" + fixed(r.t->margin_pct, 1) + "%"
               : "not shown equivalent within +-" + fixed(r.t->margin_pct, 1) +
                     "%";
  };
  json_value verdicts(rapidjson::kObjectType);
  for (const auto &r : table) {
    if (r.t) {
      put(verdicts, r.def->name, verdict(r), alloc);
    }
  }
  put(doc, "verdict", std::move(verdicts), alloc);
  if (args.has("--json")) {
    return print_json(doc);
  }

  std::cout << "A: " << side_paths[0] << " (" << a.size()
            << " runs)\nB: " << side_paths[1] << " (" << b.size()
            << " runs)\nEach run is one sample; B - A with its 95% CI.\n\n";
  std::cout << "  " << pad("metric", 26) << pad("A mean", 12)
            << pad("B mean", 12) << pad("B - A", 12) << pad("95% CI", 22)
            << "p\n";
  for (const auto &r : table) {
    std::cout << "  " << pad(r.def->name, 26) << pad(fixed(r.a.mean, 3), 12)
              << pad(fixed(r.b.mean, 3), 12)
              << pad(r.w.ok ? fixed(r.w.diff, 3) : "-", 12)
              << pad(r.w.ok
                         ? fixed(r.w.ci_low, 3) + " to " + fixed(r.w.ci_high, 3)
                         : "-",
                     22)
              << (!r.w.ok               ? std::string("-")
                  : r.w.p_value < 0.001 ? std::string("<0.001")
                                        : fixed(r.w.p_value, 3))
              << "\n";
  }
  std::cout << "\nEquivalence (TOST, alpha 0.05):\n";
  for (const auto &r : table) {
    if (r.t) {
      std::cout << "  " << pad(r.def->name, 10) << verdict(r) << "\n";
    }
  }
  std::cout << "\nPooled lows (cluster bootstrap, " << pooled.iterations
            << " resamples, seed " << pooled.seed
            << "; descriptive, not a test):\n";
  for (const auto &m : pooled.metrics) {
    std::cout << "  " << pad(m.name, 26) << "A " << fixed(m.a.estimate, 1)
              << "  B " << fixed(m.b.estimate, 1) << "  B - A "
              << fixed(m.diff.estimate, 1) << " [" << fixed(m.diff.low, 1)
              << ", " << fixed(m.diff.high, 1) << "]\n";
  }
  for (int side = 0; side < 2; ++side) {
    for (const auto &r : sides[side]) {
      if (r.summary.drift_warning) {
        std::cout << "WARNING: " << utf8(r.path) << " drifted "
                  << fixed(r.summary.drift_pct, 2)
                  << "% between its halves; the run was not steady.\n";
      }
    }
  }
  return exit_ok;
}

int bench_usage() {
  std::cerr
      << "bench loads [--home <dir>] [--logs <dir>]... [--console <dir>]... "
         "[--json]\n"
         "bench launcher [--home <dir>] [--logs <dir>]... [--json]\n"
         "bench analyze <frames.csv|folder>... [--source probe|presentmon] "
         "[--warmup-s <s>] [--json]\n"
         "bench compare --a <folder|csv> --b <folder|csv> [--source "
         "probe|presentmon] [--margin-mean 1] [--margin-p99 3] [--warmup-s "
         "<s>] [--json]\n"
         "bench run: not built yet (it needs the game).\n";
  return exit_error;
}
} // namespace

int cmd_bench(const arguments &args, const std::string &action) {
  if (action == "loads") {
    return cmd_bench_loads(args);
  }
  if (action == "launcher") {
    return cmd_bench_launcher(args);
  }
  if (action == "analyze") {
    return cmd_bench_analyze(args);
  }
  if (action == "compare") {
    return cmd_bench_compare(args);
  }
  if (action == "run") {
    std::cerr << "bench run is not built yet: it drives the game (the "
                 "in-game frame probe and the session scripts, perf plan H3 "
                 "and H5), and every game start needs your go-ahead. Offline "
                 "now: bench loads, bench launcher, bench analyze, bench "
                 "compare.\n";
    return exit_not_running;
  }
  if (!action.empty()) {
    std::cerr << "Unknown bench command \"" << action << "\".\n";
  }
  return bench_usage();
}
} // namespace xerces_cli
