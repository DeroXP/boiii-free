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
#include <vector>

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
  // The game client's data folder (ui_scripts, scripts, launcher\main.html),
  // passed to the game as XERCES_DATA_PATH: data\ next to the executable in an
  // unzipped package, the repository's data\ in a development build. Nothing
  // when neither holds it; the client then falls back to
  // %LOCALAPPDATA%\boiii\data, which nothing installs any more.
  std::optional<std::filesystem::path> client_data;
  // Where client_data was looked for (Diagnostics names it when missing).
  std::filesystem::path client_data_expected;
  // Built in a repository checkout (premake5.lua above the executable): the
  // DEV badge instead of BETA.
  bool dev_checkout = false;
  // LICENSE and THIRD_PARTY_NOTICES.txt next to the executable or in the
  // checkout above it (About); nothing when not there.
  std::optional<std::filesystem::path> licence_file;
  std::optional<std::filesystem::path> notices_file;
  std::filesystem::path config_file() const { return config / "launcher.json"; }
  std::filesystem::path journal_file() const { return logs / "journal.json"; }
  // The Workshop hash cache (catalog.hpp).
  std::filesystem::path hash_cache_file() const { return cache / "hashes.txt"; }
};

// A hash cache to seed this home's from, read only (hash_cache::load_seed):
// what XERCES_HASH_CACHE names, a hashes.txt file, another home (its
// cache\hashes.txt) or a cache folder. Nothing when it is not set, names
// nothing that exists, or names own_cache_file itself.
std::optional<std::filesystem::path>
hash_cache_seed_file(const std::filesystem::path &own_cache_file);

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

// The part of find_paths that depends on where the executable is: data,
// compat, client, client_data, dev_checkout and the licence files, for a
// program in exe_dir. program is left alone.
void locate_install(launcher_paths &out, const std::filesystem::path &exe_dir);

// What the game client needs from its data folder that the folder lacks
// ("ui_scripts", "launcher/main.html", ...). Empty: it holds the client data.
// The client refuses to start without launcher\main.html (main.cpp's
// initial_update_required).
std::vector<std::string>
missing_client_data(const std::filesystem::path &data_dir);

// The single-instance mutex of Xerces.exe is per home: one launcher per home
// keeps one config and one journal. The hash is FNV-1a over the absolute,
// normalised home path, lower-cased, without a trailing separator, so
// "C:/X/home/" and "c:\x\home" are the same home.
std::uint32_t launcher_home_hash(const std::filesystem::path &home);
std::wstring launcher_instance_name(const std::filesystem::path &home);
// A Xerces.exe runs on this home (its mutex exists).
bool launcher_running(const std::filesystem::path &home);

// Two paths name the same file or folder, compared as launcher_home_hash
// compares homes (absolute, normalised, without case or a trailing
// separator). Links are not followed.
bool same_path(const std::filesystem::path &a, const std::filesystem::path &b);

// The OneDrive folder that holds path, or nothing. roots are the folders
// OneDrive syncs (onedrive_roots()), compared as same_path compares. On
// 2026-11-06 the game crashed at every start from a OneDrive folder: Windows
// refused boiii.start next to boiii.exe (docs/TESTING.md, "OneDrive crash").
std::optional<std::filesystem::path>
onedrive_folder_of(const std::filesystem::path &path,
                   const std::vector<std::filesystem::path> &roots);
// The folders of the OneDrive, OneDriveConsumer and OneDriveCommercial
// environment variables that are set.
std::vector<std::filesystem::path> onedrive_roots();

// Holds the per-home mutex for a read, change and write of launcher.json
// (xerces_cli's edits), so a Xerces.exe started meanwhile waits for it (up to
// 30 s) instead of reading the config that is about to be replaced and
// writing its old copy back later. held() is false when a launcher, or
// another lock, already has it; nothing is held then.
class launcher_home_lock {
public:
  explicit launcher_home_lock(const std::filesystem::path &home);
  ~launcher_home_lock();
  launcher_home_lock(const launcher_home_lock &) = delete;
  launcher_home_lock &operator=(const launcher_home_lock &) = delete;
  bool held() const { return handle_ != nullptr; }

private:
  void *handle_ = nullptr;
};

// XERCES_DRY_LAUNCH=1 asks for a dry launch, as --dry-launch does: Xerces.exe
// and `xerces_cli plan apply` both honour it.
bool dry_launch_from_environment();

// The whole file, or nothing when it cannot be opened.
std::optional<std::string> read_text_file(const std::filesystem::path &path);
// Writes through <path>.tmp and a rename, so a reader never sees half a
// file; creates the folder. The temporary file goes when anything fails.
// why, when given, receives the reason as one fixed sentence (ui_text) a
// screen translates; detail, the file's name and the system's message, for
// the log. Both are cleared when the write worked.
bool write_text_file(const std::filesystem::path &path, std::string_view text,
                     std::string *why = nullptr, std::string *detail = nullptr);

std::string path_utf8(const std::filesystem::path &path); // '\' kept
std::filesystem::path path_from_utf8(std::string_view text);
} // namespace xerces_core
