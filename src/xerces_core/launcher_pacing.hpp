#pragma once

// When the launcher's loop wakes up and when it draws (docs/DESIGN.md 12.7).
// Xerces.exe draws a frame only when something changed and otherwise sleeps
// until the next thing it waits for. launcher_pacer makes those decisions,
// apart from Windows and RmlUi so the tests can check them; precise_wait is
// the wait itself, timed finer than Windows' default 15.6 ms timer tick.

#include <limits>

namespace xerces_core {

class launcher_pacer {
public:
  static constexpr double never = std::numeric_limits<double>::infinity();
  // Frames still drawn after a change: screens move focus or scroll a frame
  // or two after the change that caused it, once the rows the data model
  // adds exist.
  static constexpr int settle_frames = 3;
  // The longest wait, and the one while a game runs and the launcher is
  // behind it or minimized (the game's exit is still noticed within about a
  // second).
  static constexpr double idle_wait = 0.1;
  static constexpr double game_wait = 1.0;
  // Short waits for what counts passes: a held controller direction repeats,
  // and focus being put back (keep_focus) or the boot screen time themselves.
  static constexpr double pad_wait = 0.008;
  static constexpr double count_wait = 0.016;
  // The shortest gap between two frames, as a share of a refresh. Under one
  // refresh, so that where Present waits for the display it alone sets the
  // pace; the gap only matters where Present returns at once (behind a
  // full-screen game, the display asleep).
  static constexpr double frame_gap = 0.75;
  // Behind another window frames are also at least 1/60 s apart.
  static constexpr double behind_gap = 1.0 / 60.0;

  // What the loop knows before it waits. Times are seconds on the loop's
  // clock, `never` for none.
  struct wait_input {
    double now = 0;
    bool loading = false; // screen documents left to load (one a pass)
    bool redraw = false;  // app::redraw(), a screen shown, a dialog
    bool capture = false; // a shot waits for the next frame
    bool minimized = false;
    bool front = false;
    bool game_running = false;
    double notice_end = never;  // the footer notice goes
    double script_next = never; // the next script step
    double seconds_end = never; // --seconds
    bool pad_repeat = false;    // a controller direction is held in front
    bool counting = false;      // keep_focus or the boot screen count passes
  };
  // When the pass should stop waiting; `now` when it must not wait.
  double wake_at(const wait_input &in) const;

  // What the pass found, after RmlUi's update.
  struct pass_input {
    double now = 0;
    // A reason to draw: a message, a posted task, controller input, a script
    // step, a counter the screens draw from, the notice, focus, the window's
    // state, a screen shown, a dialog, app::redraw().
    bool changed = false;
    bool capture = false;
    bool minimized = false;
    bool front = false;
    double rml_delay = never; // Rml::Context::GetNextUpdateDelay()
  };
  // Whether this pass draws a frame. Call once a pass, after the update;
  // when it says yes, draw and then call presented().
  bool draw(const pass_input &in);

  // When the frame draw() asked for may start: at least frame_gap of a
  // refresh after the last one (a 60 Hz refresh when Windows gives none
  // that a display has),
  // and behind_gap when the window is not in front. `precise` says whether
  // waits are timed finely (precise_wait::precise()); with coarse waits a
  // gap shorter than the timer's tick would slow frames down, so in front
  // there is none and Present alone paces.
  double frame_at(double now, double refresh, bool front, bool precise) const;
  // A frame started at t.
  void presented(double t) { presented_at_ = t; }

  bool first_frame() const { return presented_at_ < 0; }

private:
  bool stale_ = false; // a change not drawn yet (minimized)
  int settle_ = 0;
  double rml_due_ = 0; // when RmlUi's next update is due
  double presented_at_ = -1;
};

// Waits timed with a high-resolution waitable timer (Windows 10 1803 and
// later). They end within about half a millisecond of the time asked, where
// a plain timeout ends at the system timer's next tick: 15.6 ms unless the
// program raised the timer's resolution, which Xerces.exe does not. Without
// such a timer the waits fall back to plain timeouts; precise() says which.
// One thread at a time.
class precise_wait {
public:
  precise_wait();
  ~precise_wait();
  precise_wait(const precise_wait &) = delete;
  precise_wait &operator=(const precise_wait &) = delete;

  bool precise() const { return precise_; }
  // Waits up to `seconds` for a message in this thread's queue (posted,
  // sent or input; one already waiting counts too) or for `handle` (a
  // process, an event; may be null) to be signalled. True for either, false
  // when the time ran out. Pumps nothing.
  bool wait_messages(double seconds, void *handle = nullptr);
  // Sleeps `seconds`; messages wait in the queue.
  void sleep(double seconds);

private:
  // Arms the timer to fire after `seconds`; false when there is none.
  bool arm(double seconds);

  void *timer_ = nullptr;
  bool precise_ = false;
};
} // namespace xerces_core
