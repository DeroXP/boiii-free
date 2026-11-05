#include "log_tail.hpp"

#include <algorithm>
#include <tuple>

#include "json.hpp"

namespace xerces_core {
namespace fs = std::filesystem;

namespace {
bool all_digits(std::string_view text) {
  return !text.empty() &&
         std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

std::string file_name(const fs::path &file) {
  const auto text = file.filename().u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

// "<date>-<time>" of a log file's name; empty when it is not a log file.
std::string start_of(const fs::path &file) {
  const std::string name = file_name(file);
  if (name.size() < 16 || !all_digits(std::string_view(name).substr(0, 8)) ||
      name[8] != '-' || !all_digits(std::string_view(name).substr(9, 6)) ||
      name[15] != '-') {
    return {};
  }
  return name.substr(0, 15);
}
} // namespace

std::string format_log_line(std::string_view line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.remove_suffix(1);
  }
  const auto parsed = json::parse(line);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::string(line);
  }
  const auto &root = parsed.document;
  const auto event = json::get_string(root, "event");
  if (!event) {
    return std::string(line);
  }
  std::string ts = json::get_string(root, "ts").value_or("");
  if (ts.size() >= 23 && ts[10] == 'T') {
    ts = ts.substr(11, 12); // 21:58:55.392
  }
  std::string level = json::get_string(root, "level").value_or("");
  if (level.size() < 5) {
    level.append(5 - level.size(), ' ');
  }
  std::string out = ts + " " + level + " " + *event + " | " +
                    json::get_string(root, "msg").value_or("");
  if (const auto *data = json::member(root, "data");
      data && data->IsObject() && data->MemberCount() > 0) {
    out += "  " + json::canonical(*data);
  }
  return out;
}

std::string log_role(const fs::path &file) {
  if (file.extension() != ".jsonl" || start_of(file).empty()) {
    return {};
  }
  const std::string stem = file_name(file.stem());
  // <date>-<time>-<role>-<session>
  const std::size_t role_start = 16;
  const std::size_t dash = stem.find('-', role_start);
  if (stem.size() <= role_start || dash == std::string::npos ||
      dash == role_start) {
    return {};
  }
  return stem.substr(role_start, dash - role_start);
}

std::optional<fs::path> newest_log(const fs::path &logs_dir,
                                   std::string_view role) {
  std::optional<fs::path> best;
  std::tuple<std::string, fs::file_time_type> best_key;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(logs_dir, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string found = log_role(entry.path());
    if (found.empty() || (!role.empty() && found != role)) {
      continue;
    }
    const auto key =
        std::make_tuple(start_of(entry.path()), entry.last_write_time(ec));
    if (!best || key > best_key) {
      best = entry.path();
      best_key = key;
    }
  }
  return best;
}

std::vector<std::string> last_lines(std::string_view text, std::size_t n) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (true) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      break; // the rest has no newline yet
    }
    std::string_view line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (!line.empty()) {
      lines.emplace_back(line);
    }
    start = end + 1;
  }
  if (n != 0 && lines.size() > n) {
    lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(n));
  }
  return lines;
}

std::optional<fs::path> todays_cli_log(const fs::path &logs_dir,
                                       std::string_view today) {
  const auto newest = newest_log(logs_dir, "cli");
  if (!newest || !start_of(*newest).starts_with(today)) {
    return std::nullopt;
  }
  return newest;
}
} // namespace xerces_core
