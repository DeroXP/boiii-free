#pragma once

// The launcher application: the window, RmlUi, the screens and the one
// action map for keyboard and controller (DESIGN.md 7.3, 7.4).

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <RmlUi/Core/Input.h>

#include "services.hpp"
#include "view.hpp"
#include "window.hpp"

namespace Rml {
class Context;
class Element;
class ElementDocument;
class Event;
class EventListener;
class SystemInterface;
} // namespace Rml

namespace xerces_launcher {
struct options {
  std::optional<fs::path> home;
  float scale = 0.f;        // 0: the config's UI scale, else Windows
  int width = 1280, height = 800; // dp
  double seconds = 0;       // quit after this long (checks)
  std::string start_view;   // show this screen after boot
  std::string script;       // scripted input for checks (app.cpp)
  std::string language;     // overrides the config ("pseudo" for 7.5)
  std::string theme;        // overrides the config
  bool dry_launch = false;  // never start the game, log the command
  bool skip_boot = false;
};

options parse_options(int argc, wchar_t **argv);

class app {
public:
  explicit app(options opts);
  ~app();
  int run();

  services &svc() { return *services_; }
  window &win() { return window_; }
  Rml::Context *context() { return context_; }
  const options &opts() const { return opts_; }

  // ------------------------------------------------------------- screens
  void show(const std::string &id);
  const std::string &current() const { return current_; }
  view *find(const std::string &id);
  void next_tab(int delta);

  // The game mode Home and Mods share (zm, mp, cp); saved in the config.
  const std::string &mode() const;
  void set_mode(const std::string &mode);
  std::uint64_t mode_rev() const { return mode_rev_; }

  // ------------------------------------------------------------ feedback
  // A line in the footer for a few seconds. It is logged too
  // ("launcher.notice"), as `logged` when that is given. Logs go into bug
  // reports, and the log's scrubber cannot spot every server address (a host
  // name without a port, one cut short for the footer) nor a favourite's name
  // (log.hpp, DESIGN.md 15.1): a notice that shows one passes its text
  // without it as `logged`.
  void notify(const std::string &text, bool error = false,
              std::string_view logged = {});
  // A modal dialog over the current screen. done gets the index of the
  // button picked, or -1 for Esc / B.
  void dialog(const std::string &title, const std::string &body,
              std::vector<std::string> buttons, std::function<void(int)> done);
  bool dialog_open() const { return dialog_open_; }

  // ------------------------------------------------------------- strings
  // English source text to the current language (data/xerces/i18n/<lang>.json
  // maps English to the translation; "pseudo" lengthens and marks every
  // string the template lists, to expose truncation, and leaves the rest as
  // it is, so text missing from the template shows as plain English).
  std::string tr(const std::string &english) const;
  const std::string &language() const { return language_; }

  // ---------------------------------------------------------------- look
  // Theme, UI scale and reduced motion from the config, on every document.
  void apply_look();

  // --------------------------------------------------------------- tools
  // Opens a link in the default browser or Steam. Only https:// links and
  // steam:// links are opened; anything else is refused and logged.
  bool open_url(const std::string &url);
  bool open_folder(const fs::path &dir);
  // Opens a text file that exists (LICENSE has no extension, so Notepad, not
  // the file's association); shows it in Explorer if Notepad cannot start.
  bool open_text_file(const fs::path &file);
  // Windows' folder picker over the launcher window; nullopt if cancelled.
  std::optional<fs::path> pick_folder(const std::string &title);
  // Windows' file pickers; filter like L"Xerces pack (*.xpack)|*.xpack".
  std::optional<fs::path> pick_open_file(const std::string &title,
                                         const std::wstring &filter);
  std::optional<fs::path> pick_save_file(const std::string &title,
                                         const std::wstring &filter,
                                         const std::wstring &default_name);
  void copy_text(const std::string &text);
  // Calls fn for event (e.g. "click") on element; the app owns the listener.
  void listen(Rml::Element *element, const std::string &event,
              std::function<void(Rml::Event &)> fn);
  // Feeds a key through the same path as the keyboard (pad, scripts).
  void inject_key(Rml::Input::KeyIdentifier key, int modifiers = 0);
  // Runs task on the UI thread (thread-safe).
  void post(std::function<void()> task);
  double now() const;
  // Escapes text for RML markup (SetInnerRML).
  static std::string escape(const std::string &text);

private:
  friend class translator;
  bool load_fonts();
  bool load_view(view &v, const view_info &info);
  void wire_shell(Rml::ElementDocument *doc);
  void run_posted();
  bool on_key(Rml::Input::KeyIdentifier key, int modifiers, bool priority);
  hit_region hit_test(int x, int y);
  void load_language();
  void close_dialog(int choice);
  void update_notice();
  void run_script(double seconds);
  // Focuses the screen's default element, scrolled into view; false when the
  // screen has none.
  bool focus_default(view &v);
  // Once per frame: focus that was lost, or left on something hidden, goes
  // back to the current screen's default.
  void keep_focus();
  // The buttons held on every connected controller (pad_bits in app.cpp).
  unsigned read_pad(double now);
  // Stops the worker, RmlUi, the screens and the window in the order they
  // depend on each other. Safe to call more than once.
  void shutdown();
  // Counts a presented frame against the window's state; every minute (and
  // at the end) logs launcher.activity.
  void count_present(double t, bool front);
  void log_activity(double t);

  options opts_;
  window window_;
  std::unique_ptr<services> services_;
  Rml::Context *context_ = nullptr;
  std::unique_ptr<Rml::SystemInterface> system_;
  bool rml_started_ = false;

  struct entry {
    view_info info;
    std::unique_ptr<view> screen;
    bool loaded = false; // its document was tried (once)
    bool failed = false; // and did not load: the screen is never shown
  };
  std::vector<entry> views_;
  std::string current_;
  std::uint64_t mode_rev_ = 1;

  std::mutex posted_mutex_;
  std::deque<std::function<void()>> posted_;

  std::string notice_text_;
  bool notice_error_ = false;
  double notice_until_ = 0;
  bool notice_dirty_ = false;

  Rml::ElementDocument *dialog_doc_ = nullptr;
  bool dialog_open_ = false;
  std::function<void(int)> dialog_done_;
  std::vector<std::unique_ptr<Rml::EventListener>> listeners_;

  std::string language_ = "en";
  // English -> translation; for "pseudo", the template's keys (values empty).
  std::map<std::string, std::string> strings_;

  double started_ = 0;
  float applied_scale_ = 0.f;
  std::string applied_look_;

  struct script_step {
    double at;
    std::string verb;
    std::string arg;
  };
  std::vector<script_step> script_;
  std::size_t script_next_ = 0;
  unsigned pad_previous_ = 0;
  // Held while the launcher was not in front: ignored until let go. All of
  // them at the start, so a button still down from whatever started the
  // launcher is not a press either.
  unsigned pad_ignored_ = ~0u;
  double pad_repeat_at_ = 0;
  unsigned script_pad_ = 0; // held by a check script (padhold)
  int script_front_ = -1;   // check script's front on (1) / off (0); -1 real
  unsigned pad_slots_ = 0;    // XInput slots that answered (bit per slot)
  double pad_rescan_at_ = 0;  // when the empty slots are asked again
  int focus_lost_frames_ = 0; // frames the focus has been lost or hidden
  void *instance_mutex_ = nullptr; // one launcher per home
  std::uintptr_t home_tag_ = 0;    // this home on the window (single instance)

  // Frames presented while the window was in front, behind another window,
  // minimized, or while a game ran (whatever the window was), and the time
  // spent in each, since the last launcher.activity line.
  enum activity_state {
    state_front,
    state_behind,
    state_minimized,
    state_game,
    state_count
  };
  std::array<std::uint64_t, state_count> frames_{};
  std::array<double, state_count> state_seconds_{};
  double activity_since_ = -1; // start of the counted span; -1 before any
  double last_frame_at_ = 0;
  double game_checked_at_ = -1;
  bool game_seen_ = false; // services::game_running, asked once a second
};
} // namespace xerces_launcher
