#include <std_include.hpp>

#include "runtime.hpp"

#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>

#include <shellapi.h>

#include <xerces_core/launch.hpp>

namespace xerces_runtime {
namespace {
std::optional<std::wstring> env(const wchar_t *name) {
  wchar_t buffer[32768];
  const DWORD size = GetEnvironmentVariableW(name, buffer, 32768);
  if (size == 0 || size >= 32768) {
    return std::nullopt;
  }
  return std::wstring(buffer, size);
}

std::string utf8(const std::wstring &text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string out(std::size_t(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(),
                      size, nullptr, nullptr);
  return out;
}

std::mutex log_mutex;
std::unique_ptr<xerces_core::json_log> session;
bool log_failed = false;
} // namespace

std::filesystem::path home() {
  static const std::filesystem::path result = [] {
    std::filesystem::path dir;
    if (const auto set = env(L"XERCES_HOME")) {
      dir = *set;
    } else if (const auto local = env(L"LOCALAPPDATA")) {
      dir = std::filesystem::path(*local) / "Xerces";
    } else {
      dir = std::filesystem::current_path() / "xerces_home";
    }
    std::error_code ec;
    for (const char *sub : {"logs", "cache", "config"}) {
      std::filesystem::create_directories(dir / sub, ec);
    }
    return dir;
  }();
  return result;
}

bool started_by_launcher() {
  static const bool result = env(L"XERCES_HOME").has_value();
  return result;
}

void log(xerces_core::log_level level, std::string_view event,
         std::string_view message, std::string_view data_json) {
  std::lock_guard lock(log_mutex);
  if (!session && !log_failed) {
    auto file = std::make_unique<xerces_core::json_log>(
        "game", std::to_string(GetCurrentProcessId()));
    if (const auto profile = env(L"USERPROFILE")) {
      file->set_profile_path(utf8(*profile));
    }
    if (file->open(home() / "logs")) {
      session = std::move(file);
    } else {
      log_failed = true;
    }
  }
  if (!session) {
    return;
  }
  xerces_core::log_event item;
  item.level = level;
  item.component = "xerces";
  item.event = std::string(event);
  item.message = std::string(message);
  item.data_json = std::string(data_json);
  session->write(item);
}

bool write_file(const std::filesystem::path &path, std::string_view text) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out.flush()) {
      return false;
    }
  }
  std::filesystem::rename(temporary, path, ec);
  return !ec;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string flag_value(std::string_view flag) {
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return {};
  }
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.push_back(utf8(argv[i]));
  }
  LocalFree(argv);
  return xerces_core::flag_value(args, flag).value_or("");
}
} // namespace xerces_runtime
