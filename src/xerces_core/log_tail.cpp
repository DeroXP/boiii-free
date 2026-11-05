#include "log_tail.hpp"

#include <algorithm>
#include <fstream>
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
    return terminal_safe(line);
  }
  const auto &root = parsed.document;
  const auto event = json::get_string(root, "event");
  if (!event) {
    return terminal_safe(line);
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
  return terminal_safe(out);
}

std::string terminal_safe(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    const auto next =
        i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0;
    if ((c < 0x20 && c != '\t' && c != '\n') || c == 0x7F) {
      out.push_back('?');
    } else if (c == 0xC2 && next >= 0x80 && next <= 0x9F) {
      // U+0080 to U+009F, the C1 controls in UTF-8: U+009B (CSI) starts an
      // escape sequence on its own.
      out.push_back('?');
      ++i;
    } else {
      out.push_back(static_cast<char>(c));
    }
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

std::string log_start(const fs::path &file) { return start_of(file); }

std::optional<file_tail> tail_file(const fs::path &file, std::size_t n) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) {
    return std::nullopt;
  }
  // Blocks from the end backwards, until they hold n complete lines with the
  // line break before the first of them (or the whole file).
  constexpr std::streamoff block = 64 * 1024;
  std::vector<std::string> chunks;
  std::streamoff pos = size;
  std::size_t found = 0; // complete lines with text, counted from the end
  bool closed = false;   // a line break ended the unfinished last line
  bool content = false;  // the line being counted has text
  while (pos > 0 && (n == 0 || found < n)) {
    const std::streamoff take = std::min(block, pos);
    pos -= take;
    std::string chunk(static_cast<std::size_t>(take), '\0');
    in.seekg(pos);
    if (!in.read(chunk.data(), static_cast<std::streamsize>(take))) {
      return std::nullopt;
    }
    if (n != 0) {
      for (auto it = chunk.rbegin(); it != chunk.rend() && found < n; ++it) {
        if (*it == '\n') {
          found += closed && content ? 1 : 0;
          closed = true;
          content = false;
        } else if (*it != '\r') {
          content = true;
        }
      }
    }
    chunks.push_back(std::move(chunk));
  }
  std::string text;
  for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
    text += *it;
  }
  file_tail out;
  const std::size_t last = text.rfind('\n');
  out.end = static_cast<std::uint64_t>(pos) +
            (last == std::string::npos ? 0 : last + 1);
  std::string_view body = text;
  if (pos > 0) {
    // The first block starts inside a line: that part is not a line.
    const std::size_t first = body.find('\n');
    body.remove_prefix(first == std::string_view::npos ? body.size()
                                                       : first + 1);
  }
  out.lines = last_lines(body, n);
  return out;
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
