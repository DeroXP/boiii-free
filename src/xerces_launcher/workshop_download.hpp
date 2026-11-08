#pragma once

// Runs `xerces_cli workshop get` (xerces_core/workshop_get.hpp) on a thread
// of its own: Steam subscribes to and downloads the given Workshop items,
// and each line of progress comes back on the UI thread.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <xerces_core/workshop_get.hpp>

namespace xerces_launcher {
class workshop_download {
public:
  using poster = std::function<void(std::function<void()>)>;
  using on_progress =
      std::function<void(const xerces_core::workshop_get::progress &)>;
  using on_done = std::function<void(int exit_code)>;

  workshop_download() = default;
  workshop_download(const workshop_download &) = delete;
  workshop_download &operator=(const workshop_download &) = delete;
  ~workshop_download(); // stops the process if it still runs

  // cli: xerces_cli.exe next to the launcher. Callbacks run through post,
  // on the UI thread, never after this object is gone.
  bool start(const std::filesystem::path &cli,
             const std::filesystem::path &game_dir,
             const std::vector<std::uint64_t> &ids, poster post,
             on_progress progress, on_done done, std::string *error);
  // Stops the process. Steam keeps what it has (a subscription stays, and
  // Steam may finish the download by itself).
  void cancel();
  bool running() const { return running_; }

private:
  struct shared {
    std::atomic<bool> alive{true};
  };
  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<void *> process_{nullptr};
  std::shared_ptr<shared> shared_ = std::make_shared<shared>();
};
} // namespace xerces_launcher
