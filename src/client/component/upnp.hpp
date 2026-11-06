#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace upnp {
// What the router said when asked to forward the game's UDP port.
enum class result { mapped, no_router, refused };
struct outcome {
  result value = result::no_router;
  std::string external_ip; // the router's outside address, when mapped
  uint16_t port = 0;
};
using callback = std::function<void(const outcome &)>;

// Asks the router (UPnP) to forward the UDP port to this PC, on the async
// pipeline. done runs there with the answer; not when another request is
// still waiting, and not when the port was closed meanwhile.
void open_port(uint16_t port, callback done = {});
// Removes the mapping, on the async pipeline.
void close_port();
// The same, at once on this thread (the game is closing), after waiting a
// moment for a request that is still out.
void close_port_now();
std::string external_endpoint();
} // namespace upnp
