// When the launcher's loop wakes up and draws (docs/DESIGN.md 12.7): only
// when something changed, waking at most every 100 ms (a second behind a
// running game), with RmlUi's transitions drawn on every pass and frames
// paced; and the finely timed waits it sleeps in.

#include "harness.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include <xerces_core/launcher_pacing.hpp>

using namespace xerces_core;
using pacer = launcher_pacer;

namespace {
bool same(double a, double b) { return std::abs(a - b) < 1e-9; }

// A pass in front with nothing new, before and after the wait.
pacer::wait_input waiting(double now) {
  pacer::wait_input in;
  in.now = now;
  in.front = true;
  return in;
}

pacer::pass_input pass(double now, bool changed = false) {
  pacer::pass_input in;
  in.now = now;
  in.changed = changed;
  in.front = true;
  return in;
}

// The first frame, drawn at 0.
pacer started() {
  pacer p;
  CHECK(same(p.wake_at(waiting(0)), 0));
  CHECK(p.draw(pass(0)));
  p.presented(0);
  CHECK(!p.first_frame());
  return p;
}

// Frames drawn by passes at `now`, 10 ms apart, until the pacer would wait.
int frames_until_idle(pacer &p, double &now) {
  int frames = 0;
  for (int i = 0; i < 100 && same(p.wake_at(waiting(now)), now); ++i) {
    if (p.draw(pass(now))) {
      p.presented(now);
      ++frames;
    }
    now += 0.01;
  }
  return frames;
}
} // namespace

TEST_CASE("launcher pacer: the first frame does not wait, then idle draws "
          "nothing") {
  pacer p = started();
  // Nothing changed: wake after 100 ms and draw nothing, again and again.
  double now = 0.01;
  for (int i = 0; i < 20; ++i) {
    const double until = p.wake_at(waiting(now));
    CHECK(same(until, now + pacer::idle_wait));
    CHECK(!p.draw(pass(until)));
    now = until;
  }
}

TEST_CASE("launcher pacer: a change draws, then three settle frames") {
  pacer p = started();
  double now = 1;
  CHECK(p.draw(pass(now, true)));
  p.presented(now);
  now += 0.01;
  CHECK_EQ(frames_until_idle(p, now), pacer::settle_frames);
  CHECK(!p.draw(pass(now)));
  // A change while settling starts the count again.
  CHECK(p.draw(pass(now, true)));
  p.presented(now);
  now += 0.01;
  CHECK(p.draw(pass(now)));
  CHECK(p.draw(pass(now + 0.01, true)));
  now += 0.02;
  CHECK_EQ(frames_until_idle(p, now), pacer::settle_frames);
}

TEST_CASE("launcher pacer: loading, a redraw and a capture do not wait") {
  pacer p = started();
  pacer::wait_input in = waiting(2);
  CHECK(p.wake_at(in) > 2);
  in.loading = true;
  CHECK(same(p.wake_at(in), 2));
  in = waiting(2);
  in.redraw = true;
  CHECK(same(p.wake_at(in), 2));
  in = waiting(2);
  in.capture = true;
  in.minimized = true;
  CHECK(same(p.wake_at(in), 2));
  // A capture draws even while the window is minimized.
  pacer::pass_input shot = pass(2);
  shot.capture = true;
  shot.minimized = true;
  shot.front = false;
  CHECK(p.draw(shot));
}

TEST_CASE("launcher pacer: minimized, a change waits for the window to show") {
  pacer p = started();
  pacer::pass_input hidden = pass(1, true);
  hidden.minimized = true;
  hidden.front = false;
  CHECK(!p.draw(hidden)); // a screen shown by a script step, say
  // Neither the change nor its settle frames keep a minimized window awake.
  pacer::wait_input asleep = waiting(1.01);
  asleep.minimized = true;
  asleep.front = false;
  CHECK(same(p.wake_at(asleep), 1.01 + pacer::idle_wait));
  hidden.now = 1.11;
  hidden.changed = false;
  CHECK(!p.draw(hidden));
  // Shown again: drawn at once, then it settles and idles.
  CHECK(same(p.wake_at(waiting(1.2)), 1.2));
  CHECK(p.draw(pass(1.2)));
  p.presented(1.2);
  double now = 1.21;
  CHECK(frames_until_idle(p, now) <= pacer::settle_frames);
  CHECK(!p.draw(pass(now)));
}

TEST_CASE("launcher pacer: a running game behind the launcher wakes it once "
          "a second") {
  pacer p = started();
  pacer::wait_input in = waiting(5);
  in.game_running = true;
  CHECK(same(p.wake_at(in), 5 + pacer::idle_wait)); // in front: as usual
  in.front = false;
  CHECK(same(p.wake_at(in), 5 + pacer::game_wait));
  in.minimized = true;
  CHECK(same(p.wake_at(in), 5 + pacer::game_wait));
  in.front = true; // minimized but still the foreground window
  CHECK(same(p.wake_at(in), 5 + pacer::game_wait));
  in.game_running = false;
  in.front = false;
  CHECK(same(p.wake_at(in), 5 + pacer::idle_wait));
  // A minute behind the game: sixty passes, no frame.
  int frames = 0;
  int passes = 0;
  for (double now = 6; now < 66;) {
    pacer::wait_input w = waiting(now);
    w.front = false;
    w.game_running = true;
    now = p.wake_at(w);
    pacer::pass_input after = pass(now);
    after.front = false;
    frames += p.draw(after) ? 1 : 0;
    ++passes;
  }
  CHECK_EQ(frames, 0);
  CHECK_EQ(passes, 60);
}

TEST_CASE("launcher pacer: the notice, script steps and --seconds end the "
          "wait; --seconds alone does not keep the loop busy") {
  pacer p = started();
  pacer::wait_input in = waiting(3);
  in.seconds_end = 15;
  CHECK(same(p.wake_at(in), 3 + pacer::idle_wait));
  in.seconds_end = 3.04;
  CHECK(same(p.wake_at(in), 3.04));
  in = waiting(3);
  in.script_next = 3.02;
  CHECK(same(p.wake_at(in), 3.02));
  in.notice_end = 3.01;
  CHECK(same(p.wake_at(in), 3.01));
  in.notice_end = 2.5; // already due: no wait
  CHECK(same(p.wake_at(in), 3));
  // Also behind a running game.
  in = waiting(3);
  in.front = false;
  in.game_running = true;
  in.script_next = 3.5;
  CHECK(same(p.wake_at(in), 3.5));
  // A held controller direction repeats; keep_focus and the boot screen
  // count passes.
  in = waiting(3);
  in.pad_repeat = true;
  CHECK(same(p.wake_at(in), 3 + pacer::pad_wait));
  in = waiting(3);
  in.counting = true;
  CHECK(same(p.wake_at(in), 3 + pacer::count_wait));
}

TEST_CASE("launcher pacer: a transition draws on every pass until it ends, "
          "also behind another window") {
  for (const bool front : {true, false}) {
    pacer p = started();
    double now = 1;
    int frames = 0;
    // A change (focus moved, say) starts a transition: RmlUi asks for the
    // next update at once for 0.2 s, a pass every 5 ms.
    for (int i = 0; i < 40; ++i) {
      now = 1 + i * 0.005;
      pacer::wait_input w = waiting(now);
      w.front = front;
      if (i > 0) {
        CHECK(same(p.wake_at(w), now));
      }
      pacer::pass_input in = pass(now, i == 0);
      in.front = front;
      in.rml_delay = 0;
      if (p.draw(in)) {
        p.presented(now);
        ++frames;
      }
    }
    CHECK_EQ(frames, 40);
    now += 0.005;
    // It ended: one more frame draws the end, then nothing.
    pacer::pass_input done = pass(now);
    done.front = front;
    CHECK(p.draw(done));
    done.now += 0.01;
    CHECK(!p.draw(done));
    pacer::wait_input w = waiting(done.now);
    w.front = front;
    CHECK(same(p.wake_at(w), done.now + pacer::idle_wait));
  }
}

TEST_CASE("launcher pacer: a transition while minimized is drawn once the "
          "window shows") {
  pacer p = started();
  pacer::pass_input in = pass(1);
  in.minimized = true;
  in.front = false;
  in.rml_delay = 0;
  CHECK(!p.draw(in));
  pacer::wait_input w = waiting(1.01);
  w.minimized = true;
  w.front = false;
  CHECK(same(p.wake_at(w), 1.01 + pacer::idle_wait)); // no busy loop
  in.now = 1.11;
  CHECK(!p.draw(in));
  CHECK(same(p.wake_at(waiting(1.2)), 1.2));
  CHECK(p.draw(pass(1.2)));
}

TEST_CASE("launcher pacer: a caret blinks in front, but behind the game it "
          "neither wakes the loop nor draws") {
  {
    pacer p = started();
    pacer::pass_input in = pass(1);
    in.rml_delay = 0.5; // a focused text field's caret
    CHECK(!p.draw(in));
    CHECK(same(p.wake_at(waiting(1.45)), 1.5)); // wakes for the blink
    in.now = 1.5;
    CHECK(p.draw(in)); // and draws it
  }
  for (const bool game : {true, false}) {
    pacer p = started();
    int frames = 0;
    int passes = 0;
    for (double now = 1; now < 13;) {
      pacer::pass_input in = pass(now);
      in.front = false;
      in.rml_delay = 0.5;
      frames += p.draw(in) ? 1 : 0;
      ++passes;
      pacer::wait_input w = waiting(now);
      w.front = false;
      w.game_running = game;
      now = p.wake_at(w);
    }
    CHECK_EQ(frames, 0);
    if (game) {
      CHECK_EQ(passes, 12);
    } else {
      CHECK(passes >= 119 && passes <= 121); // 100 ms waits
    }
  }
}

TEST_CASE("launcher pacer: frames are three quarters of a refresh apart, "
          "and 1/60 s behind another window") {
  const double hz240 = 1.0 / 240;
  pacer p;
  CHECK(same(p.frame_at(5, hz240, true, true), 5)); // the first frame
  p.presented(1);
  CHECK(same(p.frame_at(1.0005, hz240, true, true), 1 + 0.75 * hz240));
  CHECK(same(p.frame_at(1.1, hz240, true, true), 1.1)); // already later
  CHECK(same(p.frame_at(1.0005, hz240, false, true), 1 + 1.0 / 60));
  CHECK(same(p.frame_at(1.0005, 1.0 / 30, false, true), 1 + 0.75 / 30));
  CHECK(same(p.frame_at(1.0005, 1.0 / 60, true, true), 1 + 0.75 / 60));
  // Coarse waits would round a short gap up to a 15.6 ms tick: in front
  // Present alone paces then.
  CHECK(same(p.frame_at(1.0005, hz240, true, false), 1.0005));
  CHECK(same(p.frame_at(1.0005, hz240, false, false), 1 + 1.0 / 60));
  // No refresh known, or not a display's: 60 Hz.
  CHECK(same(p.frame_at(1.0005, 0, true, true), 1 + 0.75 / 60));
  CHECK(same(p.frame_at(1.0005, 1.0, true, true), 1 + 0.75 / 60));
}

TEST_CASE("launcher pacer: where Present does not wait, a transition draws "
          "at most a frame per three quarters of a refresh") {
  // The loop as app::run plays it, with Present returning at once (behind a
  // full-screen game, the display asleep): every pass is due, each frame
  // waits for frame_at.
  const double refresh = 1.0 / 240;
  pacer p = started();
  double now = 1;
  pacer::pass_input first = pass(now, true); // the change that starts it
  first.rml_delay = 0;
  REQUIRE(p.draw(first));
  p.presented(now);
  int frames = 1;
  for (int i = 0; i < 10000; ++i) {
    now = std::max(now + 0.0002, p.wake_at(waiting(now))); // a pass: 0.2 ms
    if (now >= 1.2) {
      break;
    }
    pacer::pass_input in = pass(now);
    in.rml_delay = 0;
    if (p.draw(in)) {
      now = p.frame_at(now, refresh, true, true);
      p.presented(now);
      ++frames;
    }
  }
  const int most = int(0.2 / (pacer::frame_gap * refresh)) + 2;
  CHECK(frames <= most);
  CHECK(frames >= most - 4);
}

namespace {
double seconds_since(std::chrono::steady_clock::time_point from) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - from)
      .count();
}

void drain_messages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
  }
}
} // namespace

TEST_CASE("precise wait: a signalled handle ends the wait at once") {
  precise_wait waits;
  HANDLE event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  REQUIRE(event != nullptr);
  const auto from = std::chrono::steady_clock::now();
  CHECK(waits.wait_messages(2.0, event));
  CHECK(seconds_since(from) < 0.5);
  ResetEvent(event);
  CHECK(!waits.wait_messages(0.002, event));
  CloseHandle(event);
}

TEST_CASE("precise wait: a message ends the wait, also one already queued") {
  precise_wait waits;
  drain_messages(); // also gives this thread its message queue
  REQUIRE(PostThreadMessageW(GetCurrentThreadId(), WM_APP, 0, 0));
  const auto from = std::chrono::steady_clock::now();
  CHECK(waits.wait_messages(2.0));
  CHECK(seconds_since(from) < 0.5);
  drain_messages();
  CHECK(!waits.wait_messages(0.002));
}

TEST_CASE("precise wait: the time running out ends the wait, not early, and "
          "a short one well before Windows' 15.6 ms tick") {
  precise_wait waits;
  drain_messages();
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  REQUIRE(event != nullptr);
  double fastest_wait = 1;
  double fastest_sleep = 1;
  for (int i = 0; i < 5; ++i) {
    auto from = std::chrono::steady_clock::now();
    CHECK(!waits.wait_messages(0.003, event));
    const double waited = seconds_since(from);
    CHECK(waited >= 0.0025);
    fastest_wait = std::min(fastest_wait, waited);
    from = std::chrono::steady_clock::now();
    waits.sleep(0.003);
    const double slept = seconds_since(from);
    CHECK(slept >= 0.0025);
    fastest_sleep = std::min(fastest_sleep, slept);
  }
  CloseHandle(event);
  // The high-resolution timer exists on Windows 10 1803 and later.
  CHECK(waits.precise());
  if (waits.precise()) {
    CHECK(fastest_wait < 0.010);
    CHECK(fastest_sleep < 0.010);
  }
}
