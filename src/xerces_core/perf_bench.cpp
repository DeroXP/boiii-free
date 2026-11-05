#include "perf_bench.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>

#include "json.hpp"

namespace xerces_core::bench {
namespace {
// Fixed decimals; "null" for values JSON cannot hold.
std::string number(double value, int decimals = 3) {
  if (!std::isfinite(value)) {
    return "null";
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  return buffer;
}

std::string number(const std::optional<double> &value, int decimals = 3) {
  return value ? number(*value, decimals) : "null";
}

std::string boolean(bool value) { return value ? "true" : "false"; }

std::optional<double> parse_seconds(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  double value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() ||
      !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

char fold(char c) {
  if (c == '/') {
    return '\\';
  }
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// The path's components, lower-cased, without empty ones or a trailing
// separator.
std::vector<std::string> components(const std::filesystem::path &path) {
  std::vector<std::string> out;
  for (const auto &part : path.lexically_normal()) {
    std::string text = part.string();
    std::transform(text.begin(), text.end(), text.begin(), fold);
    if (text.empty() || text == ".") {
      continue;
    }
    out.push_back(std::move(text));
  }
  return out;
}
} // namespace

double ticks_to_ms(std::int64_t ticks, std::int64_t frequency) {
  if (frequency <= 0) {
    return 0;
  }
  return static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency);
}

std::string frames_csv(std::span<const frame_sample> frames,
                       std::optional<std::int64_t> previous_qpc,
                       const run_clock &clock, std::uint64_t first_index) {
  std::string out = "i,t_ms,dt_ms,present_ms,overlay_us,cpu,flags\n";
  out.reserve(out.size() + frames.size() * 48);
  std::optional<std::int64_t> previous = previous_qpc;
  char line[160];
  std::uint64_t index = first_index;
  for (const frame_sample &frame : frames) {
    const double t = ticks_to_ms(frame.qpc - clock.t0_qpc, clock.frequency);
    const double present = ticks_to_ms(frame.present_ticks, clock.frequency);
    if (previous) {
      std::snprintf(line, sizeof(line), "%llu,%.4f,%.4f,%.4f,%u,%u,%u\n",
                    static_cast<unsigned long long>(index), t,
                    ticks_to_ms(frame.qpc - *previous, clock.frequency),
                    present, unsigned{frame.overlay_us}, unsigned{frame.cpu},
                    unsigned{frame.flags});
    } else {
      std::snprintf(line, sizeof(line), "%llu,%.4f,,%.4f,%u,%u,%u\n",
                    static_cast<unsigned long long>(index), t, present,
                    unsigned{frame.overlay_us}, unsigned{frame.cpu},
                    unsigned{frame.flags});
    }
    out += line;
    previous = frame.qpc;
    ++index;
  }
  return out;
}

std::string main_csv(std::span<const std::int64_t> stamps,
                     std::optional<std::int64_t> previous_qpc,
                     const run_clock &clock, std::uint64_t first_index) {
  std::string out = "i,t_ms,dt_ms\n";
  out.reserve(out.size() + stamps.size() * 28);
  std::optional<std::int64_t> previous = previous_qpc;
  char line[96];
  std::uint64_t index = first_index;
  for (const std::int64_t stamp : stamps) {
    const double t = ticks_to_ms(stamp - clock.t0_qpc, clock.frequency);
    if (previous) {
      std::snprintf(line, sizeof(line), "%llu,%.4f,%.4f\n",
                    static_cast<unsigned long long>(index), t,
                    ticks_to_ms(stamp - *previous, clock.frequency));
    } else {
      std::snprintf(line, sizeof(line), "%llu,%.4f,\n",
                    static_cast<unsigned long long>(index), t);
    }
    out += line;
    previous = stamp;
    ++index;
  }
  return out;
}

std::optional<auto_run> parse_auto_run(std::string_view text) {
  const auto first = text.find(':');
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  const auto second = text.find(':', first + 1);
  if (second == std::string_view::npos ||
      text.find(':', second + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  auto_run run;
  const std::string_view when = text.substr(0, first);
  if (when == "menu") {
    run.when = auto_run::trigger::menu;
  } else if (when == "ingame") {
    run.when = auto_run::trigger::ingame;
  } else {
    return std::nullopt;
  }
  const auto start = parse_seconds(text.substr(first + 1, second - first - 1));
  const auto length = parse_seconds(text.substr(second + 1));
  if (!start || !length || *start < 0 || *start > 3600 || *length <= 0 ||
      *length > 3600) {
    return std::nullopt;
  }
  run.start_s = *start;
  run.length_s = *length;
  return run;
}

std::string run_name(std::string_view requested, std::string_view fallback) {
  std::string out;
  for (const char c : requested) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (ok && !(out.empty() && c == '.') && out.size() < 64) {
      out += c;
    }
  }
  while (!out.empty() && out.back() == '.') {
    out.pop_back();
  }
  if (out.empty()) {
    return std::string(fallback);
  }
  // Windows keeps device names (CON, NUL, COM1, ...) even with an extension.
  std::string stem = out.substr(0, out.find('.'));
  std::transform(stem.begin(), stem.end(), stem.begin(), [](char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  });
  const bool numbered = stem.size() == 4 &&
                        (stem.starts_with("COM") || stem.starts_with("LPT")) &&
                        stem[3] >= '0' && stem[3] <= '9';
  if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
      numbered) {
    out = "run-" + out;
  }
  return out;
}

bool output_allowed(const std::filesystem::path &dir,
                    const std::vector<std::filesystem::path> &forbidden) {
  if (dir.empty() || !dir.is_absolute()) {
    return false;
  }
  const std::vector<std::string> target = components(dir);
  for (const auto &root : forbidden) {
    if (root.empty()) {
      continue;
    }
    const std::vector<std::string> prefix = components(root);
    if (prefix.empty() || prefix.size() > target.size()) {
      continue;
    }
    if (std::equal(prefix.begin(), prefix.end(), target.begin())) {
      return false;
    }
  }
  return true;
}

std::string_view swap_effect_name(std::uint32_t swap_effect) {
  switch (swap_effect) {
  case 0:
    return "discard";
  case 1:
    return "sequential";
  case 3:
    return "flip_sequential";
  case 4:
    return "flip_discard";
  default:
    return "unknown";
  }
}

std::string swapchain_json(const swapchain_info &info) {
  char flags[16];
  std::snprintf(flags, sizeof(flags), "0x%X", info.flags);
  std::string out = "{\"has_desc\":" + boolean(info.has_desc);
  out += ",\"width\":" + std::to_string(info.width);
  out += ",\"height\":" + std::to_string(info.height);
  out += ",\"refresh_numerator\":" + std::to_string(info.refresh_numerator);
  out += ",\"refresh_denominator\":" + std::to_string(info.refresh_denominator);
  out += ",\"refresh_hz\":" +
         (info.refresh_denominator
              ? number(static_cast<double>(info.refresh_numerator) /
                       static_cast<double>(info.refresh_denominator))
              : std::string("null"));
  out += ",\"format\":" + std::to_string(info.format);
  out += ",\"buffer_count\":" + std::to_string(info.buffer_count);
  out += ",\"swap_effect\":" + std::to_string(info.swap_effect);
  out += ",\"swap_effect_name\":" +
         json::quote(swap_effect_name(info.swap_effect));
  out += ",\"flags\":" + json::quote(flags);
  out += ",\"windowed\":" + boolean(info.windowed);
  out += ",\"sync_interval\":" + std::to_string(info.sync_interval);
  out += ",\"present_flags\":" + std::to_string(info.present_flags);
  out += ",\"swapchain1\":" + boolean(info.swapchain1);
  out += ",\"swapchain2\":" + boolean(info.swapchain2);
  return out + "}";
}

std::string run_meta_json(const run_meta &meta) {
  std::string out = "{\n  \"schema\": \"xerces.bench/1\"";
  out += ",\n  \"run\": " + json::quote(meta.run);
  out += ",\n  \"kind\": " + json::quote(meta.kind);
  out += ",\n  \"started_utc\": " + json::quote(meta.started_utc);
  out += ",\n  \"map\": " + json::quote(meta.map);
  out += ",\n  \"pid\": " + std::to_string(meta.pid);
  out += ",\n  \"qpc_frequency\": " + std::to_string(meta.qpc_frequency);
  out += ",\n  \"t0_qpc\": " + std::to_string(meta.t0_qpc);
  out += ",\n  \"t1_qpc\": " + std::to_string(meta.t1_qpc);
  out += ",\n  \"duration_ms\": " +
         number(ticks_to_ms(meta.t1_qpc - meta.t0_qpc, meta.qpc_frequency));
  out += ",\n  \"frames\": " + std::to_string(meta.frames);
  out += ",\n  \"frames_lost\": " + std::to_string(meta.frames_lost);
  out += ",\n  \"main_frames\": " + std::to_string(meta.main_frames);
  out += ",\n  \"main_lost\": " + std::to_string(meta.main_lost);
  out += ",\n  \"present_thread\": " + std::to_string(meta.present_thread);
  out += ",\n  \"main_thread\": " + std::to_string(meta.main_thread);
  out += ",\n  \"focus_losses\": " + std::to_string(meta.focus_losses);
  out += ",\n  \"chain_switches\": " + std::to_string(meta.chain_switches);
  out += ",\n  \"swapchain\": " +
         (meta.swapchain ? swapchain_json(*meta.swapchain) : "null");
  out += ",\n  \"dvars\": {";
  for (std::size_t i = 0; i < meta.dvars.size(); ++i) {
    const auto &[name, value] = meta.dvars[i];
    out += (i ? ", " : "") + json::quote(name) + ": " +
           (value ? json::quote(json::valid_utf8(*value, 256)) : "null");
  }
  out += "}";
  out += ",\n  \"columns\": {\"frames\": "
         "\"i,t_ms,dt_ms,present_ms,overlay_us,cpu,flags\", \"main\": "
         "\"i,t_ms,dt_ms\"}";
  out += ",\n  \"flag_bits\": {\"1\": \"overlay_drew\", \"2\": "
         "\"sync_interval\", \"4\": \"present_status\", \"8\": "
         "\"unfocused\", \"16\": \"new_chain\"}";
  return out + "\n}\n";
}

frames_summary summarize(std::span<const frame_sample> frames,
                         std::optional<std::int64_t> previous_qpc,
                         const run_clock &clock, std::int64_t t1_qpc) {
  frames_summary out;
  out.frames = frames.size();
  out.duration_ms = ticks_to_ms(t1_qpc - clock.t0_qpc, clock.frequency);
  if (out.duration_ms > 0) {
    out.mean_fps =
        static_cast<double>(frames.size()) * 1000.0 / out.duration_ms;
  }
  std::optional<std::int64_t> previous = previous_qpc;
  for (const frame_sample &frame : frames) {
    if (previous) {
      out.max_dt_ms = std::max(
          out.max_dt_ms, ticks_to_ms(frame.qpc - *previous, clock.frequency));
    }
    previous = frame.qpc;
  }
  return out;
}

std::string startup_json(const startup_marks &marks) {
  std::string out = "{\"first_detour_ms\":" + number(marks.first_detour_ms);
  out += ",\"last_detour_ms\":" + number(marks.last_detour_ms);
  out += ",\"ipc_start_ms\":" + number(marks.ipc_start_ms);
  out += ",\"frontend_ms\":" + number(marks.frontend_ms);
  out += ",\"menu_ready_ms\":" + number(marks.menu_ready_ms);
  out += ",\"detours\":" + std::to_string(marks.detours);
  out += ",\"menu_timed_out\":" + boolean(marks.menu_timed_out);
  return out + "}";
}

std::string hooks_json(const hook_totals &totals) {
  std::string out = "{\"created\":" + std::to_string(totals.created);
  out += ",\"enables\":" + std::to_string(totals.enables);
  out += ",\"create_ms\":" + number(totals.create_ms);
  out += ",\"enable_ms\":" + number(totals.enable_ms);
  out += ",\"max_enable_ms\":" + number(totals.max_enable_ms);
  out += ",\"created_later\":" + std::to_string(totals.created_later);
  out += ",\"timeline\":[";
  for (std::size_t i = 0; i < totals.timeline.size(); ++i) {
    const hook_point &point = totals.timeline[i];
    out += (i ? ",[" : "[") + number(point.at_ms, 2) + "," +
           number(point.create_us, 1) + "," + number(point.enable_us, 1) + "]";
  }
  return out + "]}";
}
} // namespace xerces_core::bench
