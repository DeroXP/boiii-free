#include "services.hpp"

#include <RmlUi_Include_Windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

#include <xerces_core/detect.hpp>
#include <xerces_core/json.hpp>
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
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

bool write_text(const fs::path &path, std::string_view text) {
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
      return false;
    }
  }
  fs::rename(temporary, path, ec);
  return !ec;
}

std::optional<fs::path> env_path(const wchar_t *name) {
  wchar_t buffer[32768];
  const DWORD size = GetEnvironmentVariableW(name, buffer, 32768);
  if (size == 0 || size >= 32768) {
    return std::nullopt;
  }
  return fs::path(std::wstring(buffer, size));
}

fs::path module_dir() {
  std::wstring buffer(32768, L'\0');
  const DWORD size =
      GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
  buffer.resize(size);
  return fs::path(buffer).parent_path();
}

// Walks up from the executable's folder for a file, as development builds
// sit in <repo>\build\bin\x64\Release.
std::optional<fs::path> find_up(const fs::path &start, const fs::path &relative) {
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

std::string now_utc() { return xerces_core::json_log::now_utc(); }
} // namespace

launcher_paths find_paths(const std::optional<fs::path> &home_override) {
  launcher_paths out;
  if (home_override) {
    out.home = *home_override;
  } else if (const auto home = env_path(L"XERCES_HOME")) {
    out.home = *home;
  } else if (const auto local = env_path(L"LOCALAPPDATA")) {
    out.home = *local / "Xerces";
  } else {
    out.home = module_dir() / "home";
  }
  std::error_code ec;
  out.home = fs::absolute(out.home, ec);
  out.config = out.home / "config";
  out.plans = out.config / "plans";
  out.cache = out.home / "cache";
  out.logs = out.home / "logs";
  out.crash = out.home / "crash";
  for (const fs::path &dir :
       {out.config, out.plans, out.cache, out.logs, out.crash}) {
    fs::create_directories(dir, ec);
  }

  const fs::path exe_dir = module_dir();
  if (const auto root = find_up(exe_dir, fs::path("data/xerces/ui/shell.rml"))) {
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

const xerces_core::installed_content *
catalog_state::find(std::string_view id) const {
  const xerces_core::installed_content *best = nullptr;
  for (const auto &item : items) {
    if (item.info.id == id && (!best || best->info.ver < item.info.ver)) {
      best = &item;
    }
  }
  return best;
}

services::services(launcher_paths paths, poster post, bool dry_launch)
    : paths_(std::move(paths)), post_(std::move(post)),
      dry_launch_(dry_launch) {
  load_config();
  reload_signatures();
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
  game_state next;
  next.searched = true;
  std::optional<fs::path> saved;
  if (saved_dir) {
    saved = from_utf8(*saved_dir);
  }
  const xerces_core::detection found = xerces_core::detect_game(
      saved, xerces_core::steam_roots_from_registry(),
      xerces_core::real_filesystem());
  next.tried = found.tried;
  next.dir = found.game_dir;
  if (next.dir) {
    const fs::path dir = *next.dir;
    const fs::path steamapps = dir.parent_path().parent_path();
    next.workshop_dir = steamapps / "workshop" / "content" /
                        std::string(xerces_core::bo3_app_id);
    next.mods_dir = dir / "mods";
    next.usermaps_dir = dir / "usermaps";
    if (const auto acf = read_text(steamapps / ("appmanifest_" +
                                                std::string(xerces_core::bo3_app_id) +
                                                ".acf"))) {
      next.steam_build = xerces_core::appmanifest_buildid(*acf);
    }
    std::string error;
    next.exe = xerces_core::read_exe_facts(
        dir / std::string(xerces_core::bo3_exe), &error);
    if (!next.exe) {
      next.error = "BlackOps3.exe could not be read: " + error;
    } else {
      const auto registry_text = read_text(paths_.compat / "builds.json");
      const xerces_core::build_registry registry =
          xerces_core::parse_build_registry(registry_text.value_or(""));
      if (!registry_text || !registry.ok()) {
        next.error = "compat/builds.json is missing or broken.";
      }
      next.preflight =
          xerces_core::preflight_check(*next.exe, next.steam_build, registry);
    }
  }
  return next;
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

  struct source {
    std::string name;
    fs::path dir;
  };
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
    std::vector<source> sources;
    if (game.dir) {
      sources = {{"workshop", game.workshop_dir},
                 {"mods", game.mods_dir},
                 {"usermaps", game.usermaps_dir}};
    }
    const auto started = std::chrono::steady_clock::now();
    auto result = std::make_shared<catalog_state>();
    xerces_core::hash_cache cache;
    cache.load(cache_file);
    for (const source &from : sources) {
      std::error_code ec;
      if (!fs::is_directory(from.dir, ec)) {
        continue;
      }
      xerces_core::scan_options options;
      options.cache = &cache;
      options.cancel = cancel.get();
      options.progress = [this, &from](std::size_t index, std::size_t total,
                                       const std::string &name) {
        post_([this, index, total, name, label = from.name] {
          catalog_.done = index;
          catalog_.total = total;
          catalog_.current = label + ": " + name;
          ++catalog_rev_;
        });
      };
      xerces_core::catalog_scan scan =
          xerces_core::scan_mods_directory(from.dir, options);
      for (auto &item : scan.catalog) {
        result->items.push_back(std::move(item));
        result->sources.push_back(from.name);
      }
      result->diags.append(scan.diags);
      if (scan.cancelled) {
        break;
      }
    }
    const bool cancelled = cancel->load();
    cache.save(cache_file, !cancelled);
    result->cache_hits = cache.hits();
    result->cache_misses = cache.misses();
    result->bytes_hashed = cache.bytes_hashed();
    result->seconds = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - started)
                          .count();
    result->scanned = !cancelled;
    post_([this, result] { finish_scan(result); });
  });
}

void services::finish_scan(std::shared_ptr<catalog_state> result) {
  const bool keep_old = !result->scanned && catalog_.scanned;
  if (!keep_old) {
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

xerces_core::plan services::resolve(const std::string &mode) const {
  return xerces_core::resolve_plan(catalog_.items, request_for(mode));
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
    const std::optional<std::string> &connect) const {
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
    resolved = resolve(mode);
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
  return game_process_ &&
         WaitForSingleObject(static_cast<HANDLE>(game_process_), 0) ==
             WAIT_TIMEOUT;
}

launch_outcome services::launch(const std::string &mode, bool modded,
                                bool mods_off,
                                const std::optional<std::string> &connect) {
  launch_outcome out = preview_launch(mode, modded, mods_off, connect);
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
  if (!fs::exists(paths_.client, ec)) {
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
  if (modded && !mods_off) {
    const xerces_core::plan resolved = resolve(mode);
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
  write_text(paths_.journal_file(), xerces_core::journal_to_json(journal));

  if (dry_launch_) {
    out.dry_run = true;
    out.message = "Dry launch: the command was written to the log.";
    log_info("launcher.launch.dry", out.command_line,
             "{\"plan_id\":" + xerces_core::json::quote(out.plan_id) + "}");
    return out;
  }

  // The game role finds <home> and, in a development build, its data folder
  // through the environment it inherits.
  SetEnvironmentVariableW(L"XERCES_HOME", paths_.home.wstring().c_str());
  if (paths_.dev_data) {
    SetEnvironmentVariableW(L"XERCES_DATA_PATH",
                            paths_.dev_data->wstring().c_str());
  }
  std::wstring command = L"\"" + paths_.client.wstring() + L"\" ";
  const fs::path args_path = from_utf8(out.command_line);
  command += args_path.wstring();
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const std::wstring cwd = game_.dir->wstring();
  if (!CreateProcessW(paths_.client.wstring().c_str(), command.data(), nullptr,
                      nullptr, FALSE, 0, nullptr, cwd.c_str(), &startup,
                      &process)) {
    out.message = "Windows could not start the game (error " +
                  std::to_string(GetLastError()) + ").";
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
  if (!game_process_ ||
      WaitForSingleObject(static_cast<HANDLE>(game_process_), 0) != WAIT_OBJECT_0) {
    return;
  }
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(game_process_), &code);
  CloseHandle(static_cast<HANDLE>(game_process_));
  game_process_ = nullptr;
  // The client writes clean_exit itself; a client that never moved past
  // "launching" but exited with 0 quit normally before it could.
  if (auto journal = last_journal();
      journal && code == 0 &&
      journal->phase == xerces_core::launch_phase::launching) {
    journal->phase = xerces_core::launch_phase::clean_exit;
    journal->updated = now_utc();
    write_text(paths_.journal_file(), xerces_core::journal_to_json(*journal));
  }
  ++journal_rev_;
  log_info("launcher.game.exited", "exit code " + std::to_string(code),
           "{\"code\":" + std::to_string(code) + "}");
  reload_signatures();
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

// --------------------------------------------------------------- signatures
void services::reload_signatures() {
  signature_state next;
  const auto text = read_text(paths_.cache / "signatures-report.json");
  if (text) {
    const auto parsed = xerces_core::json::parse(*text);
    if (parsed.ok && parsed.document.IsObject()) {
      const auto &root = parsed.document;
      next.known = true;
      next.game_build = xerces_core::json::get_string(root, "game_build").value_or("");
      next.checked = xerces_core::json::get_string(root, "checked").value_or("");
      if (const auto *entries = xerces_core::json::member(root, "entries");
          entries && entries->IsArray()) {
        for (const auto &entry : entries->GetArray()) {
          if (!entry.IsObject()) {
            continue;
          }
          ++next.total;
          const auto *resolved = xerces_core::json::member(entry, "resolved");
          if (resolved && resolved->IsBool() && resolved->GetBool()) {
            ++next.matched;
          } else {
            next.missing.push_back(
                xerces_core::json::get_string(entry, "name").value_or("?"));
          }
        }
      }
    }
  }
  signatures_ = std::move(next);
  ++signatures_rev_;
}
} // namespace xerces_launcher
