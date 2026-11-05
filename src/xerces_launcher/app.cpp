#include "app.hpp"

#include <RmlUi/Core.h>

#include <RmlUi_Include_Windows.h>

#include <shellapi.h>
#include <Xinput.h>
// wingdi.h's GetObject macro would rename rapidjson's GetObject().
#undef GetObject

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <sstream>

#include <xerces_core/json.hpp>

#include "log.hpp"

namespace xerces_launcher {
namespace {
std::string narrow(const wchar_t *text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                       nullptr, nullptr);
  std::string out(size > 0 ? std::size_t(size - 1) : 0, '\0');
  if (size > 1) {
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr,
                        nullptr);
  }
  return out;
}

std::wstring widen(const std::string &text) {
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring out(size > 0 ? std::size_t(size - 1) : 0, L'\0');
  if (size > 1) {
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
  }
  return out;
}

std::string generic(const fs::path &path) {
  const auto text = path.generic_u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

class function_listener final : public Rml::EventListener {
public:
  explicit function_listener(std::function<void(Rml::Event &)> fn)
      : fn_(std::move(fn)) {}
  void ProcessEvent(Rml::Event &event) override { fn_(event); }

private:
  std::function<void(Rml::Event &)> fn_;
};

bool is_text_field(const Rml::Element *element) {
  if (!element) {
    return false;
  }
  const Rml::String tag = element->GetTagName();
  if (tag == "textarea") {
    return true;
  }
  if (tag != "input") {
    return false;
  }
  const Rml::String type = element->GetAttribute<Rml::String>("type", "text");
  return type == "text" || type == "password" || type == "search";
}

bool inside(const Rml::Element *element, const char *id) {
  for (; element; element = element->GetParentNode()) {
    if (element->GetId() == id) {
      return true;
    }
  }
  return false;
}

// Pseudo-locale (7.5): accented, 30 % longer and bracketed, so truncation and
// untranslated strings show.
std::string pseudo(const std::string &text) {
  // Left alone: data bindings, the indentation between elements (RmlUi
  // passes every text node, whitespace too), and text already made pseudo
  // (RmlUi translates bound text that C++ already passed through tr()).
  const bool blank = text.find_first_not_of(" \t\r\n") == std::string::npos;
  // Every pseudo segment ends in "~]", so text holding one was built from
  // strings already translated (a C++ line made of tr() parts).
  const bool done = text.find("~]") != std::string::npos;
  if (blank || done || text.find("{{") != std::string::npos) {
    return text;
  }
  std::string out = "[";
  std::size_t letters = 0;
  for (const char c : text) {
    switch (c) {
    case 'a': out += "\xC3\xA1"; break;
    case 'e': out += "\xC3\xA9"; break;
    case 'i': out += "\xC3\xAD"; break;
    case 'o': out += "\xC3\xB6"; break;
    case 'u': out += "\xC3\xBC"; break;
    case 'A': out += "\xC3\x85"; break;
    case 'E': out += "\xC3\x89"; break;
    case 'O': out += "\xC3\x96"; break;
    default: out.push_back(c); break;
    }
    if (c != ' ') {
      ++letters;
    }
  }
  out.append((letters * 3 + 9) / 10, '~');
  out += "]";
  return out;
}

enum pad_bits : unsigned {
  pad_up = 1,
  pad_down = 2,
  pad_left = 4,
  pad_right = 8,
  pad_a = 16,
  pad_b = 32,
  pad_y = 64,
  pad_lb = 128,
  pad_rb = 256,
};

constexpr unsigned pad_directions = pad_up | pad_down | pad_left | pad_right;

// The pad_bits in bit order, as check scripts name them (padhold right).
constexpr const char *pad_names[] = {"up", "down", "left", "right", "a",
                                     "b",  "y",    "lb",   "rb"};

unsigned pad_bit_by_name(const std::string &name) {
  for (unsigned i = 0; i < std::size(pad_names); ++i) {
    if (name == pad_names[i]) {
      return 1u << i;
    }
  }
  return 0;
}

const char *pad_name(unsigned bit) {
  for (unsigned i = 0; i < std::size(pad_names); ++i) {
    if (bit == 1u << i) {
      return pad_names[i];
    }
  }
  return "?";
}

unsigned pad_buttons(const XINPUT_GAMEPAD &pad) {
  const SHORT dead = 16000;
  unsigned out = 0;
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
  if (pad.wButtons & XINPUT_GAMEPAD_Y)
    out |= pad_y;
  if (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
    out |= pad_lb;
  if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
    out |= pad_rb;
  return out;
}

// The window property that names the home a launcher window belongs to: the
// single-instance mutex is per home, so the window brought forward must be
// that home's too, not whichever launcher window comes first.
constexpr wchar_t home_property[] = L"xerces_launcher_home";

struct home_window_search {
  std::uintptr_t tag = 0;
  HWND found = nullptr;
};

BOOL CALLBACK match_home_window(HWND handle, LPARAM param) {
  auto *search = reinterpret_cast<home_window_search *>(param);
  wchar_t name[64] = {};
  if (GetClassNameW(handle, name, 64) > 0 &&
      std::wcscmp(name, window_class_name) == 0 &&
      reinterpret_cast<std::uintptr_t>(GetPropW(handle, home_property)) ==
          search->tag) {
    search->found = handle;
    return FALSE;
  }
  return TRUE;
}

HWND find_home_window(std::uintptr_t tag) {
  home_window_search search;
  search.tag = tag;
  EnumWindows(&match_home_window, reinterpret_cast<LPARAM>(&search));
  return search.found;
}

Rml::Input::KeyIdentifier key_by_name(const std::string &name) {
  static const std::map<std::string, Rml::Input::KeyIdentifier> keys = {
      {"up", Rml::Input::KI_UP},         {"down", Rml::Input::KI_DOWN},
      {"left", Rml::Input::KI_LEFT},     {"right", Rml::Input::KI_RIGHT},
      {"enter", Rml::Input::KI_RETURN},  {"esc", Rml::Input::KI_ESCAPE},
      {"tab", Rml::Input::KI_TAB},       {"q", Rml::Input::KI_Q},
      {"e", Rml::Input::KI_E},           {"slash", Rml::Input::KI_OEM_2},
      {"space", Rml::Input::KI_SPACE},   {"delete", Rml::Input::KI_DELETE},
      {"home", Rml::Input::KI_HOME},     {"end", Rml::Input::KI_END},
      {"f5", Rml::Input::KI_F5},         {"z", Rml::Input::KI_Z},
  };
  const auto it = keys.find(name);
  return it == keys.end() ? Rml::Input::KI_UNKNOWN : it->second;
}
} // namespace

// RmlUi's system interface: the window's, with logging into the launcher log
// and every text node through the translation table.
class translator final : public Rml::SystemInterface {
public:
  translator(app &owner, Rml::SystemInterface *inner)
      : owner_(owner), inner_(inner) {}
  double GetElapsedTime() override { return inner_->GetElapsedTime(); }
  int TranslateString(Rml::String &translated,
                      const Rml::String &input) override {
    translated = owner_.tr(input);
    return translated == input ? 0 : 1;
  }
  bool LogMessage(Rml::Log::Type type, const Rml::String &message) override {
    if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT) {
      log_error("launcher.rmlui.error", message);
    } else if (type == Rml::Log::LT_WARNING) {
      log_warn("launcher.rmlui.warning", message);
    }
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
  app &owner_;
  Rml::SystemInterface *inner_;
};

std::vector<view_info> &view_registry() {
  static std::vector<view_info> registry;
  return registry;
}

Rml::Element *view::default_focus() {
  if (!doc) {
    return nullptr;
  }
  Rml::ElementList found;
  doc->GetElementsByClassName(found, "default-focus");
  for (Rml::Element *element : found) {
    if (element->IsVisible(true)) {
      return element;
    }
  }
  if (Rml::Element *content = doc->GetElementById("content")) {
    Rml::ElementList buttons;
    content->GetElementsByTagName(buttons, "button");
    for (Rml::Element *element : buttons) {
      if (element->IsVisible(true)) {
        return element;
      }
    }
  }
  return nullptr;
}

Rml::Element *view::search_field() {
  return doc ? doc->GetElementById("search") : nullptr;
}

options parse_options(int argc, wchar_t **argv) {
  options out;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = narrow(argv[i]);
    const auto next = [&]() { return i + 1 < argc ? narrow(argv[++i]) : ""; };
    if (arg == "--home" || arg == "-xerces-home") {
      out.home = fs::path(widen(next()));
    } else if (arg == "--scale") {
      out.scale = std::strtof(next().c_str(), nullptr);
    } else if (arg == "--size") {
      const std::string size = next();
      std::sscanf(size.c_str(), "%dx%d", &out.width, &out.height);
    } else if (arg == "--seconds") {
      out.seconds = std::strtod(next().c_str(), nullptr);
    } else if (arg == "--view") {
      out.start_view = next();
    } else if (arg == "--script") {
      out.script = next();
    } else if (arg == "--language") {
      out.language = next();
    } else if (arg == "--theme") {
      out.theme = next();
    } else if (arg == "--dry-launch") {
      out.dry_launch = true;
    } else if (arg == "--skip-boot") {
      out.skip_boot = true;
    }
  }
  // Shared with xerces_cli plan apply, so the variable means the same there.
  if (xerces_core::dry_launch_from_environment()) {
    out.dry_launch = true;
  }
  return out;
}

app::app(options opts) : opts_(std::move(opts)) {}

app::~app() {
  // The members go in reverse order, the posted queue before the services:
  // an early return from run() would leave the scan worker posting into a
  // queue that no longer exists. shutdown() joins it first.
  shutdown();
  if (instance_mutex_) {
    ReleaseMutex(instance_mutex_);
    CloseHandle(instance_mutex_);
  }
}

void app::shutdown() {
  if (services_) {
    services_->cancel_scan();
  }
  // The window goes out of sight at once, though it is closed only after the
  // worker below has stopped. Detection cannot be cancelled (it hashes the
  // whole BlackOps3.exe) and the catalog scan stops only between mod
  // folders, so the join can take seconds, and nothing pumps messages
  // meanwhile: a window still on screen would freeze and show as Not
  // Responding. The home tag goes with it, so a second start meanwhile does
  // not bring this closing window forward; it waits for the mutex instead
  // (run()).
  if (HWND handle = static_cast<HWND>(window_.native_handle())) {
    ShowWindow(handle, SW_HIDE);
    RemovePropW(handle, home_property);
  }
  // RmlUi first, with the services still there: closing the documents moves
  // focus, so blur handlers in the screens run (and may save settings).
  if (rml_started_) {
    // A text field the player typed into and never left saves on blur;
    // RmlUi's shutdown does not send one, so blur it here first.
    if (context_) {
      if (Rml::Element *focus = context_->GetFocusElement()) {
        focus->Blur();
      }
    }
    rml_started_ = false;
    Rml::Shutdown(); // documents and data models go first
  }
  context_ = nullptr;
  dialog_doc_ = nullptr;
  for (auto &e : views_) {
    e.screen.reset();
  }
  listeners_.clear();
  // Then the scan worker is joined, while the queue it posts to and the
  // window it wakes both still exist.
  services_.reset();
  window_.close();
}

double app::now() const { return ms_since_process_start() / 1000.0 - started_; }

unsigned app::read_pad(double now) {
  // Asking an empty XInput slot is slow (the driver is asked every time), so
  // slots without a controller are asked again only every 1.5 s; the slots
  // that answered are read every time. A controller is connected when its
  // slot answers, whether or not anything on it is pressed.
  const bool rescan = now >= pad_rescan_at_;
  if (rescan) {
    pad_rescan_at_ = now + 1.5;
  }
  unsigned out = 0;
  for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
    const unsigned slot = 1u << index;
    if (!(pad_slots_ & slot) && !rescan) {
      continue;
    }
    XINPUT_STATE state{};
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
      if (pad_slots_ & slot) {
        log_info("launcher.pad.disconnected", std::to_string(index));
      }
      pad_slots_ &= ~slot;
      continue;
    }
    if (!(pad_slots_ & slot)) {
      log_info("launcher.pad.connected", std::to_string(index));
    }
    pad_slots_ |= slot;
    out |= pad_buttons(state.Gamepad);
  }
  // A check script's padhold goes through the same handling as a real
  // controller (repeat, ignored while not in front); "pad" steps do not.
  return out | script_pad_;
}

void app::post(std::function<void()> task) {
  {
    std::lock_guard lock(posted_mutex_);
    posted_.push_back(std::move(task));
  }
  window_.wake();
}

void app::run_posted() {
  std::deque<std::function<void()>> tasks;
  {
    std::lock_guard lock(posted_mutex_);
    tasks.swap(posted_);
  }
  for (auto &task : tasks) {
    task();
  }
}

std::string app::escape(const std::string &text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    case '"': out += "&quot;"; break;
    // RmlUi reads "{{" in text as a data binding even after entities are
    // decoded, so a mod or server name holding it would become an
    // expression. A space between the braces keeps it text.
    case '{':
    case '}':
      if (!out.empty() && out.back() == c) {
        out.push_back(' ');
      }
      out.push_back(c);
      break;
    default: out.push_back(c); break;
    }
  }
  return out;
}

namespace {
// RmlUi hands a document's text nodes to the translation table as written,
// line breaks and indentation included, while the template keys them with
// every run of whitespace collapsed to one space (extract_strings.py).
std::string collapse_spaces(const std::string &text) {
  std::string out;
  bool space = false;
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      space = !out.empty();
      continue;
    }
    if (space) {
      out.push_back(' ');
      space = false;
    }
    out.push_back(c);
  }
  return out;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
} // namespace

std::string app::tr(const std::string &english) const {
  // C++ strings match exactly (a literal may hold a line break on purpose);
  // document text matches once its whitespace is collapsed. Leading and
  // trailing space survive as one space, since they separate the text from
  // the inline elements around it.
  auto it = strings_.find(english);
  bool collapsed = false;
  if (it == strings_.end()) {
    it = strings_.find(collapse_spaces(english));
    collapsed = true;
  }
  if (it == strings_.end()) {
    return english;
  }
  std::string out = language_ == "pseudo" ? pseudo(collapsed ? it->first : english)
                                          : it->second;
  if (collapsed && !english.empty()) {
    if (is_space(english.front())) {
      out.insert(out.begin(), ' ');
    }
    if (is_space(english.back())) {
      out.push_back(' ');
    }
  }
  return out;
}

void app::load_language() {
  language_ = !opts_.language.empty() ? opts_.language
                                      : services_->config().language;
  strings_.clear();
  if (language_ == "en") {
    return;
  }
  // The pseudo-locale marks exactly what a translator can change: the
  // template's strings.
  const std::string file =
      language_ == "pseudo" ? std::string("template") : language_;
  std::ifstream in(services_->paths().data / "i18n" / (file + ".json"),
                   std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  const auto parsed = xerces_core::json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject()) {
    log_warn("launcher.i18n.missing", "No strings for language " + language_);
    language_ = "en";
    return;
  }
  const bool keys_only = language_ == "pseudo";
  for (const auto &member : parsed.document.GetObject()) {
    // A translator works from a copy of the template: a string still empty
    // there is not translated yet and stays English.
    if (member.value.IsString() &&
        (keys_only || member.value.GetStringLength() > 0)) {
      strings_[std::string(member.name.GetString(), member.name.GetStringLength())] =
          std::string(member.value.GetString(), member.value.GetStringLength());
    }
  }
}

const std::string &app::mode() const { return services_->config().mode; }

void app::set_mode(const std::string &mode) {
  if (mode != "zm" && mode != "mp" && mode != "cp") {
    return;
  }
  if (services_->config().mode != mode) {
    services_->config().mode = mode;
    services_->save_config();
    ++mode_rev_;
  }
}

view *app::find(const std::string &id) {
  for (auto &e : views_) {
    if (id == e.info.id) {
      return e.screen.get();
    }
  }
  return nullptr;
}

void app::listen(Rml::Element *element, const std::string &event,
                 std::function<void(Rml::Event &)> fn) {
  if (!element) {
    return;
  }
  auto listener = std::make_unique<function_listener>(std::move(fn));
  element->AddEventListener(event, listener.get());
  listeners_.push_back(std::move(listener));
}

bool app::load_fonts() {
  bool ok = true;
  for (const char *font :
       {"fonts/InterTight-wght.ttf", "fonts/IBMPlexMono-Regular.ttf",
        "fonts/IBMPlexMono-Medium.ttf"}) {
    const fs::path path = services_->paths().data / font;
    if (!Rml::LoadFontFace(generic(path))) {
      log_error("launcher.font.failed", generic(path));
      ok = false;
    }
  }
  return ok;
}

void app::wire_shell(Rml::ElementDocument *doc) {
  for (auto &e : views_) {
    if (!e.info.tab) {
      continue;
    }
    const std::string id = e.info.id;
    listen(doc->GetElementById("tab-" + id), "click", [this, id](Rml::Event &) {
      // The current tab again (Enter or A after B brought focus up here):
      // back into the screen, which arrows alone cannot reach (on_key).
      if (id == current_) {
        if (view *v = find(id)) {
          focus_default(*v);
        }
        return;
      }
      show(id);
    });
  }
  listen(doc->GetElementById("win-min"), "click",
         [this](Rml::Event &) { window_.minimize(); });
  listen(doc->GetElementById("win-max"), "click",
         [this](Rml::Event &) { window_.toggle_maximize(); });
  listen(doc->GetElementById("win-close"), "click",
         [this](Rml::Event &) { window_.request_close(); });
  listen(doc->GetElementById("shell-about"), "click",
         [this](Rml::Event &) { show("about"); });
  if (Rml::Element *version = doc->GetElementById("shell-version")) {
    version->SetInnerRML(escape(services::version()));
  }
  if (Rml::Element *badge = doc->GetElementById("shell-badge")) {
    badge->SetInnerRML(services_->paths().dev_data ? "DEV" : "BETA");
  }
}

bool app::load_view(view &v, const view_info &info) {
  const fs::path path = services_->paths().data / "ui" / v.document();
  Rml::ElementDocument *doc = context_->LoadDocument(generic(path));
  if (!doc) {
    log_error("launcher.view.load_failed", generic(path));
    return false;
  }
  v.doc = doc;
  doc->SetClass(std::string("view-") + info.id, true);
  if (doc->GetElementById("header")) {
    wire_shell(doc);
  }
  v.loaded(doc);
  applied_look_.clear(); // the new document needs the theme classes
  return true;
}

void app::show(const std::string &id) {
  entry *target = nullptr;
  for (auto &e : views_) {
    if (id == e.info.id) {
      target = &e;
    }
  }
  if (!target) {
    log_warn("launcher.view.unknown", id);
    return;
  }
  if (!target->loaded) {
    target->loaded = true;
    target->failed = !load_view(*target->screen, target->info);
    if (!target->failed) {
      apply_look();
    }
  }
  // A screen whose document did not load (missing or broken RML; the log has
  // the reason) has nothing to show, whether it was tried here or by the
  // loader in run(): stay where we are.
  if (target->failed || !target->screen->doc) {
    notify("The " + id + " screen could not be loaded.", true);
    return;
  }
  if (current_ == id) {
    return;
  }
  if (view *old = find(current_); old && old->doc) {
    old->hidden();
    old->doc->Hide();
  }
  current_ = id;
  view &v = *target->screen;
  v.doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
  v.shown();
  // Let the screen fill its model and lay out first, so the default focus
  // sees the elements data-if shows or hides.
  v.update(now());
  context_->Update();
  if (!focus_default(v)) {
    v.doc->Focus();
  }
  focus_lost_frames_ = 0;
  notice_dirty_ = true;
  log_info("launcher.view.shown", id);
}

bool app::focus_default(view &v) {
  Rml::Element *focus = v.default_focus();
  if (!focus) {
    return false;
  }
  focus->Focus(true);
  focus->ScrollIntoView(Rml::ScrollAlignment::Adaptive);
  return true;
}

void app::keep_focus() {
  view *v = find(current_);
  if (dialog_open_ || !v || !v->doc) {
    focus_lost_frames_ = 0;
    return;
  }
  // RmlUi navigates and clicks from the focused element. When data-if hides
  // the box around it, RmlUi blurs only the box and the context keeps the
  // hidden button; when its row is removed, the context has no focus at all.
  // Either way arrows, Enter and A do nothing until focus is put back.
  Rml::Element *focus = context_->GetFocusElement();
  const bool lost =
      !focus || !focus->GetOwnerDocument() || !focus->IsVisible(true);
  if (!lost) {
    focus_lost_frames_ = 0;
    return;
  }
  // Screens that move focus themselves after such a change (to the next row,
  // say) wait a frame or two for the new elements; they go first.
  if (++focus_lost_frames_ < 3) {
    return;
  }
  focus_lost_frames_ = 0;
  if (!focus_default(*v)) {
    v->doc->Focus();
  }
  log(xerces_core::log_level::debug, "launcher.focus.recovered", current_);
}

void app::next_tab(int delta) {
  std::vector<std::string> tabs;
  for (const auto &e : views_) {
    // A screen whose document failed to load is never shown (show()).
    if (e.info.tab && !e.failed) {
      tabs.push_back(e.info.id);
    }
  }
  if (tabs.empty() || delta == 0) {
    return;
  }
  const auto it = std::ranges::find(tabs, current_);
  const int size = int(tabs.size());
  const int step = delta < 0 ? -1 : 1;
  int index = (it == tabs.end() ? 0 : int(it - tabs.begin())) + delta;
  // A screen that fails to load now (its first show) is passed over too:
  // show() then leaves current_ as it was.
  for (int tries = 0; tries < size; ++tries, index += step) {
    const std::string &id = tabs[std::size_t((index % size + size) % size)];
    if (id == current_) {
      return;
    }
    show(id);
    if (current_ == id) {
      return;
    }
  }
}

void app::notify(const std::string &text, bool error) {
  notice_text_ = text;
  notice_error_ = error;
  notice_until_ = now() + (error ? 8.0 : 5.0);
  notice_dirty_ = true;
  log(error ? xerces_core::log_level::warn : xerces_core::log_level::info,
      "launcher.notice", text);
}

void app::update_notice() {
  const bool expired = !notice_text_.empty() && now() >= notice_until_;
  if (expired) {
    notice_text_.clear();
    notice_dirty_ = true;
  }
  if (!notice_dirty_) {
    return;
  }
  notice_dirty_ = false;
  view *v = find(current_);
  if (!v || !v->doc) {
    return;
  }
  for (auto &e : views_) {
    if (!e.screen->doc) {
      continue;
    }
    if (Rml::Element *notice = e.screen->doc->GetElementById("notice")) {
      const bool on = !notice_text_.empty() && e.screen.get() == v;
      if (on) {
        notice->SetInnerRML(escape(notice_text_));
      }
      notice->SetClass("on", on);
      notice->SetClass("error", notice_error_);
    }
  }
}

void app::dialog(const std::string &title, const std::string &body,
                 std::vector<std::string> buttons,
                 std::function<void(int)> done) {
  if (!dialog_doc_) {
    if (done) {
      done(-1);
    }
    return;
  }
  if (dialog_open_) {
    close_dialog(-1);
  }
  dialog_doc_->GetElementById("dialog-title")->SetInnerRML(escape(tr(title)));
  dialog_doc_->GetElementById("dialog-body")->SetInnerRML(escape(tr(body)));
  std::string markup;
  for (std::size_t i = 0; i < buttons.size(); ++i) {
    markup += "<button id=\"dialog-b" + std::to_string(i) + "\" class=\"" +
              (i == 0 ? "primary-button" : "outline") + "\"><span>" +
              escape(tr(buttons[i])) + "</span></button>";
  }
  dialog_doc_->GetElementById("dialog-buttons")->SetInnerRML(markup);
  dialog_done_ = std::move(done);
  dialog_open_ = true;
  dialog_doc_->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::None);
  dialog_doc_->PullToFront();
  if (Rml::Element *first = dialog_doc_->GetElementById("dialog-b0")) {
    first->Focus(true);
  }
  log_info("launcher.dialog.open", title);
}

void app::close_dialog(int choice) {
  if (!dialog_open_) {
    return;
  }
  dialog_open_ = false;
  dialog_doc_->Hide();
  auto done = std::move(dialog_done_);
  dialog_done_ = nullptr;
  if (view *v = find(current_); v && v->doc) {
    v->doc->Focus();
    if (Rml::Element *focus = v->default_focus()) {
      focus->Focus(true);
    }
  }
  log_info("launcher.dialog.closed", std::to_string(choice));
  if (done) {
    done(choice);
  }
}

void app::apply_look() {
  const auto &config = services_->config();
  const std::string theme = !opts_.theme.empty() ? opts_.theme : config.theme;
  const std::string key = theme + (config.reduced_motion ? "+rm" : "");
  if (key != applied_look_) {
    applied_look_ = key;
    std::vector<Rml::ElementDocument *> docs;
    for (auto &e : views_) {
      if (e.screen->doc) {
        docs.push_back(e.screen->doc);
      }
    }
    if (dialog_doc_) {
      docs.push_back(dialog_doc_);
    }
    for (Rml::ElementDocument *doc : docs) {
      doc->SetClass("theme-light", theme == "light");
      doc->SetClass("theme-contrast", theme == "contrast");
      doc->SetClass("reduced-motion", config.reduced_motion);
    }
  }
  float scale = window_.dp_ratio();
  if (opts_.scale > 0.f) {
    scale = opts_.scale;
  } else if (config.ui_scale_percent >= 75 && config.ui_scale_percent <= 200) {
    scale *= float(config.ui_scale_percent) / 100.f;
  }
  if (context_ && context_->GetDensityIndependentPixelRatio() != scale) {
    context_->SetDensityIndependentPixelRatio(scale);
    applied_scale_ = scale;
  }
}

bool app::open_url(const std::string &url) {
  const bool allowed =
      (url.starts_with("https://") || url.starts_with("steam://")) &&
      url.size() < 2048 &&
      url.find_first_of(" \"'<>\\^`{|}") == std::string::npos &&
      std::ranges::none_of(url, [](char c) {
        return static_cast<unsigned char>(c) < 0x20;
      });
  if (!allowed) {
    log_warn("launcher.url.refused", url);
    return false;
  }
  log_info("launcher.url.open", url);
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
      nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return result > 32;
}

bool app::open_folder(const fs::path &dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    return false;
  }
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
      nullptr, L"explore", dir.wstring().c_str(), nullptr, nullptr,
      SW_SHOWNORMAL));
  return result > 32;
}

void app::copy_text(const std::string &text) {
  if (system_) {
    system_->SetClipboardText(text);
  }
}

void app::inject_key(Rml::Input::KeyIdentifier key, int modifiers) {
  if (!on_key(key, modifiers, true)) {
    return;
  }
  if (context_->ProcessKeyDown(key, modifiers)) {
    on_key(key, modifiers, false);
  }
  context_->ProcessKeyUp(key, modifiers);
}

bool app::on_key(Rml::Input::KeyIdentifier key, int modifiers, bool priority) {
  if (!priority || !context_) {
    return true;
  }
  // RmlUi's modifier state includes Caps, Num and Scroll Lock; with Num Lock
  // on (the usual case) a plain Q would otherwise never count as unmodified.
  // Screens get the same masked value.
  modifiers &= Rml::Input::KM_CTRL | Rml::Input::KM_SHIFT |
               Rml::Input::KM_ALT | Rml::Input::KM_META;
  if (dialog_open_) {
    if (key == Rml::Input::KI_ESCAPE) {
      close_dialog(-1);
      return false;
    }
    return true;
  }
  view *v = find(current_);
  if (v && v->key(key, modifiers)) {
    return false;
  }
  Rml::Element *focus = context_->GetFocusElement();
  const bool ctrl = (modifiers & Rml::Input::KM_CTRL) != 0;
  const bool shift = (modifiers & Rml::Input::KM_SHIFT) != 0;
  if (is_text_field(focus)) {
    if (key == Rml::Input::KI_ESCAPE && v) {
      focus->Blur();
      if (Rml::Element *target = v->default_focus()) {
        target->Focus(true);
      }
      return false;
    }
    if (!(ctrl && key == Rml::Input::KI_TAB)) {
      return true; // typing
    }
  }
  if (ctrl && key == Rml::Input::KI_TAB) {
    next_tab(shift ? -1 : 1);
    return false;
  }
  if (modifiers == 0 && key == Rml::Input::KI_Q) {
    next_tab(-1);
    return false;
  }
  if (modifiers == 0 && key == Rml::Input::KI_E) {
    next_tab(+1);
    return false;
  }
  if (key == Rml::Input::KI_OEM_2 && v && v->search_field()) {
    // Windows has already queued this key's character ('/') behind it; were
    // the field focused now, the character would be typed into it. Posted
    // tasks run after the queued messages, so focus moves once it is gone.
    post([this, id = current_] {
      view *target = find(id);
      if (current_ != id || dialog_open_ || !target) {
        return;
      }
      if (Rml::Element *search = target->search_field()) {
        search->Focus(true);
        search->ScrollIntoView(Rml::ScrollAlignment::Adaptive);
      }
    });
    return false;
  }
  // #content is a scroll container (base.rcss), and RmlUi's spatial
  // navigation neither enters nor leaves one: from the header's tabs Down
  // would skip the screen for the footer, and Up from the footer would skip
  // it back to the tabs. Both go into the screen instead. (Screens that
  // navigate themselves, such as Mods, have used the key by now.)
  if (modifiers == 0 && v && v->doc && focus &&
      focus->GetOwnerDocument() == v->doc &&
      ((key == Rml::Input::KI_DOWN && inside(focus, "header")) ||
       (key == Rml::Input::KI_UP && inside(focus, "footer")))) {
    if (focus_default(*v)) {
      return false;
    }
  }
  if (key == Rml::Input::KI_ESCAPE && v && v->doc && inside(focus, "content")) {
    if (Rml::Element *tab = v->doc->GetElementById("tab-" + current_)) {
      tab->Focus(true);
      return false;
    }
  }
  return true;
}

hit_region app::hit_test(int x, int y) {
  if (!context_ || dialog_open_) {
    return hit_region::client;
  }
  const Rml::Element *element =
      context_->GetElementAtPoint(Rml::Vector2f(float(x), float(y)));
  for (const Rml::Element *e = element; e; e = e->GetParentNode()) {
    const Rml::String tag = e->GetTagName();
    if (tag == "button" || tag == "input" || tag == "select" ||
        tag == "textarea" || e->IsClassSet("no-drag")) {
      return hit_region::client;
    }
    if (e->GetId() == "header" || e->IsClassSet("drag-region")) {
      return hit_region::caption;
    }
  }
  return hit_region::client;
}

void app::run_script(double seconds) {
  while (script_next_ < script_.size() && script_[script_next_].at <= seconds) {
    const script_step step = script_[script_next_++];
    log_info("launcher.check.step", step.verb + " " + step.arg);
    if (step.verb == "key") {
      // "key ctrl+up", "key shift+tab"; "numlock+q" adds the lock flag a real
      // keyboard sends, to check that it is ignored.
      int modifiers = 0;
      std::string name = step.arg;
      for (std::size_t plus; (plus = name.find('+')) != std::string::npos;) {
        const std::string part = name.substr(0, plus);
        name.erase(0, plus + 1);
        modifiers |= part == "ctrl"      ? Rml::Input::KM_CTRL
                     : part == "shift"   ? Rml::Input::KM_SHIFT
                     : part == "alt"     ? Rml::Input::KM_ALT
                     : part == "numlock" ? Rml::Input::KM_NUMLOCK
                                         : 0;
      }
      inject_key(key_by_name(name), modifiers);
    } else if (step.verb == "pad") {
      const Rml::Input::KeyIdentifier key =
          step.arg == "a"       ? Rml::Input::KI_RETURN
          : step.arg == "b"     ? Rml::Input::KI_ESCAPE
          : step.arg == "y"     ? Rml::Input::KI_OEM_2
          : step.arg == "up"    ? Rml::Input::KI_UP
          : step.arg == "down"  ? Rml::Input::KI_DOWN
          : step.arg == "left"  ? Rml::Input::KI_LEFT
          : step.arg == "right" ? Rml::Input::KI_RIGHT
                                : Rml::Input::KI_UNKNOWN;
      if (step.arg == "lb") {
        next_tab(-1);
      } else if (step.arg == "rb") {
        next_tab(+1);
      } else if (key != Rml::Input::KI_UNKNOWN) {
        inject_key(key);
      }
    } else if (step.verb == "padhold") {
      // padhold right: held as on a controller until "padhold none".
      script_pad_ =
          step.arg == "none" ? 0 : script_pad_ | pad_bit_by_name(step.arg);
    } else if (step.verb == "front") {
      // front off: the launcher takes the controller as if another window
      // were in front; front on: as if it were; front auto: the real one.
      script_front_ = step.arg == "on" ? 1 : step.arg == "off" ? 0 : -1;
    } else if (step.verb == "view") {
      show(step.arg);
    } else if (step.verb == "shot") {
      window_.capture_next_frame(fs::path(widen(step.arg)));
    } else if (step.verb == "click") {
      view *v = find(current_);
      Rml::ElementDocument *doc = dialog_open_ ? dialog_doc_ : (v ? v->doc : nullptr);
      if (Rml::Element *element = doc ? doc->GetElementById(step.arg) : nullptr) {
        element->Click();
      } else {
        log_warn("launcher.check.missing", step.arg);
      }
    } else if (step.verb == "focus") {
      view *v = find(current_);
      if (Rml::Element *element = v && v->doc ? v->doc->GetElementById(step.arg) : nullptr) {
        element->Focus(true);
        element->ScrollIntoView(Rml::ScrollAlignment::Adaptive);
      }
    } else if (step.verb == "type") {
      context_->ProcessTextInput(step.arg);
    } else if (step.verb == "quit") {
      window_.request_close();
    }
  }
}

int app::run() {
  launcher_paths paths = find_paths(opts_.home);

  // One launcher per home: a second start brings the first window forward
  // instead of writing the same config and journal.
  bool taken_over = false;
  {
    // xerces_core names it, so xerces_cli can tell a launcher runs here.
    const std::uint32_t hash = xerces_core::launcher_home_hash(paths.home);
    const std::wstring name = xerces_core::launcher_instance_name(paths.home);
    // Never 0, which is what GetPropW returns for a window without the tag.
    home_tag_ = (std::uintptr_t(hash) << 1) | 1;
    instance_mutex_ = CreateMutexW(nullptr, TRUE, name.c_str());
    if (instance_mutex_ && GetLastError() == ERROR_ALREADY_EXISTS) {
      // The other launcher of this home has no window yet when it is still
      // opening (a double-click starts two at once), and none any more when
      // it is closing: it hides its window and drops the tag first, then
      // waits for its worker, which can take seconds. So: bring its window
      // forward once there is one, or take over once it lets go of the
      // mutex, rather than give up and lose this start. Up to 30 s, for a
      // launcher that is stuck with no window.
      HWND other = nullptr;
      for (int tries = 0; tries < 600 && !other && !taken_over; ++tries) {
        other = find_home_window(home_tag_);
        if (!other) {
          // Abandoned: the other one ended without letting go (a crash).
          const DWORD got = WaitForSingleObject(instance_mutex_, 50);
          taken_over = got == WAIT_OBJECT_0 || got == WAIT_ABANDONED;
        }
      }
      if (!taken_over) {
        if (other) {
          if (IsIconic(other)) {
            ShowWindow(other, SW_RESTORE);
          }
          SetForegroundWindow(other);
        }
        CloseHandle(instance_mutex_);
        instance_mutex_ = nullptr;
        return 0;
      }
    }
  }
  // From here, not from the process start: script steps and --seconds count
  // from when this launcher really starts, after any wait above.
  started_ = ms_since_process_start() / 1000.0;
  open_log(generic(paths.logs));
  log_info("launcher.start", "Xerces " + services::version(),
           "{\"home\":" + xerces_core::json::quote(generic(paths.home)) +
               ",\"data\":" + xerces_core::json::quote(generic(paths.data)) +
               ",\"dry_launch\":" + (opts_.dry_launch ? "true" : "false") + "}");
  if (taken_over) {
    log_info("launcher.instance.took_over",
             "Started once the closing launcher of this home had stopped.");
  }
  services_ = std::make_unique<services>(
      paths, [this](std::function<void()> task) { post(std::move(task)); },
      opts_.dry_launch);

  window_callbacks callbacks;
  callbacks.hit_test = [this](int x, int y) { return hit_test(x, y); };
  callbacks.key_down = [this](Rml::Input::KeyIdentifier key, int modifiers,
                              bool priority) {
    return on_key(key, modifiers, priority);
  };
  if (!window_.open("Xerces", opts_.width, opts_.height, callbacks)) {
    log_error("launcher.window.failed", "The window could not be created.");
    return 1;
  }
  SetPropW(static_cast<HWND>(window_.native_handle()), home_property,
           reinterpret_cast<HANDLE>(home_tag_));
  // Detection and the catalog scan start while RmlUi loads.
  services_->refresh(true);

  system_ = std::make_unique<translator>(*this, window_.system_interface());
  Rml::SetSystemInterface(system_.get());
  Rml::SetRenderInterface(window_.render_interface());
  Rml::Initialise();
  rml_started_ = true;
  context_ = Rml::CreateContext(
      "main", Rml::Vector2i(std::max(1, window_.width()),
                            std::max(1, window_.height())));
  if (!context_) {
    log_error("launcher.rmlui.context", "RmlUi could not create a context.");
    shutdown();
    return 1;
  }
  load_language();
  load_fonts();
  mark("rmlui and fonts");

  // Screens: data models first (documents bind to them while loading).
  for (const view_info &info : view_registry()) {
    views_.push_back({info, info.create(), false});
  }
  std::ranges::sort(views_, [](const entry &a, const entry &b) {
    return a.info.order < b.info.order;
  });
  for (auto &e : views_) {
    if (!e.screen->bind(context_, *this)) {
      log_error("launcher.view.bind_failed", e.info.id);
      shutdown();
      return 1;
    }
  }
  dialog_doc_ = context_->LoadDocument(generic(paths.data / "ui" / "dialog.rml"));
  if (dialog_doc_) {
    listen(dialog_doc_->GetElementById("dialog-buttons"), "click",
           [this](Rml::Event &event) {
             for (Rml::Element *e = event.GetTargetElement(); e;
                  e = e->GetParentNode()) {
               const std::string id = e->GetId();
               if (id.starts_with("dialog-b")) {
                 close_dialog(std::atoi(id.c_str() + 8));
                 return;
               }
             }
           });
  }

  // The first screen; show() refuses one whose document did not load, so
  // then the screen asked for, then Home. With none of them there is
  // nothing to show, and the player is told so instead of a blank window.
  std::vector<std::string> firsts;
  if (!opts_.skip_boot && find("boot") != nullptr) {
    firsts.push_back("boot");
  }
  if (!opts_.start_view.empty() && opts_.start_view != "home") {
    firsts.push_back(opts_.start_view);
  }
  firsts.push_back("home");
  for (const std::string &id : firsts) {
    if (current_.empty()) {
      show(id);
    }
  }
  if (current_.empty()) {
    log_error("launcher.view.none", "No screen could be loaded.");
    // Not in scripted checks, where nobody would close the box.
    if (opts_.script.empty() && opts_.seconds <= 0) {
      MessageBoxW(static_cast<HWND>(window_.native_handle()),
                  L"Xerces could not load its screens, so it has nothing to "
                  L"show.\n\nReinstalling Xerces usually fixes this. The log "
                  L"is in the Xerces logs folder.",
                  L"Xerces", MB_OK | MB_ICONERROR);
    }
    shutdown();
    return 1;
  }
  apply_look();
  mark("first screen loaded");

  if (!opts_.script.empty()) {
    std::ifstream in(widen(opts_.script));
    double at = 1.0;
    for (std::string line; std::getline(in, line);) {
      std::istringstream words(line);
      std::string delay, verb;
      if (!(words >> delay >> verb) || delay.empty() || delay[0] != '+') {
        continue;
      }
      std::string arg;
      std::getline(words, arg);
      arg.erase(0, arg.find_first_not_of(' '));
      at += std::strtod(delay.c_str() + 1, nullptr) / 1000.0;
      script_.push_back({at, verb, arg});
    }
  }

  bool first_frame = true;
  bool running = true;
  std::string last_focus;
  while (running) {
    const bool busy = first_frame || !script_.empty() ||
                      context_->GetNextUpdateDelay() == 0 ||
                      std::ranges::any_of(views_, [](const entry &e) {
                        return !e.loaded;
                      }) ||
                      opts_.seconds > 0 || window_.capture_pending();
    int wait = busy ? 0 : 100;
    if (!busy && context_->GetNextUpdateDelay() < 0.1) {
      wait = std::min(wait, int(context_->GetNextUpdateDelay() * 1000.0));
    }
    // Only the window in front takes the controller. XInput reads it from
    // anywhere, so a launcher left open behind the game would otherwise take
    // every press meant for the game (toggling mods, switching tabs) and
    // wake up for each one.
    const auto in_front = [this] {
      if (script_front_ >= 0) {
        return script_front_ == 1; // a check script says (front on|off)
      }
      return GetForegroundWindow() ==
             static_cast<HWND>(window_.native_handle());
    };
    const bool front = in_front();
    if (front && (pad_previous_ & ~pad_ignored_ & pad_directions)) {
      wait = std::min(wait, 8); // a held direction repeats (below)
    }
    // XInput sends no window messages. With a controller connected the idle
    // wait is cut into 8 ms slices with the controller read between them, so
    // a quick tap wakes the loop as a key would; no frames are drawn while
    // nothing changes.
    std::optional<unsigned> pad_read;
    const double wait_until = now() + wait / 1000.0;
    for (int left = wait; left > 0;
         left = int((wait_until - now()) * 1000.0)) {
      const bool pad = front && pad_slots_ != 0;
      if (window_.wait_events(pad ? std::min(left, 8) : left)) {
        break;
      }
      if (pad) {
        const unsigned buttons = read_pad(now());
        if (buttons != pad_previous_) {
          pad_read = buttons;
          break;
        }
      }
    }
    running = window_.process_events(context_, 0);
    run_posted();
    services_->poll();
    apply_look();

    // Controller: the same action map as the keyboard (7.4). Whatever is held
    // while the launcher is not in front belongs to the other window, so it
    // stays ignored until it is let go: a stick still pushed when the game
    // exits neither presses nor starts repeating on the launcher's first
    // frame in front. Only a fresh press counts there.
    const double t = now();
    const unsigned buttons = pad_read ? *pad_read : read_pad(t);
    const bool takes_pad = in_front();
    pad_ignored_ = takes_pad ? pad_ignored_ & buttons : buttons;
    const unsigned live = buttons & ~pad_ignored_;
    const unsigned pressed = live & ~pad_previous_;
    const auto fire = [&](unsigned bit) {
      if (!opts_.script.empty()) {
        log_info("launcher.check.pad", pad_name(bit));
      }
      switch (bit) {
      case pad_up: inject_key(Rml::Input::KI_UP); break;
      case pad_down: inject_key(Rml::Input::KI_DOWN); break;
      case pad_left: inject_key(Rml::Input::KI_LEFT); break;
      case pad_right: inject_key(Rml::Input::KI_RIGHT); break;
      case pad_a: inject_key(Rml::Input::KI_RETURN); break;
      case pad_b: inject_key(Rml::Input::KI_ESCAPE); break;
      case pad_y: inject_key(Rml::Input::KI_OEM_2); break;
      case pad_lb: if (!dialog_open_) next_tab(-1); break;
      case pad_rb: if (!dialog_open_) next_tab(+1); break;
      default: break;
      }
    };
    for (unsigned bit = 1; bit <= pad_rb; bit <<= 1) {
      if (pressed & bit) {
        fire(bit);
        if (bit <= pad_right) {
          pad_repeat_at_ = t + 0.3;
        }
      }
    }
    const unsigned held = live & pad_directions;
    if (held && !(pressed & held) && t >= pad_repeat_at_) {
      for (unsigned bit = 1; bit <= pad_right; bit <<= 1) {
        if (held & bit) {
          fire(bit);
        }
      }
      pad_repeat_at_ = t + 0.12;
    }
    pad_previous_ = buttons;

    run_script(t);

    // Load the remaining screens one per frame after the first frame.
    if (!first_frame) {
      for (auto &e : views_) {
        if (!e.loaded) {
          // Tried once: a document that fails is not retried every frame,
          // and show() refuses the screen (with a notice) from then on.
          e.loaded = true;
          e.failed = !load_view(*e.screen, e.info);
          if (!e.failed) {
            e.screen->doc->Hide();
          }
          break;
        }
      }
    }
    for (auto &e : views_) {
      e.screen->update(t);
    }
    update_notice();

    const std::string focus = [&] {
      Rml::Element *f = context_->GetFocusElement();
      return f ? f->GetTagName() + (f->GetId().empty() ? "" : "#" + f->GetId())
               : std::string("(none)");
    }();
    if (focus != last_focus) {
      last_focus = focus;
      log(xerces_core::log_level::debug, "launcher.focus", focus);
    }

    context_->Update();
    // After the update: data-if and data-for changes have taken effect.
    keep_focus();
    window_.begin_frame();
    context_->Render();
    const bool capturing = window_.capture_pending();
    window_.present();
    if (capturing && !window_.capture_pending()) {
      if (window_.last_capture_error().empty()) {
        log_info("launcher.check.captured", current_);
      } else {
        log_warn("launcher.check.capture", window_.last_capture_error());
      }
    }
    if (first_frame) {
      first_frame = false;
      mark("first frame presented");
    }
    if (opts_.seconds > 0 && t >= opts_.seconds) {
      running = false;
    }
  }

  log_info("launcher.exit", "closed");
  shutdown();
  return 0;
}
} // namespace xerces_launcher
