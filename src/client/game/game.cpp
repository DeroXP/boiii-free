#include <std_include.hpp>

#include "game.hpp"
#include "log.hpp"

#include <utils/finally.hpp>

#include <combaseapi.h>

namespace game {
void show_error(const std::string &text, const std::string &title) {
  if (quiet_crash()) {
    fflush(stdout);
    fflush(stderr);

    fprintf(stderr, "%s\n%s\n", title.c_str(), text.c_str());

    fflush(stderr);

    trace("[Error]{}\n{}\n", title, text);
  } else if (is_headless()) {
    puts(text.data());
  } else {
    MessageBoxA(nullptr, text.data(), title.data(),
                MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
  }
}

const std::filesystem::path &get_appdata_path() {
  static const std::filesystem::path appdata_path =
      []() -> std::filesystem::path {
    PWSTR path = nullptr;
    if (FAILED(
            SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
      throw std::runtime_error("Failed to read APPDATA path!");
    }

    auto _ = utils::finally([&path] { CoTaskMemFree(path); });

    // Ensure proper handling of wide character paths
    return std::filesystem::path(path) / L"boiii";
  }();

  return appdata_path;
}

const std::filesystem::path &get_data_path() {
  static const std::filesystem::path data_path = []() -> std::filesystem::path {
    // Xerces: development builds have no updater to install the data folder,
    // so XERCES_DATA_PATH can point at the repository's data/ (docs/DESIGN.md
    // 3.5). Nothing else under %LOCALAPPDATA%\boiii moves.
    wchar_t *override_path = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&override_path, &length, L"XERCES_DATA_PATH") == 0 &&
        override_path != nullptr) {
      std::filesystem::path result(override_path);
      std::free(override_path);
      if (!result.empty()) {
        return result;
      }
    }
    // Xerces (D9): no beta channel. Upstream read beta_data instead when the
    // old launcher's version picker was set to its beta build or a dedicated
    // server had -beta; both are gone.
    return get_appdata_path() / "data";
  }();
  return data_path;
}

const std::filesystem::path &get_game_path() {
  static const std::filesystem::path cwd = std::filesystem::current_path();
  return cwd;
}
} // namespace game
