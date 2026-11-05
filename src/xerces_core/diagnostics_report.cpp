#include "diagnostics_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>

#include "log.hpp"

namespace xerces_core {
namespace {
std::string utf8(const std::filesystem::path &path) {
  const std::u8string text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::int64_t unix_seconds(std::filesystem::file_time_type time) {
  const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
  return std::chrono::duration_cast<std::chrono::seconds>(
             system.time_since_epoch())
      .count();
}

// A crash bundle may be a folder of files (dump, logs, plan); its size is
// what it holds. Bounded so a huge folder cannot stall the screen.
std::uint64_t folder_size(const std::filesystem::path &dir) {
  std::uint64_t total = 0;
  std::error_code ec;
  std::size_t seen = 0;
  for (std::filesystem::recursive_directory_iterator it(
           dir, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       !ec && it != end && seen < 10000; it.increment(ec), ++seen) {
    std::error_code size_ec;
    if (it->is_regular_file(size_ec)) {
      const auto size = it->file_size(size_ec);
      if (!size_ec) {
        total += size;
      }
    }
  }
  return total;
}
} // namespace

std::string format_report(const std::vector<report_section> &sections,
                          std::string_view profile) {
  std::string out = "Xerces diagnostics report\r\n";
  out += std::string(25, '=') + "\r\n";
  for (const report_section &section : sections) {
    out += "\r\n" + section.title + "\r\n";
    out += std::string(std::max<std::size_t>(section.title.size(), 1), '-') +
           "\r\n";
    std::size_t width = 0;
    for (const report_field &field : section.fields) {
      width = std::max(width, field.name.size());
    }
    for (const report_field &field : section.fields) {
      std::string line = field.name;
      line.append(width - field.name.size() + 2, ' ');
      line += field.value.empty() ? "-" : field.value;
      out += line + "\r\n";
    }
    for (const std::string &line : section.lines) {
      out += "  " + line + "\r\n";
    }
  }
  return redact_profile(out, profile);
}

std::string format_bytes(std::uint64_t bytes) {
  if (bytes < 1024) {
    return std::to_string(bytes) + (bytes == 1 ? " byte" : " bytes");
  }
  static const char *const units[] = {"KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes) / 1024.0;
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(units)) {
    value /= 1024.0;
    ++unit;
  }
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f %s", value, units[unit]);
  return text;
}

std::string format_count(std::uint64_t value) {
  const std::string digits = std::to_string(value);
  std::string out;
  for (std::size_t i = 0; i < digits.size(); ++i) {
    if (i > 0 && (digits.size() - i) % 3 == 0) {
      out.push_back(',');
    }
    out.push_back(digits[i]);
  }
  return out;
}

std::string format_hex32(std::uint32_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%X", static_cast<unsigned>(value));
  return text;
}

std::string format_unix_time(std::int64_t seconds) {
  const std::chrono::sys_seconds time{std::chrono::seconds{seconds}};
  return std::format("{:%Y-%m-%d %H:%M:%S} UTC", time);
}

std::string short_hash(std::string_view hash, std::size_t length) {
  if (const auto colon = hash.find(':'); colon != std::string_view::npos) {
    hash.remove_prefix(colon + 1);
  }
  return std::string(hash.substr(0, length));
}

std::vector<crash_entry> list_crash_entries(const std::filesystem::path &dir) {
  std::vector<crash_entry> out;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    crash_entry entry;
    entry.name = utf8(it->path().filename());
    std::error_code item_ec;
    entry.folder = it->is_directory(item_ec);
    if (entry.folder) {
      entry.size = folder_size(it->path());
    } else {
      const auto size = it->file_size(item_ec);
      entry.size = item_ec ? 0 : size;
    }
    const auto modified = it->last_write_time(item_ec);
    entry.modified = item_ec ? 0 : unix_seconds(modified);
    out.push_back(std::move(entry));
  }
  std::ranges::sort(out, [](const crash_entry &a, const crash_entry &b) {
    if (a.modified != b.modified) {
      return a.modified > b.modified;
    }
    return a.name < b.name;
  });
  return out;
}

std::string profile_folder() {
  wchar_t *value = nullptr;
  std::size_t size = 0;
  if (_wdupenv_s(&value, &size, L"USERPROFILE") != 0 || value == nullptr) {
    return {};
  }
  const std::string text = utf8(std::filesystem::path(value));
  std::free(value);
  return text;
}
} // namespace xerces_core
