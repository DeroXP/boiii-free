#include "launcher_home.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwctype>
#include <fstream>
#include <iterator>

namespace xerces_core {
namespace fs = std::filesystem;

namespace {
std::optional<fs::path> env_path(const wchar_t *name) {
  std::wstring buffer(32768, L'\0');
  const DWORD size =
      GetEnvironmentVariableW(name, buffer.data(), DWORD(buffer.size()));
  if (size == 0 || size >= buffer.size()) {
    return std::nullopt;
  }
  buffer.resize(size);
  return fs::path(buffer);
}

fs::path module_path() {
  std::wstring buffer(32768, L'\0');
  const DWORD size =
      GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
  buffer.resize(size);
  return fs::path(buffer);
}

// Walks up from the executable's folder for a file, as development builds
// sit in <repo>\build\bin\x64\Release.
std::optional<fs::path> find_up(const fs::path &start,
                                const fs::path &relative) {
  fs::path dir = start;
  for (int i = 0; i < 8 && !dir.empty(); ++i) {
    std::error_code ec;
    if (fs::exists(dir / relative, ec)) {
      return dir;
    }
    const fs::path parent = dir.parent_path();
    if (parent == dir) {
      break;
    }
    dir = parent;
  }
  return std::nullopt;
}
} // namespace

fs::path find_home(const std::optional<fs::path> &home_override) {
  fs::path home;
  if (home_override) {
    home = *home_override;
  } else if (const auto set = env_path(L"XERCES_HOME")) {
    home = *set;
  } else if (const auto local = env_path(L"LOCALAPPDATA")) {
    home = *local / "Xerces";
  } else {
    home = module_path().parent_path() / "home";
  }
  std::error_code ec;
  const fs::path absolute = fs::absolute(home, ec);
  return ec ? home : absolute;
}

launcher_paths find_paths(const std::optional<fs::path> &home_override,
                          bool create) {
  launcher_paths out;
  out.home = find_home(home_override);
  out.config = out.home / "config";
  out.plans = out.config / "plans";
  out.cache = out.home / "cache";
  out.logs = out.home / "logs";
  out.crash = out.home / "crash";
  std::error_code ec;
  if (create) {
    for (const fs::path &dir :
         {out.config, out.plans, out.cache, out.logs, out.crash}) {
      fs::create_directories(dir, ec);
    }
  }

  out.program = module_path();
  const fs::path exe_dir = out.program.parent_path();
  if (const auto root =
          find_up(exe_dir, fs::path("data/xerces/ui/shell.rml"))) {
    out.data = *root / "data" / "xerces";
    // A repository checkout: the client needs its data folder (815545d3).
    if (fs::exists(*root / "premake5.lua", ec)) {
      out.dev_data = *root / "data";
    }
  } else {
    out.data = exe_dir / "data" / "xerces";
  }
  if (const auto root = find_up(exe_dir, fs::path("compat/builds.json"))) {
    out.compat = *root / "compat";
  } else {
    out.compat = exe_dir / "compat";
  }
  out.client = exe_dir / "boiii.exe";
  return out;
}

std::uint32_t launcher_home_hash(const fs::path &home) {
  std::error_code ec;
  fs::path absolute = fs::absolute(home, ec);
  if (ec) {
    absolute = home;
  }
  std::wstring text = absolute.lexically_normal().wstring();
  // "C:\" keeps its separator; any longer path drops a trailing one.
  while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) {
    text.pop_back();
  }
  std::uint32_t hash = 2166136261u;
  for (const wchar_t c : text) {
    hash = (hash ^ static_cast<std::uint32_t>(std::towlower(c))) * 16777619u;
  }
  return hash;
}

std::wstring launcher_instance_name(const fs::path &home) {
  return L"Local\\xerces_launcher_" + std::to_wstring(launcher_home_hash(home));
}

bool launcher_running(const fs::path &home) {
  const std::wstring name = launcher_instance_name(home);
  HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
  if (mutex) {
    CloseHandle(mutex);
    return true;
  }
  // It exists but this process may not open it: still a running launcher.
  return GetLastError() == ERROR_ACCESS_DENIED;
}

std::optional<std::string> read_text_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

bool write_text_file(const fs::path &path, std::string_view text) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  fs::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out.flush()) {
      out.close();
      fs::remove(temporary, ec);
      return false;
    }
  }
  fs::rename(temporary, path, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
  }
  return true;
}

std::string path_utf8(const fs::path &path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

fs::path path_from_utf8(std::string_view text) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()),
                                text.size()));
}
} // namespace xerces_core
