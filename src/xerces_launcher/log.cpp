#include "log.hpp"

#include <RmlUi_Include_Windows.h>

#include <cstdio>
#include <memory>
#include <mutex>

#include <xerces_core/json.hpp>

#include "window.hpp"

namespace xerces_launcher {
namespace {
std::mutex mutex;
std::unique_ptr<xerces_core::json_log> session;

std::string profile_dir() {
  wchar_t buffer[MAX_PATH]{};
  const DWORD size = GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH);
  if (size == 0 || size >= MAX_PATH) {
    return {};
  }
  const int bytes =
      WideCharToMultiByte(CP_UTF8, 0, buffer, int(size), nullptr, 0, nullptr,
                          nullptr);
  std::string out(std::size_t(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, buffer, int(size), out.data(), bytes, nullptr,
                      nullptr);
  return out;
}
} // namespace

bool open_log(const std::string &logs_dir) {
  std::lock_guard lock(mutex);
  auto file = std::make_unique<xerces_core::json_log>(
      "launcher", std::to_string(GetCurrentProcessId()));
  file->set_profile_path(profile_dir());
  const std::u8string dir(reinterpret_cast<const char8_t *>(logs_dir.data()),
                         logs_dir.size());
  if (!file->open(std::filesystem::path(dir))) {
    return false;
  }
  session = std::move(file);
  return true;
}

std::string log_path() {
  std::lock_guard lock(mutex);
  if (!session) {
    return {};
  }
  const auto text = session->path().generic_u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

void log(xerces_core::log_level level, std::string_view event,
         std::string_view message, std::string_view data_json) {
  xerces_core::log_event item;
  item.level = level;
  item.component = "launcher";
  item.event = std::string(event);
  item.message = std::string(message);
  item.data_json = std::string(data_json);
  const std::string line = std::string("[xerces] ") + std::string(event) +
                           ": " + std::string(message) + "\n";
  OutputDebugStringA(line.c_str());
  std::lock_guard lock(mutex);
  if (session) {
    session->write(item);
  }
}

double ms_since_process_start() {
  FILETIME creation{}, exit{}, kernel{}, user{};
  GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
  using precise_time = void(WINAPI *)(LPFILETIME);
  static const auto get_precise_time = reinterpret_cast<precise_time>(
      reinterpret_cast<void *>(GetProcAddress(
          GetModuleHandleW(L"kernel32.dll"), "GetSystemTimePreciseAsFileTime")));
  FILETIME now{};
  if (get_precise_time) {
    get_precise_time(&now);
  } else {
    GetSystemTimeAsFileTime(&now);
  }
  const auto ticks = [](const FILETIME &t) {
    return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) |
           t.dwLowDateTime;
  };
  return static_cast<double>(ticks(now) - ticks(creation)) / 10000.0;
}

void mark(const char *step) {
  const double ms = ms_since_process_start();
  char data[96];
  std::snprintf(data, sizeof(data), "{\"ms\":%.1f}", ms);
  char message[160];
  std::snprintf(message, sizeof(message), "%s at %.1f ms", step, ms);
  log_info("launcher.startup.step", message, data);
}
} // namespace xerces_launcher
