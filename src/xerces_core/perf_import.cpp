#include "perf_import.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <unordered_map>

#include "json.hpp"
#include "zone_names.hpp"

namespace xerces_core::perf {
namespace {
constexpr double nan = std::numeric_limits<double>::quiet_NaN();

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                           text.front() == '\r' || text.front() == '\n')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                           text.back() == '\r' || text.back() == '\n')) {
    text.remove_suffix(1);
  }
  return text;
}

std::string lower(std::string_view text) {
  std::string out(text);
  for (char &c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

std::string_view skip_bom(std::string_view text) {
  if (text.starts_with("\xEF\xBB\xBF")) {
    text.remove_prefix(3);
  }
  return text;
}

// Lines without their ends ("\r\n", "\n" or "\r").
std::vector<std::string_view> split_lines(std::string_view text) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n' || text[i] == '\r') {
      lines.push_back(text.substr(start, i - start));
      if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
      start = i + 1;
    }
  }
  if (start < text.size()) {
    lines.push_back(text.substr(start));
  }
  return lines;
}

// One CSV row; a field may be double-quoted ("" inside is a quote).
std::vector<std::string> split_csv(std::string_view line) {
  std::vector<std::string> fields;
  std::string field;
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
        field.push_back('"');
        ++i;
      } else if (c == '"') {
        quoted = false;
      } else {
        field.push_back(c);
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      fields.push_back(std::string(trim(field)));
      field.clear();
    } else {
      field.push_back(c);
    }
  }
  fields.push_back(std::string(trim(field)));
  return fields;
}

std::optional<double> to_number(std::string_view text) {
  text = trim(text);
  if (text.empty()) {
    return std::nullopt;
  }
  if (text.front() == '+') {
    text.remove_prefix(1);
  }
  double value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

// Column positions by lower-cased name.
struct header {
  std::unordered_map<std::string, std::size_t> columns;

  std::optional<std::size_t> find(std::initializer_list<const char *> names,
                                  std::string *found = nullptr) const {
    for (const char *name : names) {
      if (const auto it = columns.find(name); it != columns.end()) {
        if (found) {
          *found = name;
        }
        return it->second;
      }
    }
    return std::nullopt;
  }
};

header read_header(const std::vector<std::string> &fields) {
  header out;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    out.columns.emplace(lower(fields[i]), i);
  }
  return out;
}

std::string_view field_at(const std::vector<std::string> &row,
                          std::optional<std::size_t> column) {
  if (!column || *column >= row.size()) {
    return {};
  }
  return row[*column];
}

// The name PresentMon's header spells it with.
std::string original_name(const std::vector<std::string> &fields,
                          std::size_t column) {
  return column < fields.size() ? fields[column] : std::string();
}

frames_result parse_probe(const std::vector<std::string_view> &lines,
                          const std::vector<std::string> &names,
                          const header &head) {
  frame_series out;
  out.format = frames_format::probe;
  const auto dt = head.find({"dt_ms"});
  const auto t = head.find({"t_ms"});
  const auto present = head.find({"present_ms"});
  const auto overlay = head.find({"overlay_us"});
  const auto cpu = head.find({"cpu"});
  const auto flags = head.find({"flags"});
  out.dt_column = original_name(names, *dt);
  out.time_column = t ? original_name(names, *t) : std::string();
  std::optional<double> first_t;
  double summed = 0;
  for (std::size_t i = 1; i < lines.size(); ++i) {
    if (trim(lines[i]).empty()) {
      continue;
    }
    const auto row = split_csv(lines[i]);
    ++out.rows;
    const auto frame = to_number(field_at(row, dt));
    const auto at = to_number(field_at(row, t));
    if (!frame || *frame <= 0) {
      ++out.skipped;
      continue;
    }
    double time = summed;
    if (t && at) {
      if (!first_t) {
        first_t = *at;
      }
      time = *at - *first_t;
    }
    summed += *frame;
    out.t_ms.push_back(time);
    out.dt_ms.push_back(*frame);
    if (present) {
      out.present_ms.push_back(to_number(field_at(row, present)).value_or(nan));
    }
    if (overlay) {
      out.overlay_us.push_back(to_number(field_at(row, overlay)).value_or(nan));
    }
    if (const auto number = to_number(field_at(row, cpu))) {
      ++out.cpus[static_cast<int>(*number)];
    }
    if (const auto value = to_number(field_at(row, flags)); value && *value) {
      ++out.flagged;
    }
  }
  if (out.dt_ms.empty()) {
    return {std::nullopt, "no frames with a frame time in the probe CSV"};
  }
  return {std::move(out), {}};
}

frames_result parse_presentmon(const std::vector<std::string_view> &lines,
                               const std::vector<std::string> &names,
                               const header &head) {
  frame_series out;
  out.format = frames_format::presentmon;
  std::string dt_name;
  const auto dt = head.find(
      {"msbetweenpresents", "msbetweenappstart", "frametime"}, &dt_name);
  out.dt_column = original_name(names, *dt);
  // Times: seconds unless the name says milliseconds. CPUStartQPC counts
  // ticks of a frequency the file does not give, and CPUStartDateTime is a
  // date, so with those the times are summed from the frame times instead.
  std::string time_name;
  const auto t =
      head.find({"cpustarttimeinms", "cpustartqpctimeinms", "cpustarttime",
                 "cpustarttimeinseconds", "timeinseconds"},
                &time_name);
  const double time_scale = time_name.ends_with("inms") ? 1.0 : 1000.0;
  const auto cpu_busy = head.find({"mscpubusy", "cpubusy"});
  const auto gpu_busy = head.find({"msgpubusy", "gpubusy"});
  const auto displayed =
      head.find({"msuntildisplayed", "displayedtime", "displaylatency"});
  const auto dropped = head.find({"dropped"});
  const auto mode = head.find({"presentmode"});
  const auto application = head.find({"application"});
  const auto process = head.find({"processid"});
  const auto swap_chain = head.find({"swapchainaddress"});

  // Rows by process and swap chain; the one with the most rows is the game.
  std::map<std::string, std::vector<std::vector<std::string>>> groups;
  std::vector<std::string> order;
  for (std::size_t i = 1; i < lines.size(); ++i) {
    if (trim(lines[i]).empty()) {
      continue;
    }
    auto row = split_csv(lines[i]);
    std::string key = std::string(field_at(row, application)) + "\x1f" +
                      std::string(field_at(row, process)) + "\x1f" +
                      std::string(field_at(row, swap_chain));
    auto &group = groups[key];
    if (group.empty()) {
      order.push_back(key);
    }
    group.push_back(std::move(row));
  }
  if (groups.empty()) {
    return {std::nullopt, "no rows in the PresentMon CSV"};
  }
  std::string best = order.front();
  std::size_t total_rows = 0;
  for (const auto &key : order) {
    total_rows += groups[key].size();
    if (groups[key].size() > groups[best].size()) {
      best = key;
    }
  }
  const auto &rows = groups[best];
  out.other_rows = total_rows - rows.size();
  out.application = std::string(field_at(rows.front(), application));
  out.process_id = std::string(field_at(rows.front(), process));
  out.time_column = t ? original_name(names, *t) : std::string();

  std::optional<double> first_t;
  double summed = 0;
  for (const auto &row : rows) {
    ++out.rows;
    const auto frame = to_number(field_at(row, dt));
    if (!frame || *frame <= 0) {
      ++out.skipped;
      continue;
    }
    double time = summed;
    if (t) {
      if (const auto at = to_number(field_at(row, t))) {
        if (!first_t) {
          first_t = *at;
        }
        time = (*at - *first_t) * time_scale;
      }
    }
    summed += *frame;
    out.t_ms.push_back(time);
    out.dt_ms.push_back(*frame);
    if (cpu_busy) {
      out.cpu_busy_ms.push_back(
          to_number(field_at(row, cpu_busy)).value_or(nan));
    }
    if (gpu_busy) {
      out.gpu_busy_ms.push_back(
          to_number(field_at(row, gpu_busy)).value_or(nan));
    }
    const bool shown_unknown =
        displayed && !to_number(field_at(row, displayed)).has_value();
    const bool was_dropped =
        dropped && to_number(field_at(row, dropped)).value_or(0) != 0;
    if (shown_unknown || was_dropped) {
      ++out.not_displayed;
    }
    if (mode) {
      ++out.present_modes[std::string(field_at(row, mode))];
    }
  }
  if (out.dt_ms.empty()) {
    return {std::nullopt, "no frames with a frame time in the PresentMon CSV"};
  }
  return {std::move(out), {}};
}

std::vector<std::string> split_list(std::string_view text, char separator) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = std::min(text.find(separator, start), text.size());
    const auto item = trim(text.substr(start, end - start));
    if (!item.empty()) {
      out.emplace_back(item);
    }
    start = end + 1;
  }
  return out;
}

// "core_frontend", "en_core_frontend_patch", ...
bool is_frontend_zone(std::string_view zone) {
  if (zone.size() > 3 && zone[2] == '_' && !zone.starts_with("core")) {
    zone.remove_prefix(3);
  }
  return zone.starts_with("core_frontend");
}

bool parse_size(std::string_view text, std::size_t &out) {
  text = trim(text);
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), out);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
         !text.empty();
}

// days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool digits(std::string_view text, std::size_t at, std::size_t count,
            int &out) {
  if (at + count > text.size()) {
    return false;
  }
  out = 0;
  for (std::size_t i = at; i < at + count; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      return false;
    }
    out = out * 10 + (text[i] - '0');
  }
  return true;
}
} // namespace

const char *to_string(frames_format format) {
  return format == frames_format::probe ? "probe" : "presentmon";
}

frames_result parse_frames_csv(std::string_view text) {
  const auto lines = split_lines(skip_bom(text));
  std::size_t first = 0;
  while (first < lines.size() && trim(lines[first]).empty()) {
    ++first;
  }
  if (first == lines.size()) {
    return {std::nullopt, "the file is empty"};
  }
  const std::vector<std::string_view> rows(
      lines.begin() + static_cast<long long>(first), lines.end());
  const auto names = split_csv(rows.front());
  const header head = read_header(names);
  if (head.find({"dt_ms"})) {
    return parse_probe(rows, names, head);
  }
  if (head.find({"msbetweenpresents", "msbetweenappstart", "frametime"})) {
    return parse_presentmon(rows, names, head);
  }
  return {std::nullopt,
          "not a frames CSV: the header has neither the probe's dt_ms nor "
          "PresentMon's MsBetweenPresents, MsBetweenAppStart or FrameTime"};
}

frame_series drop_warmup(const frame_series &series, double warmup_s) {
  if (warmup_s <= 0 || series.t_ms.empty()) {
    return series;
  }
  frame_series out = series;
  out.t_ms.clear();
  out.dt_ms.clear();
  out.cpu_busy_ms.clear();
  out.gpu_busy_ms.clear();
  out.present_ms.clear();
  out.overlay_us.clear();
  const double from = series.t_ms.front() + warmup_s * 1000.0;
  for (std::size_t i = 0; i < series.t_ms.size(); ++i) {
    if (series.t_ms[i] < from) {
      continue;
    }
    out.t_ms.push_back(series.t_ms[i]);
    out.dt_ms.push_back(series.dt_ms[i]);
    const auto keep = [&](const std::vector<double> &from_series,
                          std::vector<double> &to) {
      if (i < from_series.size()) {
        to.push_back(from_series[i]);
      }
    };
    keep(series.cpu_busy_ms, out.cpu_busy_ms);
    keep(series.gpu_busy_ms, out.gpu_busy_ms);
    keep(series.present_ms, out.present_ms);
    keep(series.overlay_us, out.overlay_us);
  }
  return out;
}

console_log parse_console(std::string_view text) {
  console_log out;
  const auto lines = split_lines(skip_bom(text));
  out.lines = lines.size();
  constexpr std::string_view zone_prefix = "FastFileLoad: Setting ";
  constexpr std::string_view zone_middle = " to XZONE_LOADED in ";
  constexpr std::string_view supplemented = "[ Workshop ] Supplemented ";
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = trim(lines[i]);
    const std::size_t number = i + 1;
    if (i == 0 && line.starts_with("Command line:")) {
      out.from_start = true;
      const auto words = split_list(line.substr(13), ' ');
      for (std::size_t w = 0; w + 2 < words.size(); ++w) {
        if (words[w] == "+set" && words[w + 1] == "fs_game") {
          out.fs_game = words[w + 2];
        } else if (words[w] == "+set" && words[w + 1] == "fs_extramods") {
          out.fs_extramods = split_list(words[w + 2], ',');
        }
      }
      continue;
    }
    if (line == "----- FS_Startup -----") {
      out.events.push_back({console_kind::fs_startup, number, {}, 0, 0});
    } else if (line.starts_with(zone_prefix)) {
      const std::size_t middle = line.find(zone_middle);
      if (middle == std::string_view::npos) {
        continue;
      }
      std::string_view ms = line.substr(middle + zone_middle.size());
      while (!ms.empty() && (ms.back() == '.' || ms.back() == ' ')) {
        ms.remove_suffix(1);
      }
      if (!ms.ends_with("ms")) {
        continue;
      }
      ms.remove_suffix(2);
      std::size_t value = 0;
      if (!parse_size(ms, value)) {
        continue;
      }
      const auto zone =
          line.substr(zone_prefix.size(), middle - zone_prefix.size());
      out.events.push_back({console_kind::zone_loaded, number,
                            std::string(zone), static_cast<double>(value), 0});
    } else if (line.starts_with("ModLoad start: ")) {
      out.events.push_back({console_kind::mod_load_start, number,
                            std::string(trim(line.substr(15))), 0, 0});
    } else if (line.starts_with("ModLoad done: ")) {
      out.events.push_back({console_kind::mod_load_done, number,
                            std::string(trim(line.substr(14))), 0, 0});
    } else if (line.starts_with(supplemented)) {
      const auto words = split_list(line.substr(supplemented.size()), ' ');
      std::size_t count = 0;
      if (words.size() >= 2 && parse_size(words[0], count)) {
        out.events.push_back(
            {console_kind::supplemented, number, words[1], 0, count});
      }
    } else if (line.starts_with("]") && line.size() > 1) {
      out.events.push_back({console_kind::command, number,
                            std::string(trim(line.substr(1))), 0, 0});
    }
  }
  return out;
}

const char *to_string(zone_group group) {
  switch (group) {
  case zone_group::map:
    return "map";
  case zone_group::main_mod:
    return "main_mod";
  case zone_group::extra_mod:
    return "extra_mod";
  case zone_group::frontend:
    return "frontend";
  case zone_group::stock:
    break;
  }
  return "stock";
}

zone_group group_of_zone(std::string_view zone, std::string_view map,
                         std::span<const std::string> extra_ids) {
  switch (classify_zone(zone, map, extra_ids).kind) {
  case zone_kind::usermap:
    return zone_group::map;
  case zone_kind::main_tier:
    return zone_group::main_mod;
  case zone_kind::extra:
    return zone_group::extra_mod;
  case zone_kind::stock:
    break;
  }
  return is_frontend_zone(zone) ? zone_group::frontend : zone_group::stock;
}

std::vector<console_section> console_sections(const console_log &log) {
  std::vector<console_section> out;
  out.push_back({});
  out.back().kind = "startup";
  out.back().first_line = 1;
  for (const auto &event : log.events) {
    if (event.kind == console_kind::command) {
      out.back().last_line = event.line - 1;
      console_section next;
      next.command = event.name;
      next.first_line = event.line;
      const auto words = split_list(event.name, ' ');
      if (words.size() >= 2 &&
          (lower(words[0]) == "map" || lower(words[0]) == "devmap")) {
        next.kind = "map";
        next.map = words[1];
      } else {
        next.kind = "command";
      }
      out.push_back(std::move(next));
      continue;
    }
    console_section &section = out.back();
    switch (event.kind) {
    case console_kind::fs_startup:
      ++section.fs_startups;
      break;
    case console_kind::zone_loaded: {
      const zone_group group =
          group_of_zone(event.name, section.map, log.fs_extramods);
      section.zones.push_back({event.name, event.ms, group, event.line});
      section.zones_ms += event.ms;
      section.ms_by_group[group] += event.ms;
      ++section.zones_by_group[group];
      for (auto &span : section.mod_loads) {
        if (!span.done) {
          ++span.zones;
          span.zones_ms += event.ms;
        }
      }
      break;
    }
    case console_kind::mod_load_start:
      section.mod_loads.push_back({event.name, 0, 0, false});
      break;
    case console_kind::mod_load_done:
      for (auto &span : section.mod_loads) {
        if (!span.done && span.name == event.name) {
          span.done = true;
          break;
        }
      }
      break;
    case console_kind::supplemented:
      section.supplemented.emplace_back(event.name, event.count);
      break;
    case console_kind::command:
      break;
    }
  }
  out.back().last_line = log.lines;
  return out;
}

map_load_zones map_load_part(const console_section &section) {
  map_load_zones out;
  std::size_t end = section.zones.size();
  for (std::size_t i = section.zones.size(); i-- > 0;) {
    if (section.zones[i].group == zone_group::map) {
      end = i + 1;
      out.map_zone_loaded = true;
      break;
    }
  }
  for (std::size_t i = 0; i < section.zones.size(); ++i) {
    const zone_load &zone = section.zones[i];
    if (i < end) {
      ++out.zones;
      out.zones_ms += zone.ms;
      out.ms_by_group[zone.group] += zone.ms;
      ++out.zones_by_group[zone.group];
    } else {
      ++out.after_zones;
      out.after_ms += zone.ms;
    }
  }
  return out;
}

std::optional<double> log_event::number(const std::string &key) const {
  if (const auto it = numbers.find(key); it != numbers.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::string log_event::text(const std::string &key) const {
  if (const auto it = strings.find(key); it != strings.end()) {
    return it->second;
  }
  return {};
}

std::optional<std::int64_t> parse_timestamp_ms(std::string_view text) {
  text = trim(text);
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!digits(text, 0, 4, year) || text.size() < 19 || text[4] != '-' ||
      !digits(text, 5, 2, month) || text[7] != '-' ||
      !digits(text, 8, 2, day) || (text[10] != 'T' && text[10] != ' ') ||
      !digits(text, 11, 2, hour) || text[13] != ':' ||
      !digits(text, 14, 2, minute) || text[16] != ':' ||
      !digits(text, 17, 2, second)) {
    return std::nullopt;
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
      minute > 59 || second > 60) {
    return std::nullopt;
  }
  std::size_t at = 19;
  double fraction = 0;
  if (at < text.size() && text[at] == '.') {
    ++at;
    double scale = 0.1;
    const std::size_t start = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
      fraction += (text[at] - '0') * scale;
      scale /= 10;
      ++at;
    }
    if (at == start) {
      return std::nullopt;
    }
  }
  std::int64_t offset_minutes = 0;
  if (at < text.size()) {
    if (text[at] == 'Z' && at + 1 == text.size()) {
      ++at;
    } else if ((text[at] == '+' || text[at] == '-') && at + 6 == text.size() &&
               text[at + 3] == ':') {
      int oh = 0;
      int om = 0;
      if (!digits(text, at + 1, 2, oh) || !digits(text, at + 4, 2, om)) {
        return std::nullopt;
      }
      offset_minutes = (oh * 60 + om) * (text[at] == '+' ? 1 : -1);
      at += 6;
    } else {
      return std::nullopt;
    }
  }
  const std::int64_t days = days_from_civil(year, static_cast<unsigned>(month),
                                            static_cast<unsigned>(day));
  const std::int64_t seconds =
      days * 86400 + hour * 3600 + minute * 60 + second - offset_minutes * 60;
  return seconds * 1000 +
         static_cast<std::int64_t>(std::llround(fraction * 1000));
}

std::optional<log_event> parse_log_event(std::string_view line) {
  line = trim(skip_bom(line));
  if (line.empty() || line.front() != '{') {
    return std::nullopt;
  }
  const auto parsed = json::parse(line);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  const auto &root = parsed.document;
  const auto event = json::get_string(root, "event");
  if (!event) {
    return std::nullopt;
  }
  log_event out;
  out.event = *event;
  out.msg = json::get_string(root, "msg").value_or("");
  out.role = json::get_string(root, "role").value_or("");
  out.session = json::get_string(root, "session").value_or("");
  out.level = json::get_string(root, "level").value_or("");
  out.ts = json::get_string(root, "ts").value_or("");
  out.ts_ms = parse_timestamp_ms(out.ts);
  if (const auto *data = json::member(root, "data"); data && data->IsObject()) {
    for (auto it = data->MemberBegin(); it != data->MemberEnd(); ++it) {
      const std::string key(it->name.GetString(), it->name.GetStringLength());
      const auto &value = it->value;
      if (value.IsNumber()) {
        out.numbers[key] = value.GetDouble();
      } else if (value.IsString()) {
        out.strings[key] =
            std::string(value.GetString(), value.GetStringLength());
      } else if (value.IsBool()) {
        out.strings[key] = value.GetBool() ? "true" : "false";
      }
    }
  }
  return out;
}

std::vector<log_event> parse_log_events(std::string_view text) {
  std::vector<log_event> out;
  for (const auto line : split_lines(skip_bom(text))) {
    if (auto event = parse_log_event(line)) {
      out.push_back(std::move(*event));
    }
  }
  return out;
}

std::string startup_mark(std::string_view msg) {
  msg = trim(msg);
  const std::size_t at = msg.rfind(" at ");
  if (at != std::string_view::npos && msg.ends_with(" ms")) {
    const auto number = msg.substr(at + 4, msg.size() - at - 4 - 3);
    if (to_number(number)) {
      return std::string(trim(msg.substr(0, at)));
    }
  }
  return std::string(msg);
}
} // namespace xerces_core::perf
