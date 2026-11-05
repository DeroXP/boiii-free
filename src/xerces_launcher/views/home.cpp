// Home (docs/DESIGN.md 7.3): the mode switch with its profile, Launch modded
// (the multi-mod plan), Launch vanilla, the dry-run preview, the load order
// and the status strip (game build, signatures, catalog, safe mode).

#include <RmlUi/Core.h>

#include <cstdio>

#include <xerces_core/compat.hpp>
#include <xerces_core/db_pool.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/settings_options.hpp>

#include "../app.hpp"
#include "../log.hpp"

namespace xerces_launcher {
namespace {
struct load_row {
  Rml::String n, name, v;
};
struct stat_cell {
  Rml::String n, l;
};

std::string two_digits(std::size_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%02zu", value);
  return text;
}

const char *mode_name(const std::string &mode) {
  return mode == "mp" ? "Multiplayer" : mode == "cp" ? "Campaign" : "Zombies";
}

class home_view final : public view {
public:
  const char *id() const override { return "home"; }

  bool bind(Rml::Context *context, app &owner) override {
    owner_ = &owner;
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
    model.Bind("mode", &mode_);
    model.Bind("pack_label", &pack_label_);
    model.Bind("pack", &pack_);
    model.Bind("pack_long", &pack_long_);
    model.Bind("meta", &meta_);
    model.Bind("sub", &sub_);
    model.Bind("rows", &rows_);
    model.Bind("stats", &stats_);
    model.Bind("more", &more_);
    model.Bind("empty_text", &empty_text_);
    model.Bind("can_launch", &can_launch_);
    model.Bind("game_value", &game_value_);
    model.Bind("game_detail", &game_detail_);
    model.Bind("game_ok", &game_ok_);
    model.Bind("game_found", &game_found_);
    model.Bind("sig_value", &sig_value_);
    model.Bind("sig_detail", &sig_detail_);
    model.Bind("catalog_value", &catalog_value_);
    model.Bind("catalog_detail", &catalog_detail_);
    model.Bind("safe", &safe_);
    model.Bind("safe_text", &safe_text_);
    model.Bind("safe_value", &safe_value_);
    model.Bind("safe_detail", &safe_detail_);
    model.Bind("last_server", &last_server_);
    model.Bind("can_bisect", &can_bisect_);
    model.Bind("culprit_on", &culprit_on_);
    model.Bind("culprit_text", &culprit_text_);
    model.Bind("culprit_mods", &culprit_mods_);
    model.Bind("culprit_done", &culprit_done_);
    model.Bind("culprit_found", &culprit_found_);
    model.Bind("culprit_stop", &culprit_stop_);
    model.BindEventCallback(
        "pick", [this](Rml::DataModelHandle, Rml::Event &,
                       const Rml::VariantList &args) {
          if (!args.empty()) {
            owner_->set_mode(args[0].Get<Rml::String>());
          }
        });
    model.BindEventCallback(
        "act", [this](Rml::DataModelHandle, Rml::Event &,
                      const Rml::VariantList &args) {
          if (!args.empty()) {
            act(args[0].Get<Rml::String>());
          }
        });
    handle_ = model.GetModelHandle();
    return true;
  }

  void update(double) override {
    services &svc = owner_->svc();
    const std::uint64_t key[] = {svc.config_rev(),     svc.game_rev(),
                                 svc.catalog_rev(),    svc.journal_rev(),
                                 svc.signatures_rev(), owner_->mode_rev(),
                                 svc.culprit_rev()};
    if (std::equal(std::begin(key), std::end(key), std::begin(seen_))) {
      return;
    }
    std::copy(std::begin(key), std::end(key), std::begin(seen_));
    refresh();
  }

  // With a crash to deal with, its box comes first.
  Rml::Element *default_focus() override {
    if (doc) {
      for (const char *id : {"culprit-launch", "culprit-off", "culprit-stop",
                             "safe-off"}) {
        if (Rml::Element *element = doc->GetElementById(id);
            element && element->IsVisible(true)) {
          return element;
        }
      }
    }
    return view::default_focus();
  }

  bool key(Rml::Input::KeyIdentifier key, int modifiers) override {
    // LB/RB and Q/E switch tabs everywhere; on Home, Left/Right on the
    // mode row move between modes, which spatial navigation already does.
    (void)key;
    (void)modifiers;
    return false;
  }

private:
  void refresh() {
    services &svc = owner_->svc();
    const auto &config = svc.config();
    mode_ = owner_->mode();

    // ------------------------------------------------------------ the plan
    const xerces_core::plan plan = svc.resolve(mode_);
    const auto &catalog = svc.catalog();
    // A pack applied to this mode (8.3) names the load order until the
    // player edits it by hand, which clears pack_name.
    const auto profile = config.profiles.find(mode_);
    const std::string pack_name =
        profile != config.profiles.end() ? profile->second.pack_name : "";
    if (!pack_name.empty()) {
      pack_label_ = owner_->tr("Active pack");
      pack_ = pack_name;
    } else {
      pack_label_ = owner_->tr("Profile");
      pack_ = owner_->tr(mode_name(mode_));
    }
    // Pack names run to 80 characters; the title's display size fits a
    // mode name, so a long name gets a smaller one.
    pack_long_ = xerces_core::json::length(pack_) > 16;
    rows_.clear();
    const std::size_t shown = std::min<std::size_t>(plan.stack.size(), 6);
    for (std::size_t i = 0; i < shown; ++i) {
      const auto &entry = plan.stack[i];
      rows_.push_back({two_digits(i + 1),
                       entry.name.empty() ? entry.id : entry.name,
                       entry.legacy ? std::string("legacy")
                                    : entry.ver.to_string()});
    }
    more_ = plan.stack.size() > shown
                ? "+" + std::to_string(plan.stack.size() - shown) + " " +
                      owner_->tr("more")
                : "";
    const std::size_t warnings =
        plan.diags.count(xerces_core::severity::warning);
    const std::size_t errors = plan.diags.count(xerces_core::severity::error);
    stats_ = {{two_digits(plan.stack.size()), owner_->tr("mods")},
              {two_digits(plan.zones_added), owner_->tr("zones added")},
              {two_digits(errors), owner_->tr("problems")},
              {two_digits(warnings), owner_->tr("warnings")}};

    const std::size_t requested =
        config.profiles.contains(mode_) ? config.profiles.at(mode_).mods.size()
                                        : 0;
    if (requested == 0) {
      meta_ = owner_->tr("No mods enabled. Pick some in Mods, or launch vanilla.");
      empty_text_ = owner_->tr("Nothing to load: this mode runs vanilla.");
    } else if (!catalog.scanned && catalog.scanning) {
      meta_ = owner_->tr("Reading your mods...");
      empty_text_ = owner_->tr("Reading your mods...");
    } else {
      meta_ = std::to_string(requested) + " " + owner_->tr("enabled") +
              " \xC2\xB7 " + owner_->tr("plan") + " " +
              (plan.plan_id.size() > 15 ? plan.plan_id.substr(7, 8) : "");
      empty_text_ = owner_->tr("The enabled mods are not installed.");
    }

    // ----------------------------------------------------------- launching
    const auto &game = svc.game();
    can_launch_ = game.can_launch() && plan.ok() && !plan.stack.empty();
    if (!game.searched) {
      sub_ = owner_->tr("Looking for Black Ops III...");
    } else if (!game.dir) {
      sub_ = owner_->tr("Black Ops III was not found");
    } else if (!game.can_launch()) {
      sub_ = owner_->tr("This game build is not supported");
    } else if (plan.stack.empty()) {
      sub_ = owner_->tr("No mods enabled for this mode");
    } else if (!plan.ok()) {
      sub_ = owner_->tr("The plan has problems: open Mods");
    } else {
      sub_ = owner_->tr("Multi-mod") + " \xC2\xB7 " +
             std::to_string(plan.stack.size()) + " " +
             owner_->tr(plan.stack.size() == 1 ? "mod" : "mods");
    }

    // -------------------------------------------------------------- status
    game_found_ = game.dir.has_value() || !game.searched;
    game_ok_ = game.can_launch();
    if (!game.searched) {
      game_value_ = owner_->tr("Looking...");
      game_detail_.clear();
    } else if (!game.dir) {
      game_value_ = owner_->tr("Not found");
      game_detail_ = owner_->tr("Set the game folder");
    } else {
      game_value_ = owner_->tr("Found");
      game_detail_ = game.steam_build
                         ? owner_->tr("Build") + " " +
                               std::to_string(*game.steam_build)
                         : owner_->tr("Build unknown");
      if (game.preflight && !game.preflight->can_launch()) {
        game_value_ = owner_->tr(xerces_core::to_string(game.preflight->verdict));
        game_detail_ = game.preflight->message;
      }
    }
    const auto &sigs = svc.signatures();
    if (sigs.known) {
      sig_value_ = std::to_string(sigs.matched) + "/" +
                   std::to_string(sigs.total) + " " + owner_->tr("matched");
      sig_detail_ = sigs.multi_mod_off()
                        ? owner_->tr("Multi-mod was off: only the main mod loaded")
                    : sigs.missing.empty()
                        ? owner_->tr("At the last launch")
                        : owner_->tr("Missing:") + " " + sigs.missing.front();
    } else {
      sig_value_ = owner_->tr("Not checked yet");
      sig_detail_ = owner_->tr("Checked when the game starts");
    }
    if (catalog.scanning) {
      catalog_value_ = catalog.total
                           ? std::to_string(catalog.done) + "/" +
                                 std::to_string(catalog.total)
                           : owner_->tr("Reading...");
      catalog_detail_ = catalog.current;
    } else {
      catalog_value_ = std::to_string(svc.mods_for(mode_).size()) + " " +
                       owner_->tr("mods") + ", " +
                       std::to_string(svc.maps().size()) + " " +
                       owner_->tr("maps");
      char text[96];
      std::snprintf(text, sizeof(text), "%.1f s, %zu files cached",
                    catalog.seconds, catalog.cache_hits);
      catalog_detail_ = catalog.scanned ? text : "";
    }
    const xerces_core::safe_mode_decision decision = svc.safe_mode();
    // A crash is told apart from the next one by its plan and start time:
    // the box stays away for the crash the player dismissed, and comes back
    // when a newer launch crashes (5.6: after any crash).
    crash_ = decision.safe_mode
                 ? decision.crashed_plan_id + "\n" + decision.crashed_started
                 : "";
    safe_ = decision.safe_mode && crash_ != dismissed_crash_;
    safe_text_ = decision.message;
    safe_value_ = decision.safe_mode ? owner_->tr("Suggested")
                                     : owner_->tr("Off");
    safe_detail_ = decision.safe_mode
                       ? owner_->tr("The last modded launch crashed")
                       : "";
    last_server_ = config.recent_servers.empty() ? ""
                                                 : config.recent_servers.front();
    refresh_culprit(decision);
    handle_.DirtyAllVariables();
  }

  std::string mod_label(const std::string &id) const {
    const auto *item = owner_->svc().catalog().find(id);
    return item && !item->info.name.empty() ? item->info.name : id;
  }

  std::string mod_list(const std::vector<std::string> &ids) const {
    std::string out;
    for (const auto &id : ids) {
      out += (out.empty() ? "" : ", ") + mod_label(id);
    }
    return out;
  }

  // The culprit search (5.6): one test launch at a time, the result read
  // from the journal when the game exits. A crashed launch that had the
  // larger zone memory is first run again without it.
  void refresh_culprit(const xerces_core::safe_mode_decision &decision) {
    const auto &culprit = owner_->svc().culprit();
    can_bisect_ = xerces_core::culprit_search_possible(decision) && !culprit;
    culprit_on_ = culprit.has_value();
    culprit_done_ = culprit_found_ = false;
    culprit_mods_.clear();
    culprit_stop_ = owner_->tr("Stop");
    if (!culprit) {
      return;
    }
    if (culprit->search.db_pool_culprit()) {
      culprit_done_ = culprit_found_ = true;
      culprit_text_ = owner_->tr("Found it:") + " " +
                      owner_->tr("larger zone memory (experimental). The same "
                                 "mods ran fine without it. Turn it off?");
      culprit_mods_ = mod_list(culprit->crashed_stack);
      culprit_stop_ = owner_->tr("Close");
    } else if (culprit->search.culprit()) {
      culprit_done_ = culprit_found_ = true;
      culprit_text_ = owner_->tr("Found it:") + " " +
                      mod_label(*culprit->search.culprit()) + ". " +
                      owner_->tr("It crashes the game on its own. Turn it off "
                                 "in this profile?");
      culprit_stop_ = owner_->tr("Close");
    } else if (culprit->search.done()) {
      culprit_done_ = true;
      culprit_text_ = owner_->tr("No single mod crashes alone: the crash needs "
                                 "these mods together.");
      if (culprit->search.db_pool() == xerces_core::db_pool_suspect::cleared) {
        // They crashed on the stock pool, where the crashed launch had the
        // larger one: together they may simply not fit without it.
        culprit_text_ += " " + owner_->tr("They also crashed without larger "
                                          "zone memory: together they may "
                                          "need more zone memory than the "
                                          "game has without it.");
      }
      culprit_mods_ = mod_list(culprit->search.suspects());
      culprit_stop_ = owner_->tr("Close");
    } else if (culprit->search.testing_db_pool()) {
      culprit_text_ =
          owner_->tr("Test") + " " +
          std::to_string(culprit->search.launches() + 1) + ": " +
          owner_->tr("launch the crashed mods again without larger zone "
                     "memory and play to where it crashed. Quit normally if "
                     "it does not crash.");
      culprit_mods_ = mod_list(culprit->search.next_test());
    } else {
      const auto next = culprit->search.next_test();
      culprit_text_ =
          owner_->tr("Test") + " " +
          std::to_string(culprit->search.launches() + 1) + ": " +
          owner_->tr("launch with") + " " + std::to_string(next.size()) + " " +
          owner_->tr("of the") + " " +
          std::to_string(culprit->search.suspects().size()) + " " +
          owner_->tr("suspects and play to where it crashed. Quit normally if "
                     "it does not crash.");
      culprit_mods_ = mod_list(next);
    }
    if (culprit_on_) {
      safe_ = false; // the culprit box replaces the safe-mode box
    }
  }

  // True when the game started (or a dry launch was logged).
  bool launch(bool modded, bool mods_off) {
    const launch_outcome outcome =
        owner_->svc().launch(owner_->mode(), modded, mods_off);
    if (outcome.started || outcome.dry_run) {
      owner_->notify(outcome.message);
      if (outcome.started && owner_->svc().config().close_on_launch) {
        owner_->win().request_close();
      }
      return true;
    }
    owner_->notify(outcome.message, true);
    return false;
  }

  void dismiss_crash(const std::string &crash) {
    dismissed_crash_ = crash;
    if (crash == crash_) {
      safe_ = false;
      handle_.DirtyVariable("safe");
    }
  }

  void act(const std::string &action) {
    log_info("launcher.home.action", action);
    if (action == "launch modded") {
      if (!can_launch_) {
        owner_->notify(sub_, true);
        return;
      }
      const auto &sigs = owner_->svc().signatures();
      if (sigs.multi_mod_off() && owner_->svc().resolve(owner_->mode()).stack.size() > 1) {
        // The game turned multi-mod off at its last start (a signature did
        // not resolve on this build, DESIGN.md 4.4): only fs_game loads.
        owner_->dialog("Multi-mod was off last time",
                       "At the last launch this game build was missing a "
                       "signature Xerces needs for extra mods, so only the main "
                       "mod loaded. Diagnostics shows which one. Launch anyway?",
                       {"Launch anyway", "Cancel"}, [this](int choice) {
                         if (choice == 0) {
                           launch(true, false);
                         }
                       });
        return;
      }
      if (safe_) {
        owner_->dialog("The last modded launch crashed",
                       safe_text_,
                       {"Launch with mods off", "Launch with mods anyway",
                        "Cancel"},
                       [this](int choice) {
                         if (choice == 0) {
                           launch(true, true);
                         } else if (choice == 1) {
                           launch(true, false);
                         }
                       });
        return;
      }
      launch(true, false);
    } else if (action == "launch vanilla") {
      launch(false, false);
    } else if (action == "launch safe") {
      launch(true, true);
    } else if (action == "retry mods") {
      // A launch Windows refused leaves the crash, and its box, in place.
      const std::string crash = crash_;
      if (launch(true, false)) {
        dismiss_crash(crash);
      }
    } else if (action == "dismiss safe") {
      dismiss_crash(crash_);
    } else if (action == "dry-run") {
      const launch_outcome preview =
          owner_->svc().preview_launch(owner_->mode(), true, false);
      std::string body;
      if (!preview.command_line.empty()) {
        body = owner_->tr("Xerces would start the game with:") + "\n" +
               preview.command_line;
      } else {
        body = preview.message;
      }
      for (const auto &item : preview.diags.items()) {
        body += "\n" + item.message;
      }
      owner_->dialog("Dry-run preview", body, {"Close"}, nullptr);
    } else if (action == "find culprit") {
      if (owner_->svc().start_culprit()) {
        owner_->notify(owner_->tr("Each test launches part of the crashed "
                                  "stack."));
      }
    } else if (action == "culprit test") {
      const launch_outcome outcome = owner_->svc().launch_culprit_test();
      owner_->notify(outcome.message, !outcome.started && !outcome.dry_run);
    } else if (action == "culprit off") {
      const auto &culprit = owner_->svc().culprit();
      if (culprit && culprit->search.db_pool_culprit()) {
        // The setting, not a mod: the profile keeps its mods.
        auto &config = owner_->svc().config();
        config.extra_flags =
            xerces_core::with_flag(std::move(config.extra_flags),
                                   xerces_core::db_pool::flag_name, false);
        owner_->svc().save_config();
        log_info("launcher.home.db_pool", "off after the culprit search");
        owner_->notify(owner_->tr("Larger zone memory is off."));
      } else if (culprit && culprit->search.culprit()) {
        auto &profile = xerces_core::profile_for(owner_->svc().config(),
                                                 culprit->mode);
        std::erase(profile.mods, *culprit->search.culprit());
        profile.pins.clear();
        profile.pack_name.clear();
        profile.pack_id.clear();
        owner_->svc().save_config();
        owner_->notify(mod_label(*culprit->search.culprit()) + " " +
                       owner_->tr("is off."));
      }
      owner_->svc().stop_culprit();
    } else if (action == "culprit stop") {
      owner_->svc().stop_culprit();
    } else if (action == "mods") {
      owner_->show("mods");
    } else if (action == "servers") {
      owner_->show("servers");
    } else if (action == "find game") {
      owner_->show(owner_->find("first_run") ? "first_run" : "settings");
    }
  }

  app *owner_ = nullptr;
  Rml::DataModelHandle handle_;
  std::uint64_t seen_[7] = {};
  // The crash safe_mode() reports now and the one the player answered with
  // Retry or Dismiss: plan id and start time, empty for none.
  std::string crash_, dismissed_crash_;

  Rml::String mode_ = "zm";
  Rml::String pack_label_, pack_, meta_, sub_, more_, empty_text_;
  bool pack_long_ = false;
  Rml::Vector<load_row> rows_;
  Rml::Vector<stat_cell> stats_;
  bool can_launch_ = false;
  Rml::String game_value_, game_detail_;
  bool game_ok_ = false, game_found_ = true;
  Rml::String sig_value_, sig_detail_, catalog_value_, catalog_detail_;
  bool safe_ = false;
  Rml::String safe_text_, safe_value_, safe_detail_, last_server_;
  bool can_bisect_ = false, culprit_on_ = false, culprit_done_ = false,
       culprit_found_ = false;
  Rml::String culprit_text_, culprit_mods_, culprit_stop_;
};
} // namespace

XERCES_VIEW(home_view, "home", 10, true);
} // namespace xerces_launcher
