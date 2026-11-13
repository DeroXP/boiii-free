#pragma once

namespace exception {
void show_fatal_error(const std::string &message);
[[noreturn]] void restart_game();

// Xerces: a box on the screen that waits for the player (the unsafe-Lua
// question, the out-of-memory box, the fatal-error dialog). While one is open
// the freeze watchdog counts no stall: the game is waiting for an answer, not
// frozen, and closing it then took the box away before the player had read
// it. Nothing to construct or destroy but a counter.
class modal_box {
public:
  modal_box() noexcept;
  ~modal_box();
  modal_box(const modal_box &) = delete;
  modal_box &operator=(const modal_box &) = delete;
};
} // namespace exception
