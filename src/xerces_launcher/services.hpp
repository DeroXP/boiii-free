#pragma once

// What the launcher's screens work with (DESIGN.md 3.5, 4.2, 4.7, 5.3 to 5.6):
// paths under <home>, the config, game detection and the pre-flight check,
// the catalog (scanned on a worker thread with the hash cache), plans, the
// launch, the journal and safe mode. Everything here is called on the UI
// thread; the scan reports back through post(). Screens notice changes by
// comparing the revision counters.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <xerces_core/catalog.hpp>
#include <xerces_core/compat.hpp>
#include <xerces_core/journal.hpp>
#include <xerces_core/launch.hpp>
#include <xerces_core/launcher_config.hpp>
#include <xerces_core/launcher_home.hpp>
#include <xerces_core/launcher_launch.hpp>
#include <xerces_core/launcher_state.hpp>
#include <xerces_core/pack_apply.hpp>
#include <xerces_core/planner.hpp>
#include <xerces_core/sigregistry.hpp>

namespace xerces_launcher {
namespace fs = std::filesystem;

// The paths under <home>, the game, the catalog and the signature report are
// xerces_core's (launcher_home.hpp, launcher_state.hpp), so xerces_cli sees
// exactly what the launcher sees.
using xerces_core::catalog_state;
using xerces_core::find_paths;
using xerces_core::game_state;
using xerces_core::launcher_paths;
using xerces_core::signature_state;

// A launch is xerces_core's too (launcher_launch.hpp), shared with
// `xerces_cli plan apply`.
using xerces_core::launch_outcome;

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
  // False when the write failed; config_error() then says why.
  bool save_config();
  std::uint64_t config_rev() const { return config_rev_; }
  // What is wrong with launcher.json now: why the last save failed, or, until
  // a save works, why the file could not be read at start-up. Empty when the
  // file holds the settings the launcher uses. A failed save's reason is a
  // fixed sentence: screens pass it to tr().
  std::string config_error() const { return config_error_; }
  // The file and the system's message for a failed save (for the log and the
  // copied report, in English); empty otherwise.
  std::string config_error_detail() const { return config_error_detail_; }
  // Why launcher.json could not be read at start-up (defaults were used and
  // the file was kept as launcher.json.broken); empty when it was read. Stays
  // for the session, also after the file has been written again.
  std::string config_load_problem() const { return config_load_problem_; }

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
  // How long resolve() took, over every call so far (launcher.activity).
  struct resolve_timing {
    std::size_t count = 0;
    double total_ms = 0, max_ms = 0;
  };
  resolve_timing resolve_stats() const { return resolve_stats_; }
  xerces_core::plan_request request_for(const std::string &mode) const;
  // The game's launcher_settings.json (read on every call: Settings edits
  // it), as text for plan requests and as the client's pool settings.
  std::string launcher_settings_text() const;
  xerces_core::pool_settings pool_settings() const;
  // Content that can be enabled in a mode (mods that support it), catalog
  // order.
  std::vector<const xerces_core::installed_content *>
  mods_for(const std::string &mode) const;
  std::vector<const xerces_core::installed_content *> maps() const;

  // ----------------------------------------------------------------- launch
  // modded false: vanilla. mods_off: safe mode (journal says the last modded
  // launch crashed). Writes the plan file and the journal (with the mode and
  // a fresh launch id, passed in XERCES_LAUNCH_ID), then starts the client
  // from the game folder; if Windows refuses, the old journal is put back.
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
  // The game process started by the last launch is still running, or any
  // game announces itself in <home>\run (one this launcher did not start).
  bool game_running() const;
  // Notices the game exiting (call once per frame): the one this launcher
  // started, or one only seen in <home>\run. Bumps journal_rev and settles
  // a culprit test once its game is gone.
  void poll();
  std::uint64_t journal_rev() const { return journal_rev_; }

  // ---------------------------------------------------- journal, safe mode
  std::optional<xerces_core::launch_journal> last_journal() const;
  xerces_core::safe_mode_decision safe_mode() const;

  // ------------------------------------------------------ find the culprit
  // After a crash with mods (5.6): halve the crashed stack over a few test
  // launches, after running it once without the larger zone memory when the
  // crashed launch had it. Every test leaves that setting out, as safe mode
  // does. Kept in <home>/config/culprit.json so it survives restarts; each
  // test's result comes from the journal when the game exits.
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
  // launch() and the culprit tests: launch_game and the process handle.
  launch_outcome start(const xerces_core::launch_inputs &in);
  xerces_core::launch_inputs inputs(const std::string &mode, bool modded,
                                    bool mods_off,
                                    const std::optional<std::string> &connect,
                                    const std::vector<std::string> *mods) const;
  void load_config();
  // The scan worker's body (refresh): the cache, detection, the pre-flight
  // next to the catalog scan, the save; posts finish_scan.
  void run_refresh(const fs::path &cache_file,
                   std::shared_ptr<std::atomic<bool>> cancel, bool detect,
                   std::optional<std::string> saved_dir,
                   std::optional<game_state> known, unsigned hash_threads);
  // What the worker posts instead when run_refresh threw: an error in the
  // catalog's diagnostics and the log, never std::terminate.
  void fail_refresh(bool detect, std::string_view what);
  void finish_scan(std::shared_ptr<catalog_state> result);
  void save_culprit();
  // Records the result of a culprit test launch that has ended.
  void settle_culprit_test(bool crashed);
  // poll() without a process handle: watches <home>\run.
  void watch_run_folder();
  game_state detect_now(const std::optional<std::string> &saved_dir,
                        bool preflight, xerces_core::hash_cache *cache) const;
  void apply_detection(game_state next);
  // launcher.catalog.scanned, with the hashing throughput.
  void log_scan(const catalog_state &scan) const;

  launcher_paths paths_;
  poster post_;
  bool dry_launch_ = false;

  xerces_core::launcher_config config_;
  std::string config_error_;
  std::string config_error_detail_;
  std::string config_load_problem_;
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
  std::chrono::steady_clock::time_point last_run_check_{};
  bool game_was_live_ = false; // a game was in <home>\run at the last check
  std::optional<xerces_core::culprit_progress> culprit_;
  std::uint64_t culprit_rev_ = 1;
  mutable resolve_timing resolve_stats_;
};
} // namespace xerces_launcher
