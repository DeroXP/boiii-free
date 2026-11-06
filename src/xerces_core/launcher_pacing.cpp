#include "launcher_pacing.hpp"

#include <algorithm>
#include <cmath>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace xerces_core {

double launcher_pacer::wake_at(const wait_input &in) const {
  // Passes that cannot wait: the first frame, screens still loading (one a
  // pass), a redraw asked for, a capture, and while the window shows: a
  // change not drawn yet, the frames after a change, and RmlUi's update once
  // it is due.
  const bool shows = !in.minimized;
  if (first_frame() || in.loading || in.redraw || in.capture ||
      (shows && (stale_ || settle_ > 0 || rml_due_ <= in.now))) {
    return in.now;
  }
  const bool game_behind = in.game_running && (!in.front || in.minimized);
  double until = in.now + (game_behind ? game_wait : idle_wait);
  if (shows) {
    until = std::min(until, rml_due_);
  }
  until = std::min({until, in.notice_end, in.script_next, in.seconds_end});
  if (in.pad_repeat) {
    until = std::min(until, in.now + pad_wait);
  }
  if (in.counting) {
    until = std::min(until, in.now + count_wait);
  }
  return std::max(until, in.now);
}

bool launcher_pacer::draw(const pass_input &in) {
  // RmlUi's own reasons: it asks for the next update at once while an
  // animation or a transition runs, and later for a caret blink or a held
  // scroll arrow. The pass it asked for draws what that update moved. The
  // later ones count only while the window is in front: behind the game the
  // launcher neither blinks its caret nor wakes up for it. Coming to the
  // front is a change, so the caret is drawn again then.
  const bool animating = rml_due_ <= in.now;
  const bool live = in.front && !in.minimized;
  rml_due_ = (in.rml_delay <= 0 || live) ? in.now + std::max(0.0, in.rml_delay)
                                         : never;

  const bool settling = settle_ > 0;
  const bool drawn =
      first_frame() || in.capture ||
      (!in.minimized && (in.changed || stale_ || settling || animating));
  if (in.changed) {
    settle_ = settle_frames;
  } else if (drawn && settling) {
    --settle_;
  }
  if (drawn) {
    stale_ = false;
  } else if (in.changed || animating) {
    stale_ = true; // minimized: drawn once the window shows again
  }
  return drawn;
}

double launcher_pacer::frame_at(double now, double refresh, bool front,
                                bool precise) const {
  if (first_frame() || (front && !precise)) {
    return now;
  }
  // No refresh, or one slower than 20 Hz (not a display's): 60 Hz.
  double gap =
      frame_gap * (refresh > 0 && refresh <= 0.05 ? refresh : 1.0 / 60);
  if (!front) {
    gap = std::max(gap, behind_gap);
  }
  return std::max(now, presented_at_ + gap);
}

namespace {
// A plain timeout in whole milliseconds, rounded up.
DWORD timeout_ms(double seconds) {
  return DWORD(std::ceil(std::clamp(seconds, 0.0, 86400.0) * 1000.0));
}
// With the timer armed the timeout is only a backstop.
constexpr DWORD backstop_ms = 50;
} // namespace

precise_wait::precise_wait() {
  timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                  CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                  TIMER_ALL_ACCESS);
  precise_ = timer_ != nullptr;
}

precise_wait::~precise_wait() {
  if (timer_) {
    CloseHandle(static_cast<HANDLE>(timer_));
  }
}

bool precise_wait::arm(double seconds) {
  if (!timer_) {
    return false;
  }
  // Relative due times are negative, in 100 ns units. Setting the timer
  // again also cancels one that a message ended the wait for.
  LARGE_INTEGER due{};
  due.QuadPart = -std::max<LONGLONG>(
      1, LONGLONG(std::ceil(std::min(seconds, 86400.0) * 1e7)));
  return SetWaitableTimer(static_cast<HANDLE>(timer_), &due, 0, nullptr,
                          nullptr, FALSE) != FALSE;
}

bool precise_wait::wait_messages(double seconds, void *handle) {
  HANDLE handles[2];
  DWORD count = 0;
  if (handle) {
    handles[count++] = static_cast<HANDLE>(handle);
  }
  DWORD timeout = 0;
  if (seconds > 0) {
    timeout = timeout_ms(seconds);
    if (arm(seconds)) {
      handles[count++] = static_cast<HANDLE>(timer_);
      timeout += backstop_ms;
    }
  }
  // MWMO_INPUTAVAILABLE: a message already in the queue also ends the wait,
  // not only one that arrives during it. WAIT_OBJECT_0 + i is handles[i] (the
  // timer is last: time ran out), WAIT_OBJECT_0 + count a message; a timeout
  // or a failure is past both.
  const DWORD got =
      MsgWaitForMultipleObjectsEx(count, count ? handles : nullptr, timeout,
                                  QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  return got == WAIT_OBJECT_0 + count || (handle && got == WAIT_OBJECT_0);
}

void precise_wait::sleep(double seconds) {
  if (seconds <= 0) {
    return;
  }
  if (arm(seconds)) {
    WaitForSingleObject(static_cast<HANDLE>(timer_),
                        timeout_ms(seconds) + backstop_ms);
  } else {
    Sleep(timeout_ms(seconds));
  }
}
} // namespace xerces_core
