#pragma once

// Hole punching for friends' joins (docs/DESIGN.md 10.13).
//
// A player joining a friend's lobby connects to the host's public address.
// Many home routers drop that first packet: the host's router has no mapping
// for the joiner. When the joiner is about to connect, the backend tells the
// host's launcher the joiner's public address and game port, and the
// launcher has this game send a small packet there from its own socket every
// second for a few minutes. That opens the host's router to the joiner (a
// router that keeps the game's port, as most do), and the joiner's connect
// gets through. Routers that change the port for each destination still
// need a forwarded port or UPnP.

#include <string>

namespace xerces_punch {
// Starts sending to address ("a.b.c.d:port", a public IPv4 address and a
// port of 1024 or above) while this game hosts; false when it is not one,
// or 16 others are going. A second call for the same address starts its
// time again.
bool start(const std::string &address);
} // namespace xerces_punch
