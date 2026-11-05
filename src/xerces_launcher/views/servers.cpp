// Servers (docs/DESIGN.md 7.3, 11.1; D14): join a self-hosted or LAN server
// by its address, favourites and recent servers. There is no matchmaking and
// no network code here: a join only hands "+connect host:port" to the game
// through services::launch. The public list stays in the game's own server
// browser until Xerces' backend lists servers (M6).

#include <RmlUi/Core.h>

#include <algorithm>
#include <optional>
#include <string>

#include <xerces_core/launch.hpp>
#include <xerces_core/servers_book.hpp>

#include "../app.hpp"
#include "../log.hpp"

namespace xerces_launcher {
namespace {
struct server_row {
  Rml::String name, address;
  bool saved = false; // a recent server that is also a favourite
};

const char *mode_name(const std::string &mode) {
  return mode == "mp" ? "Multiplayer" : mode == "cp" ? "Campaign" : "Zombies";
}

// A name short enough for the one-line footer notice: at most 28 bytes, cut
// at a UTF-8 character boundary, with "..." when it was longer.
std::string short_name(const std::string &name) {
  constexpr std::size_t limit = 28;
  if (name.size() <= limit) {
    return name;
  }
  std::size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  return name.substr(0, cut) + "...";
}

// What a favourite is called on screen: its name, or its address when a
// hand-edited launcher.json left the name empty.
const std::string &shown_name(const xerces_core::favorite_server &server) {
  return server.name.empty() ? server.address : server.name;
}

bool is_text_field(const Rml::Element *element) {
  return element && element->GetTagName() == "input" &&
         element->GetAttribute<Rml::String>("type", "text") == "text";
}

class servers_view final : public view {
public:
  const char *id() const override { return "servers"; }

  bool bind(Rml::Context *context, app &owner) override {
    owner_ = &owner;
    Rml::DataModelConstructor model = context->CreateDataModel("servers");
    if (!model) {
      return false;
    }
    if (auto row = model.RegisterStruct<server_row>()) {
      row.RegisterMember("name", &server_row::name);
      row.RegisterMember("address", &server_row::address);
      row.RegisterMember("saved", &server_row::saved);
    }
    model.RegisterArray<Rml::Vector<server_row>>();
    model.RegisterArray<Rml::Vector<Rml::String>>();

    model.Bind("mode", &mode_);
    model.Bind("address", &address_);
    model.Bind("address_problem", &address_problem_);
    model.Bind("enter_modded", &enter_modded_);
    model.Bind("join_note", &join_note_);
    model.Bind("game_problem", &game_problem_);

    model.Bind("has_result", &has_result_);
    model.Bind("result_ok", &result_ok_);
    model.Bind("result_label", &result_label_);
    model.Bind("result_target", &result_target_);
    model.Bind("result_message", &result_message_);
    model.Bind("result_lines", &result_lines_);
    model.Bind("result_command", &result_command_);
    model.Bind("result_mods", &result_mods_);

    model.Bind("favorites", &favorites_);
    model.Bind("fav_count", &fav_count_);
    model.Bind("fav_name", &fav_name_);
    model.Bind("fav_address", &fav_address_);
    model.Bind("fav_problem", &fav_problem_);
    model.Bind("editing", &editing_);
    model.Bind("form_label", &form_label_);
    model.Bind("form_button", &form_button_);
    model.Bind("name_placeholder", &name_placeholder_);

    model.Bind("recents", &recents_);
    model.Bind("recent_count", &recent_count_);

    model.BindEventCallback("act", [this](Rml::DataModelHandle, Rml::Event &,
                                          const Rml::VariantList &args) {
      if (!args.empty()) {
        act(args[0].Get<Rml::String>());
      }
    });
    model.BindEventCallback("pick", [this](Rml::DataModelHandle, Rml::Event &,
                                           const Rml::VariantList &args) {
      if (!args.empty()) {
        owner_->set_mode(args[0].Get<Rml::String>());
      }
    });
    model.BindEventCallback("fav", [this](Rml::DataModelHandle, Rml::Event &,
                                          const Rml::VariantList &args) {
      if (args.size() >= 2) {
        favorite_action(args[0].Get<Rml::String>(), args[1].Get<int>());
      }
    });
    model.BindEventCallback("recent", [this](Rml::DataModelHandle, Rml::Event &,
                                             const Rml::VariantList &args) {
      if (args.size() >= 2) {
        recent_action(args[0].Get<Rml::String>(), args[1].Get<int>());
      }
    });
    // Problems are shown once the player tried to use the address, then
    // follow every key so they disappear as soon as it is fixed; showing
    // them from the first key would complain about every half-typed address.
    model.BindEventCallback("typed", [this](Rml::DataModelHandle,
                                            Rml::Event &event,
                                            const Rml::VariantList &) {
      if (!address_checked_) {
        return;
      }
      const Rml::String value = event.GetParameter<Rml::String>("value", "");
      set_address_problem(xerces_core::trim_server_address(value));
    });
    model.BindEventCallback(
        "fav_typed",
        [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) {
          if (!fav_problem_.empty()) {
            fav_problem_.clear();
            handle_.DirtyVariable("fav_problem");
          }
        });
    handle_ = model.GetModelHandle();
    form_label_ = owner_->tr("Add a favourite");
    form_button_ = owner_->tr("Add favourite");
    name_placeholder_ = owner_->tr("Name (optional)");
    return true;
  }

  void shown() override {
    // Seen again: the last attempt's panel stays, but focus restoring from
    // an earlier visit must not jump around.
    pending_focus_.clear();
    pending_scroll_.clear();
  }

  void update(double) override {
    services &svc = owner_->svc();
    const std::uint64_t key[] = {svc.config_rev(), svc.game_rev(),
                                 svc.catalog_rev(), svc.journal_rev(),
                                 owner_->mode_rev()};
    if (!std::equal(std::begin(key), std::end(key), std::begin(seen_))) {
      std::copy(std::begin(key), std::end(key), std::begin(seen_));
      refresh();
    }
    // Focus waits for the data views: rows added or removed this frame only
    // exist (or are gone) after the context's next update.
    if (!pending_scroll_.empty() && --scroll_frames_ <= 0) {
      Rml::Element *element =
          doc ? doc->GetElementById(pending_scroll_) : nullptr;
      pending_scroll_.clear();
      if (element) {
        element->ScrollIntoView(Rml::ScrollIntoViewOptions(
            Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest));
      }
    }
    if (!pending_focus_.empty() && --pending_frames_ <= 0) {
      const std::string target = std::move(pending_focus_);
      pending_focus_.clear();
      if (owner_->current() == id() && !owner_->dialog_open()) {
        focus_now(target);
      }
    }
  }

  bool key(Rml::Input::KeyIdentifier key, int modifiers) override {
    Rml::Context *context = owner_->context();
    Rml::Element *focus = context ? context->GetFocusElement() : nullptr;
    if (!doc || (focus && focus->GetOwnerDocument() != doc)) {
      return false;
    }
    const Rml::String focus_id = focus ? focus->GetId() : Rml::String();
    const bool in_field = is_text_field(focus);
    const bool plain =
        (modifiers & (Rml::Input::KM_CTRL | Rml::Input::KM_ALT)) == 0;

    if (plain &&
        (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER)) {
      if (focus_id == "address") {
        join(field_value("address"), enter_modded_);
        return true;
      }
      if (focus_id == "fav-name" || focus_id == "fav-address") {
        save_form();
        return true;
      }
    }
    // Esc leaves a field for the button next to it, so a second Esc reaches
    // the tabs (the app would send it to the default focus, which is where
    // the player already is when that is a field).
    if (key == Rml::Input::KI_ESCAPE && in_field) {
      if (focus_id == "address") {
        focus_later("join-modded", 0);
        return true;
      }
      if (focus_id == "fav-name" || focus_id == "fav-address") {
        focus_later("fav-save", 0);
        return true;
      }
    }
    // "/" and Y go to the address field (this screen's only search-like
    // field). Focus moves after the key's own character was delivered, or
    // the "/" would be typed into the field.
    if (key == Rml::Input::KI_OEM_2 && !in_field) {
      focus_later("address", 1);
      return true;
    }
    return false;
  }

private:
  // ------------------------------------------------------------- refresh
  void refresh() {
    services &svc = owner_->svc();
    const auto &config = svc.config();
    mode_ = owner_->mode();

    const std::size_t requested = config.profiles.contains(mode_)
                                      ? config.profiles.at(mode_).mods.size()
                                      : 0;
    enter_modded_ = requested > 0;
    const std::string profile =
        owner_->tr(mode_name(mode_)) + " " + owner_->tr("profile");
    if (requested == 0) {
      join_note_ = profile + " \xC2\xB7 " + owner_->tr("no mods enabled") +
                   " \xC2\xB7 " + owner_->tr("Enter joins vanilla");
    } else {
      const auto &catalog = svc.catalog();
      std::string loads;
      if (!catalog.scanned && catalog.scanning) {
        loads = owner_->tr("reading your mods...");
      } else {
        const xerces_core::plan plan = svc.resolve(mode_);
        loads = std::to_string(plan.stack.size()) + " " +
                owner_->tr(plan.stack.size() == 1 ? "mod" : "mods");
        if (!plan.ok()) {
          loads += " \xC2\xB7 " + owner_->tr("the plan has problems");
        }
      }
      join_note_ = profile + " \xC2\xB7 " + loads + " \xC2\xB7 " +
                   owner_->tr("Enter joins modded");
    }

    const auto &game = svc.game();
    if (!game.searched || game.can_launch()) {
      game_problem_.clear();
    } else if (!game.dir) {
      game_problem_ = owner_->tr(
          "Black Ops III was not found. Set its folder in Settings to join.");
    } else if (game.preflight) {
      game_problem_ = owner_->tr(game.preflight->message);
    } else {
      game_problem_ = owner_->tr(
          game.error.empty() ? "The game cannot be started." : game.error);
    }

    favorites_.clear();
    for (const auto &favorite : config.favorites) {
      favorites_.push_back({shown_name(favorite), favorite.address, true});
    }
    fav_count_ = favorites_.empty() ? owner_->tr("None saved")
                                    : std::to_string(favorites_.size()) + " " +
                                          owner_->tr("saved");
    recents_.clear();
    for (const auto &address : config.recent_servers) {
      recents_.push_back(
          {address, address,
           xerces_core::find_favorite(config.favorites, address).has_value()});
    }
    recent_count_ = std::to_string(recents_.size()) + " " + owner_->tr("of") +
                    " " + std::to_string(xerces_core::max_recent_servers);

    // An edit whose favourite vanished (removed meanwhile) becomes an add.
    if (editing_ && editing_index_ >= config.favorites.size()) {
      stop_editing();
    }

    // Not DirtyAllVariables: the text fields' variables follow what is
    // typed and must not be written back mid-word.
    for (const char *name :
         {"mode", "enter_modded", "join_note", "game_problem", "favorites",
          "fav_count", "recents", "recent_count"}) {
      handle_.DirtyVariable(name);
    }
  }

  // ----------------------------------------------------------- joining
  // True when the game was started (or the dry run logged); false when the
  // address or the launch was refused, or a dialog asks first.
  bool join(const std::string &typed, bool modded, bool mods_off = false) {
    const std::string address = xerces_core::trim_server_address(typed);
    if (address != address_) {
      address_ = address;
      handle_.DirtyVariable("address");
    }
    address_checked_ = true;
    if (address.empty()) {
      address_problem_ =
          owner_->tr("Type the server's address first, for example "
                     "192.168.1.20:27017.");
      handle_.DirtyVariable("address_problem");
      owner_->notify(address_problem_, true);
      focus_later("address", 0);
      return false;
    }
    if (!xerces_core::valid_server_address(address)) {
      set_address_problem(address);
      owner_->notify(address_problem_, true);
      focus_later("address", 0);
      return false;
    }
    set_address_problem(address);

    services &svc = owner_->svc();
    if (modded && !mods_off) {
      const xerces_core::safe_mode_decision decision = svc.safe_mode();
      if (decision.safe_mode) {
        owner_->dialog(
            "The last modded launch crashed", decision.message,
            {"Join with mods off", "Join with mods anyway", "Cancel"},
            [this, address](int choice) {
              if (choice == 0) {
                join(address, true, true);
              } else if (choice == 1) {
                launch_join(address, true, false);
              }
            });
        return false;
      }
    }
    return launch_join(address, modded, mods_off);
  }

  bool launch_join(const std::string &address, bool modded, bool mods_off) {
    services &svc = owner_->svc();
    const std::string mode = owner_->mode();
    log_info("launcher.servers.join", address + (mods_off ? " mods off"
                                                 : modded ? " modded"
                                                          : " vanilla"));
    const launch_outcome outcome = svc.launch(mode, modded, mods_off, address);
    const bool ok = outcome.started || outcome.dry_run;

    has_result_ = true;
    result_ok_ = ok;
    result_label_ = outcome.started   ? owner_->tr("Joining")
                    : outcome.dry_run ? owner_->tr("Dry launch")
                                      : owner_->tr("Could not join");
    result_target_ = address + " \xC2\xB7 " + owner_->tr(mode_name(mode)) +
                     " \xC2\xB7 " +
                     owner_->tr(mods_off ? "mods off"
                                : modded ? "modded"
                                         : "vanilla");
    result_message_ = owner_->tr(outcome.message);
    result_lines_.clear();
    for (const auto &item : outcome.diags.items()) {
      if (item.message != outcome.message &&
          std::find(result_lines_.begin(), result_lines_.end(),
                    owner_->tr(item.message)) == result_lines_.end()) {
        result_lines_.push_back(owner_->tr(item.message));
      }
    }
    result_command_ = outcome.command_line;
    // A refused modded join is almost always about the plan: offer Mods.
    result_mods_ =
        !ok && modded && !mods_off &&
        std::ranges::any_of(outcome.diags.items(), [](const auto &item) {
          return !item.code.starts_with("launch.") ||
                 item.code.starts_with("launch.plan");
        });
    for (const char *name :
         {"has_result", "result_ok", "result_label", "result_target",
          "result_message", "result_lines", "result_command", "result_mods"}) {
      handle_.DirtyVariable(name);
    }

    if (!ok) {
      owner_->notify(result_message_, true);
      return false;
    }
    // Every join that got as far as the game (or the dry-run log) is a
    // recent server, newest first.
    auto &config = svc.config();
    if (xerces_core::remember_recent_server(config.recent_servers, address) &&
        !svc.save_config()) {
      owner_->notify(owner_->tr("The recent list could not be saved:") + " " +
                         svc.config_error(),
                     true);
    } else {
      owner_->notify(result_message_);
    }
    if (outcome.started && config.close_on_launch) {
      owner_->win().request_close();
    }
    return true;
  }

  void set_address_problem(const std::string &address) {
    const std::string problem = xerces_core::server_address_problem(address);
    address_problem_ = problem.empty() ? "" : owner_->tr(problem);
    handle_.DirtyVariable("address_problem");
  }

  // -------------------------------------------------------- favourites
  void favorite_action(const std::string &action, int index) {
    services &svc = owner_->svc();
    auto &favorites = svc.config().favorites;
    if (index < 0 || std::size_t(index) >= favorites.size()) {
      return;
    }
    const xerces_core::favorite_server server = favorites[std::size_t(index)];
    log_info("launcher.servers.favorite", action + " " + server.address);
    if (action == "join") {
      join(server.address, enter_modded_);
    } else if (action == "edit") {
      editing_ = true;
      editing_index_ = std::size_t(index);
      fav_name_ = server.name;
      fav_address_ = server.address;
      fav_problem_.clear();
      form_label_ = owner_->tr("Edit favourite");
      form_button_ = owner_->tr("Save changes");
      for (const char *name : {"editing", "fav_name", "fav_address",
                               "fav_problem", "form_label", "form_button"}) {
        handle_.DirtyVariable(name);
      }
      focus_later("fav-name", 1);
    } else if (action == "remove") {
      // A hand-edited launcher.json can hold a favourite without a name.
      const std::string shown =
          server.name.empty() || server.name == server.address
              ? server.address
              : server.name + " (" + server.address + ")";
      owner_->dialog(
          "Remove this favourite?", shown, {"Remove", "Cancel"},
          [this, server](int choice) {
            if (choice != 0) {
              return;
            }
            services &s = owner_->svc();
            auto &list = s.config().favorites;
            const auto found = xerces_core::find_favorite(list, server.address);
            if (!found) {
              return;
            }
            list.erase(list.begin() + std::ptrdiff_t(*found));
            if (editing_ && editing_index_ == *found) {
              stop_editing();
            } else if (editing_ && editing_index_ > *found) {
              --editing_index_;
            }
            if (!s.save_config()) {
              owner_->notify(owner_->tr("Favourites could not be saved:") +
                                 " " + s.config_error(),
                             true);
              return;
            }
            owner_->notify(owner_->tr("Removed from favourites:") + " " +
                           short_name(shown_name(server)));
            focus_after_removal("fav-remove-", *found, list.size(), "fav-save");
          });
    }
  }

  void save_form() {
    const std::string name = field_value("fav-name");
    const std::string address = field_value("fav-address");
    fav_name_ = name;
    fav_address_ = address;
    services &svc = owner_->svc();
    auto &favorites = svc.config().favorites;
    const bool was_editing = editing_;
    const xerces_core::favorite_edit edit = xerces_core::save_favorite(
        favorites, name, address,
        editing_ ? std::optional<std::size_t>(editing_index_) : std::nullopt);
    switch (edit.status) {
    case xerces_core::favorite_status::saved:
      break;
    case xerces_core::favorite_status::bad_address: {
      const std::string trimmed = xerces_core::trim_server_address(address);
      show_form_problem(
          trimmed.empty()
              ? owner_->tr("Type the server's address, for example "
                           "192.168.1.20:27017.")
              : owner_->tr(xerces_core::server_address_problem(trimmed)));
      focus_later("fav-address", 0);
      return;
    }
    case xerces_core::favorite_status::duplicate:
      show_form_problem(owner_->tr("Already in your favourites as") + " \"" +
                            shown_name(favorites[edit.index]) + "\".",
                        owner_->tr("Already in your favourites."));
      return;
    case xerces_core::favorite_status::full:
      show_form_problem(owner_->tr(
          "Favourites are full (100). Remove one before adding another."));
      return;
    case xerces_core::favorite_status::missing:
      stop_editing();
      show_form_problem(
          owner_->tr("That favourite was removed. Add it again."));
      return;
    }
    if (!svc.save_config()) {
      show_form_problem(owner_->tr("Favourites could not be saved:") + " " +
                        svc.config_error());
      return;
    }
    owner_->notify(
        owner_->tr(was_editing ? "Favourite saved:" : "Added to favourites:") +
        " " + short_name(shown_name(favorites[edit.index])));
    stop_editing();
    focus_later("fav-join-" + std::to_string(edit.index), 2);
  }

  // Saves an address as a favourite named after itself (Edit renames it).
  void quick_save(const std::string &typed, const char *problem_field) {
    const std::string address = xerces_core::trim_server_address(typed);
    services &svc = owner_->svc();
    auto &favorites = svc.config().favorites;
    const xerces_core::favorite_edit edit =
        xerces_core::save_favorite(favorites, "", address);
    switch (edit.status) {
    case xerces_core::favorite_status::saved:
      if (!svc.save_config()) {
        owner_->notify(owner_->tr("Favourites could not be saved:") + " " +
                           svc.config_error(),
                       true);
        return;
      }
      owner_->notify(owner_->tr("Saved to favourites. Use Edit to name it:") +
                     " " + short_name(address));
      return;
    case xerces_core::favorite_status::duplicate:
      owner_->notify(owner_->tr("Already in your favourites as") + " \"" +
                         short_name(shown_name(favorites[edit.index])) + "\".",
                     true);
      return;
    case xerces_core::favorite_status::full:
      owner_->notify(owner_->tr("Favourites are full (100). Remove one before "
                                "adding another."),
                     true);
      return;
    default:
      if (std::string(problem_field) == "address") {
        address_checked_ = true;
        if (address.empty()) {
          address_problem_ =
              owner_->tr("Type the server's address first, for example "
                         "192.168.1.20:27017.");
          handle_.DirtyVariable("address_problem");
        } else {
          set_address_problem(address);
        }
        owner_->notify(address_problem_, true);
        focus_later("address", 0);
      } else {
        owner_->notify(owner_->tr("Not a server address:") + " " +
                           short_name(address),
                       true);
      }
      return;
    }
  }

  // Under the form and in the footer; the form scrolls up so the line
  // under it shows (it only exists after the next data update).
  void show_form_problem(const std::string &problem,
                         const std::string &notice = {}) {
    fav_problem_ = problem;
    handle_.DirtyVariable("fav_problem");
    owner_->notify(notice.empty() ? problem : notice, true);
    pending_scroll_ = "fav-form";
    scroll_frames_ = 2;
  }

  void stop_editing() {
    editing_ = false;
    editing_index_ = 0;
    fav_name_.clear();
    fav_address_.clear();
    fav_problem_.clear();
    form_label_ = owner_->tr("Add a favourite");
    form_button_ = owner_->tr("Add favourite");
    for (const char *name : {"editing", "fav_name", "fav_address",
                             "fav_problem", "form_label", "form_button"}) {
      handle_.DirtyVariable(name);
    }
  }

  // ------------------------------------------------------------ recents
  void recent_action(const std::string &action, int index) {
    services &svc = owner_->svc();
    auto &recent = svc.config().recent_servers;
    if (index < 0 || std::size_t(index) >= recent.size()) {
      return;
    }
    const std::string address = recent[std::size_t(index)];
    log_info("launcher.servers.recent", action + " " + address);
    if (action == "join") {
      // A joined server moves to the top: focus follows its Join button.
      if (join(address, enter_modded_) && index != 0) {
        focus_later("recent-join-0", 2);
      }
    } else if (action == "save") {
      quick_save(address, "recent");
    } else if (action == "remove") {
      xerces_core::forget_recent_server(recent, address);
      if (!svc.save_config()) {
        owner_->notify(owner_->tr("The recent list could not be saved:") + " " +
                           svc.config_error(),
                       true);
        return;
      }
      owner_->notify(owner_->tr("Removed from recent servers:") + " " +
                     short_name(address));
      focus_after_removal("recent-remove-", std::size_t(index), recent.size(),
                          "join-modded");
    }
  }

  // ------------------------------------------------------------ actions
  void act(const std::string &action) {
    log_info("launcher.servers.action", action);
    if (action == "join modded") {
      join(field_value("address"), true);
    } else if (action == "join vanilla") {
      join(field_value("address"), false);
    } else if (action == "save address") {
      quick_save(field_value("address"), "address");
    } else if (action == "save favorite") {
      save_form();
    } else if (action == "cancel edit") {
      stop_editing();
      focus_later("fav-save", 1);
    } else if (action == "clear recent") {
      if (owner_->svc().config().recent_servers.empty()) {
        return;
      }
      owner_->dialog("Clear recent servers?",
                     "The list of servers you joined is emptied. Favourites "
                     "stay.",
                     {"Clear", "Cancel"}, [this](int choice) {
                       if (choice != 0) {
                         return;
                       }
                       services &svc = owner_->svc();
                       svc.config().recent_servers.clear();
                       if (!svc.save_config()) {
                         owner_->notify(
                             owner_->tr("The recent list could not be saved:") +
                                 " " + svc.config_error(),
                             true);
                         return;
                       }
                       owner_->notify(owner_->tr("Recent servers cleared."));
                       focus_later("join-modded", 2);
                     });
    } else if (action == "settings") {
      owner_->show("settings");
    } else if (action == "home") {
      owner_->show("home");
    } else if (action == "mods") {
      owner_->show("mods");
    }
  }

  // --------------------------------------------------------------- focus
  // After a row is removed, focus the same button on the row that took its
  // place (or the one above); with no rows left, the fallback.
  void focus_after_removal(const std::string &prefix, std::size_t removed,
                           std::size_t remaining, const char *fallback) {
    if (remaining == 0) {
      focus_later(fallback, 2);
      return;
    }
    const std::size_t next = std::min(removed, remaining - 1);
    focus_later(prefix + std::to_string(next), 2);
  }

  // frames 0 focuses now (nothing about the target changes this frame).
  void focus_later(const std::string &element_id, int frames) {
    if (frames <= 0) {
      pending_focus_.clear();
      focus_now(element_id);
      return;
    }
    pending_focus_ = element_id;
    pending_frames_ = frames;
  }

  void focus_now(const std::string &element_id) {
    Rml::Element *element = doc ? doc->GetElementById(element_id) : nullptr;
    if (!element) {
      return;
    }
    element->Focus(true);
    // As RmlUi's own spatial navigation does: centred when it was out of
    // view, so the whole row and the focus ring show, not just the button.
    element->ScrollIntoView(Rml::ScrollIntoViewOptions(
        Rml::ScrollAlignment::Adaptive, Rml::ScrollAlignment::Nearest));
  }

  std::string field_value(const char *element_id) const {
    Rml::Element *element = doc ? doc->GetElementById(element_id) : nullptr;
    return element ? element->GetAttribute<Rml::String>("value", "")
                   : std::string();
  }

  app *owner_ = nullptr;
  Rml::DataModelHandle handle_;
  std::uint64_t seen_[5] = {};
  std::string pending_focus_, pending_scroll_;
  int pending_frames_ = 0, scroll_frames_ = 0;
  bool address_checked_ = false;
  std::size_t editing_index_ = 0;

  Rml::String mode_ = "zm";
  Rml::String address_, address_problem_, join_note_, game_problem_;
  bool enter_modded_ = false;

  bool has_result_ = false, result_ok_ = false, result_mods_ = false;
  Rml::String result_label_, result_target_, result_message_, result_command_;
  Rml::Vector<Rml::String> result_lines_;

  Rml::Vector<server_row> favorites_;
  Rml::String fav_count_, fav_name_, fav_address_, fav_problem_;
  bool editing_ = false;
  Rml::String form_label_, form_button_, name_placeholder_;

  Rml::Vector<server_row> recents_;
  Rml::String recent_count_;
};
} // namespace

XERCES_VIEW(servers_view, "servers", 40, true);
} // namespace xerces_launcher
