#include "services.hpp"

#include <RmlUi_Include_Windows.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <mutex>

#include <xerces_core/bundle.hpp>
#include <xerces_core/crash_outbox.hpp>
#include <xerces_core/crash_report.hpp>
#include <xerces_core/detect.hpp>
#include <xerces_core/ipc.hpp>
#include <xerces_core/join_check.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/launcher_home.hpp>
#include <xerces_core/launcher_launch.hpp>
#include <xerces_core/launcher_state.hpp>
#include <xerces_core/ui_text.hpp>
#include <xerces_core/version.hpp>

#include "log.hpp"

namespace xerces_launcher {
namespace {
std::string utf8(const fs::path &path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::optional<std::string> read_text(const fs::path &path) {
  return xerces_core::read_text_file(path);
}

bool write_text(const fs::path &path, std::string_view text) {
  return xerces_core::write_text_file(path, text);
}

std::optional<fs::path> env_path(const wchar_t *name) {
  wchar_t buffer[32768];
  const DWORD size = GetEnvironmentVariableW(name, buffer, 32768);
  if (size == 0 || size >= 32768) {
    return std::nullopt;
  }
  return fs::path(std::wstring(buffer, size));
}

std::string now_utc() { return xerces_core::json_log::now_utc(); }

crash_notice notice(crash_notice::kind what, std::size_t count = 0,
                    std::string server_id = {}, std::string error = {}) {
  crash_notice out;
  out.what = what;
  out.count = count;
  out.server_id = std::move(server_id);
  out.error = std::move(error);
  return out;
}

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// A mode for a log line; never the caller's text, which could hold quotes.
const char *valid_mode_name(const std::string &mode) {
  return mode == "zm" ? "zm" : mode == "mp" ? "mp" : mode == "cp" ? "cp" : "?";
}
} // namespace

// The crash reports' worker owns this; the launcher only reads its events.
struct services::crash_state {
  struct event {
    crash_job job; // what was asked (journal_text cleared)
    std::optional<xerces_core::collect_result> collected;
    std::vector<xerces_core::send_result> sent;
    bool dry = false;
    std::optional<std::int64_t> deleted;
    std::string delete_error;
    std::string error;
  };
  std::mutex mutex;
  std::vector<event> events;
  std::atomic<bool> send_enabled{true};
  std::atomic<bool> busy{false};
};

services::services(launcher_paths paths, poster post, bool dry_launch)
    : paths_(std::move(paths)), post_(std::move(post)), dry_launch_(dry_launch),
      crash_state_(std::make_shared<crash_state>()) {
  load_config();
  reload_signatures();
  // Crashes the client recorded while no launcher watched (a game restarted
  // from the crash dialog, one xerces_cli started), and the queue. A game
  // still running keeps its crashes until it has exited. poll() starts this
  // once the first detection is done, so the reports carry the game's build.
  crash_state_->send_enabled = config_.send_crash_reports;
  crash_job job;
  job.collect = !game_running();
  job.send = true;
  crash_next_ = std::move(job);
  if (const auto text = read_text(paths_.config / "culprit.json")) {
    culprit_ = xerces_core::parse_culprit_progress(*text);
  }
  // A test launch that ended while the launcher was closed: the journal
  // says how it went. One still running (the player reopened Xerces
  // mid-test) is settled by poll() once it is gone.
  if (culprit_ && !culprit_->testing.empty() && !game_running()) {
    settle_culprit_from_journal();
  }
}

services::~services() {
  cancel_scan();
  if (scan_thread_.joinable()) {
    scan_thread_.join();
  }
  if (game_process_) {
    CloseHandle(static_cast<HANDLE>(game_process_));
  }
}

std::string services::version() { return xerces_core::xerces_version_text; }

std::string services::version_label() {
  return xerces_core::xerces_version_label;
}

// ------------------------------------------------------------------- config
void services::load_config() {
  const auto text = read_text(paths_.config_file());
  if (!text) {
    config_ = {};
    return;
  }
  auto parsed = xerces_core::parse_launcher_config(*text);
  if (!parsed.ok()) {
    // Keep the broken file for the player; start from defaults.
    std::error_code ec;
    fs::copy_file(paths_.config_file(),
                  paths_.config / "launcher.json.broken",
                  fs::copy_options::overwrite_existing, ec);
    config_load_problem_ = parsed.diags.items().empty()
                               ? "launcher.json could not be read"
                               : parsed.diags.items().front().message;
    config_error_ = config_load_problem_;
    log_warn("launcher.config.broken", config_load_problem_);
    // The rule the game client follows too: a file that is not JSON sends
    // no crash reports until the switch is turned on again, since it may
    // have said off (an empty file is no file: on).
    parsed.value.send_crash_reports = xerces_core::crash_reports_enabled(*text);
  }
  config_ = std::move(parsed.value);
}

bool services::save_config() {
  ++config_rev_;
  // A write that worked also ends a problem from start-up: the file holds
  // valid settings again.
  const bool ok = xerces_core::write_text_file(
      paths_.config_file(), xerces_core::launcher_config_to_json(config_),
      &config_error_, &config_error_detail_);
  if (!ok) {
    log_error("launcher.config.write",
              config_error_ + " (" + config_error_detail_ + ")");
  }
  return ok;
}

// --------------------------------------------------------------------- game
game_state services::detect_now(const std::optional<std::string> &saved_dir,
                                bool preflight,
                                xerces_core::hash_cache *cache) const {
  return xerces_core::detect_game_state(saved_dir, paths_.compat, preflight,
                                        cache);
}

void services::apply_detection(game_state next) {
  game_ = std::move(next);
  ++game_rev_;
  log_info("launcher.game.detected",
           game_.dir ? "Black Ops III found at " + utf8(*game_.dir)
                     : std::string("Black Ops III was not found"),
           "{\"verdict\":" +
               xerces_core::json::quote(
                   game_.preflight ? xerces_core::to_string(game_.preflight->verdict)
                                   : "none") +
               "}");
}

void services::detect_game() {
  apply_detection(detect_now(config_.game_dir, true, nullptr));
}

bool services::set_game_dir(const fs::path &dir) {
  std::error_code ec;
  if (!fs::exists(dir / std::string(xerces_core::bo3_exe), ec)) {
    game_.error = "There is no BlackOps3.exe in " + utf8(dir) + ".";
    ++game_rev_;
    return false;
  }
  config_.game_dir = utf8(fs::absolute(dir, ec));
  save_config();
  refresh(true);
  return true;
}

// ------------------------------------------------------------------ catalog
void services::start_scan() { refresh(false); }

void services::refresh(bool detect) {
  if (catalog_.scanning || detecting_) {
    refresh_again_ = true;
    refresh_detect_ = refresh_detect_ || detect;
    return;
  }
  if (scan_thread_.joinable()) {
    scan_thread_.join();
  }
  detecting_ = detect;
  catalog_.scanning = true;
  catalog_.done = catalog_.total = 0;
  catalog_.current.clear();
  ++catalog_rev_;
  scan_cancel_ = std::make_shared<std::atomic<bool>>(false);

  const fs::path cache_file = paths_.hash_cache_file();
  auto cancel = scan_cancel_;
  std::optional<std::string> saved_dir = config_.game_dir;
  std::optional<game_state> known;
  if (!detect) {
    known = game_;
  }
  // While a game runs, one file at a time: a rescan then never competes
  // with it for the disk and several cores.
  const unsigned hash_threads = game_running() ? 1 : 0;
  scan_thread_ = std::thread([this, cache_file, cancel, detect, saved_dir,
                              known, hash_threads] {
    // Nothing thrown here may end the launcher (std::terminate, at every
    // start while the cause stays): the scan ends with an error instead.
    try {
      run_refresh(cache_file, cancel, detect, saved_dir, known, hash_threads);
    } catch (const std::exception &error) {
      fail_refresh(detect, error.what());
    } catch (...) {
      fail_refresh(detect, "an unknown error");
    }
  });
}

void services::run_refresh(const fs::path &cache_file,
                           std::shared_ptr<std::atomic<bool>> cancel,
                           bool detect, std::optional<std::string> saved_dir,
                           std::optional<game_state> known,
                           unsigned hash_threads) {
  using clock = std::chrono::steady_clock;
  const auto ms_since = [](clock::time_point from) {
    return std::chrono::duration<double, std::milli>(clock::now() - from)
        .count();
  };
  const auto started = clock::now();
  // One cache for the pre-flight's BlackOps3.exe and the catalog, saved
  // once both are done.
  xerces_core::hash_cache cache;
  const xerces_core::hash_cache_opened opened =
      xerces_core::open_hash_cache(cache, cache_file);
  const double cache_load_ms = ms_since(started);
  // Finding the game takes milliseconds; the pre-flight hashes
  // BlackOps3.exe (about 100 MB when it is not cached), so it runs next to
  // the scan, which needs only the folders, and its result goes to the
  // screens as soon as it is there.
  auto detect_started = clock::now();
  game_state game = known ? *known : detect_now(saved_dir, false, nullptr);
  const double detect_ms = ms_since(detect_started);
  std::atomic<double> preflight_ms{0};
  std::thread preflight;
  // Joined on every way out, a throw included, before the cache it uses
  // goes (a joinable thread's destructor would end the process).
  struct joiner {
    std::thread &thread;
    ~joiner() {
      if (thread.joinable()) {
        thread.join();
      }
    }
  } join_preflight{preflight};
  if (detect) {
    preflight = std::thread([this, game, &cache, &preflight_ms]() mutable {
      const auto preflight_started = clock::now();
      try {
        xerces_core::preflight_game_state(game, paths_.compat, &cache);
      } catch (const std::exception &error) {
        game.exe.reset();
        game.preflight.reset(); // cannot launch
        game.error = "The pre-flight check failed: " +
                     xerces_core::json::valid_utf8(error.what(), 512);
      } catch (...) {
        game.exe.reset();
        game.preflight.reset();
        game.error = "The pre-flight check failed.";
      }
      preflight_ms = std::chrono::duration<double, std::milli>(
                         clock::now() - preflight_started)
                         .count();
      // Always posted: detecting_ holds every later refresh back until then.
      post_([this, game] {
        detecting_ = false;
        apply_detection(game);
      });
    });
  }
  const auto scan_started = clock::now();
  auto result = std::make_shared<catalog_state>(xerces_core::scan_catalog(
      game, cache, cancel.get(),
      [this](const std::string &source, std::size_t index, std::size_t total,
             const std::string &name) {
        post_([this, index, total, name, source] {
          catalog_.done = index;
          catalog_.total = total;
          catalog_.current = source + ": " + name;
          ++catalog_rev_;
        });
      },
      hash_threads));
  const double scan_ms = ms_since(scan_started);
  if (preflight.joinable()) {
    preflight.join();
  }
  const auto save_started = clock::now();
  xerces_core::save_hash_cache(cache, cache_file, *result);
  const double save_ms = ms_since(save_started);
  char data[512];
  std::snprintf(data, sizeof(data),
                "{\"detect\":%s,\"cache_load_ms\":%.1f,\"detect_ms\":%.1f,"
                "\"preflight_ms\":%.1f,\"scan_ms\":%.1f,\"cache_save_ms\":%.1f,"
                "\"total_ms\":%.1f,\"cache_version\":%d,\"cache_entries\":%zu,"
                "\"seed_entries\":%zu}",
                detect ? "true" : "false", cache_load_ms, detect_ms,
                preflight_ms.load(), scan_ms, save_ms, ms_since(started),
                opened.version, opened.entries, opened.seed_entries);
  log_info("launcher.refresh.timing",
           detect ? "detection, pre-flight and scan" : "scan", data);
  if (opened.version == 1) {
    log_info("launcher.catalog.cache_migrated",
             "A version 1 hash cache was read and saved as version 2.");
  }
  if (opened.seed) {
    log_info("launcher.catalog.cache_seed",
             "Hashes the home's cache lacked were looked up in " +
                 utf8(*opened.seed) + ".");
  }
  post_([this, result] { finish_scan(result); });
}

void services::fail_refresh(bool detect, std::string_view what) {
  // what() may hold the system's ANSI text.
  const std::string message = "The scan stopped on an error: " +
                              xerces_core::json::valid_utf8(what, 512);
  log_error("launcher.refresh.failed", message);
  // catalog.read, so the Mods screen shows it with the folders it could not
  // read. A catalog an earlier scan read stays (finish_scan).
  auto result = std::make_shared<catalog_state>();
  result->diags.error("catalog.read", "", message);
  post_([this, result, detect] {
    if (detect) {
      // The pre-flight (joined before this) has posted its own result if it
      // started; if the error came before it, nothing else clears this.
      detecting_ = false;
    }
    finish_scan(result);
  });
}

void services::log_scan(const catalog_state &scan) const {
  // Throughput: per file is each file's bytes over its own read time (what
  // one thread manages), overall is every byte hashed over the whole scan
  // (several threads at once, plus the folders read from the cache).
  const double mb = static_cast<double>(scan.bytes_hashed) / 1e6;
  const double per_file_mb_s =
      scan.hash_seconds > 0 ? mb / scan.hash_seconds : 0;
  const double overall_mb_s = scan.seconds > 0 ? mb / scan.seconds : 0;
  const double slowest_mb_s =
      scan.slowest_seconds > 0
          ? static_cast<double>(scan.slowest_bytes) / 1e6 / scan.slowest_seconds
          : 0;
  std::string data = "{\"items\":" + std::to_string(scan.items.size()) +
                     ",\"scanned\":" + (scan.scanned ? "true" : "false");
  char numbers[512];
  std::snprintf(
      numbers, sizeof(numbers),
      ",\"seconds\":%.2f,\"cache_hits\":%zu,\"cache_misses\":%zu,"
      "\"seed_hits\":%zu,\"bytes_hashed\":%llu,\"threads\":%u,"
      "\"hash_seconds\":%.2f,\"per_file_mb_s\":%.0f,\"overall_mb_s\":%.0f,"
      "\"slowest_mb\":%.1f,\"slowest_seconds\":%.2f,\"slowest_mb_s\":%.0f",
      scan.seconds, scan.cache_hits, scan.cache_misses, scan.seed_hits,
      static_cast<unsigned long long>(scan.bytes_hashed), scan.hash_threads,
      scan.hash_seconds, per_file_mb_s, overall_mb_s,
      static_cast<double>(scan.slowest_bytes) / 1e6, scan.slowest_seconds,
      slowest_mb_s);
  data += numbers;
  data +=
      ",\"slowest_file\":" + xerces_core::json::quote(scan.slowest_file) + "}";
  log_info("launcher.catalog.scanned",
           std::to_string(scan.items.size()) + " items", data);
}

void services::finish_scan(std::shared_ptr<catalog_state> result) {
  log_scan(*result);
  const bool keep_old = !result->scanned && catalog_.scanned;
  if (!keep_old) {
    // Names come without colour codes (scan_catalog).
    catalog_ = std::move(*result);
  }
  catalog_.scanning = false;
  catalog_.current.clear();
  ++catalog_rev_;
  if (refresh_again_) {
    const bool detect = refresh_detect_;
    refresh_again_ = refresh_detect_ = false;
    post_([this, detect] { refresh(detect); });
  }
}

void services::cancel_scan() {
  if (scan_cancel_) {
    *scan_cancel_ = true;
  }
}

// --------------------------------------------------------------------- plan
xerces_core::plan_request services::request_for(const std::string &mode) const {
  return xerces_core::profile_request(config_, mode, game_.steam_build,
                                      launcher_settings_text(),
                                      game_.zone_memory);
}

std::string services::launcher_settings_text() const {
  return xerces_core::read_launcher_settings(game_);
}

xerces_core::pool_settings services::pool_settings() const {
  return xerces_core::parse_pool_settings(launcher_settings_text());
}

xerces_core::plan services::resolve(const std::string &mode,
                                   const std::vector<std::string> *mods) const {
  const auto started = std::chrono::steady_clock::now();
  xerces_core::plan plan = xerces_core::resolve_profile(
      catalog_.items, config_, mode, game_.steam_build, mods,
      launcher_settings_text(), game_.zone_memory);
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started)
                        .count();
  ++resolve_stats_.count;
  resolve_stats_.total_ms += ms;
  resolve_stats_.max_ms = std::max(resolve_stats_.max_ms, ms);
  char data[160];
  std::snprintf(data, sizeof(data),
                "{\"mode\":\"%s\",\"ms\":%.2f,\"items\":%zu,\"stack\":%zu}",
                valid_mode_name(mode), ms, catalog_.items.size(),
                plan.stack.size());
  log(xerces_core::log_level::debug, "launcher.services.resolve", mode, data);
  return plan;
}

std::vector<const xerces_core::installed_content *>
services::mods_for(const std::string &mode) const {
  std::vector<const xerces_core::installed_content *> out;
  for (const auto &item : catalog_.items) {
    if (item.info.kind == xerces_core::content_kind::mod &&
        item.info.supports_mode(mode)) {
      out.push_back(&item);
    }
  }
  return out;
}

std::vector<const xerces_core::installed_content *> services::maps() const {
  std::vector<const xerces_core::installed_content *> out;
  for (const auto &item : catalog_.items) {
    if (item.info.kind == xerces_core::content_kind::map) {
      out.push_back(&item);
    }
  }
  return out;
}

// ------------------------------------------------------------------- launch
xerces_core::launch_inputs services::inputs(
    const std::string &mode, bool modded, bool mods_off,
    const std::optional<std::string> &connect,
    const std::vector<std::string> *mods) const {
  xerces_core::launch_inputs in;
  in.paths = &paths_;
  in.config = &config_;
  in.game = &game_;
  in.catalog = &catalog_.items;
  in.mode = mode;
  in.modded = modded;
  in.mods_off = mods_off;
  in.connect = connect;
  in.mods = mods;
  in.dry_launch = dry_launch_;
  return in;
}

launch_outcome services::preview_launch(
    const std::string &mode, bool modded, bool mods_off,
    const std::optional<std::string> &connect,
    const std::vector<std::string> *mods) const {
  return xerces_core::preview_launch(
      inputs(mode, modded, mods_off, connect, mods));
}

bool services::game_running() const {
  if (game_process_ &&
      WaitForSingleObject(static_cast<HANDLE>(game_process_), 0) ==
          WAIT_TIMEOUT) {
    return true;
  }
  // A game this launcher did not start (or started before it was closed and
  // opened again) announces itself in the run folder while it runs (15.2).
  return xerces_core::game_in_run_folder(paths_.home);
}

launch_outcome services::launch(const std::string &mode, bool modded,
                                bool mods_off,
                                const std::optional<std::string> &connect,
                                const std::vector<std::string> *mods) {
  return start(inputs(mode, modded, mods_off, connect, mods));
}

launch_outcome services::start(const xerces_core::launch_inputs &in) {
  void *process = nullptr;
  // A real launch also looks for a client process: a game xerces_cli started
  // seconds ago has no handle here and no run entry yet.
  launch_outcome out = xerces_core::launch_game(
      in,
      [this] {
        return game_running() ||
               (!dry_launch_ &&
                xerces_core::game_process_running(paths_.client));
      },
      launcher_events(), &process);
  if (!out.dry_run && (out.started || out.refusal ==
                                          xerces_core::launch_refusal::start)) {
    ++journal_rev_; // the journal was written; screens re-read it
  }
  if (process) {
    if (game_process_) {
      CloseHandle(static_cast<HANDLE>(game_process_));
    }
    game_process_ = process;
  }
  return out;
}

void services::poll() {
  drain_crash_events();
  // Waits for detection: a report names the game's build.
  if (crash_next_ && !crash_state_->busy && !detecting_) {
    crash_job job = std::move(*crash_next_);
    crash_next_.reset();
    run_crash_job(std::move(job));
  }
  if (crash_retry_ && std::chrono::steady_clock::now() >= crash_retry_at_ &&
      !crash_state_->busy) {
    crash_retry_ = false;
    crash_job job;
    job.send = true;
    queue_crash_job(std::move(job));
  }
  if (!game_process_) {
    watch_run_folder();
    return;
  }
  if (WaitForSingleObject(static_cast<HANDLE>(game_process_), 0) !=
      WAIT_OBJECT_0) {
    return;
  }
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(game_process_), &code);
  CloseHandle(static_cast<HANDLE>(game_process_));
  game_process_ = nullptr;
  // The client writes every phase itself, clean_exit included, once it has
  // taken the journal over. A game that exited with code 0 before it did
  // (the journal still holds the launch id) quit normally. Any other way
  // leaves the journal as the client left it: the crash dialog's "Restart
  // game" also exits with code 0, and that is still a crash.
  if (auto journal = last_journal();
      journal && xerces_core::exit_counts_as_clean(*journal, code)) {
    journal->phase = xerces_core::launch_phase::clean_exit;
    journal->launch_id.clear(); // spent
    journal->updated = now_utc();
    write_text(paths_.journal_file(), xerces_core::journal_to_json(*journal));
  }
  ++journal_rev_;
  if (culprit_ && !culprit_->testing.empty()) {
    settle_culprit_from_journal();
  }
  log_info("launcher.game.exited", "exit code " + std::to_string(code),
           "{\"code\":" + std::to_string(code) + "}");
  reload_signatures();
  // A crash report, when it was a crash (xerces_core::exit_is_crash decides
  // on the worker, with the journal as it is now).
  crash_job job;
  job.collect = true;
  job.exit_code = code;
  job.journal_text = read_text(paths_.journal_file()).value_or("");
  job.send = true;
  queue_crash_job(std::move(job));
}

void services::watch_run_folder() {
  // A game this launcher did not start (started before Xerces was reopened,
  // or restarted from the crash dialog) has no process handle here; it is
  // seen through its entry in <home>\run, read at most about once a second:
  // 900 ms, so the launcher's one-second waits while a game runs (app.cpp)
  // read it on every pass rather than every other one.
  const auto now = std::chrono::steady_clock::now();
  if (now - last_run_check_ < std::chrono::milliseconds(900)) {
    return;
  }
  last_run_check_ = now;
  const bool live = game_running();
  if (live != game_was_live_) {
    // Home and Diagnostics read the journal again either way. A game that
    // appeared (the crash dialog's restart, one started elsewhere) is never
    // shown as a crash while it runs (5.6); one that exited may have
    // crashed.
    ++journal_rev_;
  }
  if (game_was_live_ && !live) {
    log_info("launcher.game.gone", "a game this launcher did not start exited");
    reload_signatures();
    // Its exit code is not known here: only what the client recorded.
    crash_job job;
    job.collect = true;
    job.send = true;
    queue_crash_job(std::move(job));
  } else if (!game_was_live_ && live) {
    log_info("launcher.game.seen",
             "a game this launcher did not start is running");
  }
  game_was_live_ = live;
  // A culprit test that was still running when this launcher opened.
  if (!live && culprit_ && !culprit_->testing.empty()) {
    settle_culprit_from_journal();
  }
}

// ------------------------------------------------------- journal, safe mode
std::optional<xerces_core::launch_journal> services::last_journal() const {
  const auto text = read_text(paths_.journal_file());
  if (!text) {
    return std::nullopt;
  }
  return xerces_core::parse_journal(*text);
}

xerces_core::safe_mode_decision services::safe_mode() const {
  if (game_running()) {
    return {};
  }
  return xerces_core::decide_safe_mode(last_journal());
}

// -------------------------------------------------------------- diagnostics
fs::path services::make_bundle(std::string *error) const {
  std::string stamp = now_utc().substr(0, 19);
  std::ranges::replace(stamp, ':', '-');
  const fs::path out = paths_.crash / ("xerces-bundle-" + stamp + ".zip");
  std::string profile;
  if (const auto dir = env_path(L"USERPROFILE")) {
    profile = utf8(*dir);
  }
  const xerces_core::bundle_result result = xerces_core::make_bundle(
      paths_.home, out, profile, xerces_core::system_info_json());
  if (!result.ok) {
    if (error) {
      *error = result.error;
    }
    return {};
  }
  log_info("launcher.diag.bundle", utf8(out));
  return out;
}

// ------------------------------------------------------------ crash reports
void services::queue_crash_job(crash_job job) {
  if (crash_state_->busy || crash_next_) {
    // One worker at a time: what is asked meanwhile runs after it, merged.
    crash_job &next = crash_next_ ? *crash_next_ : crash_next_.emplace();
    next.collect = next.collect || job.collect;
    if (job.exit_code) {
      next.exit_code = job.exit_code;
      next.journal_text = std::move(job.journal_text);
    }
    next.send = next.send || job.send;
    next.remove_remote = next.remove_remote || job.remove_remote;
    return;
  }
  run_crash_job(std::move(job));
}

void services::run_crash_job(crash_job job) {
  crash_state_->busy = true;
  xerces_core::crash_context ctx;
  ctx.home = paths_.home;
  ctx.version = xerces_core::xerces_version_text;
  ctx.commit = xerces_core::xerces_report_commit;
  ctx.dirty = xerces_core::xerces_dirty;
  if (game_.exe) {
    ctx.game_checksum = game_.exe->checksum;
  }
  ctx.steam_build = game_.steam_build;
  if (const auto profile = env_path(L"USERPROFILE")) {
    ctx.profile = utf8(*profile);
  }
  if (const auto user = env_path(L"USERNAME")) {
    ctx.user_name = utf8(*user);
  }
  // Only for the report's flags and the words to hide (player name,
  // favourites); the report never carries the config itself.
  ctx.config_text = xerces_core::launcher_config_to_json(config_);
  ctx.journal_text = std::move(job.journal_text);
  job.journal_text.clear();
  ctx.system = [] { return xerces_core::collect_crash_system(); };
  ctx.now_utc = now_utc();
  ctx.now = unix_now();
  const bool dry = dry_launch_;
  std::thread([state = crash_state_, ctx = std::move(ctx), job, dry] {
    crash_state::event event;
    event.job = job;
    event.dry = dry;
    try {
      if (job.collect) {
        event.collected = xerces_core::collect_crashes(
            ctx, state->send_enabled.load(), job.exit_code);
      }
      const fs::path crash_dir = ctx.home / "crash";
      xerces_core::send_options options;
      options.user_agent = "Xerces/" + ctx.version;
      options.base_url = xerces_core::crash_base_url().value_or("");
      options.enabled = [state] { return state->send_enabled.load(); };
      if (job.send && !dry && state->send_enabled &&
          !xerces_core::list_outbox(crash_dir).empty()) {
        options.install_id = xerces_core::crash_install_id(ctx.home);
        event.sent = xerces_core::send_outbox(crash_dir, options,
                                              xerces_core::winhttp_send);
      }
      if (job.remove_remote) {
        // An install without an id never sent a report: nothing to delete.
        options.install_id = xerces_core::crash_install_id(ctx.home, false);
        if (dry) {
          event.delete_error = "dry";
        } else if (options.install_id.empty()) {
          event.deleted = 0;
        } else {
          event.deleted = xerces_core::delete_remote_reports(
              options, xerces_core::winhttp_send, &event.delete_error);
        }
      }
    } catch (const std::exception &error) {
      event.error = xerces_core::json::valid_utf8(error.what(), 512);
    } catch (...) {
      event.error = "an unknown error";
    }
    {
      std::lock_guard lock(state->mutex);
      state->events.push_back(std::move(event));
    }
    state->busy = false;
  }).detach();
}

void services::drain_crash_events() {
  std::vector<crash_state::event> events;
  {
    std::lock_guard lock(crash_state_->mutex);
    events.swap(crash_state_->events);
  }
  for (const auto &event : events) {
    if (!event.error.empty()) {
      log_error("launcher.crash.failed",
                "Crash reports stopped on an error: " + event.error);
    }
    if (event.collected) {
      for (const auto &id : event.collected->queued) {
        log_info("launcher.crash.queued", id,
                 "{\"report_id\":" + xerces_core::json::quote(id) + "}");
      }
      if (event.collected->dropped > 0) {
        log_info("launcher.crash.dropped",
                 std::to_string(event.collected->dropped) +
                     " crash(es) not queued: sending is off");
      }
      if (event.job.exit_code && event.collected->crashed &&
          !crash_state_->send_enabled) {
        crash_notices_.push_back(notice(crash_notice::kind::off));
      }
    }
    std::size_t sent = 0;
    std::string last_id;
    bool retry = false;
    int wait_s = 600;
    for (const auto &result : event.sent) {
      log(result.outcome == xerces_core::send_outcome::retry ||
                  result.outcome == xerces_core::send_outcome::rejected
              ? xerces_core::log_level::warn
              : xerces_core::log_level::info,
          "launcher.crash.send", xerces_core::to_string(result.outcome),
          "{\"report_id\":" + xerces_core::json::quote(result.report_id) +
              ",\"outcome\":" +
              xerces_core::json::quote(xerces_core::to_string(result.outcome)) +
              ",\"status\":" + std::to_string(result.status) +
              ",\"server_id\":" + xerces_core::json::quote(result.server_id) +
              ",\"error\":" +
              xerces_core::json::quote(
                  xerces_core::json::valid_utf8(result.error, 256)) +
              "}");
      switch (result.outcome) {
      case xerces_core::send_outcome::sent:
      case xerces_core::send_outcome::duplicate:
        ++sent;
        last_id = result.server_id;
        break;
      case xerces_core::send_outcome::rejected:
        crash_notices_.push_back(
            notice(crash_notice::kind::rejected, 1, {}, result.error));
        break;
      case xerces_core::send_outcome::retry:
        retry = true;
        if (result.retry_after) {
          wait_s = std::max(wait_s, *result.retry_after);
        }
        break;
      case xerces_core::send_outcome::off:
        break;
      }
    }
    if (sent > 0) {
      crash_notices_.push_back(notice(crash_notice::kind::sent, sent, last_id));
    }
    const bool fresh = event.collected && !event.collected->queued.empty();
    if (fresh && event.dry) {
      log_info("launcher.crash.dry",
               "--dry-launch: crash reports are queued, never sent");
    } else if (fresh && sent == 0 && crash_state_->send_enabled &&
               xerces_core::reports_wait(event.sent)) {
      // Not after the server refused the report: that notice says so.
      crash_notices_.push_back(
          notice(crash_notice::kind::later, event.collected->queued.size()));
    }
    if (retry) {
      // While Xerces stays open it tries again in 10 minutes (or when the
      // server says), and at every start.
      crash_retry_ = true;
      crash_retry_at_ =
          std::chrono::steady_clock::now() + std::chrono::seconds(wait_s);
    }
    if (event.job.remove_remote) {
      if (event.deleted) {
        log_info("launcher.crash.deleted",
                 std::to_string(*event.deleted) + " report(s)");
        crash_notices_.push_back(
            notice(crash_notice::kind::deleted,
                   static_cast<std::size_t>(
                       std::max<std::int64_t>(0, *event.deleted))));
      } else {
        log_warn("launcher.crash.delete_failed", event.delete_error);
        crash_notices_.push_back(notice(crash_notice::kind::delete_failed, 0,
                                        {}, event.delete_error));
      }
    }
  }
}

void services::crash_reports_setting_changed() {
  crash_state_->send_enabled = config_.send_crash_reports;
  if (!config_.send_crash_reports) {
    // Off means nothing leaves this PC, also what was waiting.
    const std::size_t dropped = xerces_core::clear_outbox(paths_.crash);
    crash_retry_ = false;
    if (crash_next_) {
      crash_next_->send = false; // a delete asked for still runs
    }
    log_info("launcher.crash.off", "Crash reports off; " +
                                       std::to_string(dropped) +
                                       " queued report(s) removed");
    return;
  }
  log_info("launcher.crash.on", "Crash reports on");
  crash_job job;
  job.collect = !game_running();
  job.send = true;
  queue_crash_job(std::move(job));
}

void services::delete_crash_reports() {
  crash_job job;
  job.remove_remote = true;
  queue_crash_job(std::move(job));
}

bool services::crash_reports_busy() const {
  return crash_state_->busy || crash_next_.has_value();
}

std::vector<crash_notice> services::take_crash_notices() {
  std::vector<crash_notice> out;
  out.swap(crash_notices_);
  return out;
}

// ---------------------------------------------------------- find the culprit
void services::save_culprit() {
  ++culprit_rev_;
  const fs::path file = paths_.config / "culprit.json";
  if (!culprit_) {
    std::error_code ec;
    fs::remove(file, ec);
    return;
  }
  write_text(file, xerces_core::culprit_progress_to_json(*culprit_));
}

bool services::start_culprit() {
  const xerces_core::safe_mode_decision decision = safe_mode();
  if (!xerces_core::culprit_search_possible(decision)) {
    return false;
  }
  xerces_core::culprit_progress progress;
  // Test in the mode that crashed, whatever Home shows now; a journal from
  // before the mode was recorded leaves only Home's mode to go on.
  progress.mode =
      decision.crashed_mode.empty() ? config_.mode : decision.crashed_mode;
  progress.crashed_stack = decision.crashed_stack;
  // A crashed launch with experimental settings has them as one suspect
  // too: the first test runs the same stack without them. Every test keeps
  // the profile's map, so the map's dependencies load in all of them; the
  // search splits the other suspects first.
  progress.settings = xerces_core::crashed_settings(decision);
  progress.search = xerces_core::culprit_search(
      decision.crashed_stack, !progress.settings.empty(),
      xerces_core::culprit_always_loaded(catalog_.items, config_, progress.mode,
                                         game_.steam_build,
                                         decision.crashed_stack));
  culprit_ = std::move(progress);
  culprit_test_joined_ = false;
  save_culprit();
  std::string settings;
  for (const auto &name : culprit_->settings) {
    settings += " and -" + name;
  }
  log_info("launcher.culprit.start",
           std::to_string(culprit_->crashed_stack.size()) +
               (culprit_->crashed_stack.size() == 1 ? " mod" : " mods") +
               settings,
           std::string("{\"db_pool\":") +
               (decision.crashed_with_db_pool ? "true" : "false") +
               ",\"precise_limiter\":" +
               (decision.crashed_with_precise_limiter ? "true" : "false") +
               ",\"flip_model\":" +
               (decision.crashed_with_flip_model ? "true" : "false") + "}");
  return true;
}

void services::stop_culprit() {
  culprit_.reset();
  culprit_test_joined_ = false;
  save_culprit();
  log_info("launcher.culprit.stop", "stopped");
}

launch_outcome services::launch_culprit_test() {
  launch_outcome out;
  if (!culprit_ || culprit_->search.done()) {
    out.message = xerces_core::ui_text("There is no test to run.");
    out.diags.error("culprit.none", "", out.message);
    return out;
  }
  const std::vector<std::string> mods = culprit_->search.next_test();
  // Without the experimental settings, as in safe mode: every test runs on
  // the stock pool, the engine's own frame pacing and its own swap chain, so
  // a crash a setting causes never narrows the mods.
  xerces_core::launch_inputs in =
      inputs(culprit_->mode, true, false, std::nullopt, &mods);
  in.crash_followup = true;
  out = start(in);
  if (out.started) {
    culprit_->testing = mods;
    // What the test really loads: the planner adds what the mods asked for
    // need, and what the map needs. A crash narrows the suspects to these.
    culprit_->loaded.clear();
    if (out.resolved) {
      for (const auto &entry : out.resolved->stack) {
        culprit_->loaded.push_back(entry.id);
      }
    }
    culprit_test_joined_ = false;
    save_culprit();
  }
  return out;
}

void services::settle_culprit_from_journal() {
  if (!culprit_ || culprit_->testing.empty()) {
    return;
  }
  switch (xerces_core::culprit_test_result(last_journal())) {
  case xerces_core::culprit_test_end::crashed:
    settle_culprit_test(true);
    return;
  case xerces_core::culprit_test_end::ran_fine:
    settle_culprit_test(false);
    return;
  case xerces_core::culprit_test_end::not_counted:
    break;
  }
  // The test launch joined another player's game and ran the host's mods
  // from then on, not the ones under test: its crash or clean run says
  // nothing about them. Nothing is recorded; the same test is next.
  culprit_->testing.clear();
  culprit_->loaded.clear();
  culprit_test_joined_ = true;
  save_culprit();
  log_info("launcher.culprit.joined",
           "the test launch joined another player's game; nothing recorded",
           "{\"launches\":" + std::to_string(culprit_->search.launches()) +
               "}");
}

void services::settle_culprit_test(bool crashed) {
  if (!culprit_ || culprit_->testing.empty()) {
    return;
  }
  // A test saved before "loaded" was kept loaded what it asked for.
  culprit_->search.record(crashed, culprit_->loaded.empty() ? culprit_->testing
                                                            : culprit_->loaded);
  culprit_->testing.clear();
  culprit_->loaded.clear();
  save_culprit();
  log_info("launcher.culprit.result", crashed ? "crashed" : "ran fine",
           "{\"launches\":" + std::to_string(culprit_->search.launches()) +
               ",\"done\":" + (culprit_->search.done() ? "true" : "false") +
               ",\"settings_culprit\":" +
               (culprit_->search.settings_culprit() ? "true" : "false") + "}");
}

// --------------------------------------------------------------- signatures
void services::reload_signatures() {
  signatures_ = xerces_core::parse_signature_report(
      read_text(paths_.cache / "signatures-report.json").value_or(""));
  ++signatures_rev_;
}
} // namespace xerces_launcher
