#pragma once

// <home> as the launcher and xerces_cli both see it (DESIGN.md 3.5): the
// folders under it, where data\ and compat\ are, the game client next to the
// running executable, and the per-home single-instance mutex Xerces.exe holds
// while it runs. Shared so the CLI and Xerces.exe always look at the same
// files. Headless: no UI, no game headers. Windows only, as the launcher is.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace xerces_core {
struct launcher_paths {
  std::filesystem::path home;   // %LOCALAPPDATA%\Xerces, XERCES_HOME or --home
  std::filesystem::path config; // <home>\config
  std::filesystem::path plans;  // <home>\config\plans
  std::filesystem::path cache;  // <home>\cache
  std::filesystem::path logs;   // <home>\logs
  std::filesystem::path crash;  // <home>\crash
  std::filesystem::path data;   // data\xerces: ui, fonts, i18n
  std::filesystem::path compat; // compat\: builds.json, signatures.json
  // The game role's executable (boiii.exe until the rename), next to the
  // running program.
  std::filesystem::path client;
  std::filesystem::path program; // the running executable
  // A repository checkout's data\, for XERCES_DATA_PATH.
  std::optional<std::filesystem::path> dev_data;
  std::filesystem::path config_file() const { return config / "launcher.json"; }
  std::filesystem::path journal_file() const { return logs / "journal.json"; }
};

// <home>: the override, else XERCES_HOME, else %LOCALAPPDATA%\Xerces, as an
// absolute path.
std::filesystem::path
find_home(const std::optional<std::filesystem::path> &home_override);

// Finds the paths: <home> as find_home does; data and compat next to the
// executable or, in a development build, in the repository the executable
// was built in. Creates the folders under <home> when create is true.
launcher_paths
find_paths(const std::optional<std::filesystem::path> &home_override,
           bool create = true);

// The single-instance mutex of Xerces.exe is per home: one launcher per home
// keeps one config and one journal. The hash is FNV-1a over the absolute,
// normalised home path, lower-cased, without a trailing separator, so
// "C:/X/home/" and "c:\x\home" are the same home.
std::uint32_t launcher_home_hash(const std::filesystem::path &home);
std::wstring launcher_instance_name(const std::filesystem::path &home);
// A Xerces.exe runs on this home (its mutex exists).
bool launcher_running(const std::filesystem::path &home);

// XERCES_DRY_LAUNCH=1 asks for a dry launch, as --dry-launch does: Xerces.exe
// and `xerces_cli plan apply` both honour it.
bool dry_launch_from_environment();

// The whole file, or nothing when it cannot be opened.
std::optional<std::string> read_text_file(const std::filesystem::path &path);
// Writes through <path>.tmp and a rename, so a reader never sees half a
// file; creates the folder. The temporary file goes when the rename fails.
bool write_text_file(const std::filesystem::path &path, std::string_view text);

std::string path_utf8(const std::filesystem::path &path); // '\' kept
std::filesystem::path path_from_utf8(std::string_view text);
} // namespace xerces_core
