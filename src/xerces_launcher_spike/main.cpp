// xerces_launcher_spike: the C / Signal home screen drawn with RmlUi, built
// once per renderer (xerces_spike_dx11, xerces_spike_gl3), to measure the D4
// gate in docs/DESIGN.md 7.1: typography at 100/150/200 percent, transitions at
// 144 Hz, first frame under 150 ms, controller navigation.
//
//   xerces_spike_dx11 [--scale 1.5] [--size 1280x800] [--seconds 10]
//                     [--animate] [--script inputs.txt] [--log spike.log]
//                     [--data <repo>/data/xerces]
//
// --animate switches the game mode every 600 ms (200 ms transitions) and logs
// frame times; --script plays controller inputs ("+500 down", "+300 a", ...)
// through the same mapping as a real pad, since this PC's controller cannot be
// pressed by a script.

#include <RmlUi/Core.h>

#include <RmlUi_Backend.h>
#include <RmlUi_Include_Windows.h>

#include <shellapi.h>
#include <Xinput.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
// ---------------------------------------------------------------- logging --
std::ofstream log_file;

void log_line(const std::string &text) {
  if (log_file) {
    log_file << text << '\n';
    log_file.flush();
  }
  OutputDebugStringA((text + "\n").c_str());
}

// RmlUi's Win32 system interface shows message boxes for errors; the spike
// logs instead so measured runs never block.
class spike_system final : public Rml::SystemInterface {
public:
  explicit spike_system(Rml::SystemInterface *inner) : inner_(inner) {}
  double GetElapsedTime() override { return inner_->GetElapsedTime(); }
  bool LogMessage(Rml::Log::Type type, const Rml::String &message) override {
    const char *level = type == Rml::Log::LT_ERROR     ? "error"
                        : type == Rml::Log::LT_ASSERT  ? "assert"
                        : type == Rml::Log::LT_WARNING ? "warning"
                                                       : "info";
    log_line(std::string("[rmlui ") + level + "] " + message);
    return true;
  }
  void SetMouseCursor(const Rml::String &name) override {
    inner_->SetMouseCursor(name);
  }
  void SetClipboardText(const Rml::String &text) override {
    inner_->SetClipboardText(text);
  }
  void GetClipboardText(Rml::String &text) override {
    inner_->GetClipboardText(text);
  }

private:
  Rml::SystemInterface *inner_;
};

// ---------------------------------------------------------------- options --
struct options {
  float scale = 0.f; // 0: the monitor's own scale
  int width = 1280;
  int height = 800;
  double seconds = 0.0; // 0: until the window is closed
  bool animate = false;
  bool async = false; // overlapped GPU start (deferred_render.hpp)
  std::string script;
  std::string log = "xerces_spike.log";
  std::string data;
};

options parse_options(int argc, wchar_t **argv) {
  options out;
  auto narrow = [](const wchar_t *text) {
    std::string result;
    for (const wchar_t *c = text; *c; ++c) {
      result.push_back(static_cast<char>(*c));
    }
    return result;
  };
  for (int i = 1; i < argc; ++i) {
    const std::string arg = narrow(argv[i]);
    const auto next = [&]() { return i + 1 < argc ? narrow(argv[++i]) : ""; };
    if (arg == "--scale") {
      out.scale = std::strtof(next().c_str(), nullptr);
    } else if (arg == "--size") {
      const std::string size = next();
      std::sscanf(size.c_str(), "%dx%d", &out.width, &out.height);
    } else if (arg == "--seconds") {
      out.seconds = std::strtod(next().c_str(), nullptr);
    } else if (arg == "--animate") {
      out.animate = true;
    } else if (arg == "--async") {
      out.async = true;
    } else if (arg == "--script") {
      out.script = next();
    } else if (arg == "--log") {
      out.log = next();
    } else if (arg == "--data") {
      out.data = next();
    }
  }
  return out;
}

// data/xerces next to the repository the exe was built in, unless --data.
fs::path find_data(const options &opts) {
  if (!opts.data.empty()) {
    return fs::path(opts.data);
  }
  wchar_t module[MAX_PATH]{};
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  fs::path dir = fs::path(module).parent_path();
  for (int i = 0; i < 8 && !dir.empty(); ++i, dir = dir.parent_path()) {
    if (fs::exists(dir / "data" / "xerces" / "ui" / "home.rml")) {
      return dir / "data" / "xerces";
    }
  }
  return fs::path("data") / "xerces";
}

// ------------------------------------------------------------------ timing --
double ms_since_process_start() {
  // GetSystemTimePreciseAsFileTime (Windows 8+) is looked up at run time: the
  // RmlUi Windows header targets an older _WIN32_WINNT.
  using precise_time = void(WINAPI *)(LPFILETIME);
  static const auto get_precise_time =
      reinterpret_cast<precise_time>(reinterpret_cast<void *>(
          GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                         "GetSystemTimePreciseAsFileTime")));
  FILETIME creation{}, exit{}, kernel{}, user{};
  GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
  FILETIME now{};
  if (get_precise_time) {
    get_precise_time(&now);
  } else {
    GetSystemTimeAsFileTime(&now);
  }
  const auto ticks = [](const FILETIME &t) {
    return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) |
           t.dwLowDateTime;
  };
  return static_cast<double>(ticks(now) - ticks(creation)) / 10000.0;
}

void mark(const char *step) {
  char text[120];
  std::snprintf(text, sizeof(text), "startup: %-22s %7.1f ms", step,
                ms_since_process_start());
  log_line(text);
}

HWND own_window() {
  HWND found = nullptr;
  EnumThreadWindows(
      GetCurrentThreadId(),
      [](HWND window, LPARAM out) -> BOOL {
        if (IsWindowVisible(window)) {
          *reinterpret_cast<HWND *>(out) = window;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&found));
  return found;
}

// Sizes the window so its client area is width x height physical pixels.
void set_client_size(HWND window, int width, int height) {
  RECT outer{}, inner{};
  GetWindowRect(window, &outer);
  GetClientRect(window, &inner);
  const int frame_w = (outer.right - outer.left) - (inner.right - inner.left);
  const int frame_h = (outer.bottom - outer.top) - (inner.bottom - inner.top);
  SetWindowPos(window, nullptr, 0, 0, width + frame_w, height + frame_h,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int refresh_rate() {
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  return EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)
             ? static_cast<int>(mode.dmDisplayFrequency)
             : 0;
}

std::string frame_report(std::vector<double> frames, int hz) {
  if (frames.size() < 2) {
    return "frames: too few to measure";
  }
  std::sort(frames.begin(), frames.end());
  const auto at = [&](double q) {
    return frames[std::min(frames.size() - 1,
                           static_cast<std::size_t>(q * frames.size()))];
  };
  double sum = 0;
  for (const double f : frames) {
    sum += f;
  }
  const double median = at(0.5);
  const auto late = std::count_if(frames.begin(), frames.end(),
                                  [&](double f) { return f > median * 1.5; });
  const double budget_144 = 1000.0 / 144.0;
  const auto over_144 =
      std::count_if(frames.begin(), frames.end(),
                    [&](double f) { return f > budget_144 * 1.5; });
  char text[400];
  std::snprintf(text, sizeof(text),
                "frames: %zu, monitor %d Hz, mean %.3f ms, median %.3f, p95 "
                "%.3f, p99 %.3f, max %.3f; over 1.5x median: %lld; over 1.5x "
                "the 144 Hz budget (%.2f ms): %lld",
                frames.size(), hz, sum / frames.size(), median, at(0.95),
                at(0.99), frames.back(), static_cast<long long>(late),
                budget_144 * 1.5, static_cast<long long>(over_144));
  return text;
}

// -------------------------------------------------------------- home model --
struct load_row {
  Rml::String n, name, v;
};
struct stat_cell {
  Rml::String n, l;
};
struct home_state {
  Rml::String mode = "zm";
  Rml::String pack, meta, sub;
  Rml::Vector<load_row> rows;
  Rml::Vector<stat_cell> stats;
  bool has_more = false;
  bool empty = false;
  bool can_launch = true;
  Rml::String last_action = "none";
};

home_state home;
Rml::DataModelHandle home_handle;

void set_mode(const Rml::String &mode) {
  const Rml::Vector<load_row> first = {{"01", "Perk Rebalance", "2.1.0"},
                                       {"02", "Better HUD", "1.4.2"},
                                       {"03", "Extended Box Pool", "0.9.0"},
                                       {"04", "Round Timer", "1.0.3"}};
  home.mode = mode;
  if (mode == "mp") {
    home.pack = "Classic Lobby";
    home.meta = "Pinned versions";
    home.sub = "Multi-mod \xC2\xB7 4 mods";
    home.rows = first;
    home.has_more = false;
    home.empty = false;
    home.stats = {{"04", "mods"},
                  {"00", "conflicts"},
                  {"00", "merged"},
                  {"00", "overrides"}};
  } else if (mode == "cp") {
    home.pack = "No pack (vanilla)";
    home.meta = "Mods are off for Campaign";
    home.sub = "Pick a pack first";
    home.rows.clear();
    home.has_more = false;
    home.empty = true;
    home.stats = {{"00", "mods"}};
  } else {
    home.mode = "zm";
    home.pack = "Weekend Zombies";
    home.meta = "Pinned versions";
    home.sub = "Multi-mod \xC2\xB7 7 mods";
    home.rows = first;
    home.has_more = true;
    home.empty = false;
    home.stats = {{"07", "mods"},
                  {"00", "conflicts"},
                  {"02", "merged"},
                  {"01", "override"}};
  }
  home.can_launch = home.mode != "cp";
  if (home_handle) {
    home_handle.DirtyAllVariables();
  }
}

void cycle_mode(int delta) {
  static const Rml::String order[] = {"zm", "mp", "cp"};
  int index = 0;
  for (int i = 0; i < 3; ++i) {
    if (order[i] == home.mode) {
      index = i;
    }
  }
  set_mode(order[(index + 3 + delta) % 3]);
}

bool bind_home(Rml::Context *context) {
  Rml::DataModelConstructor model = context->CreateDataModel("home");
  if (!model) {
    return false;
  }
  if (auto row = model.RegisterStruct<load_row>()) {
    row.RegisterMember("n", &load_row::n);
    row.RegisterMember("name", &load_row::name);
    row.RegisterMember("v", &load_row::v);
  }
  if (auto cell = model.RegisterStruct<stat_cell>()) {
    cell.RegisterMember("n", &stat_cell::n);
    cell.RegisterMember("l", &stat_cell::l);
  }
  model.RegisterArray<Rml::Vector<load_row>>();
  model.RegisterArray<Rml::Vector<stat_cell>>();
  model.Bind("mode", &home.mode);
  model.Bind("pack", &home.pack);
  model.Bind("meta", &home.meta);
  model.Bind("sub", &home.sub);
  model.Bind("rows", &home.rows);
  model.Bind("stats", &home.stats);
  model.Bind("has_more", &home.has_more);
  model.Bind("empty", &home.empty);
  model.Bind("can_launch", &home.can_launch);
  model.Bind("last_action", &home.last_action);
  model.BindEventCallback("pick", [](Rml::DataModelHandle, Rml::Event &,
                                     const Rml::VariantList &args) {
    if (!args.empty()) {
      set_mode(args[0].Get<Rml::String>());
      log_line("action: mode " + home.mode);
    }
  });
  model.BindEventCallback("act", [](Rml::DataModelHandle handle, Rml::Event &,
                                    const Rml::VariantList &args) {
    if (!args.empty()) {
      home.last_action = args[0].Get<Rml::String>();
      handle.DirtyVariable("last_action");
      log_line("action: " + home.last_action);
    }
  });
  home_handle = model.GetModelHandle();
  set_mode("zm");
  return true;
}

// -------------------------------------------------------------- controller --
// One action map for keyboard and controller (DESIGN.md 7.4): the pad becomes
// the same keys, so RmlUi's spatial navigation (nav: auto) and Enter-to-click
// do the rest. LB/RB switch the game mode here, as Q/E do.
enum pad_button : unsigned {
  pad_up = 1,
  pad_down = 2,
  pad_left = 4,
  pad_right = 8,
  pad_a = 16,
  pad_b = 32,
  pad_lb = 64,
  pad_rb = 128,
};

unsigned read_xinput() {
  for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
    XINPUT_STATE state{};
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
      continue;
    }
    const XINPUT_GAMEPAD &pad = state.Gamepad;
    unsigned out = 0;
    const SHORT dead = 16000;
    if ((pad.wButtons & XINPUT_GAMEPAD_DPAD_UP) || pad.sThumbLY > dead)
      out |= pad_up;
    if ((pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || pad.sThumbLY < -dead)
      out |= pad_down;
    if ((pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || pad.sThumbLX < -dead)
      out |= pad_left;
    if ((pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || pad.sThumbLX > dead)
      out |= pad_right;
    if (pad.wButtons & XINPUT_GAMEPAD_A)
      out |= pad_a;
    if (pad.wButtons & XINPUT_GAMEPAD_B)
      out |= pad_b;
    if (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
      out |= pad_lb;
    if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
      out |= pad_rb;
    return out;
  }
  return 0;
}

class pad_mapper {
public:
  // Feeds one frame of pad state; held directions repeat after 300 ms, every
  // 120.
  void update(Rml::Context *context, unsigned buttons, double now) {
    const unsigned pressed = buttons & ~previous_;
    for (unsigned bit = 1; bit <= pad_rb; bit <<= 1) {
      if (pressed & bit) {
        fire(context, bit);
        if (bit <= pad_right) {
          repeat_at_ = now + 0.3;
        }
      }
    }
    const unsigned held_direction =
        buttons & (pad_up | pad_down | pad_left | pad_right);
    if (held_direction && !(pressed & held_direction) && now >= repeat_at_) {
      for (unsigned bit = 1; bit <= pad_right; bit <<= 1) {
        if (held_direction & bit) {
          fire(context, bit);
        }
      }
      repeat_at_ = now + 0.12;
    }
    previous_ = buttons;
  }

private:
  static void fire(Rml::Context *context, unsigned bit) {
    switch (bit) {
    case pad_up:
      key(context, Rml::Input::KI_UP);
      break;
    case pad_down:
      key(context, Rml::Input::KI_DOWN);
      break;
    case pad_left:
      key(context, Rml::Input::KI_LEFT);
      break;
    case pad_right:
      key(context, Rml::Input::KI_RIGHT);
      break;
    case pad_a:
      key(context, Rml::Input::KI_RETURN);
      break;
    case pad_b:
      key(context, Rml::Input::KI_ESCAPE);
      break;
    case pad_lb:
      cycle_mode(-1);
      break;
    case pad_rb:
      cycle_mode(+1);
      break;
    default:
      break;
    }
  }
  static void key(Rml::Context *context, Rml::Input::KeyIdentifier id) {
    context->ProcessKeyDown(id, 0);
    context->ProcessKeyUp(id, 0);
  }
  unsigned previous_ = 0;
  double repeat_at_ = 0;
};

struct script_step {
  double at = 0; // seconds since the window opened
  unsigned buttons = 0;
  std::string name;
};

std::vector<script_step> load_script(const std::string &path) {
  std::vector<script_step> steps;
  std::ifstream in(path);
  double at = 1.0; // let the first frame settle
  for (std::string line; std::getline(in, line);) {
    std::istringstream words(line);
    std::string delay, name;
    if (!(words >> delay >> name) || delay.empty() || delay[0] != '+') {
      continue;
    }
    at += std::strtod(delay.c_str() + 1, nullptr) / 1000.0;
    unsigned buttons = name == "up"      ? pad_up
                       : name == "down"  ? pad_down
                       : name == "left"  ? pad_left
                       : name == "right" ? pad_right
                       : name == "a"     ? pad_a
                       : name == "b"     ? pad_b
                       : name == "lb"    ? pad_lb
                       : name == "rb"    ? pad_rb
                                         : 0u;
    steps.push_back({at, buttons, name});
    steps.push_back({at + 0.05, 0u, "release"});
  }
  return steps;
}

std::string describe_focus(Rml::Context *context) {
  Rml::Element *focus = context->GetFocusElement();
  if (!focus) {
    return "(none)";
  }
  const std::string id = focus->GetId();
  return focus->GetTagName() + (id.empty() ? "" : "#" + id);
}

bool key_down_callback(Rml::Context *context, Rml::Input::KeyIdentifier key,
                       int, float, bool priority) {
  if (!priority) {
    return true;
  }
  if (key == Rml::Input::KI_Q) {
    cycle_mode(-1);
    return false;
  }
  if (key == Rml::Input::KI_E) {
    cycle_mode(+1);
    return false;
  }
  if (key == Rml::Input::KI_ESCAPE && context->GetFocusElement() == nullptr) {
    Backend::RequestExit();
    return false;
  }
  return true;
}
} // namespace

// Called from the backends (dx11/, gl3/) to split the first-frame time.
void xerces_spike_mark(const char *step) { mark(step); }

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const double started_ms = ms_since_process_start();
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  const options opts = parse_options(argc, argv);
  LocalFree(argv);
  log_file.open(opts.log, std::ios::trunc);

#if defined(XERCES_SPIKE_GL3)
  const char *renderer = "gl3";
#else
  const char *renderer = "dx11";
#endif
  log_line(std::string("xerces_launcher_spike renderer=") + renderer +
           " rmlui=" + Rml::GetVersion());
  log_line("main entered " + std::to_string(started_ms) +
           " ms after process start");

  // The backend opens the window at width x height dp of the monitor's scale;
  // --scale then resizes it to exactly width x height x scale pixels.
  const int width = opts.width;
  const int height = opts.height;
  SetEnvironmentVariableW(L"XERCES_SPIKE_ASYNC", opts.async ? L"1" : L"0");
  log_line(opts.async ? "start: overlapped (GPU on a worker thread)"
                      : "start: sequential");
  if (!Backend::Initialize("Xerces (RmlUi spike)", width, height, true)) {
    log_line("backend failed to initialize");
    return 1;
  }
  mark("backend ready");
  HWND window = own_window();
  if (window && opts.scale > 0.f) {
    set_client_size(window, static_cast<int>(width * opts.scale + 0.5f),
                    static_cast<int>(height * opts.scale + 0.5f));
  }
  spike_system system(Backend::GetSystemInterface());
  Rml::SetSystemInterface(&system);
  Rml::SetRenderInterface(Backend::GetRenderInterface());
  Rml::Initialise();

  Rml::Context *context =
      Rml::CreateContext("main", Rml::Vector2i(width, height));
  if (!context || !bind_home(context)) {
    log_line("could not create the context or the data model");
    Rml::Shutdown();
    Backend::Shutdown();
    return 1;
  }
  mark("rmlui ready");

  const fs::path data = find_data(opts);
  for (const char *font :
       {"fonts/InterTight-wght.ttf", "fonts/IBMPlexMono-Regular.ttf",
        "fonts/IBMPlexMono-Medium.ttf"}) {
    if (!Rml::LoadFontFace((data / font).string())) {
      log_line(std::string("font failed: ") + font);
    }
  }
  mark("fonts loaded");
  Rml::ElementDocument *document =
      context->LoadDocument((data / "ui" / "home.rml").string());
  mark("document loaded");
  if (!document) {
    log_line("home.rml failed to load from " + (data / "ui").string());
    Rml::Shutdown();
    Backend::Shutdown();
    return 1;
  }
  document->Show();
  if (Rml::Element *launch = document->GetElementById("launch-modded")) {
    launch->Focus(true);
  }

  const int hz = refresh_rate();
  const std::vector<script_step> script = opts.script.empty()
                                              ? std::vector<script_step>{}
                                              : load_script(opts.script);
  std::size_t script_next = 0;
  pad_mapper pad;
  std::vector<double> frame_ms;
  frame_ms.reserve(200000);
  bool first_frame = true;
  bool running = true;
  double next_animation = 1.0;
  std::string last_focus;
  LARGE_INTEGER frequency{}, previous{}, opened{};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&opened);
  previous = opened;

  while (running) {
    running = Backend::ProcessEvents(context, &key_down_callback, false);
    if (opts.scale > 0.f &&
        context->GetDensityIndependentPixelRatio() != opts.scale) {
      context->SetDensityIndependentPixelRatio(opts.scale);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double seconds = static_cast<double>(now.QuadPart - opened.QuadPart) /
                           frequency.QuadPart;

    unsigned buttons = read_xinput();
    while (script_next < script.size() && script[script_next].at <= seconds) {
      if (script[script_next].name != "release") {
        log_line("script: " + script[script_next].name);
      }
      pad.update(context, script[script_next].buttons, seconds);
      ++script_next;
    }
    if (script.empty()) {
      pad.update(context, buttons, seconds);
    }
    const std::string focus = describe_focus(context);
    if (focus != last_focus) {
      log_line("focus: " + focus);
      last_focus = focus;
    }
    if (opts.animate && seconds >= next_animation) {
      cycle_mode(+1);
      next_animation = seconds + 0.6;
    }

    context->Update();
    if (first_frame) {
      mark("first update");
    }
    Backend::BeginFrame();
    context->Render();
    if (first_frame) {
      mark("first render recorded");
    }
    Backend::PresentFrame();

    QueryPerformanceCounter(&now);
    if (first_frame) {
      first_frame = false;
      RECT client{};
      if (window) {
        GetClientRect(window, &client);
      }
      char text[260];
      std::snprintf(text, sizeof(text),
                    "first frame presented %.1f ms after process start "
                    "(client %ldx%ld px, dp ratio %.2f, foreground %s)",
                    ms_since_process_start(), client.right, client.bottom,
                    context->GetDensityIndependentPixelRatio(),
                    GetForegroundWindow() == window ? "yes" : "no");
      log_line(text);
    } else if (seconds > 1.0) {
      frame_ms.push_back(static_cast<double>(now.QuadPart - previous.QuadPart) *
                         1000.0 / frequency.QuadPart);
    }
    previous = now;
    if (opts.seconds > 0 && seconds >= opts.seconds) {
      running = false;
    }
  }

  log_line(frame_report(frame_ms, hz));
  log_line("last action: " + home.last_action + ", mode: " + home.mode);
  Rml::Shutdown();
  Backend::Shutdown();
  return 0;
}
