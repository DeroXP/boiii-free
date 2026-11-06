#pragma once

// The network password checked by the host (docs/DESIGN.md 11.10): the game
// role's side of xerces_core/join_password.hpp.
//
// The host answers challenges and checks the answers ("xercesJoinChallenge",
// "xercesJoinProof"), and asks admits() on every way in: nat.cpp before it
// answers a lobby join, and a network.cpp guard before auth.cpp sees a
// connect into the running match. The joiner runs prove() after the host's
// getinfo says it checks a password (party.cpp), before the lobby join or
// the connect.

#include <cstdint>
#include <functional>
#include <string>

#include <game/game.hpp>

namespace xerces_join_password {
// ------------------------------------------------------------ the host
// Whether sender may join this game's lobby or connect to its match. With
// net_password empty, always, and sender is remembered, so a password set
// later does not lock out who already joined. With a password, only an
// address that passed the check (or was remembered before). This PC itself
// (loopback, its own addresses, bots) always may. Call on the thread that
// handles packets.
bool admits(const game::net::netadr_t &sender);

// ----------------------------------------------------------- the joiner
enum class outcome {
  passed,  // the host accepted the password: join
  open,    // the host has no password after all: join
  needed,  // this game has no net_password
  wrong,   // the host did not accept it
  wait,    // too many wrong answers from this address lately
  stale,   // the check did not finish (twice)
  address, // the host did not recognise the address dialled
  timeout, // the host did not answer
  newer,   // the host checks in a newer way than this build knows
  refused, // the host refused a lobby join that brought no passed check
};
using done_callback = std::function<void(outcome, std::uint32_t wait_seconds)>;

// Runs the check with the host at `host`, with this game's net_password.
// done runs once, on the main thread. A newer prove() replaces one still
// running; the older done never runs.
void prove(const game::net::netadr_t &host, done_callback done);
// The popup for an outcome that stops the join; "" for passed and open.
std::string message(outcome result, std::uint32_t wait_seconds);
// Shows message() as the game's error popup, on the main thread.
void show(outcome result, std::uint32_t wait_seconds);

// For the two-PC test (docs/TWO_PC_TEST.md): with the dvar
// xerces_join_skip_password_check 1, a join skips this game's side of the
// check, as a modified client would, so the host's refusal can be seen. Off
// by default and never saved.
bool skip_check_for_test();
} // namespace xerces_join_password
