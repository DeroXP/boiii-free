#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "updater.hpp"
#include "game/game.hpp"

#include <utils/flags.hpp>
#include <utils/progress_ui.hpp>
#include <updater/updater.hpp>

namespace updater {
void update() {
  // bo3-bundle fork: always skip the self-updater. Otherwise our custom build
  // gets overwritten by the upstream official binary on every launch and any
  // patches we add (e.g. bundle_test_loadmod) disappear before main() finishes.
  return;
  if (utils::flags::has_flag("noupdate")) {
    return;
  }

  try {
    run(game::get_appdata_path());
  } catch (update_cancelled &) {
    TerminateProcess(GetCurrentProcess(), 0);
  } catch (const std::exception &e) {
    utils::progress_ui::show_error("Updater Error", e.what());
  } catch (...) {
    utils::progress_ui::show_error("Updater Error",
                                   "Unknown error occurred during update.");
  }
}

class component final : public generic_component {
public:
  component() {
    this->update_thread_ = std::thread([this] { update(); });
  }

  void pre_destroy() override { join(); }

  void post_unpack() override { join(); }

  component_priority priority() const override {
    return component_priority::updater;
  }

private:
  std::thread update_thread_{};

  void join() {
    if (this->update_thread_.joinable()) {
      this->update_thread_.join();
    }
  }
};
} // namespace updater

REGISTER_COMPONENT(updater::component)
