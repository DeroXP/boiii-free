#include <std_include.hpp>

// Hole punching for friends' joins: punch.hpp.

#include "punch.hpp"

#include "runtime.hpp"

#include <component/nat.hpp>
#include <component/network.hpp>
#include <component/scheduler.hpp>

#include <game/game.hpp>
#include <game/utils.hpp>

#include <loader/component_loader.hpp>

namespace xerces_punch {
namespace {
using namespace std::chrono_literals;
using steady = std::chrono::steady_clock;

// The packet: a connectionless command both sides drop without a word.
constexpr const char *command = "xercesPunch";
// Long enough for the joiner's game to start and reach its main menu.
constexpr std::chrono::seconds lifetime = 180s;
constexpr std::size_t max_targets = 16;

struct target {
  game::net::netadr_t address{};
  steady::time_point until{};
};

std::mutex targets_mutex;
std::vector<target> targets;

// The lobby open to direct connect, or a match this game runs. Not the front
// end, which runs as a local server too (seen 2026-11-07: game::
// server_running() was true at the main menu).
bool hosting() {
  if (!nat::joinable_lobby().empty()) {
    return true;
  }
  return game::server_running() && game::com::Com_IsInGame() &&
         !game::com::Com_IsRunningUILevel();
}

// Every second on the main thread.
void send_all() {
  std::vector<game::net::netadr_t> now_due;
  {
    std::lock_guard lock(targets_mutex);
    const auto now = steady::now();
    std::erase_if(targets,
                  [now](const target &entry) { return entry.until <= now; });
    if (targets.empty()) {
      return;
    }
    if (!hosting()) {
      // The lobby closed: nobody is joining it now.
      targets.clear();
      return;
    }
    for (const target &entry : targets) {
      now_due.push_back(entry.address);
    }
  }
  for (const auto &address : now_due) {
    network::send(address, command, "1");
  }
}
} // namespace

bool start(const std::string &address) {
  // A numeric IPv4 address only: a name would be looked up here.
  const std::size_t colon = address.rfind(':');
  in_addr numeric{};
  if (colon == std::string::npos ||
      inet_pton(AF_INET, address.substr(0, colon).c_str(), &numeric) != 1) {
    return false;
  }
  const game::net::netadr_t parsed = network::address_from_string(address);
  if (!network::is_connectable_address(parsed) || !hosting()) {
    return false;
  }
  std::lock_guard lock(targets_mutex);
  const auto until = steady::now() + lifetime;
  for (target &entry : targets) {
    if (network::are_addresses_equal(entry.address, parsed)) {
      entry.until = until;
      return true;
    }
  }
  if (targets.size() >= max_targets) {
    return false;
  }
  targets.push_back(target{parsed, until});
  // The address stays out of the log (DESIGN.md 15.1).
  xerces_runtime::log_info("xerces.punch.start",
                           "opening this host's router to a joining friend");
  return true;
}

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_punch");

public:
  void post_unpack() override {
    if (!game::is_client()) {
      return;
    }
    // The joiner gets these too, once its router lets them in: drop them.
    network::on(command,
                [](const game::net::netadr_t &, const network::data_view &,
                   game::LocalClientNum_t) {});
    scheduler::loop(send_all, scheduler::main, 1s);
  }
};
} // namespace xerces_punch

REGISTER_COMPONENT(xerces_punch::component)
