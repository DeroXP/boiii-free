#pragma once

// What the launcher's screens work with (DESIGN.md 3.5, 4.2, 4.7, 5.3 to 5.6):
// paths under <home>, the config, game detection and the pre-flight check,
// the catalog (scanned on a worker thread with the hash cache), plans, the
// launch, the journal and safe mode. Everything here is called on the UI
// thread; the scan reports back through post(). Screens notice changes by
// comparing the revision counters.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <xerces_core/catalog.hpp>
#include <xerces_core/compat.hpp>
#include <xerces_core/journal.hpp>
#include <xerces_core/launch.hpp>
#include <xerces_core/launcher_config.hpp>
#include <xerces_core/planner.hpp>
#include <xerces_core/sigregistry.hpp>

namespace xerces_launcher {
namespace fs = std::filesystem;

struct launcher_paths {
  fs::path home;    // %LOCALAPPDATA%\Xerces, XERCES_HOME or --home
  fs::path config;  // <home>\config
  fs::path plans;   // <home>\config\plans
  fs::path cache;   // <home>\cache
  fs::path logs;    // <home>\logs
  fs::path crash;   // <home>\crash
  fs::path data;    // data\xerces: ui, fonts, i18n
  fs::path compat;  // compat\: builds.json, signatures.json
  fs::path client;  // the game role's executable (boiii.exe until the rename)
  std::optional<fs::path> dev_data; // repo data\ for XERCES_DATA_PATH
  fs::path config_file() const { return config / "launcher.json"; }
  fs::path journal_file() const { return logs / "journal.json"; }
};

// Finds the paths: <home> from the override, else XERCES_HOME, else
// %LOCALAPPDATA%\Xerces; data and compat next to the executable or, in a
// development build, in the repository the executable was built in.
launcher_paths find_paths(const std::optional<fs::path> &home_override);

struct game_state {
  bool searched = false;
  std::optional<fs::path> dir; // the Black Ops III folder
  std::vector<std::string> tried;
  std::optional<std::uint64_t> steam_build; // appmanifest "buildid"
  std::optional<xerces_core::exe_facts> exe;
  std::optional<xerces_core::preflight_result> preflight;
  std::string error; // why the exe or the registry could not be read
  fs::path workshop_dir; // <library>\steamapps\workshop\content\311210
  fs::path mods_dir;     // <game>\mods
  fs::path usermaps_dir; // <game>\usermaps
  bool can_launch() const { return dir && preflight && preflight->can_launch(); }
};

struct catalog_state {
  bool scanning = false;
  bool scanned = false; // at least one full scan finished
  std::size_t done = 0, total = 0;
  std::string current; // folder being read
  // Workshop items, then the game's mods and usermaps folders.
  std::vector<xerces_core::installed_content> items;
  xerces_core::diagnostics diags;
  std::size_t cache_hits = 0, cache_misses = 0;
  std::uint64_t bytes_hashed = 0;
  double seconds = 0;
  // Where each item came from: "workshop", "mods" or "usermaps", by index.
  std::vector<std::string> sources;
  const xerces_core::installed_content *find(std::string_view id) const;
};

// The client's last signature report (<home>\cache\signatures-report.json,
// written by the game role when it resolves the registry, DESIGN.md 4.3).
struct signature_state {
  bool known = false;
  std::size_t matched = 0, total = 0;
  std::string game_build;
  std::string checked; // UTC time
  std::vector<std::string> missing; // names that did not resolve
  std::vector<std::string> disabled; // features the game turned off
  std::string problem; // why nothing resolved (registry missing, ...)
  bool multi_mod_off() const {
    return std::find(disabled.begin(), disabled.end(), "multi_mod") !=
           disabled.end();
  }
};

struct launch_outcome {
  bool started = false;
  bool dry_run = false;    // --dry-launch: built and logged, not started
  std::string message;     // what the screen shows
  std::string command_line;
  std::string plan_id;
  unsigned long pid = 0;
  xerces_core::diagnostics diags;
};

class services {
public:
  using poster = std::function<void(std::function<void()>)>;

  services(launcher_paths paths, poster post, bool dry_launch);
  ~services();
  services(const services &) = delete;
  services &operator=(const services &) = delete;

  const launcher_paths &paths() const { return paths_; }

  // ----------------------------------------------------------------- config
  xerces_core::launcher_config &config() { return config_; }
  const xerces_core::launcher_config &config() const { return config_; }
  // Writes launcher.json (through a temporary file) and bumps config_rev.
  bool save_config();
  std::uint64_t config_rev() const { return config_rev_; }
  std::string config_error() const { return config_error_; }

  // ------------------------------------------------------------------- game
  const game_state &game() const { return game_; }
  // Detection (4.7): saved path, then Steam's libraries; then the pre-flight.
  // Synchronous: hashes BlackOps3.exe. Screens use refresh(true).
  void detect_game();
  bool detecting() const { return detecting_; }
  // A folder the player picked; false (and game().error) if it has no
  // BlackOps3.exe. Saves it in the config.
  bool set_game_dir(const fs::path &dir);
  std::uint64_t game_rev() const { return game_rev_; }

  // ---------------------------------------------------------------- catalog
  const catalog_state &catalog() const { return catalog_; }
  // Detects the game (when asked), then scans the Workshop folder, mods\ and
  // usermaps\ on the worker thread. Asked again while busy, it runs once more
  // afterwards.
  void refresh(bool detect);
  void start_scan(); // refresh(false)
  void cancel_scan();
  std::uint64_t catalog_rev() const { return catalog_rev_; }

  // -------------------------------------------------------------- the plan
  // The current mode's profile resolved against the catalog; mods, when
  // given, replace the profile's list (a culprit-search test launch).
  xerces_core::plan resolve(const std::string &mode,
                            const std::vector<std::string> *mods = nullptr) const;
  xerces_core::plan_request request_for(const std::string &mode) const;
  // Content that can be enabled in a mode (mods that support it), catalog
  // order.
  std::vector<const xerces_core::installed_content *>
  mods_for(const std::string &mode) const;
  std::vector<const xerces_core::installed_content *> maps() const;

  // ----------------------------------------------------------------- launch
  // modded false: vanilla. mods_off: safe mode (journal says the last modded
  // launch crashed). Writes the plan file and the journal, then starts the
  // client from the game folder.
  // connect: join a self-hosted server ("+connect host:port", Servers).
  // mods: launch these instead of the profile's list.
  launch_outcome launch(const std::string &mode, bool modded, bool mods_off,
                        const std::optional<std::string> &connect = {},
                        const std::vector<std::string> *mods = nullptr);
  // The command line the launch would use, without starting anything.
  launch_outcome preview_launch(const std::string &mode, bool modded,
                                bool mods_off,
                                const std::optional<std::string> &connect = {},
                                const std::vector<std::string> *mods = nullptr) const;
  bool dry_launch() const { return dry_launch_; }
  // The game process started by the last launch is still running.
  bool game_running() const;
  // Notices the game exiting (call once per frame); bumps journal_rev.
  void poll();
  std::uint64_t journal_rev() const { return journal_rev_; }

  // ---------------------------------------------------- journal, safe mode
  std::optional<xerces_core::launch_journal> last_journal() const;
  xerces_core::safe_mode_decision safe_mode() const;

  // ------------------------------------------------------ find the culprit
  // After a crash with mods (5.6): halve the crashed stack over a few test
  // launches. Kept in <home>/config/culprit.json so it survives restarts;
  // each test's result comes from the journal when the game exits.
  const std::optional<xerces_core::culprit_progress> &culprit() const {
    return culprit_;
  }
  std::uint64_t culprit_rev() const { return culprit_rev_; }
  // Starts from the last crashed launch (safe_mode()); false if there is none.
  bool start_culprit();
  void stop_culprit();
  // Launches the next test (a dry launch records nothing).
  launch_outcome launch_culprit_test();

  // ------------------------------------------------------------- signatures
  const signature_state &signatures() const { return signatures_; }
  void reload_signatures();
  std::uint64_t signatures_rev() const { return signatures_rev_; }

  // ------------------------------------------------------------ diagnostics
  // The scrubbed diagnostics zip (5.10) in <home>\crash; empty on failure.
  fs::path make_bundle(std::string *error = nullptr) const;

  // ---------------------------------------------------------------- version
  static std::string version();

private:
  void load_config();
  void finish_scan(std::shared_ptr<catalog_state> result);
  void save_culprit();
  // Records the result of a culprit test launch that has ended.
  void settle_culprit_test(bool crashed);
  game_state detect_now(const std::optional<std::string> &saved_dir) const;
  void apply_detection(game_state next);

  launcher_paths paths_;
  poster post_;
  bool dry_launch_ = false;

  xerces_core::launcher_config config_;
  std::string config_error_;
  std::uint64_t config_rev_ = 1;

  game_state game_;
  std::uint64_t game_rev_ = 1;

  catalog_state catalog_;
  std::uint64_t catalog_rev_ = 1;
  std::thread scan_thread_;
  std::shared_ptr<std::atomic<bool>> scan_cancel_;
  bool detecting_ = false;
  bool refresh_again_ = false;
  bool refresh_detect_ = false;

  signature_state signatures_;
  std::uint64_t signatures_rev_ = 1;
  std::uint64_t journal_rev_ = 1;
  void *game_process_ = nullptr; // HANDLE of the last launched client
  std::optional<xerces_core::culprit_progress> culprit_;
  std::uint64_t culprit_rev_ = 1;
};
} // namespace xerces_launcher
