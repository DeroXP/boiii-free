// bo3-bundle fork: tee every console message to a log file on disk.
//
// BOIII routes printf -> print_stub -> queue_message, which fans out to the
// in-game tilde console + any registered interceptor. We register an
// interceptor at startup that appends every message to
//   %LOCALAPPDATA%\boiii\console.log
// so we can scroll back through full traces (the in-game console truncates).
//
// Truncates the log on each launch so traces stay scoped to one session.
#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "console.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace console_logger {
namespace {

std::ofstream g_log_file;
std::mutex g_log_mutex;

std::string current_timestamp() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto t = system_clock::to_time_t(now);
  const auto ms =
      duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  std::tm tm_buf{};
  localtime_s(&tm_buf, &t);
  std::ostringstream os;
  os << std::put_time(&tm_buf, "%H:%M:%S") << '.'
     << std::setfill('0') << std::setw(3) << ms.count();
  return os.str();
}

void write_to_log(const std::string &message) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (!g_log_file.is_open()) {
    return;
  }
  g_log_file << '[' << current_timestamp() << "] " << message;
  if (message.empty() || message.back() != '\n') {
    g_log_file << '\n';
  }
  g_log_file.flush();
}

void open_log_file() {
  try {
    const auto log_dir = game::get_appdata_path();
    std::filesystem::create_directories(log_dir);
    const auto log_path = log_dir / "console.log";
    g_log_file.open(log_path, std::ios::out | std::ios::trunc);
    if (g_log_file.is_open()) {
      g_log_file << "===== boiii console log opened at "
                 << current_timestamp() << " =====\n";
      g_log_file.flush();
    }
  } catch (...) {
    // best-effort; if we can't open the log just stay silent.
  }
}

class component final : public generic_component {
public:
  void post_unpack() override {
    open_log_file();
    console::set_interceptor(write_to_log);
  }

  void pre_destroy() override {
    console::remove_interceptor();
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file.is_open()) {
      g_log_file << "===== boiii console log closed at "
                 << current_timestamp() << " =====\n";
      g_log_file.close();
    }
  }
};

} // namespace
} // namespace console_logger

REGISTER_COMPONENT(console_logger::component)
