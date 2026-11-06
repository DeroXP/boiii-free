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
// The same, at once (the game is closing), after waiting up to 1 s for a
// request that is still out. Returns after at most about 3 s, even when the
// router does not answer; the mapping may then stay on the router.
void close_port_now();
// The router's outside address and the port while it forwards the port; ""
// when it does not, or did not say its outside address.
std::string external_endpoint();
// Whether the router forwards the port now, with or without an outside
// address.
bool port_mapped();
} // namespace upnp
