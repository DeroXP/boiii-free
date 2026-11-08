#include "workshop_download.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "log.hpp"

namespace xerces_launcher {
workshop_download::~workshop_download() {
  shared_->alive = false;
  cancel();
  if (worker_.joinable()) {
    worker_.join();
  }
}

bool workshop_download::start(const std::filesystem::path &cli,
                              const std::filesystem::path &game_dir,
                              const std::vector<std::uint64_t> &ids,
                              poster post, on_progress progress, on_done done,
                              std::string *error) {
  if (running_ || ids.empty()) {
    *error = "A download is already running.";
    return false;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  running_ = true;
  log_info("launcher.workshop.start", "Steam is asked to get " +
                                          std::to_string(ids.size()) +
                                          " Workshop item(s)");
  std::weak_ptr<shared> weak = shared_;
  worker_ = std::thread([this, cli, game_dir, ids, weak, post, progress, done] {
    std::string why;
    const int code = xerces_core::workshop_get::run_helper(
        cli, game_dir, ids,
        [&](const xerces_core::workshop_get::progress &line) {
          post([weak, progress, line] {
            if (const auto alive = weak.lock(); alive && alive->alive) {
              progress(line);
            }
          });
        },
        &process_, &why);
    running_ = false;
    if (code < 0) {
      post([weak, progress, why] {
        if (const auto alive = weak.lock(); alive && alive->alive) {
          progress({0, "error", 0, 0, why});
        }
      });
    }
    log_info("launcher.workshop.done",
             "the Workshop download ended (" + std::to_string(code) + ")");
    post([weak, done, code] {
      if (const auto alive = weak.lock(); alive && alive->alive) {
        done(code);
      }
    });
  });
  return true;
}

void workshop_download::cancel() {
  if (void *process = process_.load()) {
    TerminateProcess(static_cast<HANDLE>(process), 3);
  }
}
} // namespace xerces_launcher
