#pragma once

// The launcher's window (docs/DESIGN.md 7.1): Win32 with RmlUi's platform
// helpers and its Direct3D 11 renderer. The window paints black (the token
// background) as soon as it exists; the D3D11 device and the shaders are
// created on a worker thread while RmlUi loads, behind a deferred render
// interface. The window has no system title bar: the header of every screen
// is the caption (hit_test), with resize edges, snap and the DWM shadow kept.

#include <filesystem>
#include <functional>
#include <string>

#include <RmlUi/Core/Input.h>

namespace Rml {
class Context;
class RenderInterface;
class SystemInterface;
} // namespace Rml

namespace xerces_launcher {
enum class hit_region { client, caption };

// The launcher window's class; a second launcher looks for the first by it.
inline constexpr wchar_t window_class_name[] = L"xerces_launcher_window";

struct window_callbacks {
  // Client-area pixel to region; the header's empty space is the caption.
  std::function<hit_region(int x, int y)> hit_test;
  // Keys before the context sees them (priority) and after it ignored them.
  // Return false when the key was used.
  std::function<bool(Rml::Input::KeyIdentifier key, int modifiers,
                     bool priority)>
      key_down;
};

class window {
public:
  window() = default;
  window(const window &) = delete;
  window &operator=(const window &) = delete;
  ~window();

  // width and height in dp; the window opens centred on the main monitor.
  bool open(const std::string &title, int width_dp, int height_dp,
            window_callbacks callbacks);
  void close();

  Rml::SystemInterface *system_interface();
  Rml::RenderInterface *render_interface();

  // Waits up to `seconds` for a window message (input, a wake) or, when
  // given, for `also` (a process handle) to be signalled; true when either
  // happened. Pumps nothing. Timed finely (xerces_core::precise_wait): the
  // wait ends within about half a millisecond of `seconds`, not at Windows'
  // next 15.6 ms timer tick.
  bool wait_events(double seconds, void *also = nullptr);
  // Sleeps `seconds`, timed the same way; messages wait in the queue.
  void sleep(double seconds);
  // Whether the waits are timed finely (Windows 10 1803 and later).
  bool precise_waits() const;
  // Pumps messages; waits up to max_wait_ms when idle (0: never waits).
  // Returns false once the window was asked to close. *pumped (when given)
  // says whether there was a message (other than WM_SYSTIMER, Windows'
  // internal timer): something may have changed.
  bool process_events(Rml::Context *context, int max_wait_ms,
                      bool *pumped = nullptr);
  void begin_frame();
  void present();

  void minimize();
  void toggle_maximize();
  bool maximized() const;
  void request_close();
  // Wakes process_events from another thread. Safe while the window closes
  // (it then does nothing).
  void wake();

  float dp_ratio() const;
  // Seconds between the desktop's refreshes (DWM composition), 1/60 when
  // Windows does not say.
  double refresh_interval() const;
  void *native_handle() const;
  // Client size in pixels.
  int width() const;
  int height() const;
  void resize_client(int width_px, int height_px);

  // Saves the next presented frame as a PNG (DESIGN.md 15.3: screens are
  // checked from captures).
  void capture_next_frame(std::filesystem::path png);
  bool capture_pending() const;
  std::string last_capture_error() const;
};

// Startup timing (ms since process start) for the log, from the window code.
void mark(const char *step);
double ms_since_process_start();
} // namespace xerces_launcher
