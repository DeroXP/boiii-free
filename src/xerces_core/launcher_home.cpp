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

#include "ui_text.hpp"

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

std::vector<std::string> missing_client_data(const fs::path &data_dir) {
  // A few files and folders every client data folder has; the rest of the
  // folder comes with them in a package and in the repository.
  struct need {
    const char *relative;
    bool folder;
  };
  static constexpr need needs[] = {
      {"ui_scripts", true},
      {"scripts", true},
      {"lookup_tables", true},
      {"launcher/main.html", false},
  };
  std::vector<std::string> missing;
  std::error_code ec;
  for (const need &n : needs) {
    const fs::path path = data_dir / fs::path(n.relative);
    const bool there =
        n.folder ? fs::is_directory(path, ec) : fs::is_regular_file(path, ec);
    if (!there) {
      missing.emplace_back(n.relative);
    }
  }
  return missing;
}

void locate_install(launcher_paths &out, const fs::path &exe_dir) {
  std::error_code ec;
  out.client_data.reset();
  out.dev_checkout = false;
  fs::path root = exe_dir;
  if (const auto found =
          find_up(exe_dir, fs::path("data/xerces/ui/shell.rml"))) {
    root = *found;
    // A repository checkout: the build sits in <repo>\build\bin\x64\Release.
    out.dev_checkout = fs::exists(*found / "premake5.lua", ec);
  }
  out.data = root / "data" / "xerces";
  // The client data sits next to the launcher's own (an unzipped package, or
  // the repository), and is passed whenever it is there (815545d3).
  out.client_data_expected = root / "data";
  if (missing_client_data(out.client_data_expected).empty()) {
    out.client_data = out.client_data_expected;
  }
  if (const auto found = find_up(exe_dir, fs::path("compat/builds.json"))) {
    out.compat = *found / "compat";
  } else {
    out.compat = exe_dir / "compat";
  }
  out.client = exe_dir / "boiii.exe";
  out.licence_file.reset();
  out.notices_file.reset();
  if (const auto found = find_up(exe_dir, fs::path("LICENSE"))) {
    out.licence_file = *found / "LICENSE";
  }
  if (const auto found =
          find_up(exe_dir, fs::path("THIRD_PARTY_NOTICES.txt"))) {
    out.notices_file = *found / "THIRD_PARTY_NOTICES.txt";
  }
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
  locate_install(out, out.program.parent_path());
  return out;
}

std::optional<fs::path> hash_cache_seed_file(const fs::path &own_cache_file) {
  const auto named = env_path(L"XERCES_HASH_CACHE");
  if (!named || named->empty()) {
    return std::nullopt;
  }
  std::error_code ec;
  std::optional<fs::path> found;
  if (fs::is_directory(*named, ec)) {
    for (const fs::path &file :
         {*named / "cache" / "hashes.txt", *named / "hashes.txt"}) {
      if (fs::is_regular_file(file, ec)) {
        found = file;
        break;
      }
    }
  } else if (fs::is_regular_file(*named, ec)) {
    found = *named;
  }
  if (!found) {
    return std::nullopt;
  }
  // The home's own file is never its seed: it is read and saved as itself.
  if (fs::equivalent(*found, own_cache_file, ec) && !ec) {
    return std::nullopt;
  }
  return fs::absolute(*found, ec);
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

namespace {
// The form same_path compares: absolute, normalised, lower-case, without a
// trailing separator ("C:\" keeps its own).
std::wstring folded(const fs::path &path) {
  std::error_code ec;
  fs::path absolute = fs::absolute(path, ec);
  if (ec) {
    absolute = path;
  }
  std::wstring text = absolute.lexically_normal().wstring();
  while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) {
    text.pop_back();
  }
  for (wchar_t &c : text) {
    c = static_cast<wchar_t>(std::towlower(c));
  }
  return text;
}
} // namespace

bool same_path(const fs::path &a, const fs::path &b) {
  return folded(a) == folded(b);
}

std::optional<fs::path> onedrive_folder_of(const fs::path &path,
                                           const std::vector<fs::path> &roots) {
  const std::wstring inside = folded(path);
  for (const fs::path &root : roots) {
    if (root.empty()) {
      continue;
    }
    const std::wstring base = folded(root);
    const bool under = inside.size() > base.size() &&
                       inside.starts_with(base) &&
                       (base.back() == L'\\' || inside[base.size()] == L'\\');
    if (under || inside == base) {
      return root;
    }
  }
  return std::nullopt;
}

std::vector<fs::path> onedrive_roots() {
  std::vector<fs::path> out;
  for (const wchar_t *name :
       {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"}) {
    if (const auto folder = env_path(name); folder && !folder->empty()) {
      out.push_back(*folder);
    }
  }
  return out;
}

launcher_home_lock::launcher_home_lock(const fs::path &home) {
  const std::wstring name = launcher_instance_name(home);
  HANDLE mutex = CreateMutexW(nullptr, TRUE, name.c_str());
  // Already there: a launcher (or another lock) has it, and TRUE did not make
  // this one its owner. Null: it exists and this process may not open it.
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    mutex = nullptr;
  }
  handle_ = mutex;
}

launcher_home_lock::~launcher_home_lock() {
  if (handle_) {
    ReleaseMutex(static_cast<HANDLE>(handle_));
    CloseHandle(static_cast<HANDLE>(handle_));
  }
}

bool dry_launch_from_environment() {
  wchar_t value[8] = {};
  return GetEnvironmentVariableW(L"XERCES_DRY_LAUNCH", value, 8) > 0 &&
         value[0] == L'1';
}

std::optional<std::string> read_text_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

bool write_text_file(const fs::path &path, std::string_view text,
                     std::string *why, std::string *detail) {
  const std::string name = path_utf8(path.filename());
  // The reason is a fixed sentence, so a screen can translate it; the file
  // and the system's words go to the detail.
  const auto fail = [&](const char *reason, std::string what) {
    if (why) {
      *why = reason;
    }
    if (detail) {
      *detail = std::move(what);
    }
    return false;
  };
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  fs::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      return fail(ui_text("A temporary file could not be created in its "
                          "folder."),
                  name + ".tmp");
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close(); // flushes: failbit when the data did not reach the file
    if (out.fail()) {
      fs::remove(temporary, ec);
      return fail(ui_text("Writing the temporary file failed (is the disk "
                          "full?)."),
                  name + ".tmp");
    }
  }
  fs::rename(temporary, path, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return fail(ui_text("The old file could not be replaced."),
                name + ": " + ec.message());
  }
  if (why) {
    why->clear();
  }
  if (detail) {
    detail->clear();
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
