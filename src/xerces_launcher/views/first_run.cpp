// First run (docs/DESIGN.md 7.3, 3.5, 4.7): shown after boot when the setup
// was never finished or no game folder is known (boot.cpp decides), and from
// Home's "Set folder". Four steps with Back and Next:
//   01 Game     what detection found, the pre-flight verdict, a folder picker
//   02 Import   the read-only BOIII import (xerces_core/first_run_import)
//   03 Look     theme, UI scale and reduced motion, applied live
//   04 Privacy  the plain statement
// Finish (and "Skip setup") marks the setup done and opens Home. Nothing on
// this screen blocks: every step can be left as it is.

#include <RmlUi/Core.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <xerces_core/compat.hpp>
#include <xerces_core/first_run_import.hpp>

#include "../app.hpp"
#include "../log.hpp"

namespace xerces_launcher {
namespace {
constexpr int step_count = 4;

struct path_row {
  Rml::String n, path;
};
struct place_row {
  Rml::String name, path, state;
};
struct import_row {
  Rml::String key, title, value, detail;
  bool on = false;
  bool done = false;      // nothing (more) of it can go into the Xerces config
  Rml::String done_note;  // footer line when a done row is picked (C++ only)
};

std::string two_digits(std::size_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%02zu", value);
  return text;
}

std::string utf8(const fs::path &path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

// %LOCALAPPDATA%\boiii, where upstream BOIII keeps its data and (in user\)
// the identity key. The import only checks it is there.
std::optional<fs::path> boiii_appdata() {
  wchar_t *value = nullptr;
  std::size_t size = 0;
  if (_wdupenv_s(&value, &size, L"LOCALAPPDATA") != 0 || value == nullptr) {
    return std::nullopt;
  }
  fs::path dir = fs::path(value) / "boiii";
  std::free(value);
  return dir;
}

const char *verdict_name(xerces_core::preflight_verdict verdict) {
  switch (verdict) {
  case xerces_core::preflight_verdict::supported:
    return "Supported";
  case xerces_core::preflight_verdict::upstream_only:
    return "Runs, some features off";
  case xerces_core::preflight_verdict::unsupported:
    return "Not supported";
  case xerces_core::preflight_verdict::newer:
    return "Newer than Xerces knows";
  case xerces_core::preflight_verdict::unknown:
    return "Unknown build";
  }
  return "Unknown build";
}

class first_run_view final : public view {
public:
  const char *id() const override { return "first_run"; }

  bool bind(Rml::Context *context, app &owner) override {
    owner_ = &owner;
    context_ = context;
    Rml::DataModelConstructor model = context->CreateDataModel("first_run");
    if (!model) {
      return false;
    }
    if (auto row = model.RegisterStruct<path_row>()) {
      row.RegisterMember("n", &path_row::n);
      row.RegisterMember("path", &path_row::path);
    }
    if (auto row = model.RegisterStruct<place_row>()) {
      row.RegisterMember("name", &place_row::name);
      row.RegisterMember("path", &place_row::path);
      row.RegisterMember("state", &place_row::state);
    }
    if (auto row = model.RegisterStruct<import_row>()) {
      row.RegisterMember("key", &import_row::key);
      row.RegisterMember("title", &import_row::title);
      row.RegisterMember("value", &import_row::value);
      row.RegisterMember("detail", &import_row::detail);
      row.RegisterMember("on", &import_row::on);
      row.RegisterMember("done", &import_row::done);
    }
    model.RegisterArray<Rml::Vector<path_row>>();
    model.RegisterArray<Rml::Vector<place_row>>();
    model.RegisterArray<Rml::Vector<import_row>>();
    model.RegisterArray<Rml::Vector<Rml::String>>();

    model.Bind("step", &step_);
    model.Bind("next_label", &next_label_);
    model.Bind("progress", &progress_);

    model.Bind("looking", &looking_);
    model.Bind("game_found", &game_found_);
    model.Bind("game_ok", &game_ok_);
    model.Bind("game_bad", &game_bad_);
    model.Bind("game_title", &game_title_);
    model.Bind("game_path", &game_path_);
    model.Bind("game_build", &game_build_);
    model.Bind("game_verdict", &game_verdict_);
    model.Bind("game_known_as", &game_known_as_);
    model.Bind("game_message", &game_message_);
    model.Bind("game_error", &game_error_);
    model.Bind("tried", &tried_);

    model.Bind("places", &places_);
    model.Bind("imports", &imports_);
    model.Bind("import_notes", &import_notes_);
    model.Bind("can_import", &can_import_);
    model.Bind("import_done", &import_done_);
    model.Bind("import_empty", &import_empty_);

    model.Bind("theme", &theme_);
    model.Bind("scale", &scale_);
    model.Bind("reduced", &reduced_);
    model.Bind("scale_help", &scale_help_);
    model.Bind("home_path", &home_path_);

    model.BindEventCallback(
        "act", [this](Rml::DataModelHandle, Rml::Event &,
                      const Rml::VariantList &args) {
          if (!args.empty()) {
            act(args[0].Get<Rml::String>());
          }
        });
    model.BindEventCallback(
        "go", [this](Rml::DataModelHandle, Rml::Event &,
                     const Rml::VariantList &args) {
          if (!args.empty()) {
            go(args[0].Get<int>());
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
        "set_theme", [this](Rml::DataModelHandle, Rml::Event &,
                            const Rml::VariantList &args) {
          if (!args.empty()) {
            set_theme(args[0].Get<Rml::String>());
          }
        });
    model.BindEventCallback(
        "set_scale", [this](Rml::DataModelHandle, Rml::Event &,
                            const Rml::VariantList &args) {
          if (!args.empty()) {
            set_scale(args[0].Get<int>());
          }
        });
    handle_ = model.GetModelHandle();
    return true;
  }

  void shown() override {
    // Opened again from Home's "Set folder": start at the game.
    set_step(1);
    scan_import();
    refresh();
    focus_in_ = 2;
    focus_force_ = true;
  }

  void update(double) override {
    services &svc = owner_->svc();
    if (detect_requested_ && svc.game_rev() != detect_from_rev_ &&
        !svc.detecting()) {
      detect_requested_ = false;
    }
    const bool looking = svc.detecting() || detect_requested_;
    const std::uint64_t key[] = {svc.config_rev(), svc.game_rev(),
                                 looking ? 1u : 0u};
    if (!std::equal(std::begin(key), std::end(key), std::begin(seen_))) {
      const bool game_changed = key[1] != seen_[1];
      const bool was_found = game_found_;
      std::copy(std::begin(key), std::end(key), std::begin(seen_));
      if (game_changed && owner_->current() == id()) {
        // A new folder can bring a boiii_players with it.
        scan_import();
      }
      refresh();
      // Shown while detection was still running, focus started on "Choose
      // folder"; once the game turns up, Next is the useful default. Only
      // when it turns up: a failed pick also bumps the game revision (with an
      // error) while the old folder stays, and the player is on "Choose
      // folder" to try again.
      if (game_changed && !was_found && game_found_ &&
          owner_->current() == id() && step_ == 1 && context_ && doc) {
        Rml::Element *focus = context_->GetFocusElement();
        Rml::Element *next = doc->GetElementById("fr-next");
        if (focus && focus->GetId() == "fr-choose" && next) {
          next->Focus(true);
        }
      }
    }

    // Focus after a step change: the new step's controls only show once the
    // data model has updated, which happens after this frame.
    if (focus_in_ > 0 && --focus_in_ == 0 && owner_->current() == id() &&
        !owner_->dialog_open()) {
      Rml::Element *focus = context_ ? context_->GetFocusElement() : nullptr;
      // data-if hides a step's controls (display: none) rather than
      // removing them, so focus can sit on one that is no longer shown.
      const bool lost = focus == nullptr || focus->GetOwnerDocument() != doc ||
                        focus->GetTagName() != "button" ||
                        !focus->IsVisible(true);
      if (focus_force_ || lost) {
        if (Rml::Element *target = default_focus()) {
          target->Focus(true);
          target->ScrollIntoView(Rml::ScrollAlignment::Adaptive);
        }
      }
      focus_force_ = false;
    }
  }

  Rml::Element *default_focus() override {
    if (!doc) {
      return nullptr;
    }
    const auto visible = [this](const char *element_id) -> Rml::Element * {
      Rml::Element *element = doc->GetElementById(element_id);
      return element && element->IsVisible(true) ? element : nullptr;
    };
    Rml::Element *target = nullptr;
    if (step_ == 1 && !owner_->svc().game().dir) {
      target = visible("fr-choose");
    } else if (step_ == 2 && can_import_) {
      target = visible("fr-import");
    } else if (step_ == 3) {
      target = visible(("fr-theme-" + theme_).c_str());
    }
    return target ? target : visible("fr-next");
  }

  bool key(Rml::Input::KeyIdentifier key, int modifiers) override {
    // Esc / B steps back; on the first step the app's own Esc applies.
    if (key == Rml::Input::KI_ESCAPE && modifiers == 0 && step_ > 1) {
      go(step_ - 1);
      return true;
    }
    return false;
  }

private:
  // ------------------------------------------------------------- steps
  void set_step(int step) {
    step_ = std::clamp(step, 1, step_count);
    next_label_ = owner_->tr(step_ == step_count ? "Finish" : "Next");
    progress_ = two_digits(std::size_t(step_)) + " / " +
                two_digits(std::size_t(step_count));
    handle_.DirtyVariable("step");
    handle_.DirtyVariable("next_label");
    handle_.DirtyVariable("progress");
  }

  void go(int step) {
    if (step < 1 || step > step_count || step == step_) {
      return;
    }
    log_info("launcher.first_run.step", std::to_string(step));
    set_step(step);
    if (step_ == 2) {
      scan_import(); // the game folder may have changed on step 1
      refresh();
    }
    focus_in_ = 2;
  }

  void finish() {
    services &svc = owner_->svc();
    svc.config().first_run_done = true;
    if (!svc.save_config()) {
      owner_->notify(owner_->tr("Xerces could not save its settings. This "
                                "setup will show again next time."),
                     true);
    }
    log_info("launcher.first_run.done",
             svc.game().dir ? "game found" : "no game folder");
    owner_->show("home");
  }

  void act(const std::string &action) {
    log_info("launcher.first_run.action", action);
    services &svc = owner_->svc();
    if (action == "next") {
      if (step_ == step_count) {
        finish();
      } else {
        go(step_ + 1);
      }
    } else if (action == "back") {
      go(step_ - 1);
    } else if (action == "skip") {
      finish();
    } else if (action == "choose") {
      // pick_folder translates its title itself: pass the English key.
      const auto picked = owner_->pick_folder("Choose the Black Ops III folder");
      if (!picked) {
        return;
      }
      if (!svc.set_game_dir(*picked)) {
        owner_->notify(svc.game().error.empty()
                           ? owner_->tr("That folder has no BlackOps3.exe.")
                           : svc.game().error,
                       true);
        return;
      }
      detect_requested_ = true;
      detect_from_rev_ = svc.game_rev();
      owner_->notify(owner_->tr("Checking the game in") + " " + utf8(*picked));
    } else if (action == "detect") {
      if (looking_) {
        return;
      }
      detect_requested_ = true;
      detect_from_rev_ = svc.game_rev();
      svc.refresh(true);
    } else if (action == "import") {
      import_now();
    } else if (action == "motion") {
      svc.config().reduced_motion = !svc.config().reduced_motion;
      save_look();
    }
  }

  // ------------------------------------------------------------ import
  void scan_import() {
    const auto &game = owner_->svc().game();
    found_ = xerces_core::find_boiii_import(boiii_appdata(), game.dir);
    for (const auto &item : found_.diags.items()) {
      log_info("launcher.first_run.import_note", item.message + " " + item.path);
    }
  }

  void toggle(const std::string &key) {
    for (const import_row &row : imports_) {
      if (row.key == key && row.done) {
        owner_->notify(row.done_note.empty() ? owner_->tr("Already in Xerces.")
                                             : row.done_note);
        return;
      }
    }
    if (key == "name") {
      choice_.player_name = !choice_.player_name;
    } else if (key == "favorites") {
      choice_.favorites = !choice_.favorites;
    } else if (key == "recent") {
      choice_.recent_servers = !choice_.recent_servers;
    }
    refresh();
  }

  void import_now() {
    if (!can_import_) {
      owner_->notify(owner_->tr("Nothing selected to import."));
      return;
    }
    services &svc = owner_->svc();
    const xerces_core::boiii_import_result result =
        xerces_core::apply_boiii_import(svc.config(), found_, choice_);
    if (!result.changed()) {
      owner_->notify(owner_->tr("Nothing new to import."));
      refresh();
      return;
    }
    if (!svc.save_config()) {
      owner_->notify(owner_->tr("The import could not be saved."), true);
      return;
    }
    std::string text = owner_->tr("Imported from BOIII:");
    std::string sep = " ";
    if (result.player_name) {
      text += sep + owner_->tr("player name");
      sep = ", ";
    }
    if (result.favorites > 0) {
      text += sep + std::to_string(result.favorites) + " " +
              owner_->tr(result.favorites == 1 ? "favourite server"
                                               : "favourite servers");
      sep = ", ";
    }
    if (result.recent_servers > 0) {
      text += sep + std::to_string(result.recent_servers) + " " +
              owner_->tr(result.recent_servers == 1 ? "recent server"
                                                    : "recent servers");
    }
    log_info("launcher.first_run.imported", text);
    owner_->notify(text);
    refresh();
    // The import button goes away once everything is in: move on to Next.
    if (Rml::Element *next = doc ? doc->GetElementById("fr-next") : nullptr) {
      next->Focus(true);
      next->ScrollIntoView(Rml::ScrollAlignment::Adaptive);
    }
  }

  // -------------------------------------------------------------- look
  void set_theme(const std::string &theme) {
    if (theme != "dark" && theme != "light" && theme != "contrast") {
      return;
    }
    owner_->svc().config().theme = theme;
    save_look();
  }

  void set_scale(int percent) {
    if (percent != 0 && (percent < 75 || percent > 200)) {
      return;
    }
    owner_->svc().config().ui_scale_percent = percent;
    save_look();
  }

  void save_look() {
    if (!owner_->svc().save_config()) {
      owner_->notify(owner_->tr("The look could not be saved."), true);
    }
    owner_->apply_look();
    refresh();
  }

  // ----------------------------------------------------------- refresh
  void refresh() {
    services &svc = owner_->svc();
    const auto &config = svc.config();
    const auto &game = svc.game();

    // 01 Game.
    looking_ = svc.detecting() || detect_requested_ || !game.searched;
    game_found_ = game.dir.has_value();
    game_ok_ = game.can_launch();
    game_bad_ = game.searched && !looking_ && !game_ok_;
    game_path_ = game.dir ? utf8(*game.dir) : "";
    game_build_.clear();
    game_verdict_.clear();
    game_known_as_.clear();
    game_message_.clear();
    game_error_ = game.error;
    tried_.clear();
    if (looking_) {
      game_title_ = owner_->tr("Looking for Black Ops III...");
    } else if (!game.dir) {
      game_title_ = owner_->tr("Black Ops III was not found");
      for (std::size_t i = 0; i < game.tried.size(); ++i) {
        tried_.push_back({two_digits(i + 1), game.tried[i]});
      }
    } else {
      game_title_ = owner_->tr(game_ok_ ? "Found, ready to play"
                                        : "Found, but it cannot start yet");
    }
    if (game.dir) {
      game_build_ = game.steam_build ? std::to_string(*game.steam_build)
                                     : owner_->tr("Unknown");
      if (game.preflight) {
        game_verdict_ = owner_->tr(verdict_name(game.preflight->verdict));
        game_message_ = game.preflight->message;
        game_known_as_ = game.preflight->build ? game.preflight->build->name
                                               : owner_->tr("Not a listed build");
      } else {
        game_verdict_ = owner_->tr("Not checked");
        game_known_as_ = "-";
      }
    }

    // 02 Import.
    places_.clear();
    for (const auto &place : found_.places) {
      place_row row;
      row.path = utf8(place.path);
      if (place.kind == "appdata") {
        row.name = "%LOCALAPPDATA%\\boiii";
        row.state = place.exists
                        ? owner_->tr("Found. Nothing here maps onto Xerces "
                                     "settings. Its user folder, where BOIII "
                                     "keeps your identity key, is never opened.")
                        : owner_->tr("Not on this PC.");
      } else {
        row.name = "boiii_players";
        row.state = place.exists ? owner_->tr("Found in the game folder.")
                                 : owner_->tr("Not in the game folder.");
      }
      places_.push_back(std::move(row));
    }
    if (!game.dir) {
      places_.push_back({"boiii_players", owner_->tr("In the game folder"),
                         owner_->tr("Needs the game folder from step 01.")});
    }

    imports_.clear();
    const auto relative = [&](const fs::path &file) {
      const fs::path players = game.dir ? *game.dir / "boiii_players" : fs::path();
      const fs::path rel = players.empty() ? file : file.lexically_relative(players);
      return "boiii_players\\" + utf8(rel.empty() ? file : rel);
    };
    if (found_.player_name) {
      import_row row;
      row.key = "name";
      row.title = owner_->tr("Player name");
      row.value = *found_.player_name;
      row.done = config.player_name == *found_.player_name;
      row.on = choice_.player_name && !row.done;
      row.detail = row.done ? owner_->tr("Already your name in Xerces")
                            : relative(found_.player_name_file);
      if (!row.done && !config.player_name.empty()) {
        row.detail += " \xC2\xB7 " + owner_->tr("replaces") + " " +
                      config.player_name;
      }
      imports_.push_back(std::move(row));
    }
    if (!found_.favorites.empty()) {
      const std::size_t fresh = xerces_core::new_boiii_favorites(config, found_);
      import_row row;
      row.key = "favorites";
      row.title = owner_->tr("Favourite servers");
      row.value = std::to_string(found_.favorites.size());
      row.done = fresh == 0;
      row.on = choice_.favorites && !row.done;
      row.detail = row.done ? owner_->tr("All already in Xerces")
                            : relative(found_.favorites_file);
      if (!row.done && fresh != found_.favorites.size()) {
        row.detail += " \xC2\xB7 " + std::to_string(fresh) + " " + owner_->tr("new");
      }
      imports_.push_back(std::move(row));
    }
    if (!found_.recent_servers.empty()) {
      // Only what fits in Xerces' recent list counts: the import stops there.
      std::size_t no_room = 0;
      const std::size_t fresh =
          xerces_core::new_boiii_recent_servers(config, found_, &no_room);
      import_row row;
      row.key = "recent";
      row.title = owner_->tr("Recently joined servers");
      row.value = std::to_string(found_.recent_servers.size());
      row.done = fresh == 0;
      row.on = choice_.recent_servers && !row.done;
      if (row.done) {
        row.detail = owner_->tr(no_room > 0 ? "Xerces' recent list is full"
                                            : "All already in Xerces");
        if (no_room > 0) {
          row.done_note = owner_->tr("Xerces' recent list is full: there is "
                                     "no room for more servers.");
        }
      } else {
        row.detail = relative(found_.recent_file);
        if (fresh != found_.recent_servers.size()) {
          row.detail +=
              " \xC2\xB7 " + std::to_string(fresh) + " " + owner_->tr("new");
        }
      }
      imports_.push_back(std::move(row));
    }
    can_import_ = std::ranges::any_of(
        imports_, [](const import_row &row) { return row.on && !row.done; });
    const bool all_done =
        !imports_.empty() && std::ranges::all_of(imports_, [](const import_row &row) {
          return row.done;
        });
    import_done_ = all_done;
    import_empty_ =
        game.dir ? owner_->tr("Nothing from BOIII to import. That is fine: "
                              "continue with Next.")
                 : owner_->tr("Nothing from BOIII found yet. With the game "
                              "folder set, its boiii_players is checked too.");
    import_notes_.clear();
    if (found_.skipped_lines > 0) {
      import_notes_.push_back(
          std::to_string(found_.skipped_lines) + " " +
          owner_->tr(found_.skipped_lines == 1
                         ? "server line was not host:port and is left out."
                         : "server lines were not host:port and are left out."));
    }
    for (const auto &item : found_.diags.items()) {
      if (item.level == xerces_core::severity::warning) {
        // The core's messages are fixed English sentences: translation keys.
        import_notes_.push_back(owner_->tr(item.message) + " " + item.path);
      }
    }

    // 03 Look.
    theme_ = config.theme;
    scale_ = config.ui_scale_percent;
    reduced_ = config.reduced_motion;
    const int windows = int(owner_->win().dp_ratio() * 100.f + 0.5f);
    scale_help_ = owner_->tr("Windows' display scale here is") + " " +
                  std::to_string(windows) + " %.";
    if (scale_ != 0) {
      scale_help_ += " " + owner_->tr("Xerces draws at") + " " +
                     std::to_string(scale_) + " % " + owner_->tr("of that.");
    }
    if (owner_->opts().scale > 0.f) {
      scale_help_ += " " + owner_->tr("A --scale option overrides this now.");
    }

    // 04 Privacy.
    home_path_ = utf8(svc.paths().home);

    handle_.DirtyAllVariables();
  }

  app *owner_ = nullptr;
  Rml::Context *context_ = nullptr;
  Rml::DataModelHandle handle_;
  std::uint64_t seen_[3] = {};
  int focus_in_ = 0;
  bool focus_force_ = false;
  bool detect_requested_ = false;
  std::uint64_t detect_from_rev_ = 0;

  xerces_core::boiii_import found_;
  xerces_core::boiii_import_choice choice_;

  int step_ = 1;
  Rml::String next_label_ = "Next", progress_ = "01 / 04";

  bool looking_ = true, game_found_ = false, game_ok_ = false, game_bad_ = false;
  Rml::String game_title_, game_path_, game_build_, game_verdict_,
      game_known_as_, game_message_, game_error_;
  Rml::Vector<path_row> tried_;

  Rml::Vector<place_row> places_;
  Rml::Vector<import_row> imports_;
  Rml::Vector<Rml::String> import_notes_;
  bool can_import_ = false;
  bool import_done_ = false;
  Rml::String import_empty_;

  Rml::String theme_ = "dark";
  int scale_ = 0;
  bool reduced_ = false;
  Rml::String scale_help_, home_path_;
};
} // namespace

XERCES_VIEW(first_run_view, "first_run", 5, false);
} // namespace xerces_launcher
