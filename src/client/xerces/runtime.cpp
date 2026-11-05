#include <std_include.hpp>

#include "runtime.hpp"

#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>

#include <shellapi.h>

#include <game/game.hpp>

#include <xerces_core/json.hpp>
#include <xerces_core/launch.hpp>
#include <xerces_core/text_tail.hpp>

#include "perf.hpp"

namespace xerces_runtime {
namespace {
std::optional<std::wstring> env(const wchar_t *name) {
  wchar_t buffer[32768];
  const DWORD size = GetEnvironmentVariableW(name, buffer, 32768);
  if (size == 0 || size >= 32768) {
    return std::nullopt;
  }
  return std::wstring(buffer, size);
}

std::string utf8(const std::wstring &text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(
      CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(std::size_t(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(),
                      size, nullptr, nullptr);
  return out;
}

std::mutex log_mutex;
std::unique_ptr<xerces_core::json_log> session;
bool log_failed = false;
} // namespace

std::filesystem::path home() {
  static const std::filesystem::path result = [] {
    std::filesystem::path dir;
    if (const auto set = env(L"XERCES_HOME")) {
      dir = *set;
    } else if (const auto local = env(L"LOCALAPPDATA")) {
      dir = std::filesystem::path(*local) / "Xerces";
    } else {
      dir = std::filesystem::current_path() / "xerces_home";
    }
    std::error_code ec;
    for (const char *sub : {"logs", "cache", "config"}) {
      std::filesystem::create_directories(dir / sub, ec);
    }
    return dir;
  }();
  return result;
}

bool started_by_launcher() {
  static const bool result = env(L"XERCES_HOME").has_value();
  return result;
}

void log(xerces_core::log_level level, std::string_view event,
         std::string_view message, std::string_view data_json) {
  std::lock_guard lock(log_mutex);
  if (!session && !log_failed) {
    auto file = std::make_unique<xerces_core::json_log>(
        "game", std::to_string(GetCurrentProcessId()));
    if (const auto profile = env(L"USERPROFILE")) {
      file->set_profile_path(utf8(*profile));
    }
    if (file->open(home() / "logs")) {
      session = std::move(file);
    } else {
      log_failed = true;
    }
  }
  if (!session) {
    return;
  }
  xerces_core::log_event item;
  item.level = level;
  item.component = "xerces";
  item.event = std::string(event);
  item.message = std::string(message);
  item.data_json = std::string(data_json);
  session->write(item);
}

bool write_file(const std::filesystem::path &path, std::string_view text) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path temporary = path;
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
  std::filesystem::rename(temporary, path, ec);
  return !ec;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> command_line_args() {
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return {};
  }
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.push_back(utf8(argv[i]));
  }
  LocalFree(argv);
  return args;
}

std::string flag_value(std::string_view flag) {
  return xerces_core::flag_value(command_line_args(), flag).value_or("");
}

// ----------------------------------------------------------- the journal
namespace {
// Plain values only: pre_destroy can run again during static destruction.
std::mutex journal_mutex;
bool journal_decided = false; // the first call looked at the launch id
bool journal_owned = false;
xerces_core::launch_phase journal_current =
    xerces_core::launch_phase::launching;

// XERCES_LAUNCH_ID as this process started with it; empty when it had none.
// A fixed buffer rather than a std::string, for the same reason as above: the
// journal can still be decided during static destruction.
char start_launch_id[64] = {};

// The id is taken out of the environment while the C runtime starts this
// program, before main(). The crash dialog's "Restart game" starts a copy of
// this process with its environment, and that copy must never take over the
// record of this launch. The dialog exists from the exception component's
// constructor on, which runs inside main(); waiting for the first journal
// call instead (the signature lookup in post_unpack) left the id in place
// through every post_load and the post_unpacks before it, so a crash there
// restarted a copy that still held it. Any other copy (the updater's
// relaunch) goes without it too: only the process the launcher started owns
// the record.
[[maybe_unused]] const bool launch_id_taken = [] {
  const auto id = env(L"XERCES_LAUNCH_ID");
  SetEnvironmentVariableW(L"XERCES_LAUNCH_ID", nullptr);
  if (id) {
    // The launcher's ids are 32 hex digits. One that does not fit could
    // never match a journal, so it is treated as no id at all.
    const std::string text = utf8(*id);
    if (text.size() < sizeof(start_launch_id)) {
      std::memcpy(start_launch_id, text.data(), text.size());
    }
  }
  return true;
}();

std::filesystem::path journal_path() {
  return home() / "logs" / "journal.json";
}

// With journal_mutex held. Runs once, at the first journal call, which is
// the signature lookup in the first post_unpack that needs it.
void decide_journal() {
  if (journal_decided) {
    return;
  }
  journal_decided = true;
  if (!game::is_client() || !game::is_new_client()) {
    return; // only the client role walks through the phases
  }
  const std::string_view id(start_launch_id);
  if (id.empty()) {
    // Not started by the launcher, or restarted from a crash. Said in the
    // log so a bug report shows why this run has no phases.
    log_info("loader.journal.none",
             "Started without a launch id; the journal is left alone");
    return;
  }
  auto journal = xerces_core::parse_journal(read_file(journal_path()));
  if (!xerces_core::journal_claimable(journal, id)) {
    log_warn("loader.journal.skipped",
             "The journal is not this launch's; leaving it alone");
    return;
  }
  // An empty id tells the launcher the game took over: from here on an exit
  // code of 0 alone does not mean a clean exit.
  journal->launch_id.clear();
  journal->updated = xerces_core::json_log::now_utc();
  journal_owned =
      write_file(journal_path(), xerces_core::journal_to_json(*journal));
}

// With journal_mutex held.
void write_phase(xerces_core::launch_phase phase) {
  decide_journal();
  if (!journal_owned || phase == journal_current) {
    return;
  }
  std::string plan_id;
  {
    // Read, parse and write on the caller's thread, timed for perf.io. The
    // loader.phase line below is left out: its flushed write is log.cpp's.
    xerces_perf::timed timing(xerces_perf::step_kind::io, "journal");
    auto journal = xerces_core::parse_journal(read_file(journal_path()));
    if (!journal || journal->phase == xerces_core::launch_phase::clean_exit ||
        !journal->launch_id.empty()) {
      // Closed already, or the launcher recorded a newer launch: leave it.
      journal_owned = false;
      return;
    }
    journal_current = phase;
    journal->phase = phase;
    journal->updated = xerces_core::json_log::now_utc();
    const std::string text = xerces_core::journal_to_json(*journal);
    write_file(journal_path(), text);
    timing.set_bytes(text.size());
    plan_id = std::move(journal->plan_id);
  }
  log_info(
      "loader.phase", xerces_core::to_string(phase),
      "{\"phase\":" + xerces_core::json::quote(xerces_core::to_string(phase)) +
          ",\"plan_id\":" + xerces_core::json::quote(plan_id) + "}");
}
} // namespace

bool journal_active() {
  std::lock_guard lock(journal_mutex);
  decide_journal();
  return journal_owned;
}

xerces_core::launch_phase journal_phase() {
  std::lock_guard lock(journal_mutex);
  return journal_current;
}

void set_journal_phase(xerces_core::launch_phase phase) {
  std::lock_guard lock(journal_mutex);
  write_phase(phase);
}

journal_step::journal_step(xerces_core::launch_phase phase,
                           xerces_core::launch_phase after)
    : after_(after) {
  std::lock_guard lock(journal_mutex);
  decide_journal();
  if (journal_owned &&
      journal_current == xerces_core::launch_phase::launching) {
    entered_ = true;
    write_phase(phase);
  }
}

journal_step::~journal_step() {
  if (entered_) {
    set_journal_phase(after_);
  }
}

journal_step journal_symbols_step() {
  return journal_step(xerces_core::launch_phase::symbols,
                      xerces_core::launch_phase::launching);
}

// ---------------------------------------------------------------- the exit
namespace {
// Plain values: read during shutdown, after other statics may be gone.
std::atomic_bool quit_seen{false};
std::atomic_bool crashed{false};
std::atomic_bool exit_code_known{false};
std::atomic<std::uint32_t> exit_code_value{0};

std::mutex console_mutex;
xerces_core::text_tail console_tail; // a fixed buffer, no destructor work
} // namespace

void note_quit_requested() { quit_seen = true; }
bool quit_requested() { return quit_seen; }
void note_crash() { crashed = true; }
bool crash_seen() { return crashed; }

void journal_quit_forced() {
  if (quit_seen && !crashed) {
    set_journal_phase(xerces_core::launch_phase::clean_exit);
  }
}

void note_exit_code(std::uint32_t code) {
  exit_code_value = code;
  exit_code_known = true;
}

std::optional<std::uint32_t> exit_code() {
  if (!exit_code_known) {
    return std::nullopt;
  }
  return exit_code_value.load();
}

void console_append(std::string_view text) {
  std::lock_guard lock(console_mutex);
  console_tail.append(text);
}

std::vector<std::string> console_last_lines(std::size_t count) {
  // A crash can come while another thread holds the lock: never wait long.
  std::unique_lock lock(console_mutex, std::defer_lock);
  for (int attempt = 0; attempt < 50 && !lock.try_lock(); ++attempt) {
    Sleep(2);
  }
  if (!lock.owns_lock()) {
    return {};
  }
  return console_tail.last_lines(count);
}
} // namespace xerces_runtime
