#include "services.hpp"

#include <RmlUi_Include_Windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

#include <xerces_core/bundle.hpp>
#include <xerces_core/detect.hpp>
#include <xerces_core/ipc.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/launcher_home.hpp>
#include <xerces_core/launcher_state.hpp>
#include <xerces_core/version.hpp>

#include "log.hpp"

namespace xerces_launcher {
namespace {
std::string utf8(const fs::path &path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

fs::path from_utf8(std::string_view text) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()),
                                text.size()));
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
} // namespace

services::services(launcher_paths paths, poster post, bool dry_launch)
    : paths_(std::move(paths)), post_(std::move(post)),
      dry_launch_(dry_launch) {
  load_config();
  reload_signatures();
  if (const auto text = read_text(paths_.config / "culprit.json")) {
    culprit_ = xerces_core::parse_culprit_progress(*text);
  }
  // A test launch that ended while the launcher was closed: the journal
  // says how it went. One still running (the player reopened Xerces
  // mid-test) is settled by poll() once it is gone.
  if (culprit_ && !culprit_->testing.empty() && !game_running()) {
    settle_culprit_test(xerces_core::journal_crashed(last_journal()));
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
    config_error_ = parsed.diags.items().empty()
                        ? "launcher.json could not be read"
                        : parsed.diags.items().front().message;
    log_warn("launcher.config.broken", config_error_);
  }
  config_ = std::move(parsed.value);
}

bool services::save_config() {
  ++config_rev_;
  const bool ok = write_text(paths_.config_file(),
                             xerces_core::launcher_config_to_json(config_));
  if (!ok) {
    log_error("launcher.config.write", "Could not write launcher.json");
  }
  return ok;
}

// --------------------------------------------------------------------- game
game_state services::detect_now(
    const std::optional<std::string> &saved_dir) const {
  return xerces_core::detect_game_state(saved_dir, paths_.compat);
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

void services::detect_game() { apply_detection(detect_now(config_.game_dir)); }

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

  const fs::path cache_file = paths_.cache / "hashes.txt";
  auto cancel = scan_cancel_;
  std::optional<std::string> saved_dir = config_.game_dir;
  std::optional<game_state> known;
  if (!detect) {
    known = game_;
  }
  scan_thread_ = std::thread([this, cache_file, cancel, detect, saved_dir,
                              known] {
    // Detection hashes BlackOps3.exe (about 100 MB), so it runs here too.
    game_state game = known ? *known : detect_now(saved_dir);
    if (detect) {
      post_([this, game] {
        detecting_ = false;
        apply_detection(game);
      });
    }
    auto result = std::make_shared<catalog_state>(xerces_core::scan_catalog(
        game, cache_file, cancel.get(),
        [this](const std::string &source, std::size_t index, std::size_t total,
               const std::string &name) {
          post_([this, index, total, name, source] {
            catalog_.done = index;
            catalog_.total = total;
            catalog_.current = source + ": " + name;
            ++catalog_rev_;
          });
        }));
    post_([this, result] { finish_scan(result); });
  });
}

void services::finish_scan(std::shared_ptr<catalog_state> result) {
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
  char data[200];
  std::snprintf(data, sizeof(data),
                "{\"items\":%zu,\"seconds\":%.2f,\"cache_hits\":%zu,"
                "\"cache_misses\":%zu,\"bytes_hashed\":%llu}",
                catalog_.items.size(), catalog_.seconds, catalog_.cache_hits,
                catalog_.cache_misses,
                static_cast<unsigned long long>(catalog_.bytes_hashed));
  log_info("launcher.catalog.scanned",
           std::to_string(catalog_.items.size()) + " items", data);
}

void services::cancel_scan() {
  if (scan_cancel_) {
    *scan_cancel_ = true;
  }
}

// --------------------------------------------------------------------- plan
xerces_core::plan_request services::request_for(const std::string &mode) const {
  xerces_core::plan_request request;
  request.mode = mode;
  if (const auto it = config_.profiles.find(mode); it != config_.profiles.end()) {
    request.mods = it->second.mods;
    request.map = it->second.map;
    request.pins = it->second.pins;
    request.allowed_conflicts = it->second.allowed_conflicts;
    request.settings = it->second.settings;
  }
  request.saved_positions = config_.saved_positions;
  if (game_.steam_build) {
    request.game_build = std::to_string(*game_.steam_build);
  }
  request.xerces_version =
      xerces_core::version::parse(xerces_core::xerces_version_text)
          .value_or(xerces_core::version{});
  request.protocol_version = xerces_core::protocol_version;
  return request;
}

xerces_core::plan services::resolve(const std::string &mode,
                                   const std::vector<std::string> *mods) const {
  xerces_core::plan_request request = request_for(mode);
  if (mods) {
    request.mods = *mods;
  }
  return xerces_core::resolve_plan(catalog_.items, request);
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
launch_outcome services::preview_launch(
    const std::string &mode, bool modded, bool mods_off,
    const std::optional<std::string> &connect,
    const std::vector<std::string> *mods) const {
  launch_outcome out;
  xerces_core::launch_options options;
  options.connect = connect;
  options.nointro = config_.nointro;
  options.mods_off = mods_off;
  options.extra_flags = config_.extra_flags;
  for (const auto &[name, value] : config_.dvars) {
    options.dvars.emplace_back(name, value);
  }
  std::optional<xerces_core::plan> resolved;
  if (modded && !mods_off) {
    resolved = resolve(mode, mods);
    out.plan_id = resolved->plan_id;
    if (!resolved->ok()) {
      out.diags.append(resolved->diags);
      out.message = "The plan has problems. Open Mods to fix them.";
      return out;
    }
    if (resolved->stack.empty()) {
      out.message = "No mods are enabled for this mode. Enable some in Mods, "
                    "or launch vanilla.";
      out.diags.error("launch.plan.empty", "", out.message);
      return out;
    }
  }
  const xerces_core::launch_command command = xerces_core::build_launch_command(
      resolved ? &*resolved : nullptr, options);
  out.diags.append(command.diags);
  out.command_line = command.command_line;
  if (!command.ok()) {
    out.message = command.diags.items().empty()
                      ? "The launch command could not be built."
                      : command.diags.items().front().message;
  }
  return out;
}

bool services::game_running() const {
  if (game_process_ &&
      WaitForSingleObject(static_cast<HANDLE>(game_process_), 0) ==
          WAIT_TIMEOUT) {
    return true;
  }
  // A game this launcher did not start (or started before it was closed and
  // opened again) announces itself in the run folder while it runs (15.2).
  const auto entries = xerces_core::ipc::live_run_entries(paths_.home / "run");
  return std::ranges::any_of(entries, [](const xerces_core::ipc::run_entry &e) {
    return e.role == "game";
  });
}

launch_outcome services::launch(const std::string &mode, bool modded,
                                bool mods_off,
                                const std::optional<std::string> &connect,
                                const std::vector<std::string> *mods) {
  launch_outcome out = preview_launch(mode, modded, mods_off, connect, mods);
  if (out.diags.has_errors()) {
    log_warn("launcher.launch.refused", out.message);
    return out;
  }
  if (!game_.can_launch()) {
    out.message = !game_.dir ? "Black Ops III was not found. Set its folder "
                               "in Settings."
                             : (game_.preflight ? game_.preflight->message
                                                : game_.error);
    out.diags.error("launch.game", "", out.message);
    return out;
  }
  if (game_running()) {
    out.message = "The game is already running.";
    out.diags.error("launch.running", "", out.message);
    return out;
  }
  std::error_code ec;
  const bool client_present = fs::exists(paths_.client, ec);
  // A dry launch starts nothing, so a checkout without boiii.exe can still
  // check its command lines; it says what a real launch would refuse.
  if (!client_present && !dry_launch_) {
    out.message = "The game client (" + utf8(paths_.client.filename()) +
                  ") is missing next to Xerces.exe.";
    out.diags.error("launch.client", "", out.message);
    return out;
  }

  // The plan file the client reads (5.4) and the journal (5.6).
  xerces_core::launch_journal journal;
  journal.phase = xerces_core::launch_phase::launching;
  journal.started = journal.updated = now_utc();
  journal.safe_mode = mods_off;
  journal.mode = mode;
  journal.launch_id = xerces_core::new_launch_id();
  if (modded && !mods_off) {
    const xerces_core::plan resolved = resolve(mode, mods);
    const std::string id = xerces_core::plan_file_id(resolved.plan_id);
    write_text(paths_.plans / (id + ".json"),
               xerces_core::plan_to_json(resolved, true));
    journal.plan_id = resolved.plan_id;
    for (const auto &entry : resolved.stack) {
      journal.stack.push_back(entry.id);
    }
  } else if (mods_off) {
    if (const auto last = last_journal()) {
      journal.plan_id = last->plan_id;
      journal.stack = last->stack;
    }
  }
  if (dry_launch_) {
    out.dry_run = true;
    out.message = client_present
                      ? "Dry launch: the command was written to the log."
                      : "Dry launch: the command was written to the log (a "
                        "real launch would stop: " +
                            utf8(paths_.client.filename()) + " is missing).";
    log_info("launcher.launch.dry", out.command_line,
             "{\"plan_id\":" + xerces_core::json::quote(out.plan_id) + "}");
    return out; // no journal: nothing ran that could crash
  }
  // Kept so a launch Windows refuses can put it back (below).
  const std::optional<std::string> previous_journal =
      read_text(paths_.journal_file());
  write_text(paths_.journal_file(), xerces_core::journal_to_json(journal));
  ++journal_rev_; // screens re-read it after this call, either way

  // The game role finds <home> and, in a development build, its data folder
  // through the environment it inherits, and the launch id that lets it take
  // this journal over.
  SetEnvironmentVariableW(L"XERCES_HOME", paths_.home.wstring().c_str());
  if (paths_.dev_data) {
    SetEnvironmentVariableW(L"XERCES_DATA_PATH",
                            paths_.dev_data->wstring().c_str());
  }
  const std::wstring launch_id(journal.launch_id.begin(),
                               journal.launch_id.end()); // hex digits
  SetEnvironmentVariableW(L"XERCES_LAUNCH_ID", launch_id.c_str());
  std::wstring command = L"\"" + paths_.client.wstring() + L"\" ";
  const fs::path args_path = from_utf8(out.command_line);
  command += args_path.wstring();
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const std::wstring cwd = game_.dir->wstring();
  const BOOL created = CreateProcessW(
      paths_.client.wstring().c_str(), command.data(), nullptr, nullptr, FALSE,
      0, nullptr, cwd.c_str(), &startup, &process);
  const DWORD error = GetLastError();
  // The id belongs to this one launch; nothing else this launcher opens
  // (Steam, Explorer) may carry it.
  SetEnvironmentVariableW(L"XERCES_LAUNCH_ID", nullptr);
  if (!created) {
    // Nothing ran, so nothing crashed: put the journal back as it was. Left
    // as "launching" it would read as a crash next time, and it would have
    // replaced the record of a real crash.
    if (previous_journal) {
      write_text(paths_.journal_file(), *previous_journal);
    } else {
      fs::remove(paths_.journal_file(), ec);
    }
    out.message = "Windows could not start the game (error " +
                  std::to_string(error) + ").";
    out.diags.error("launch.start", "", out.message);
    log_error("launcher.launch.failed", out.message);
    return out;
  }
  CloseHandle(process.hThread);
  if (game_process_) {
    CloseHandle(static_cast<HANDLE>(game_process_));
  }
  game_process_ = process.hProcess;
  out.started = true;
  out.pid = process.dwProcessId;
  out.message = modded && !mods_off ? "Starting Black Ops III with mods."
                                    : "Starting Black Ops III.";
  log_info("launcher.launch.started", out.command_line,
           "{\"pid\":" + std::to_string(out.pid) + ",\"plan_id\":" +
               xerces_core::json::quote(out.plan_id) + "}");
  return out;
}

void services::poll() {
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
    settle_culprit_test(xerces_core::journal_crashed(last_journal()));
  }
  log_info("launcher.game.exited", "exit code " + std::to_string(code),
           "{\"code\":" + std::to_string(code) + "}");
  reload_signatures();
}

void services::watch_run_folder() {
  // A game this launcher did not start (started before Xerces was reopened,
  // or restarted from the crash dialog) has no process handle here; it is
  // seen through its entry in <home>\run, read at most once a second.
  const auto now = std::chrono::steady_clock::now();
  if (now - last_run_check_ < std::chrono::seconds(1)) {
    return;
  }
  last_run_check_ = now;
  const bool live = game_running();
  if (game_was_live_ && !live) {
    // It exited: its journal may now say it crashed, so Home and
    // Diagnostics read it again.
    ++journal_rev_;
    log_info("launcher.game.gone", "a game this launcher did not start exited");
    reload_signatures();
  }
  game_was_live_ = live;
  // A culprit test that was still running when this launcher opened.
  if (!live && culprit_ && !culprit_->testing.empty()) {
    settle_culprit_test(xerces_core::journal_crashed(last_journal()));
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
  if (!decision.safe_mode || decision.crashed_stack.size() < 2) {
    return false;
  }
  xerces_core::culprit_progress progress;
  // Test in the mode that crashed, whatever Home shows now; a journal from
  // before the mode was recorded leaves only Home's mode to go on.
  progress.mode =
      decision.crashed_mode.empty() ? config_.mode : decision.crashed_mode;
  progress.crashed_stack = decision.crashed_stack;
  progress.search = xerces_core::culprit_search(decision.crashed_stack);
  culprit_ = std::move(progress);
  save_culprit();
  log_info("launcher.culprit.start",
           std::to_string(culprit_->crashed_stack.size()) + " mods");
  return true;
}

void services::stop_culprit() {
  culprit_.reset();
  save_culprit();
  log_info("launcher.culprit.stop", "stopped");
}

launch_outcome services::launch_culprit_test() {
  launch_outcome out;
  if (!culprit_ || culprit_->search.done()) {
    out.message = "There is no test to run.";
    out.diags.error("culprit.none", "", out.message);
    return out;
  }
  const std::vector<std::string> mods = culprit_->search.next_test();
  out = launch(culprit_->mode, true, false, std::nullopt, &mods);
  if (out.started) {
    culprit_->testing = mods;
    save_culprit();
  }
  return out;
}

void services::settle_culprit_test(bool crashed) {
  if (!culprit_ || culprit_->testing.empty()) {
    return;
  }
  culprit_->search.record(crashed);
  culprit_->testing.clear();
  save_culprit();
  log_info("launcher.culprit.result", crashed ? "crashed" : "ran fine",
           "{\"launches\":" + std::to_string(culprit_->search.launches()) +
               ",\"done\":" + (culprit_->search.done() ? "true" : "false") +
               "}");
}

// --------------------------------------------------------------- signatures
void services::reload_signatures() {
  signatures_ = xerces_core::parse_signature_report(
      read_text(paths_.cache / "signatures-report.json").value_or(""));
  ++signatures_rev_;
}
} // namespace xerces_launcher
