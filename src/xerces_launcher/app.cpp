#include "app.hpp"

#include <RmlUi/Core.h>

#include <RmlUi_Include_Windows.h>

#include <shellapi.h>
#include <Xinput.h>
// wingdi.h's GetObject macro would rename rapidjson's GetObject().
#undef GetObject

#include <algorithm>
#include <cstdio>
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
  if (text.find("{{") != std::string::npos || text.empty()) {
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

unsigned read_pad() {
  for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
    XINPUT_STATE state{};
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
      continue;
    }
    const XINPUT_GAMEPAD &pad = state.Gamepad;
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
  return 0;
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
  wchar_t value[8] = {};
  if (GetEnvironmentVariableW(L"XERCES_DRY_LAUNCH", value, 8) > 0 &&
      value[0] == L'1') {
    out.dry_launch = true;
  }
  return out;
}

app::app(options opts) : opts_(std::move(opts)) {}

app::~app() {
  if (instance_mutex_) {
    ReleaseMutex(instance_mutex_);
    CloseHandle(instance_mutex_);
  }
}

double app::now() const { return ms_since_process_start() / 1000.0 - started_; }

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
    default: out.push_back(c); break;
    }
  }
  return out;
}

std::string app::tr(const std::string &english) const {
  if (language_ == "pseudo") {
    return pseudo(english);
  }
  const auto it = strings_.find(english);
  return it == strings_.end() ? english : it->second;
}

void app::load_language() {
  language_ = !opts_.language.empty() ? opts_.language
                                      : services_->config().language;
  strings_.clear();
  if (language_ == "en" || language_ == "pseudo") {
    return;
  }
  std::ifstream in(services_->paths().data / "i18n" / (language_ + ".json"),
                   std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  const auto parsed = xerces_core::json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject()) {
    log_warn("launcher.i18n.missing", "No strings for language " + language_);
    language_ = "en";
    return;
  }
  for (const auto &member : parsed.document.GetObject()) {
    if (member.value.IsString()) {
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
    listen(doc->GetElementById("tab-" + id), "click",
           [this, id](Rml::Event &) { show(id); });
  }
  listen(doc->GetElementById("win-min"), "click",
         [this](Rml::Event &) { window_.minimize(); });
  listen(doc->GetElementById("win-max"), "click",
         [this](Rml::Event &) { window_.toggle_maximize(); });
  listen(doc->GetElementById("win-close"), "click",
         [this](Rml::Event &) { window_.request_close(); });
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
    target->loaded = load_view(*target->screen, target->info);
    if (!target->loaded) {
      notify("The " + id + " screen could not be loaded.", true);
      return;
    }
    apply_look();
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
  if (Rml::Element *focus = v.default_focus()) {
    focus->Focus(true);
    focus->ScrollIntoView(Rml::ScrollAlignment::Adaptive);
  } else {
    v.doc->Focus();
  }
  notice_dirty_ = true;
  log_info("launcher.view.shown", id);
}

void app::next_tab(int delta) {
  std::vector<std::string> tabs;
  for (const auto &e : views_) {
    if (e.info.tab) {
      tabs.push_back(e.info.id);
    }
  }
  if (tabs.empty()) {
    return;
  }
  const auto it = std::ranges::find(tabs, current_);
  const int index = it == tabs.end() ? 0 : int(it - tabs.begin());
  const int size = int(tabs.size());
  show(tabs[std::size_t(((index + delta) % size + size) % size)]);
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
  if (key == Rml::Input::KI_OEM_2 && v) {
    if (Rml::Element *search = v->search_field()) {
      search->Focus(true);
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
      inject_key(key_by_name(step.arg));
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
      }
    } else if (step.verb == "type") {
      context_->ProcessTextInput(step.arg);
    } else if (step.verb == "quit") {
      window_.request_close();
    }
  }
}

int app::run() {
  started_ = ms_since_process_start() / 1000.0;
  launcher_paths paths = find_paths(opts_.home);

  // One launcher per home: a second start brings the first window forward
  // instead of writing the same config and journal.
  {
    std::uint32_t hash = 2166136261u;
    for (const wchar_t c : paths.home.wstring()) {
      hash = (hash ^ static_cast<std::uint32_t>(towlower(c))) * 16777619u;
    }
    const std::wstring name = L"Local\\xerces_launcher_" + std::to_wstring(hash);
    instance_mutex_ = CreateMutexW(nullptr, TRUE, name.c_str());
    if (instance_mutex_ && GetLastError() == ERROR_ALREADY_EXISTS) {
      if (HWND other = FindWindowW(L"xerces_launcher_window", nullptr)) {
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
  open_log(generic(paths.logs));
  log_info("launcher.start", "Xerces " + services::version(),
           "{\"home\":" + xerces_core::json::quote(generic(paths.home)) +
               ",\"data\":" + xerces_core::json::quote(generic(paths.data)) +
               ",\"dry_launch\":" + (opts_.dry_launch ? "true" : "false") + "}");
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
  // Detection and the catalog scan start while RmlUi loads.
  services_->refresh(true);

  system_ = std::make_unique<translator>(*this, window_.system_interface());
  Rml::SetSystemInterface(system_.get());
  Rml::SetRenderInterface(window_.render_interface());
  Rml::Initialise();
  context_ = Rml::CreateContext(
      "main", Rml::Vector2i(std::max(1, window_.width()),
                            std::max(1, window_.height())));
  if (!context_) {
    log_error("launcher.rmlui.context", "RmlUi could not create a context.");
    Rml::Shutdown();
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
      Rml::Shutdown();
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

  const bool boot = !opts_.skip_boot && find("boot") != nullptr;
  show(boot ? "boot" : (opts_.start_view.empty() ? "home" : opts_.start_view));
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
    const bool pad_connected = pad_previous_ != 0 || read_pad() != 0;
    const bool busy = first_frame || !script_.empty() ||
                      context_->GetNextUpdateDelay() == 0 ||
                      std::ranges::any_of(views_, [](const entry &e) {
                        return !e.loaded;
                      }) ||
                      opts_.seconds > 0 || window_.capture_pending();
    int wait = busy ? 0 : (pad_connected ? 8 : 100);
    if (!busy && context_->GetNextUpdateDelay() < 0.1) {
      wait = std::min(wait, int(context_->GetNextUpdateDelay() * 1000.0));
    }
    running = window_.process_events(context_, wait);
    run_posted();
    services_->poll();
    apply_look();

    // Controller: the same action map as the keyboard (7.4).
    const unsigned buttons = read_pad();
    const unsigned pressed = buttons & ~pad_previous_;
    const double t = now();
    const auto fire = [&](unsigned bit) {
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
    const unsigned held = buttons & (pad_up | pad_down | pad_left | pad_right);
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
          e.loaded = load_view(*e.screen, e.info);
          if (e.loaded && e.screen->doc) {
            e.screen->doc->Hide();
          }
          if (!e.loaded) {
            e.loaded = true; // do not retry every frame; show() reports it
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
  services_->cancel_scan();
  Rml::Shutdown(); // documents and data models go first
  context_ = nullptr;
  dialog_doc_ = nullptr;
  for (auto &e : views_) {
    e.screen.reset();
  }
  listeners_.clear();
  window_.close();
  services_.reset();
  return 0;
}
} // namespace xerces_launcher
