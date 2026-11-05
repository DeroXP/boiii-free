#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core {
// Valve's KeyValues text format (libraryfolders.vdf, appmanifest_*.acf).
struct kv_node {
  std::string key;
  std::string value;
  std::vector<kv_node> children;
  bool section = false;

  // Case-insensitive, as Steam treats keys.
  const kv_node *child(std::string_view name) const;
};

std::optional<kv_node> parse_vdf(std::string_view text,
                                 std::string *error = nullptr);

inline constexpr std::string_view bo3_app_id = "311210";
inline constexpr std::string_view bo3_exe = "BlackOps3.exe";

// Every library folder listed in libraryfolders.vdf (new and old layouts).
std::vector<std::filesystem::path> steam_libraries(std::string_view vdf);
// "installdir" from an appmanifest.
std::optional<std::string> appmanifest_installdir(std::string_view acf);
// "buildid" from an appmanifest: Steam's build number of the installed game.
std::optional<std::uint64_t> appmanifest_buildid(std::string_view acf);

struct detection {
  std::optional<std::filesystem::path> game_dir;
  std::vector<std::string> tried; // what was looked at, in order
};

// Steps 1 to 3 of DESIGN.md 4.7: the saved path, then every Steam root's
// libraries. File access is passed in so tests run on fixtures.
struct detection_io {
  std::function<std::optional<std::string>(const std::filesystem::path &)> read;
  std::function<bool(const std::filesystem::path &)> exists;
};

detection detect_game(const std::optional<std::filesystem::path> &saved,
                      const std::vector<std::filesystem::path> &steam_roots,
                      const detection_io &io);

// HKCU\Software\Valve\Steam\SteamPath and
// HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath, when set.
std::vector<std::filesystem::path> steam_roots_from_registry();
detection_io real_filesystem();
} // namespace xerces_core
