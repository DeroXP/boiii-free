// Binds and asset pools (docs/DESIGN.md 7.3 parity table: upstream's launcher
// had both): two files in <game>\boiii_players\user that the client reads
// when it starts. Custom binds go to binds.cfg (component/binds.cpp runs them
// after the config); the larger asset pools are switched in
// launcher_settings.json (component/asset_limits.cpp grows the pools at
// DB_AssetPoolInit unless asset_limits_enabled is "0"). The rules for both
// files are xerces_core/game_files, the same as upstream's launcher, so
// either can edit them. Nothing is written while the game runs: it reads and
// writes binds.cfg itself. That includes a client Xerces did not start (run
// directly or from upstream's launcher), found by its process name
// (xerces_core::running_game_client). Opened from Settings; not a tab.

#include <RmlUi/Core.h>

#include <fstream>
#include <iterator>

#include <xerces_core/game_files.hpp>

#include "../app.hpp"
#include "../log.hpp"

namespace xerces_launcher {
namespace {
struct bind_row {
  Rml::String key, command;
};

std::optional<std::string> read_text(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

// Through a temporary file, so the game never reads half a file.
bool write_text(const fs::path &path, const std::string &text) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  fs::path temporary = path;
  temporary += ".xerces-tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out << text;
    if (!out.flush()) {
      return false;
    }
  }
  fs::rename(temporary, path, ec);
  return !ec;
}

class game_files_view final : public view {
public:
  const char *id() const override { return "game_files"; }

  bool bind(Rml::Context *context, app &owner) override {
    owner_ = &owner;
    Rml::DataModelConstructor model = context->CreateDataModel("game_files");
    if (!model) {
      return false;
    }
    if (auto row = model.RegisterStruct<bind_row>()) {
      row.RegisterMember("key", &bind_row::key);
      row.RegisterMember("command", &bind_row::command);
    }
    model.RegisterArray<Rml::Vector<bind_row>>();
    model.Bind("game_found", &game_found_);
    model.Bind("game_note", &game_note_);
    model.Bind("pools_on", &pools_on_);
    model.Bind("binds", &binds_);
    model.Bind("new_key", &new_key_);
    model.Bind("new_command", &new_command_);
    model.Bind("key_hint", &key_hint_);
    model.Bind("command_hint", &command_hint_);
    model.Bind("bind_error", &bind_error_);
    model.BindEventCallback(
        "act", [this](Rml::DataModelHandle, Rml::Event &,
                      const Rml::VariantList &args) {
          if (!args.empty()) {
            act(args[0].Get<Rml::String>());
          }
        });
    model.BindEventCallback(
        "remove", [this](Rml::DataModelHandle, Rml::Event &,
                         const Rml::VariantList &args) {
          if (!args.empty()) {
            remove(args[0].Get<Rml::String>());
          }
        });
    handle_ = model.GetModelHandle();
    // RmlUi translates text nodes only, never a placeholder attribute, so
    // the fields' placeholders come from the model.
    key_hint_ = owner_->tr("Key");
    command_hint_ = owner_->tr("Command, for example toggle cg_drawFPS");
    return true;
  }

  void shown() override { load(); }

  void update(double) override {
    const std::uint64_t key[] = {owner_->svc().game_rev(),
                                 owner_->svc().journal_rev()};
    if (!std::equal(std::begin(key), std::end(key), std::begin(seen_))) {
      std::copy(std::begin(key), std::end(key), std::begin(seen_));
      load();
    }
  }

  // Enter in either field adds the bind. Esc (B on a controller) goes back
  // to Settings, which opened this screen: it has no tab of its own for the
  // app's Esc to go to. In a field, Esc first only leaves the field (the app
  // does that).
  bool key(Rml::Input::KeyIdentifier key, int) override {
    Rml::Element *focus = owner_->context()->GetFocusElement();
    const bool in_field = focus && (focus->GetId() == "bind-key" ||
                                    focus->GetId() == "bind-command");
    if (in_field &&
        (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER)) {
      act("add");
      return true;
    }
    if (key == Rml::Input::KI_ESCAPE && !in_field && focus && doc &&
        focus->GetOwnerDocument() == doc) {
      act("settings");
      return true;
    }
    return false;
  }

private:
  std::optional<fs::path> folder() const {
    const auto &game = owner_->svc().game();
    if (!game.dir) {
      return std::nullopt;
    }
    return *game.dir / "boiii_players" / "user";
  }

  void load() {
    const auto dir = folder();
    game_found_ = dir.has_value();
    binds_.clear();
    if (!dir) {
      game_note_ = owner_->tr("Black Ops III was not found. Set its folder in "
                              "Settings first.");
      handle_.DirtyAllVariables();
      return;
    }
    game_note_ = game_running()
                     ? owner_->tr("The game is running: close it to change "
                                  "these. It reads them when it starts.")
                     : std::string();
    pools_on_ = xerces_core::asset_pools_enabled(
        read_text(*dir / "launcher_settings.json").value_or(""));
    for (const auto &bind : xerces_core::parse_binds(
             read_text(*dir / "binds.cfg").value_or(""))) {
      binds_.push_back({bind.key, bind.command});
    }
    handle_.DirtyAllVariables();
  }

  // A game Xerces started or that announces itself in <home>\run, or any
  // running client by its process name (one started directly or by
  // upstream's launcher announces nothing, and would write binds.cfg back
  // from its own copy after the next bind change in the game).
  bool game_running() {
    if (owner_->svc().game_running()) {
      return true;
    }
    const auto client = xerces_core::running_game_client(
        owner_->svc().paths().client.filename().string());
    if (client) {
      log_info("launcher.game_files.client_running", *client);
    }
    return client.has_value();
  }

  // Both files belong to the game while it runs.
  bool writable() {
    if (!folder()) {
      owner_->notify(game_note_, true);
      return false;
    }
    if (game_running()) {
      owner_->notify(owner_->tr("Close the game first: it reads and writes "
                                "these files while it runs."),
                     true);
      return false;
    }
    return true;
  }

  bool save_binds(const std::vector<xerces_core::game_bind> &binds) {
    if (!write_text(*folder() / "binds.cfg", xerces_core::binds_to_text(binds))) {
      owner_->notify(owner_->tr("binds.cfg could not be written."), true);
      return false;
    }
    return true;
  }

  std::vector<xerces_core::game_bind> current() const {
    std::vector<xerces_core::game_bind> out;
    for (const auto &row : binds_) {
      out.push_back({row.key, row.command});
    }
    return out;
  }

  void act(const std::string &action) {
    log_info("launcher.game_files.action", action);
    if (action == "settings") {
      owner_->show("settings");
      return;
    }
    if (action == "pools") {
      if (!writable()) {
        return;
      }
      // The file holds more than this switch (the pool sizes, friends_only):
      // one that cannot be read, or does not read as JSON, is left alone
      // instead of being replaced by a file with the switch only.
      const fs::path file = *folder() / "launcher_settings.json";
      using xerces_core::pool_switch;
      const pool_switch done = xerces_core::switch_asset_pools(file, !pools_on_);
      if (done != pool_switch::written) {
        log_warn("launcher.game_files.pools_refused",
                 done == pool_switch::unreadable      ? "unreadable"
                 : done == pool_switch::not_an_object ? "not a JSON object"
                                                      : "not written");
        owner_->notify(
            done == pool_switch::unreadable
                ? owner_->tr("launcher_settings.json could not be read, so it "
                             "was left as it is.")
            : done == pool_switch::not_an_object
                ? owner_->tr("launcher_settings.json is not valid JSON, so it "
                             "was left as it is. Fix or delete the file "
                             "first.")
                : owner_->tr("launcher_settings.json could not be written."),
            true);
        return;
      }
      load();
      owner_->notify(owner_->tr(pools_on_ ? "Larger asset pools are on."
                                          : "Larger asset pools are off."));
      return;
    }
    if (action == "add") {
      bind_error_.clear();
      std::string key = new_key_;
      std::string command = new_command_;
      // Plain keys are stored in lower case, as the game and upstream do.
      std::ranges::transform(key, key.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
      });
      if (!xerces_core::valid_bind_key(key)) {
        bind_error_ = owner_->tr("A key is 1 to 32 characters without spaces, "
                                 "quotes or ';', for example f, mouse4 or "
                                 "kp_plus.");
      } else if (!xerces_core::valid_bind_command(command)) {
        bind_error_ = owner_->tr("A command is 1 to 512 characters without "
                                 "quotes or line breaks.");
      } else if (writable()) {
        auto binds = current();
        std::erase_if(binds, [&](const auto &b) { return b.key == key; });
        binds.push_back({key, command});
        if (save_binds(binds)) {
          new_key_.clear();
          new_command_.clear();
          load();
          owner_->notify(owner_->tr("Saved. The game runs it when it starts."));
        }
      }
      handle_.DirtyAllVariables();
    }
  }

  void remove(const std::string &key) {
    if (!writable()) {
      return;
    }
    auto binds = current();
    std::erase_if(binds, [&](const auto &b) { return b.key == key; });
    if (save_binds(binds)) {
      load();
      owner_->notify(owner_->tr("Bind removed."));
      if (doc) {
        if (Rml::Element *add = doc->GetElementById("bind-key")) {
          add->Focus(true);
        }
      }
    }
  }

  app *owner_ = nullptr;
  Rml::DataModelHandle handle_;
  std::uint64_t seen_[2] = {};
  bool game_found_ = false;
  Rml::String game_note_;
  bool pools_on_ = true;
  Rml::Vector<bind_row> binds_;
  Rml::String new_key_, new_command_, bind_error_;
  Rml::String key_hint_, command_hint_; // placeholders, translated
};
} // namespace

XERCES_VIEW(game_files_view, "game_files", 65, false);
} // namespace xerces_launcher
