#pragma once

// -xerces-fast-loads (opt-in; fast_loads.cpp, docs/DESIGN.md 12.4).
namespace xerces_fast_loads {
// From the lobbyLaunchGame wrapper (mod_stack.cpp), on the main thread, just
// before this game's own lobby launches its match: with the flag and no
// other player in the game lobby, the lobby's waits for the others'
// acknowledgements are 0 for this launch; otherwise they are what they were.
void before_lobby_launch();
} // namespace xerces_fast_loads
