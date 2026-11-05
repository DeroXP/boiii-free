// Settings (docs/DESIGN.md 7.3, 7.5, 14; upstream's launcher settings for
// parity): the game folder and the player name, display options the game gets
// on its command line, launch options, look and accessibility, language,
// privacy and updates. Every change saves launcher.json at once and says so in
// the footer.
//
// Where each setting goes, and why:
// - Player name: the client reads "playerName" from
//   <game>\boiii_players\properties.json when it starts (name.cpp), so Xerces
//   writes it there (xerces_core/settings_options.cpp), like upstream's
//   savePlayerName did, and keeps it in config.player_name too.
// - Display mode and "skip all cinematics": the client's own flags
//   (-windowed, -borderless, -nocinematics), which it applies after its saved
//   config has loaded; they live in config.extra_flags.
// - FPS limit, FOV, V-Sync, FPS counter: dvars confirmed in the client
//   (com_maxfps, cg_fov_default, r_vsync, com_drawFPS_PC), kept in
//   config.dvars and passed as "+set <dvar> <value>".

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>

#include <xerces_core/settings_options.hpp>

#include "../app.hpp"
#include "../log.hpp"

namespace xerces_launcher {
namespace {
namespace core = xerces_core;

struct dvar_row {
  Rml::String name, value;
};

std::string utf8(const fs::path &path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

// The four text fields, by element id.
enum class text_field { none, name, fps, fov, flags };

text_field field_of(const std::string &id) {
  if (id == "player-name") return text_field::name;
  if (id == "fps-field") return text_field::fps;
  if (id == "fov-field") return text_field::fov;
  if (id == "flags-field") return text_field::flags;
  return text_field::none;
}

const char *field_id(text_field field) {
  switch (field) {
  case text_field::name: return "player-name";
  case text_field::fps: return "fps-field";
  case text_field::fov: return "fov-field";
  case text_field::flags: return "flags-field";
  default: return "";
  }
}

text_field field_named(const std::string &name) {
  if (name == "name") return text_field::name;
  if (name == "fps") return text_field::fps;
  if (name == "fov") return text_field::fov;
  if (name == "flags") return text_field::flags;
  return text_field::none;
}

// A control next to each text field, for leaving it with Esc.
const char *near_control(text_field field) {
  switch (field) {
  case text_field::fps: return "fps-slider";
  case text_field::fov: return "fov-slider";
  case text_field::flags: return "close-on-launch";
  default: return "game-change";
  }
}

// The FOV slider's left end, one below the engine's minimum, stands for "the
// game's setting".
constexpr int fov_slider_game = core::fov_min - 1;

class settings_view final : public view {
public:
  const char *id() const override { return "settings"; }

  bool bind(Rml::Context *context, app &owner) override {
    owner_ = &owner;
    Rml::DataModelConstructor model = context->CreateDataModel("settings");
    if (!model) {
      return false;
    }
    if (auto row = model.RegisterStruct<dvar_row>()) {
      row.RegisterMember("name", &dvar_row::name);
      row.RegisterMember("value", &dvar_row::value);
    }
    model.RegisterArray<Rml::Vector<dvar_row>>();

    model.Bind("game_path", &game_path_);
    model.Bind("game_detail", &game_detail_);
    model.Bind("game_tried", &game_tried_);
    model.Bind("game_found", &game_found_);
    model.Bind("game_ok", &game_ok_);
    model.Bind("game_busy", &game_busy_);
    model.Bind("name", &name_);
    model.Bind("name_placeholder", &name_placeholder_);
    model.Bind("name_note", &name_note_);

    model.Bind("display", &display_);
    model.Bind("fps", &fps_);
    model.Bind("fps_stop", &fps_stop_);
    model.Bind("fps_max", &fps_max_);
    model.Bind("fps_note", &fps_note_);
    model.Bind("fov", &fov_);
    model.Bind("fov_stop", &fov_stop_);
    model.Bind("fov_note", &fov_note_);
    model.Bind("vsync", &vsync_);
    model.Bind("drawfps", &drawfps_);
    model.Bind("others", &others_);

    model.Bind("nointro", &nointro_);
    model.Bind("nocinematics", &nocinematics_);
    model.Bind("close_on_launch", &close_on_launch_);
    model.Bind("flags", &flags_);
    model.Bind("flags_error", &flags_error_);
    model.Bind("command", &command_);
    model.Bind("command_bad", &command_bad_);

    model.Bind("theme", &theme_);
    model.Bind("scale", &scale_);
    model.Bind("reduced_motion", &reduced_motion_);
    model.Bind("look_note", &look_note_);
    model.Bind("language", &language_);
    model.Bind("language_note", &language_note_);
    model.Bind("home_path", &home_path_);
    model.Bind("check_updates", &check_updates_);
    model.Bind("version", &version_);

    model.BindEventCallback(
        "act", [this](Rml::DataModelHandle, Rml::Event &,
                      const Rml::VariantList &args) {
          if (!args.empty()) {
            act(args[0].Get<Rml::String>());
          }
        });
    model.BindEventCallback(
        "choose", [this](Rml::DataModelHandle, Rml::Event &,
                         const Rml::VariantList &args) {
          if (args.size() >= 2) {
            choose(args[0].Get<Rml::String>(), args[1].Get<Rml::String>());
          }
        });
    model.BindEventCallback(
        "toggle", [this](Rml::DataModelHandle, Rml::Event &,
                         const Rml::VariantList &args) {
          if (!args.empty()) {
            toggle(args[0].Get<Rml::String>());
          }
        });
    model.BindEventCallback(
        "commit", [this](Rml::DataModelHandle, Rml::Event &,
                         const Rml::VariantList &args) {
          if (!args.empty()) {
            commit(field_named(args[0].Get<Rml::String>()), false);
          }
        });
    model.BindEventCallback(
        "slid", [this](Rml::DataModelHandle, Rml::Event &event,
                       const Rml::VariantList &args) {
          if (!args.empty()) {
            slid(args[0].Get<Rml::String>(), event);
          }
        });
    model.BindEventCallback(
        "remove_dvar", [this](Rml::DataModelHandle, Rml::Event &,
                              const Rml::VariantList &args) {
          if (!args.empty()) {
            remove_dvar(args[0].Get<Rml::String>());
          }
        });
    handle_ = model.GetModelHandle();
    fps_max_ = core::fps_slider_max();
    return true;
  }

  void update(double) override {
    remember_focus();
    services &svc = owner_->svc();
    const std::uint64_t key[] = {svc.config_rev(), svc.game_rev(),
                                 svc.journal_rev(), owner_->mode_rev(),
                                 svc.detecting() ? 1u : 0u};
    if (std::equal(std::begin(key), std::end(key), std::begin(seen_))) {
      return;
    }
    const bool game_changed = key[1] != seen_[1] || key[2] != seen_[2];
    std::copy(std::begin(key), std::end(key), std::begin(seen_));
    if (game_changed) {
      after_game_change();
    }
    refresh();
  }

  bool key(Rml::Input::KeyIdentifier key, int modifiers) override {
    Rml::Context *context = owner_->context();
    Rml::Element *focus = context ? context->GetFocusElement() : nullptr;
    if (!focus || !doc || focus->GetOwnerDocument() != doc) {
      return false;
    }
    const int held = modifiers & (Rml::Input::KM_CTRL | Rml::Input::KM_SHIFT |
                                  Rml::Input::KM_ALT | Rml::Input::KM_META);
    if ((key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN) && held == 0 &&
        move_row(focus, key == Rml::Input::KI_DOWN ? 1 : -1)) {
      return true;
    }
    const text_field field = field_of(focus->GetId());
    if (field == text_field::none) {
      return false;
    }
    if (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER) {
      commit(field, true);
      return true;
    }
    if (key == Rml::Input::KI_ESCAPE) {
      // Esc leaves the field (the app blurs it and asks default_focus());
      // what was typed and not saved goes back to the saved value first, so
      // the blur that follows has nothing to save.
      revert(field);
      escaping_from_ = field;
    }
    return false;
  }

  // Where focus goes when the screen is shown or a text field is left with
  // Esc: the control the player was on (the page keeps its scroll position,
  // so a control at the top would be out of sight), never a text field
  // (typing would swallow Q and E), and after Esc one in the same row.
  Rml::Element *default_focus() override {
    std::string target = last_focus_;
    if (escaping_from_ != text_field::none) {
      target = field_id(escaping_from_);
      escaping_from_ = text_field::none;
    }
    if (const text_field field = field_of(target); field != text_field::none) {
      target = near_control(field);
    }
    if (Rml::Element *element = visible(target)) {
      return element;
    }
    return view::default_focus();
  }

private:
  services &svc() { return owner_->svc(); }
  core::launcher_config &config() { return owner_->svc().config(); }

  // ----------------------------------------------------------- navigation
  // Up and Down go row by row through the form. RmlUi's spatial navigation
  // only looks at controls that overlap the focused one, and here most
  // controls sit at the right end of their rows, so it skipped whole
  // sections. Left and Right stay spatial (they move within a row).
  std::vector<std::vector<std::string>> nav_rows() const {
    std::vector<std::vector<std::string>> rows = {
        {"jump-game", "jump-display", "jump-launch", "jump-look",
         "jump-language", "jump-privacy", "jump-updates"},
        {"game-change", "game-detect", "game-open"},
        {"player-name"},
        {"display-game", "display-windowed", "display-borderless"},
        {"fps-field", "fps-slider"},
        {"fov-field", "fov-slider"},
        {"vsync-game", "vsync-on", "vsync-off"},
        {"drawfps-game", "drawfps-on", "drawfps-off"},
    };
    for (const dvar_row &row : others_) {
      rows.push_back({"dvar-" + row.name});
    }
    const std::vector<std::vector<std::string>> rest = {
        {"nointro"},
        {"nocinematics"},
        {"close-on-launch"},
        {"flags-field"},
        {"theme-dark", "theme-light", "theme-contrast"},
        {"scale-0", "scale-75", "scale-100", "scale-125", "scale-150",
         "scale-175", "scale-200"},
        {"reduced-motion"},
        {"lang-en", "lang-pseudo"},
        {"open-home"},
        {"check-updates"},
    };
    rows.insert(rows.end(), rest.begin(), rest.end());
    return rows;
  }

  static bool inside(const Rml::Element *element, const char *id) {
    for (; element; element = element->GetParentNode()) {
      if (element->GetId() == id) {
        return true;
      }
    }
    return false;
  }

  Rml::Element *visible(const std::string &id) const {
    Rml::Element *element = doc && !id.empty() ? doc->GetElementById(id) : nullptr;
    return element && element->IsVisible(true) ? element : nullptr;
  }

  // In a row of choices the chosen one; else the same column, else the
  // first control that is shown.
  Rml::Element *pick(const std::vector<std::string> &row, std::size_t column) const {
    for (const std::string &id : row) {
      if (Rml::Element *element = visible(id); element && element->IsClassSet("on") &&
                                               element->IsClassSet("choice")) {
        return element;
      }
    }
    if (Rml::Element *element = visible(row[std::min(column, row.size() - 1)])) {
      return element;
    }
    for (const std::string &id : row) {
      if (Rml::Element *element = visible(id)) {
        return element;
      }
    }
    return nullptr;
  }

  void focus_on(Rml::Element *target, bool top) {
    target->Focus(true);
    if (top) {
      if (Rml::Element *content = doc->GetElementById("content")) {
        content->SetScrollTop(0.f);
      }
      return;
    }
    // Keep the whole row in view, and a section's title with its first row.
    Rml::Element *field = target;
    while (field && !field->IsClassSet("field")) {
      field = field->GetParentNode();
    }
    Rml::Element *section = field;
    while (section && !section->IsClassSet("set-section")) {
      section = section->GetParentNode();
    }
    if (section) {
      Rml::ElementList fields;
      section->GetElementsByClassName(fields, "field");
      if (!fields.empty() && fields.front() == field) {
        section->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Start));
        return;
      }
    }
    (field ? field : target)
        ->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
  }

  bool move_row(Rml::Element *focus, int delta) {
    // From the tabs, Down comes back to the control the player left.
    if (inside(focus, "header")) {
      if (delta < 0) {
        return false;
      }
      Rml::Element *target = visible(last_focus_);
      if (!target) {
        target = visible("game-change");
      }
      if (!target) {
        return false;
      }
      focus_on(target, false);
      return true;
    }
    const auto rows = nav_rows();
    const std::string id = focus->GetId();
    int row = -1;
    std::size_t column = 0;
    for (std::size_t r = 0; r < rows.size() && row < 0; ++r) {
      for (std::size_t c = 0; c < rows[r].size(); ++c) {
        if (rows[r][c] == id) {
          row = int(r);
          column = c;
          break;
        }
      }
    }
    if (row < 0) {
      return false;
    }
    for (int r = row + delta; r >= 0 && r < int(rows.size()); r += delta) {
      if (Rml::Element *target = pick(rows[std::size_t(r)], column)) {
        focus_on(target, r == 0);
        return true;
      }
    }
    // Up from the first row goes to this screen's tab, as Esc does (the
    // tabs do not sit above the index, so spatial navigation misses them);
    // Down from the last row stays.
    if (delta < 0) {
      if (Rml::Element *tab = visible(std::string("tab-") + this->id())) {
        tab->Focus(true);
        return true;
      }
      return false;
    }
    return true;
  }

  void remember_focus() {
    if (!doc || owner_->current() != id()) {
      return;
    }
    Rml::Context *context = owner_->context();
    const Rml::Element *focus = context ? context->GetFocusElement() : nullptr;
    if (focus && focus->GetOwnerDocument() == doc && inside(focus, "content") &&
        !focus->GetId().empty()) {
      last_focus_ = focus->GetId();
    }
  }

  // ------------------------------------------------------------- refresh
  void refresh() {
    services &s = svc();
    const core::launcher_config &c = s.config();
    const game_state &game = s.game();

    // Game folder.
    game_busy_ = s.detecting() || detect_pending_ || !game.searched;
    game_found_ = game.dir.has_value();
    game_ok_ = game.can_launch();
    game_tried_.clear();
    if (game.dir) {
      game_path_ = utf8(*game.dir);
      std::string detail = game.steam_build
                               ? owner_->tr("Steam build") + " " +
                                     std::to_string(*game.steam_build)
                               : owner_->tr("Steam build unknown");
      if (game.preflight && !game.preflight->can_launch()) {
        detail += " \xC2\xB7 " + game.preflight->message;
      } else if (!game.error.empty()) {
        detail += " \xC2\xB7 " + game.error;
      }
      game_detail_ = detail;
    } else if (game_busy_) {
      game_path_ = owner_->tr("Looking for Black Ops III...");
      game_detail_.clear();
    } else {
      game_path_ = owner_->tr("Not found");
      game_detail_ = game.error.empty()
                         ? owner_->tr("Pick the folder that has BlackOps3.exe in it.")
                         : game.error;
      if (!game.tried.empty()) {
        std::string tried = owner_->tr("Looked in:");
        const std::size_t shown = std::min<std::size_t>(game.tried.size(), 4);
        for (std::size_t i = 0; i < shown; ++i) {
          tried += "\n" + game.tried[i];
        }
        if (game.tried.size() > shown) {
          tried += "\n+" + std::to_string(game.tried.size() - shown) + " " +
                   owner_->tr("more");
        }
        game_tried_ = tried;
      }
    }

    // Player name, and what the game's own file holds.
    if (keep(text_field::name)) {
      name_ = c.player_name;
    }
    std::optional<std::string> in_game;
    bool file_there = false;
    if (game.dir) {
      const fs::path file = core::player_properties_file(*game.dir);
      std::error_code ec;
      file_there = fs::is_regular_file(file, ec);
      if (file_there) {
        in_game = core::read_player_name(read_small(file));
      }
    }
    name_placeholder_ = in_game ? *in_game : owner_->tr("Your Steam name");
    if (!name_error_.empty()) {
      name_note_ = owner_->tr("The game's file was not changed:") + " " +
                   name_error_;
    } else if (!game.dir) {
      name_note_ = owner_->tr("Set the game folder first: the game reads the "
                              "name from a file in it.");
    } else if (!file_there) {
      name_note_ = owner_->tr("The game has not made "
                              "boiii_players\\properties.json yet. Start the "
                              "game once, then press Enter here to write the "
                              "name.");
    } else if (in_game && !c.player_name.empty() && *in_game != c.player_name) {
      // Renamed in the game (its "name" command), or a write that failed:
      // Xerces does not overwrite it behind the player's back.
      name_note_ = owner_->tr("The game's file says:") + " " + *in_game + ". " +
                   owner_->tr("Press Enter in the field to write this name "
                              "there instead.");
    } else if (in_game) {
      name_note_ = owner_->tr("The game's file says:") + " " + *in_game;
    } else {
      name_note_ = owner_->tr("The game's file has no name: it uses your "
                              "Steam name.");
    }

    // Display.
    display_ = core::display_mode(c.extra_flags);
    const std::optional<int> fps = dvar_number(core::dvar_fps_limit);
    if (keep(text_field::fps)) {
      fps_ = dvar_text(core::dvar_fps_limit);
    }
    fps_stop_ = core::fps_slider_index(fps);
    fps_note_ = !c.dvars.contains(std::string(core::dvar_fps_limit))
                    ? owner_->tr("The game's setting")
                : fps && *fps == 0 ? owner_->tr("No limit")
                : fps ? "+set com_maxfps " + std::to_string(*fps)
                      : owner_->tr("Not a whole number");
    const std::optional<int> fov = dvar_number(core::dvar_fov);
    if (keep(text_field::fov)) {
      fov_ = dvar_text(core::dvar_fov);
    }
    fov_stop_ = fov ? std::clamp(*fov, core::fov_min, core::fov_max)
                    : fov_slider_game;
    fov_note_ = !c.dvars.contains(std::string(core::dvar_fov))
                    ? owner_->tr("The game's setting")
                : fov ? "+set cg_fov_default " + std::to_string(*fov)
                      : owner_->tr("Not a whole number");
    vsync_ = tri_state(core::dvar_vsync);
    drawfps_ = tri_state(core::dvar_draw_fps);
    others_.clear();
    for (const auto &[name, value] : c.dvars) {
      if (name != core::dvar_fps_limit && name != core::dvar_fov &&
          name != core::dvar_vsync && name != core::dvar_draw_fps) {
        others_.push_back({name, value});
      }
    }

    // Launch.
    nointro_ = c.nointro;
    nocinematics_ = core::has_flag(c.extra_flags, "nocinematics");
    close_on_launch_ = c.close_on_launch;
    if (keep(text_field::flags)) {
      flags_ = core::join_flags(c.extra_flags);
      flags_error_.clear();
    }
    const launch_outcome preview =
        s.preview_launch(owner_->mode(), false, false);
    command_bad_ = preview.command_line.empty();
    command_ = command_bad_ ? preview.message : preview.command_line;

    // Look, language, privacy, updates.
    theme_ = c.theme;
    scale_ = c.ui_scale_percent;
    reduced_motion_ = c.reduced_motion;
    look_note_.clear();
    if (!owner_->opts().theme.empty()) {
      look_note_ = owner_->tr("This session was started with --theme, so the "
                              "theme above is used from the next start.");
    }
    if (owner_->opts().scale > 0.f) {
      look_note_ += (look_note_.empty() ? "" : " ") +
                    owner_->tr("This session was started with --scale, so the "
                               "UI scale above is used from the next start.");
    }
    language_ = c.language; // a language this screen does not list lights none
    if (!owner_->opts().language.empty()) {
      language_note_ = owner_->tr("This session was started with --language; "
                                  "the choice above is used from the next "
                                  "start.");
    } else if (language_ != owner_->language()) {
      language_note_ = owner_->tr("Restart Xerces to switch the language.");
    } else {
      language_note_.clear();
    }
    home_path_ = utf8(s.paths().home);
    check_updates_ = c.check_updates;
    version_ = "Xerces " + services::version();

    force_ = text_field::none;
    handle_.DirtyAllVariables();
  }

  // A text field takes the saved value unless the player is typing in it.
  bool keep(text_field field) const {
    if (force_ == field) {
      return true;
    }
    Rml::Context *context = owner_->context();
    const Rml::Element *focus = context ? context->GetFocusElement() : nullptr;
    return !(focus && doc && focus->GetOwnerDocument() == doc &&
             field_of(focus->GetId()) == field);
  }

  static std::string read_small(const fs::path &file) {
    std::error_code ec;
    if (fs::file_size(file, ec) > 64 * 1024 || ec) {
      return {};
    }
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  }

  std::string dvar_text(std::string_view name) {
    const auto &dvars = config().dvars;
    const auto it = dvars.find(std::string(name));
    return it == dvars.end() ? std::string() : it->second;
  }

  std::optional<int> dvar_number(std::string_view name) {
    const auto &dvars = config().dvars;
    const auto it = dvars.find(std::string(name));
    if (it == dvars.end()) {
      return std::nullopt;
    }
    const core::number_input number =
        core::parse_number_input(it->second, 0, 1000000);
    if (number.state != core::number_input::kind::ok) {
      return std::nullopt;
    }
    return number.value;
  }

  std::string tri_state(std::string_view name) {
    const auto &dvars = config().dvars;
    const auto it = dvars.find(std::string(name));
    if (it == dvars.end()) {
      return "game";
    }
    return it->second == "0" ? "off" : "on";
  }

  // ------------------------------------------------------------- saving
  void save(const std::string &what, const std::string &message) {
    log_info("launcher.settings.changed", what);
    if (svc().save_config()) {
      owner_->notify(message);
    } else {
      log_error("launcher.settings.save", utf8(svc().paths().config_file()));
      owner_->notify(owner_->tr("The settings could not be saved."), true);
    }
  }

  void set_dvar(std::string_view name, std::optional<std::string> value,
                const std::string &message) {
    auto &dvars = config().dvars;
    const std::string key(name);
    const auto it = dvars.find(key);
    if (!value) {
      if (it == dvars.end()) {
        return;
      }
      dvars.erase(it);
    } else {
      if (it != dvars.end() && it->second == *value) {
        return;
      }
      dvars[key] = *value;
    }
    save("dvar " + key + (value ? "=" + *value : " removed"), message);
  }

  void set_flags(std::vector<std::string> flags, const std::string &what,
                 const std::string &message) {
    if (flags == config().extra_flags) {
      return;
    }
    config().extra_flags = std::move(flags);
    force_ = text_field::flags;
    save(what, message);
  }

  // -------------------------------------------------------------- actions
  void act(const std::string &action) {
    if (action.starts_with("jump ")) {
      jump(action.substr(5));
      return;
    }
    log_info("launcher.settings.action", action);
    services &s = svc();
    if (action == "change folder") {
      const std::optional<fs::path> picked =
          owner_->pick_folder("Pick the Black Ops III folder");
      if (!picked) {
        return;
      }
      if (s.set_game_dir(*picked)) {
        detect_pending_ = true;
        detect_from_ = s.game_rev();
        owner_->notify(owner_->tr("Game folder saved. Checking it now..."));
      } else {
        owner_->notify(s.game().error.empty()
                           ? owner_->tr("That folder has no BlackOps3.exe in it.")
                           : s.game().error,
                       true);
      }
      refresh();
    } else if (action == "detect") {
      if (game_busy_) {
        return;
      }
      detect_pending_ = true;
      detect_from_ = s.game_rev();
      s.refresh(true);
      owner_->notify(owner_->tr("Looking for Black Ops III..."));
      refresh();
    } else if (action == "open game") {
      if (s.game().dir && !owner_->open_folder(*s.game().dir)) {
        owner_->notify(owner_->tr("The game folder could not be opened."), true);
      }
    } else if (action == "open home") {
      if (!owner_->open_folder(s.paths().home)) {
        owner_->notify(owner_->tr("The folder could not be opened."), true);
      }
    }
  }

  // The first control of each section; the index at the top jumps there.
  void jump(const std::string &section) {
    static const std::pair<const char *, const char *> targets[] = {
        {"game", "game-change"},     {"display", "display-game"},
        {"launch", "nointro"},       {"look", "theme-dark"},
        {"language", "lang-en"},     {"privacy", "open-home"},
        {"updates", "check-updates"},
    };
    if (!doc) {
      return;
    }
    for (const auto &[name, target] : targets) {
      if (section != name) {
        continue;
      }
      Rml::Element *control = doc->GetElementById(target);
      Rml::Element *head = doc->GetElementById(std::string("section-") + name);
      if (control) {
        control->Focus(true);
      }
      if (head) {
        head->ScrollIntoView(true);
      }
      return;
    }
  }

  void choose(const std::string &key, const std::string &value) {
    core::launcher_config &c = config();
    if (key == "display") {
      set_flags(core::with_display_mode(c.extra_flags, value),
                "display " + value,
                value == "windowed" ? owner_->tr("Display mode: windowed (-windowed).")
                : value == "borderless"
                    ? owner_->tr("Display mode: borderless (-borderless).")
                    : owner_->tr("Display mode: the game's setting."));
    } else if (key == "vsync" || key == "drawfps") {
      const std::string_view dvar =
          key == "vsync" ? core::dvar_vsync : core::dvar_draw_fps;
      const std::string label =
          key == "vsync" ? owner_->tr("V-Sync") : owner_->tr("FPS counter");
      if (value == "game") {
        set_dvar(dvar, std::nullopt, label + ": " + owner_->tr("the game's setting."));
      } else {
        set_dvar(dvar, value == "on" ? "1" : "0",
                 label + ": " + owner_->tr(value == "on" ? "on." : "off."));
      }
    } else if (key == "theme") {
      if (value != "dark" && value != "light" && value != "contrast") {
        return;
      }
      if (c.theme == value) {
        return;
      }
      c.theme = value;
      save("theme " + value, owner_->tr("Theme saved."));
      owner_->apply_look();
    } else if (key == "scale") {
      const int percent = std::atoi(value.c_str());
      if (!(percent == 0 || (percent >= 75 && percent <= 200))) {
        return;
      }
      if (c.ui_scale_percent == percent) {
        return;
      }
      c.ui_scale_percent = percent;
      save("ui scale " + value,
           percent == 0 ? owner_->tr("UI scale: follows Windows.")
                        : owner_->tr("UI scale:") + " " + value + "%.");
      owner_->apply_look();
    } else if (key == "language") {
      if (value != "en" && value != "pseudo") {
        return;
      }
      if (c.language == value) {
        return;
      }
      c.language = value;
      save("language " + value,
           value == owner_->language()
               ? owner_->tr("Language saved.")
               : owner_->tr("Language saved. Restart Xerces to switch."));
    }
  }

  void toggle(const std::string &key) {
    core::launcher_config &c = config();
    if (key == "nointro") {
      c.nointro = !c.nointro;
      save(std::string("nointro ") + (c.nointro ? "on" : "off"),
           c.nointro ? owner_->tr("The intro video is skipped.")
                     : owner_->tr("The intro video plays."));
    } else if (key == "nocinematics") {
      const bool on = !core::has_flag(c.extra_flags, "nocinematics");
      set_flags(core::with_flag(c.extra_flags, "nocinematics", on),
                std::string("nocinematics ") + (on ? "on" : "off"),
                on ? owner_->tr("Cinematics are skipped (-nocinematics).")
                   : owner_->tr("Cinematics play."));
    } else if (key == "close") {
      c.close_on_launch = !c.close_on_launch;
      save(std::string("close_on_launch ") + (c.close_on_launch ? "on" : "off"),
           c.close_on_launch
               ? owner_->tr("Xerces closes when the game starts.")
               : owner_->tr("Xerces stays open while you play."));
    } else if (key == "reduced") {
      c.reduced_motion = !c.reduced_motion;
      save(std::string("reduced_motion ") + (c.reduced_motion ? "on" : "off"),
           c.reduced_motion ? owner_->tr("Reduced motion is on.")
                            : owner_->tr("Reduced motion is off."));
      owner_->apply_look();
    } else if (key == "updates") {
      c.check_updates = !c.check_updates;
      save(std::string("check_updates ") + (c.check_updates ? "on" : "off"),
           c.check_updates
               ? owner_->tr("Saved. Nothing is checked until the update "
                            "client exists.")
               : owner_->tr("Saved. Xerces will not check for updates."));
    }
  }

  void remove_dvar(const std::string &name) {
    // The button goes away with its row: move focus on first, so a
    // controller is never left without a focused control.
    Rml::Context *context = owner_->context();
    if (Rml::Element *focus = context ? context->GetFocusElement() : nullptr;
        focus && focus->GetId() == "dvar-" + name && !move_row(focus, +1)) {
      move_row(focus, -1);
    }
    set_dvar(name, std::nullopt,
             owner_->tr("Removed") + " +set " + name + ".");
  }

  // ----------------------------------------------------------- text fields
  Rml::ElementFormControl *control(text_field field) {
    if (!doc) {
      return nullptr;
    }
    return rmlui_dynamic_cast<Rml::ElementFormControl *>(
        doc->GetElementById(field_id(field)));
  }

  std::string typed(text_field field) {
    Rml::ElementFormControl *input = control(field);
    return input ? std::string(input->GetValue()) : std::string();
  }

  // The saved value of a field, as the field shows it.
  std::string saved(text_field field) {
    switch (field) {
    case text_field::name: return config().player_name;
    case text_field::fps: return dvar_text(core::dvar_fps_limit);
    case text_field::fov: return dvar_text(core::dvar_fov);
    case text_field::flags: return core::join_flags(config().extra_flags);
    default: return {};
    }
  }

  void revert(text_field field) {
    const std::string value = saved(field);
    if (Rml::ElementFormControl *input = control(field)) {
      input->SetValue(value);
    }
    if (field == text_field::flags) {
      flags_error_.clear();
    }
    force_ = field;
    refresh();
  }

  // by_enter: the player pressed Enter; otherwise the field lost focus.
  void commit(text_field field, bool by_enter) {
    switch (field) {
    case text_field::name: commit_name(by_enter); break;
    case text_field::fps: commit_number(text_field::fps); break;
    case text_field::fov: commit_number(text_field::fov); break;
    case text_field::flags: commit_flags(); break;
    default: break;
    }
  }

  void commit_name(bool by_enter) {
    const core::player_name_result cleaned = core::clean_player_name(typed(text_field::name));
    core::launcher_config &c = config();
    force_ = text_field::name;
    // An unchanged name is only written again on Enter, when the game's file
    // still lacks it (no file yet, a failed write, or renamed in the game);
    // leaving the field never rewrites it.
    if (cleaned.name == c.player_name &&
        (!by_enter || (!name_pending_ && !game_file_differs(cleaned.name)))) {
      refresh(); // shows the cleaned text if spaces were trimmed
      return;
    }
    c.player_name = cleaned.name;
    log_info("launcher.settings.changed", "player_name");
    if (!svc().save_config()) {
      log_error("launcher.settings.save", utf8(svc().paths().config_file()));
      owner_->notify(owner_->tr("The settings could not be saved."), true);
      return;
    }
    // Footer notices stay short (one line); the details are on the screen.
    const std::string message = apply_name();
    if (cleaned.dropped && name_error_.empty()) {
      owner_->notify(owner_->tr("Name saved without the characters the game "
                                "cannot show."),
                     true);
    } else if (cleaned.shortened && name_error_.empty()) {
      owner_->notify(owner_->tr("Name saved, cut to 16 characters."), true);
    } else {
      owner_->notify(message, !name_error_.empty());
    }
    refresh();
  }

  bool game_file_differs(const std::string &name) {
    const game_state &game = svc().game();
    if (name.empty() || !game.dir) {
      return false;
    }
    const fs::path file = core::player_properties_file(*game.dir);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
      return false;
    }
    return core::read_player_name(read_small(file)).value_or("") != name;
  }

  // Writes the saved name into the game's properties file; the message for
  // the footer.
  std::string apply_name() {
    const game_state &game = svc().game();
    const std::string &name = config().player_name;
    name_error_.clear();
    if (!game.dir) {
      name_pending_ = true;
      return owner_->tr("Name saved. Set the game folder so the game gets it.");
    }
    const core::name_apply_result result = core::apply_player_name(*game.dir, name);
    switch (result.status) {
    case core::name_apply::written:
    case core::name_apply::unchanged:
      name_pending_ = false;
      log_info("launcher.settings.player_name", "written to properties.json");
      return name.empty()
                 ? owner_->tr("Name cleared: the game uses your Steam name from its next start.")
                 : owner_->tr("Name saved. The game uses it from its next start.");
    case core::name_apply::no_file:
      name_pending_ = true;
      return owner_->tr("Name saved. Start the game once, then press Enter here.");
    case core::name_apply::failed:
    default:
      name_pending_ = true;
      name_error_ = result.message;
      log_warn("launcher.settings.player_name", result.message);
      return owner_->tr("Name saved, but the game's file was not changed.");
    }
  }

  // The game folder changed or the game exited: a name that could not be
  // written before may go in now.
  void after_game_change() {
    services &s = svc();
    if (detect_pending_ && !s.detecting() && s.game_rev() != detect_from_) {
      detect_pending_ = false;
      const game_state &game = s.game();
      if (game.dir) {
        owner_->notify(game.can_launch()
                           ? owner_->tr("Black Ops III found.")
                           : owner_->tr("Black Ops III found; see the line "
                                        "under its folder."));
      } else {
        owner_->notify(owner_->tr("Black Ops III was not found. Use Change "
                                  "folder to pick it."),
                       true);
      }
    }
    if (name_pending_ && s.game().dir) {
      const core::name_apply_result result =
          core::apply_player_name(*s.game().dir, config().player_name);
      if (result.status == core::name_apply::written ||
          result.status == core::name_apply::unchanged) {
        name_pending_ = false;
        name_error_.clear();
        log_info("launcher.settings.player_name",
                 "written to properties.json after the game changed");
      }
    }
  }

  void commit_number(text_field field) {
    const bool fps = field == text_field::fps;
    const int min = fps ? core::fps_limit_min : core::fov_min;
    const int max = fps ? core::fps_limit_max : core::fov_max;
    const std::string_view dvar = fps ? core::dvar_fps_limit : core::dvar_fov;
    const std::string label = fps ? owner_->tr("FPS limit") : owner_->tr("Field of view");
    const core::number_input number =
        core::parse_number_input(typed(field), min, max);
    force_ = field;
    switch (number.state) {
    case core::number_input::kind::empty:
      if (config().dvars.contains(std::string(dvar))) {
        set_dvar(dvar, std::nullopt, label + ": " + owner_->tr("the game's setting."));
      } else {
        refresh();
      }
      break;
    case core::number_input::kind::ok: {
      const std::string value = std::to_string(number.value);
      if (dvar_text(dvar) == value) {
        refresh();
        break;
      }
      set_dvar(dvar, value,
               fps && number.value == 0
                   ? label + ": " + owner_->tr("no limit.")
                   : label + ": " + value + ".");
      break;
    }
    case core::number_input::kind::invalid:
      owner_->notify(label + ": " + owner_->tr("a whole number from") + " " +
                         std::to_string(min) + " " + owner_->tr("to") + " " +
                         std::to_string(max) + ", " +
                         owner_->tr("or empty for the game's setting."),
                     true);
      revert(field);
      break;
    }
  }

  void slid(const std::string &which, Rml::Event &event) {
    Rml::ElementFormControl *slider =
        rmlui_dynamic_cast<Rml::ElementFormControl *>(event.GetCurrentElement());
    if (!slider) {
      return;
    }
    // The slider also reports changes made by the screen itself (loading,
    // refresh); only a focused slider is being moved by the player.
    Rml::Context *context = owner_->context();
    if (!context || context->GetFocusElement() != event.GetCurrentElement()) {
      return;
    }
    const int stop = int(std::lround(std::atof(slider->GetValue().c_str())));
    if (which == "fps") {
      // Programmatic moves land on the stop the saved value maps to; only a
      // different stop is the player's.
      if (stop == core::fps_slider_index(dvar_number(core::dvar_fps_limit))) {
        return;
      }
      const std::optional<int> limit = core::fps_slider_value(stop);
      const std::string label = owner_->tr("FPS limit");
      set_dvar(core::dvar_fps_limit,
               limit ? std::optional<std::string>(std::to_string(*limit))
                     : std::nullopt,
               !limit       ? label + ": " + owner_->tr("the game's setting.")
               : *limit == 0 ? label + ": " + owner_->tr("no limit.")
                             : label + ": " + std::to_string(*limit) + ".");
      force_ = text_field::fps;
    } else if (which == "fov") {
      const std::optional<int> fov = dvar_number(core::dvar_fov);
      const int current =
          fov ? std::clamp(*fov, core::fov_min, core::fov_max) : fov_slider_game;
      if (stop == current) {
        return;
      }
      const std::string label = owner_->tr("Field of view");
      if (stop <= fov_slider_game) {
        set_dvar(core::dvar_fov, std::nullopt,
                 label + ": " + owner_->tr("the game's setting."));
      } else {
        const int value = std::clamp(stop, core::fov_min, core::fov_max);
        set_dvar(core::dvar_fov, std::to_string(value),
                 label + ": " + std::to_string(value) + ".");
      }
      force_ = text_field::fov;
    }
  }

  void commit_flags() {
    const core::flags_parse parsed = core::parse_launch_flags(typed(text_field::flags));
    std::string problem;
    if (!parsed.invalid.empty()) {
      problem = owner_->tr("Not saved:") + " " + core::join_flags(parsed.invalid) +
                " " +
                owner_->tr(parsed.invalid.size() == 1
                               ? "is not a flag (a '-' then letters, digits, '-' or '_')."
                               : "are not flags (a '-' then letters, digits, '-' or '_').");
    } else if (!parsed.reserved.empty()) {
      problem = owner_->tr("Not saved:") + " " +
                core::join_flags(parsed.reserved) + " " +
                owner_->tr("is set by Xerces itself.");
    } else if (parsed.too_many) {
      problem = owner_->tr("Not saved: at most 32 flags.");
    }
    if (!problem.empty()) {
      flags_error_ = problem;
      handle_.DirtyVariable("flags_error");
      owner_->notify(problem, true);
      return;
    }
    flags_error_.clear();
    handle_.DirtyVariable("flags_error");
    std::vector<std::string> flags = parsed.flags;
    // -nointro has its own switch; keep one place for it.
    if (core::has_flag(flags, "nointro")) {
      flags = core::with_flag(std::move(flags), "nointro", false);
      if (!config().nointro) {
        config().nointro = true;
        force_ = text_field::flags;
        config().extra_flags = flags;
        save("extra flags with -nointro",
             owner_->tr("Launch flags saved; -nointro turned Skip the intro "
                        "video on."));
        return;
      }
    }
    force_ = text_field::flags;
    if (flags == config().extra_flags) {
      refresh();
      return;
    }
    set_flags(flags, "extra flags",
              flags.empty() ? owner_->tr("Launch flags cleared.")
                            : owner_->tr("Launch flags saved."));
  }

  app *owner_ = nullptr;
  Rml::DataModelHandle handle_;
  std::uint64_t seen_[5] = {};
  text_field force_ = text_field::none;
  bool detect_pending_ = false;
  std::uint64_t detect_from_ = 0;
  bool name_pending_ = false;
  std::string last_focus_;
  text_field escaping_from_ = text_field::none;
  std::string name_error_; // why the game's file could not take the name

  Rml::String game_path_, game_detail_, game_tried_;
  bool game_found_ = false, game_ok_ = false, game_busy_ = true;
  Rml::String name_, name_placeholder_, name_note_;

  Rml::String display_ = "game";
  Rml::String fps_, fps_note_, fov_, fov_note_;
  int fps_stop_ = 0, fps_max_ = 15, fov_stop_ = fov_slider_game;
  Rml::String vsync_ = "game", drawfps_ = "game";
  Rml::Vector<dvar_row> others_;

  bool nointro_ = true, nocinematics_ = false, close_on_launch_ = false;
  Rml::String flags_, flags_error_, command_;
  bool command_bad_ = false;

  Rml::String theme_ = "dark";
  int scale_ = 0;
  bool reduced_motion_ = false;
  Rml::String look_note_, language_ = "en", language_note_;
  Rml::String home_path_, version_;
  bool check_updates_ = true;
};
} // namespace

XERCES_VIEW(settings_view, "settings", 60, true);
} // namespace xerces_launcher
