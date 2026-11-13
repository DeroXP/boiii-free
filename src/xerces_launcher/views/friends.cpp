// Friends (docs/DESIGN.md 7.3, 10): a Xerces account, the friends list with
// presence, friend requests, blocks, privacy, game invites and invite links.
// The network work is friends_service's, on its own threads; this screen
// shows its snapshot and turns clicks into its actions.
//
// Joining (10.12, 10.13): an invite carries the host's lobby address, a
// ticket for that one invite, and the host's mods as a pack. Join fetches
// the pack and checks which mods are here; missing ones are got the way a
// player gets any mod, from their Workshop pages in Steam (subscribe, Steam
// downloads). Then the running game joins from its main menu, or restarts
// with the host's mods first (the player agrees), or the game starts with
// the join. A crash in a friend's lobby offers to rejoin it.

#include <RmlUi/Core.h>

#include <algorithm>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <xerces_core/crash_outbox.hpp>
#include <xerces_core/invite_pack.hpp>
#include <xerces_core/log.hpp>
#include <xerces_core/pack.hpp>

#include "../app.hpp"
#include "../discord.hpp"
#include "../friends_service.hpp"
#include "../link_protocol.hpp"
#include "../log.hpp"
#include "../workshop_download.hpp"

namespace xerces_launcher {
namespace {
namespace core = xerces_core;

struct friend_row {
  Rml::String id, name, status;
  bool online = false;
};
struct person_row {
  Rml::String id, name;
};
struct invite_row {
  Rml::String id, text;
};
struct mod_row {
  Rml::String name;
  bool on_workshop = false;
  Rml::String status; // what Steam is doing with it
};

// "120 MB"
std::string megabytes(std::uint64_t bytes) {
  return std::to_string((bytes + 512 * 1024) / (1024 * 1024)) + " MB";
}

const char *mode_name(const std::string &mode) {
  return mode == "mp" ? "Multiplayer" : mode == "cp" ? "Campaign" : "Zombies";
}

std::string trimmed(const std::string &text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

// After a crash, Rejoin is offered for this long after the join.
constexpr double rejoin_seconds = 3.0 * 60 * 60;
// While mods download, the folders are scanned again this often.
constexpr double rescan_every = 8.0;
// A game asked to quit for a join gets this long to close.
constexpr double quit_wait = 60.0;
// A player let in on Discord asks through the link; their knock is let in at
// once within this long.
constexpr double discord_accept_seconds = 180.0;

int party_max(const std::string &mode) { return mode == "mp" ? 18 : 4; }

class friends_view final : public view {
public:
  const char *id() const override { return "friends"; }

  bool bind(Rml::Context *context, app &owner) override {
    owner_ = &owner;
    Rml::DataModelConstructor model = context->CreateDataModel("friends");
    if (!model) {
      return false;
    }
    if (auto row = model.RegisterStruct<friend_row>()) {
      row.RegisterMember("id", &friend_row::id);
      row.RegisterMember("name", &friend_row::name);
      row.RegisterMember("status", &friend_row::status);
      row.RegisterMember("online", &friend_row::online);
    }
    if (auto row = model.RegisterStruct<person_row>()) {
      row.RegisterMember("id", &person_row::id);
      row.RegisterMember("name", &person_row::name);
    }
    if (auto row = model.RegisterStruct<invite_row>()) {
      row.RegisterMember("id", &invite_row::id);
      row.RegisterMember("text", &invite_row::text);
    }
    if (auto row = model.RegisterStruct<mod_row>()) {
      row.RegisterMember("name", &mod_row::name);
      row.RegisterMember("on_workshop", &mod_row::on_workshop);
      row.RegisterMember("status", &mod_row::status);
    }
    model.RegisterArray<Rml::Vector<friend_row>>();
    model.RegisterArray<Rml::Vector<person_row>>();
    model.RegisterArray<Rml::Vector<invite_row>>();
    model.RegisterArray<Rml::Vector<mod_row>>();

    model.Bind("available", &available_);
    model.Bind("signed_in", &signed_in_);
    model.Bind("restoring", &restoring_);
    model.Bind("username", &username_);
    model.Bind("connection", &connection_);
    model.Bind("connected", &connected_);
    model.Bind("busy", &busy_);
    model.Bind("login_name", &login_name_);
    model.Bind("login_password", &login_password_);
    model.Bind("form_problem", &form_problem_);
    model.Bind("add_name", &add_name_);
    model.Bind("add_placeholder", &add_placeholder_);
    model.Bind("hosting", &hosting_);
    model.Bind("invite_note", &invite_note_);
    model.Bind("friends", &friends_);
    model.Bind("friend_count", &friend_count_);
    model.Bind("incoming", &incoming_);
    model.Bind("outgoing", &outgoing_);
    model.Bind("invites", &invites_);
    model.Bind("blocks", &blocks_);
    model.Bind("privacy_requests", &privacy_requests_);
    model.Bind("privacy_presence", &privacy_presence_);
    model.Bind("privacy_invites", &privacy_invites_);
    // Joining a lobby.
    model.Bind("join_active", &join_active_);
    model.Bind("join_title", &join_title_);
    model.Bind("join_status", &join_status_);
    model.Bind("join_ready", &join_ready_);
    model.Bind("join_mods", &join_mods_);
    model.Bind("join_pages", &join_pages_);
    model.Bind("join_can_download", &join_can_download_);
    model.Bind("rejoin", &rejoin_);
    model.Bind("rejoin_text", &rejoin_text_);
    // Links and knocks.
    model.Bind("knocks", &knocks_);
    model.Bind("has_link", &has_link_);
    model.Bind("link_url", &link_url_);
    model.Bind("link_input", &link_input_);
    model.Bind("link_placeholder", &link_placeholder_);
    model.Bind("links_open_text", &links_open_text_);
    model.Bind("lobby_guests", &lobby_guests_);
    model.Bind("follow_text", &follow_text_);
    model.Bind("discord_text", &discord_text_);

    model.BindEventCallback("act", [this](Rml::DataModelHandle, Rml::Event &,
                                          const Rml::VariantList &args) {
      if (!args.empty()) {
        act(args[0].Get<Rml::String>());
      }
    });
    model.BindEventCallback("row", [this](Rml::DataModelHandle, Rml::Event &,
                                          const Rml::VariantList &args) {
      if (args.size() >= 2) {
        row(args[0].Get<Rml::String>(), args[1].Get<int>());
      }
    });
    handle_ = model.GetModelHandle();
    add_placeholder_ = owner_->tr("Their Xerces username");
    link_placeholder_ = owner_->tr("A link or XJN- code");

    service_ = std::make_unique<friends_service>(
        xerces_core::crash_base_url(), owner.svc().paths().home,
        [&owner](std::function<void()> task) { owner.post(std::move(task)); });
    const auto &config = owner.svc().config();
    service_->set_invite_only(config.friends_invite_only);
    service_->set_follow(config.friends_follow);
    service_->on_invite = [this](const account::invite &received) {
      const auto game = account::parse_session_text(received.session);
      owner_->notify(received.from_username + " " +
                         owner_->tr("invited you to") + " " +
                         owner_->tr(mode_name(game ? game->mode : "zm")) +
                         ". " + owner_->tr("Join from Friends."),
                     false, "an invite came in");
    };
    service_->on_auto_join = [this](const account::invite &received) {
      // A host this player asked to join, or joined lately: no second
      // question. A join already under way keeps its place.
      if (join_ || busy_) {
        service_->on_invite(received);
        return;
      }
      owner_->notify(received.from_username + " " +
                         owner_->tr("let you in. Joining..."),
                     false, "following a host's invite");
      join_invite(received.id, false);
    };
    service_->on_knock = [this](const account::knock_info &knock) {
      if (let_in_from_discord(knock)) {
        return;
      }
      owner_->notify(knock.from_username + " " +
                         owner_->tr("asks to join your lobby. Let them in "
                                    "from Friends."),
                     false, "someone knocked");
    };
    service_->on_party_back = [this](const std::vector<std::string> &ids) {
      // The game restarted: the players of its last lobby follow, with the
      // mods it runs now.
      const std::string pack = current_pack_json();
      for (const std::string &user_id : ids) {
        service_->invite(user_id, pack, [this](const account::outcome &r) {
          if (r.ok) {
            owner_->notify(owner_->tr("Invited your last lobby's players to "
                                      "this one."),
                           false, "party invited again");
          }
        });
      }
    };
    service_->on_news = [this](const std::string &text) {
      owner_->notify(text, false, "friends news");
    };
    if (config.friends_discord) {
      start_discord();
    }
    refresh_settings();
    return true;
  }

  void update(double now) override {
    services &svc = owner_->svc();
    // The game's state, for presence and Invite, once a second.
    if (now >= next_game_check_) {
      next_game_check_ = now + 1.0;
      const bool running = svc.game_running();
      std::string mode = owner_->mode();
      if (const auto journal = svc.last_journal();
          running && journal && !journal->mode.empty()) {
        mode = journal->mode;
      }
      game_mode_ = mode;
      service_->set_game(running, mode);
      if (was_running_ && !running) {
        game_stopped();
      }
      if (!was_running_ && running) {
        game_started_at_ = static_cast<std::int64_t>(std::time(nullptr));
      }
      was_running_ = running;
      update_discord();
    }
    discord_.pump();
    if (owner_->join_link_rev() != seen_link_rev_) {
      seen_link_rev_ = owner_->join_link_rev();
      if (const std::string code = owner_->take_join_link(); !code.empty()) {
        owner_->show("friends");
        open_link(code);
      }
    }
    update_join(now);
    if (service_->rev() != seen_rev_) {
      seen_rev_ = service_->rev();
      refresh();
    }
  }

private:
  // ------------------------------------------------------------ joining
  enum class join_stage {
    mods,       // waiting for the host's mods to download
    ready,      // everything is here: Join goes
    restarting, // the game was asked to quit, to start with the host's mods
  };
  struct pending_join {
    friends_service::join_plan plan;
    std::vector<std::string> launch_mods; // the pack's, as installed here
    std::vector<core::invite_pack_check::missing_mod> missing;
    bool has_pack = false;
    join_stage stage = join_stage::ready;
    std::size_t next_page = 0;
    double next_scan = 0;
    double quit_deadline = 0;
    std::uint64_t seen_catalog = 0;
    // Steam getting the missing mods (workshop_download): each item's last
    // progress, and why Steam could not be asked, if it could not
    std::map<std::uint64_t, core::workshop_get::progress> steam;
    bool steam_running = false;
    std::string steam_error;
  };
  // The last join, for Rejoin after a crash.
  struct last_join {
    friends_service::join_plan plan;
    std::vector<std::string> launch_mods;
    bool has_pack = false;
    double when = 0;
  };

  void join_invite(const std::string &id, bool asked) {
    busy_ = true;
    handle_.DirtyVariable("busy");
    service_->accept_invite(
        id, [this, asked](const account::outcome &result,
                          std::optional<friends_service::join_plan> plan) {
          busy_ = false;
          handle_.DirtyVariable("busy");
          if (!result.ok || !plan) {
            owner_->notify(result.message, true,
                           "invite join failed: " + result.code);
            return;
          }
          start_join(std::move(*plan), asked);
        });
  }

  void start_join(friends_service::join_plan plan, bool asked) {
    pending_join join;
    join.plan = std::move(plan);
    if (join.plan.pack_missing) {
      owner_->notify(owner_->tr("The host's mod list could not be fetched. "
                                "Joining with your own mods."),
                     true, "invite pack missing");
    }
    if (!join.plan.pack_json.empty()) {
      const core::pack_result parsed = core::parse_pack(join.plan.pack_json);
      if (parsed.ok()) {
        join.has_pack = true;
        const core::invite_pack_check check =
            core::check_invite_pack(parsed.value, owner_->svc().catalog());
        join.launch_mods = check.launch_mods;
        join.missing = check.missing;
      } else {
        owner_->notify(owner_->tr("The host's mod list could not be read. "
                                  "Joining with your own mods."),
                       true, "invite pack unreadable");
      }
    }
    join.seen_catalog = owner_->svc().catalog_rev();
    join.stage = join.missing.empty() ? join_stage::ready : join_stage::mods;
    join_ = std::move(join);
    rejoin_ = false;
    handle_.DirtyVariable("rejoin");
    if (join_->stage == join_stage::mods) {
      owner_->show("friends");
      if (asked) {
        // The player pressed Join: Steam gets the mods at once
        steam_download();
      }
      show_join();
      return;
    }
    show_join();
    go();
  }

  // Steam subscribes to and downloads the missing mods that are on the
  // Workshop, under the player's account (xerces_core/workshop_get.hpp); the
  // folder rescans then see them and the join goes on.
  void steam_download() {
    if (!join_ || download_.running()) {
      return;
    }
    std::vector<std::uint64_t> ids;
    for (const auto &mod : join_->missing) {
      if (mod.workshop_id) {
        ids.push_back(*mod.workshop_id);
      }
    }
    services &svc = owner_->svc();
    if (ids.empty() || !svc.game().dir) {
      return;
    }
    const auto cli = svc.paths().program.parent_path() / "xerces_cli.exe";
    std::string error;
    join_->steam_error.clear();
    const bool started = download_.start(
        cli, *svc.game().dir, ids,
        [this](std::function<void()> task) { owner_->post(std::move(task)); },
        [this](const core::workshop_get::progress &line) {
          steam_progress(line);
        },
        [this](int code) { steam_done(code); }, &error);
    if (!started) {
      join_->steam_error = error;
      owner_->notify(owner_->tr("Steam could not be asked for the mods:") +
                         " " + error,
                     true, "workshop download did not start");
      show_join();
      return;
    }
    join_->steam_running = true;
    owner_->notify(owner_->tr("Steam is getting your friend's mods."));
    show_join();
  }

  void steam_progress(const core::workshop_get::progress &line) {
    if (!join_) {
      return;
    }
    if (line.state == "error") {
      join_->steam_error = line.message;
      owner_->notify(line.message, true, "workshop download: steam error");
    } else if (line.id != 0) {
      join_->steam[line.id] = line;
      if (line.state == "installed") {
        owner_->svc().start_scan(); // the rescan finds it, and the join goes on
      }
    }
    show_join();
  }

  void steam_done(int) {
    if (!join_) {
      return;
    }
    join_->steam_running = false;
    owner_->svc().start_scan();
    show_join();
  }

  // A mod's line on the join panel while Steam gets it
  std::string
  steam_status(const core::invite_pack_check::missing_mod &mod) const {
    if (!mod.workshop_id) {
      return owner_->tr("Not on the Workshop");
    }
    const auto it = join_->steam.find(*mod.workshop_id);
    if (it == join_->steam.end()) {
      return join_->steam_running ? owner_->tr("Waiting for Steam") : "";
    }
    const auto &line = it->second;
    if (line.state == "downloading" && line.total > 0) {
      const auto percent = line.done * 100 / line.total;
      return owner_->tr("Downloading") + " " + std::to_string(percent) + "% (" +
             megabytes(line.done) + " / " + megabytes(line.total) + ")";
    }
    if (line.state == "downloading") {
      return owner_->tr("Downloading");
    }
    if (line.state == "queued") {
      return owner_->tr("Queued in Steam");
    }
    if (line.state == "subscribing") {
      return owner_->tr("Subscribing");
    }
    if (line.state == "installed") {
      return owner_->tr("Downloaded");
    }
    if (line.state == "failed") {
      return owner_->tr("Failed:") + " " + line.message;
    }
    return "";
  }

  void open_next_page() {
    if (!join_) {
      return;
    }
    for (; join_->next_page < join_->missing.size(); ++join_->next_page) {
      const auto &mod = join_->missing[join_->next_page];
      if (mod.workshop_id) {
        if (owner_->open_url(core::workshop_page_url(*mod.workshop_id))) {
          owner_->notify(owner_->tr("Opened a Workshop page in Steam. "
                                    "Subscribe, and Steam downloads it."));
        } else {
          owner_->notify(owner_->tr("Steam could not be opened."), true);
        }
        ++join_->next_page;
        return;
      }
    }
    join_->next_page = 0; // round again for a page closed too soon
  }

  void update_join(double now) {
    if (!join_) {
      return;
    }
    services &svc = owner_->svc();
    if (join_->stage == join_stage::mods) {
      if (svc.catalog_rev() != join_->seen_catalog) {
        join_->seen_catalog = svc.catalog_rev();
        recheck_mods();
        return;
      }
      if (now >= join_->next_scan && !svc.catalog().scanning) {
        join_->next_scan = now + rescan_every;
        svc.start_scan();
      }
    } else if (join_->stage == join_stage::restarting) {
      if (!svc.game_running()) {
        launch_join();
      } else if (now >= join_->quit_deadline) {
        join_->stage = join_stage::ready;
        owner_->notify(owner_->tr("The game did not close. Close it, then "
                                  "press Join."),
                       true);
        show_join();
      }
    }
  }

  void recheck_mods() {
    const core::pack_result parsed = core::parse_pack(join_->plan.pack_json);
    if (!parsed.ok()) {
      return;
    }
    const core::invite_pack_check check =
        core::check_invite_pack(parsed.value, owner_->svc().catalog());
    join_->launch_mods = check.launch_mods;
    join_->missing = check.missing;
    if (join_->next_page > join_->missing.size()) {
      join_->next_page = 0;
    }
    if (check.complete()) {
      join_->stage = join_stage::ready;
      owner_->notify(owner_->tr("Every mod is here. Joining..."));
      show_join();
      go();
      return;
    }
    show_join();
  }

  // Everything is here: the running game joins, restarts first, or starts.
  void go() {
    if (!join_) {
      return;
    }
    services &svc = owner_->svc();
    if (!svc.game_running()) {
      launch_join();
      return;
    }
    if (!same_stack_running()) {
      owner_->dialog(
          "Restart the game to join?",
          join_->has_pack
              ? "Your friend's lobby runs other mods than your game. The game "
                "closes and starts again with theirs, then joins."
              : "Your friend's lobby runs without mods. The game closes and "
                "starts again without them, then joins.",
          {"Restart and join", "Cancel"}, [this](int choice) {
            if (!join_) {
              return;
            }
            if (choice != 0) {
              join_->stage = join_stage::ready;
              show_join();
              return;
            }
            service_->quit_game([this](const account::outcome &result) {
              if (!join_) {
                return;
              }
              if (!result.ok) {
                owner_->notify(result.message, true, "quit for join failed");
                return;
              }
              join_->stage = join_stage::restarting;
              join_->quit_deadline = owner_->now() + quit_wait;
              show_join();
            });
          });
      return;
    }
    busy_ = true;
    handle_.DirtyVariable("busy");
    service_->join_running(join_->plan, [this](const account::outcome &result) {
      busy_ = false;
      handle_.DirtyVariable("busy");
      if (!result.ok) {
        owner_->notify(result.message, true, "join in the running game failed");
        show_join(); // Join stays, to try again from the main menu
        return;
      }
      owner_->notify(owner_->tr("The game is joining your friend's lobby."));
      finish_join();
    });
  }

  // Whether the running game runs what the join needs: the pack's mods, or
  // no mods for a vanilla lobby. Without a pack (an older host, or the pack
  // could not be fetched) the running game joins as it is.
  bool same_stack_running() const {
    const auto journal = owner_->svc().last_journal();
    if (!journal) {
      return true;
    }
    if (!join_->plan.game.modded) {
      return journal->stack.empty() || journal->safe_mode;
    }
    if (!join_->has_pack) {
      return true;
    }
    const core::plan wanted =
        owner_->svc().resolve(join_->plan.game.mode, &join_->launch_mods);
    return wanted.ok() && wanted.plan_id == journal->plan_id;
  }

  // The game starts with the host's mods and the join; the hole punch
  // follows once its menu is up.
  void launch_join() {
    services &svc = owner_->svc();
    const auto &game = join_->plan.game;
    bool modded = game.modded;
    const std::vector<std::string> *mods = nullptr;
    if (modded && join_->has_pack) {
      mods = &join_->launch_mods;
      modded = !join_->launch_mods.empty();
    } else if (modded) {
      const core::plan plan = svc.resolve(game.mode);
      modded = plan.ok() && !plan.stack.empty();
    }
    log_info("launcher.friends.join",
             std::string(core::redacted_address) +
                 (modded ? (mods ? " with the host's mods" : " modded")
                         : " vanilla"));
    const std::optional<std::string> ticket =
        game.ticket.empty() ? std::nullopt : std::optional(game.ticket);
    const launch_outcome outcome =
        svc.launch(game.mode, modded, false, game.address, mods, ticket);
    if (!(outcome.started || outcome.dry_run)) {
      owner_->notify(owner_->tr(outcome.message), true);
      join_->stage = join_stage::ready;
      show_join();
      return;
    }
    service_->watch_join(join_->plan);
    owner_->notify(owner_->tr("Starting the game to join your friend."));
    finish_join();
    // "Close Xerces on launch" only minimises here: the hole punch for this
    // join is asked for by the friends worker once the game reaches its
    // menu, tens of seconds from now, and a closed launcher never asked
    // (review, 2026-11-13)
    if (outcome.started && svc.config().close_on_launch) {
      owner_->win().minimize();
    }
  }

  void finish_join() {
    last_ = last_join{join_->plan, join_->launch_mods, join_->has_pack,
                      owner_->now()};
    join_.reset();
    show_join();
  }

  void cancel_join() {
    download_.cancel();
    join_.reset();
    show_join();
  }

  // The game closed. A crash soon after joining a friend offers Rejoin.
  void game_stopped() {
    if (!last_ || owner_->now() - last_->when > rejoin_seconds || join_) {
      return;
    }
    const auto journal = owner_->svc().last_journal();
    if (!journal || journal->phase == core::launch_phase::clean_exit) {
      return;
    }
    rejoin_ = true;
    rejoin_text_ = owner_->tr("The game closed while you played in") + " " +
                   last_->plan.host_username + owner_->tr("'s lobby.");
    handle_.DirtyVariable("rejoin");
    handle_.DirtyVariable("rejoin_text");
    owner_->notify(owner_->tr("Rejoin your friend's lobby from Friends."),
                   false, "rejoin offered");
  }

  void rejoin() {
    if (!last_) {
      return;
    }
    rejoin_ = false;
    handle_.DirtyVariable("rejoin");
    pending_join join;
    join.plan = last_->plan;
    join.launch_mods = last_->launch_mods;
    join.has_pack = last_->has_pack;
    join.stage = join_stage::ready;
    join_ = std::move(join);
    show_join();
    go();
  }

  void show_join() {
    join_active_ = join_.has_value();
    join_mods_.clear();
    join_pages_ = false;
    join_can_download_ = false;
    if (join_) {
      join_title_ = owner_->tr("Joining") + " " + join_->plan.host_username +
                    owner_->tr("'s lobby");
      for (const auto &mod : join_->missing) {
        join_mods_.push_back(
            {mod.id, mod.workshop_id.has_value(), steam_status(mod)});
        join_pages_ = join_pages_ || mod.workshop_id.has_value();
      }
      join_can_download_ = join_pages_ && !join_->steam_running &&
                           join_->stage == join_stage::mods;
      switch (join_->stage) {
      case join_stage::mods: {
        const std::size_t count = join_->missing.size();
        const std::size_t off = std::ranges::count_if(
            join_->missing, [](const auto &m) { return !m.workshop_id; });
        join_status_ =
            count == 1
                ? owner_->tr("One of your friend's mods is not here yet.")
                : std::to_string(count) + " " +
                      owner_->tr("of your friend's mods are not here yet.");
        if (off < count && join_->steam_running) {
          join_status_ += " " + owner_->tr("Steam is subscribing to them and "
                                           "downloading them; the join starts "
                                           "by itself once they are here.");
        } else if (off < count && !join_->steam_error.empty()) {
          join_status_ += " " + owner_->tr("Steam could not get them:") + " " +
                          join_->steam_error + " " +
                          owner_->tr("Subscribe on each Workshop page "
                                     "instead; the join starts by itself "
                                     "once Steam has downloaded them.");
        } else if (off < count) {
          join_status_ += " " + owner_->tr("Get them through Steam, or "
                                           "subscribe on each Workshop page; "
                                           "the join starts by itself once "
                                           "Steam has downloaded them.");
        }
        if (off > 0) {
          join_status_ +=
              " " + owner_->tr(off == count
                                   ? "It is not on the Workshop: ask your "
                                     "friend where to get it. The join starts "
                                     "once it is in the game's mods folder."
                                   : "Some are not on the Workshop: ask your "
                                     "friend where to get them, for the "
                                     "game's mods folder.");
        }
        break;
      }
      case join_stage::ready:
        join_status_ = owner_->tr("Ready. Join has the game join from its "
                                  "main menu, or starts it.");
        break;
      case join_stage::restarting:
        join_status_ = owner_->tr("The game is closing; it starts again with "
                                  "your friend's mods.");
        break;
      }
      join_ready_ = join_->stage == join_stage::ready;
    } else {
      join_title_.clear();
      join_status_.clear();
      join_ready_ = false;
    }
    handle_.DirtyAllVariables();
  }

  // The stack the running game loaded, as a pack for an invite (from the
  // launch's journal, not the profile, which may have changed since or been
  // launched without mods), or "" for a game without mods.
  std::string current_pack_json() const {
    services &svc = owner_->svc();
    const auto journal = svc.last_journal();
    if (!svc.game_running() || !journal || journal->stack.empty() ||
        journal->safe_mode) {
      return {};
    }
    const core::plan plan = svc.resolve(game_mode_, &journal->stack);
    if (!plan.ok() || plan.stack.empty()) {
      return {};
    }
    std::map<std::string, std::uint64_t> workshop_ids;
    for (const auto &entry : plan.stack) {
      if (const auto *content = svc.catalog().find(entry.id);
          content && content->info.workshop_id) {
        workshop_ids[entry.id] = *content->info.workshop_id;
      }
    }
    core::pack value =
        core::pack_from_plan(plan, service_->snapshot().username + "'s lobby",
                             core::new_pack_id(), workshop_ids);
    value.mode = game_mode_;
    if (svc.game().steam_build) {
      value.game_build = std::to_string(*svc.game().steam_build);
    }
    // The same stack gives the same JSON (so the same code) all session.
    value.id = "00000000-0000-4000-8000-000000000000";
    return core::pack_to_json(value);
  }

  // ------------------------------------------------------------ Discord
  void start_discord() {
    const auto &paths = owner_->svc().paths();
    discord_presence::handlers handlers;
    handlers.join_request = [this](const std::string &user_id,
                                   const std::string &name) {
      discord_request(user_id, name);
    };
    handlers.join_game = [this](const std::string &secret) {
      discord_join(secret);
    };
    handlers.connection = [this](bool) { refresh_settings(); };
    discord_.start(paths.program, paths.home, std::move(handlers));
  }

  // What the Discord profile shows: the game's mode and where it is; with
  // the lobby open and a link, a party and the join secret (Ask to Join).
  // Nothing while the game is closed.
  void update_discord() {
    if (!discord_.started()) {
      return;
    }
    const friends_snapshot &s = service_->snapshot();
    if (!s.game_running) {
      discord_.show(std::nullopt);
      party_id_.clear();
      link_asked_ = false;
      return;
    }
    discord_activity activity;
    activity.details = mode_name(game_mode_);
    activity.state = s.in_match  ? "In a match"
                     : s.hosting ? "In the lobby, open to friends"
                                 : "In the menus";
    activity.started = game_started_at_;
    if (s.hosting && !s.in_match && s.signed_in) {
      if (s.link) {
        if (party_id_.empty()) {
          party_id_ = account::new_ticket();
        }
        activity.party_id = party_id_;
        activity.party_max = party_max(game_mode_);
        activity.party_size = std::min(1 + s.joined, activity.party_max);
        activity.join_secret = account::discord_join_secret(s.link->code);
      } else if (!link_asked_) {
        // Ask to Join carries the player's invite link: make one.
        link_asked_ = true;
        service_->make_link(game_mode_, {});
      }
    }
    discord_.show(activity);
  }

  // The host: someone on Discord asks to join.
  void discord_request(const std::string &user_id, const std::string &name) {
    owner_->dialog(
        "Let them join?",
        name + " " + owner_->tr("asks on Discord to join your lobby."),
        {"Let in", "No"}, [this, user_id](int choice) {
          const bool yes = choice == 0;
          discord_.respond(user_id, yes);
          log_info("launcher.discord.request", yes ? "let a Discord friend in"
                                                   : "turned a Discord "
                                                     "friend down");
          if (yes) {
            discord_accepts_.push_back(owner_->now() + discord_accept_seconds);
          }
        });
  }

  // The host: a knock right after a Discord yes is that player's.
  bool let_in_from_discord(const account::knock_info &knock) {
    const double now = owner_->now();
    std::erase_if(discord_accepts_,
                  [now](double until) { return until < now; });
    if (discord_accepts_.empty() || !service_->snapshot().hosting) {
      return false;
    }
    discord_accepts_.erase(discord_accepts_.begin());
    service_->let_in(knock.id, knock.from_user_id, current_pack_json(),
                     report(owner_->tr("Let in from Discord:") + " " +
                            knock.from_username + "."));
    return true;
  }

  // The joiner: the host let this player in on Discord. Ask through the
  // host's link; the host's Xerces lets the knock in, and its invite is
  // taken at once (follow).
  void discord_join(const std::string &secret) {
    const auto code = account::link_from_discord_secret(secret);
    if (!code) {
      log_warn("launcher.discord.join", "a Discord join without a Xerces link");
      return;
    }
    owner_->show("friends");
    if (!service_->snapshot().signed_in) {
      owner_->notify(owner_->tr("Sign in to Xerces to join through Discord."),
                     true);
      return;
    }
    service_->look_up_link(
        *code, [this, code = *code](const account::outcome &result,
                                    std::optional<account::link_owner> owner) {
          if (!result.ok || !owner) {
            owner_->notify(result.status == 404
                               ? owner_->tr("That lobby is not open any "
                                            "more.")
                               : result.message,
                           true, "discord join lookup failed: " + result.code);
            return;
          }
          service_->knock(code, owner->host_user_id,
                          report(owner_->tr("Joining") + " " +
                                 owner->host_username +
                                 owner_->tr("'s lobby...")));
        });
  }

  // ------------------------------------------------------- invite links
  void open_link(const std::string &code) {
    if (!service_->snapshot().signed_in) {
      owner_->notify(owner_->tr("Sign in to Xerces to use an invite link."),
                     true);
      return;
    }
    busy_ = true;
    handle_.DirtyVariable("busy");
    service_->look_up_link(
        code, [this, code](const account::outcome &result,
                           std::optional<account::link_owner> owner) {
          busy_ = false;
          handle_.DirtyVariable("busy");
          if (!result.ok || !owner) {
            owner_->notify(result.status == 404
                               ? owner_->tr("That invite link has expired or "
                                            "does not exist.")
                               : result.message,
                           true, "invite link lookup failed: " + result.code);
            return;
          }
          const std::string host_id = owner->host_user_id;
          owner_->dialog(
              "Ask to join?",
              owner->host_username + " " + owner_->tr("invites you to") + " " +
                  owner_->tr(mode_name(owner->mode)) + ". " +
                  owner_->tr("Ask them to let you in? Once they do, Xerces "
                             "joins their lobby."),
              {"Ask to join", "Cancel"}, [this, code, host_id](int choice) {
                if (choice != 0) {
                  return;
                }
                service_->knock(code, host_id,
                                report(owner_->tr("Asked. You join once they "
                                                  "let you in.")));
              });
        });
  }

  void refresh_settings() {
    const auto &config = owner_->svc().config();
    lobby_guests_ =
        owner_->tr(config.friends_invite_only ? "Invited players" : "Anyone");
    follow_text_ = owner_->tr(config.friends_follow ? "On" : "Off");
    discord_text_ = owner_->tr(!config.friends_discord ? "Off"
                               : discord_.connected()  ? "On"
                                                       : "On, Discord closed");
    handle_.DirtyVariable("discord_text");
    const auto &paths = owner_->svc().paths();
    switch (link_protocol::current(paths.program, paths.home)) {
    case link_protocol::state::here:
      links_open_text_ = owner_->tr("On");
      break;
    case link_protocol::state::elsewhere:
      links_open_text_ = owner_->tr("Another copy");
      break;
    case link_protocol::state::off:
      links_open_text_ = owner_->tr("Off");
      break;
    }
    handle_.DirtyVariable("lobby_guests");
    handle_.DirtyVariable("follow_text");
    handle_.DirtyVariable("links_open_text");
  }

  // ------------------------------------------------------------ the data
  void refresh() {
    const friends_snapshot &s = service_->snapshot();
    available_ = s.available;
    signed_in_ = s.signed_in;
    restoring_ = s.restoring;
    username_ = s.username;
    connection_ = !s.signed_in  ? ""
                  : s.restoring ? owner_->tr("Checking your sign-in...")
                  : s.connected ? owner_->tr("Online")
                                : owner_->tr("Connecting...");
    connected_ = s.signed_in && s.connected;
    hosting_ = s.hosting;
    // Hosting a lobby of their own, the player is no longer in a friend's:
    // a crash now is not one to rejoin a friend after (seen 2026-11-08).
    if (s.hosting) {
      last_.reset();
    }
    invite_note_ =
        !s.game_running
            ? owner_->tr("To invite a friend, start the game and open your "
                         "lobby to direct connect.")
        : !s.hosting
            ? owner_->tr("In the game, set your lobby's DIRECT CONNECT to "
                         "open; then Invite sends a friend a way in.")
            : owner_->tr("Your lobby is open: Invite sends a friend its "
                         "address, your mods and a ticket only they can "
                         "use.");

    friends_.clear();
    std::size_t online = 0;
    for (const auto &entry : s.friends) {
      friend_row row{entry.user_id, entry.username, status_text(entry.now),
                     !entry.now.status.empty()};
      online += row.online ? 1 : 0;
      friends_.push_back(std::move(row));
    }
    friend_count_ = std::to_string(online) + " " + owner_->tr("online") +
                    " \xC2\xB7 " + std::to_string(s.friends.size()) + " " +
                    owner_->tr("in all");
    incoming_.clear();
    for (const auto &request : s.requests.incoming) {
      incoming_.push_back({request.id, request.username});
    }
    outgoing_.clear();
    for (const auto &request : s.requests.outgoing) {
      outgoing_.push_back({request.id, request.username});
    }
    invites_.clear();
    for (const auto &received : s.invites) {
      const auto game = account::parse_session_text(received.session);
      std::string text = received.from_username + " " +
                         owner_->tr("invited you to") + " " +
                         owner_->tr(mode_name(game ? game->mode : "zm"));
      if (!received.pack_code.empty()) {
        text += " " + owner_->tr("with mods");
      }
      invites_.push_back({received.id, text});
    }
    blocks_.clear();
    for (const auto &blocked : s.blocks) {
      blocks_.push_back({blocked.user_id, blocked.username});
    }
    knocks_.clear();
    for (const auto &knock : s.knocks) {
      knocks_.push_back({knock.id, knock.from_username + " \xC2\xB7 " +
                                       owner_->tr(mode_name(knock.mode))});
    }
    has_link_ = s.link.has_value();
    link_url_ = s.link ? service_->link_url(*s.link) : std::string();
    privacy_requests_ = owner_->tr(
        s.privacy.friend_requests == "nobody" ? "Nobody" : "Everyone");
    privacy_presence_ =
        owner_->tr(s.privacy.presence == "nobody" ? "Nobody" : "Friends");
    privacy_invites_ =
        owner_->tr(s.privacy.invites == "nobody" ? "Nobody" : "Friends");
    handle_.DirtyAllVariables();
  }

  std::string status_text(const account::presence &now) const {
    if (now.status.empty()) {
      return owner_->tr("Offline");
    }
    if (now.status == "away") {
      return owner_->tr("Away");
    }
    if (now.status == "in_game") {
      std::string text =
          owner_->tr("Playing") + " " + owner_->tr(mode_name(now.mode));
      if (now.joinable) {
        text += " \xC2\xB7 " + owner_->tr("lobby open");
      }
      return text;
    }
    return owner_->tr("Online in Xerces");
  }

  // A finished action: a problem in the footer, or the success line. The log
  // gets neither (they can name a friend, and logs go into bug reports): the
  // backend's error code, or a fixed line.
  friends_service::done report(std::string success) {
    busy_ = true;
    handle_.DirtyVariable("busy");
    return
        [this, success = std::move(success)](const account::outcome &result) {
          busy_ = false;
          handle_.DirtyVariable("busy");
          if (!result.ok) {
            owner_->notify(result.message, true,
                           "friends action failed: " + result.code);
          } else if (!success.empty()) {
            owner_->notify(success, false, "friends action done");
          }
        };
  }

  void act(const std::string &action) {
    if (action == "join cancel") {
      cancel_join(); // always, even while a call is out
      return;
    }
    if (busy_) {
      return;
    }
    if (action == "sign in" || action == "sign up") {
      const std::string name = trimmed(login_name_);
      if (name.empty() || login_password_.empty()) {
        form_problem_ = owner_->tr("Type your username and password.");
        handle_.DirtyVariable("form_problem");
        return;
      }
      busy_ = true;
      form_problem_.clear();
      handle_.DirtyVariable("busy");
      handle_.DirtyVariable("form_problem");
      auto finished = [this, up = action ==
                                  "sign up"](const account::outcome &result) {
        busy_ = false;
        handle_.DirtyVariable("busy");
        if (!result.ok) {
          form_problem_ = result.message;
          handle_.DirtyVariable("form_problem");
          return;
        }
        // The password is not kept anywhere once it was used.
        login_password_.clear();
        handle_.DirtyVariable("login_password");
        owner_->notify(
            owner_->tr(up ? "Account made. Signed in as" : "Signed in as") +
                " " + service_->snapshot().username + ".",
            false, up ? "account made" : "signed in");
      };
      if (action == "sign up") {
        service_->sign_up(name, login_password_, finished);
      } else {
        service_->sign_in(name, login_password_, finished);
      }
    } else if (action == "sign out") {
      owner_->dialog("Sign out?",
                     "Friends stop seeing you online, and invites wait until "
                     "you sign in again.",
                     {"Sign out", "Cancel"}, [this](int choice) {
                       if (choice == 0) {
                         service_->sign_out();
                         owner_->notify(owner_->tr("Signed out."));
                       }
                     });
    } else if (action == "add") {
      const std::string name = trimmed(add_name_);
      if (name.empty()) {
        owner_->notify(owner_->tr("Type your friend's Xerces username first."),
                       true);
        return;
      }
      add_name_.clear();
      handle_.DirtyVariable("add_name");
      // The backend answers the same whether or not the name exists or
      // takes requests (DESIGN.md 10.6), so this cannot say more either.
      service_->add_friend(name, report(owner_->tr("Friend request sent.")));
    } else if (action == "refresh") {
      service_->refresh();
    } else if (action.starts_with("privacy ")) {
      account::privacy_settings next = service_->snapshot().privacy;
      const std::string which = action.substr(8);
      if (which == "requests") {
        next.friend_requests =
            next.friend_requests == "nobody" ? "everyone" : "nobody";
      } else if (which == "presence") {
        next.presence = next.presence == "nobody" ? "friends" : "nobody";
      } else if (which == "invites") {
        next.invites = next.invites == "nobody" ? "friends" : "nobody";
      } else {
        return;
      }
      service_->set_privacy(next, report(owner_->tr("Privacy saved.")));
    } else if (action == "join go") {
      if (join_ && join_->stage == join_stage::ready) {
        go();
      }
    } else if (action == "join next") {
      open_next_page();
    } else if (action == "join download") {
      steam_download();
    } else if (action == "rejoin") {
      rejoin();
    } else if (action == "rejoin close") {
      rejoin_ = false;
      handle_.DirtyVariable("rejoin");
    } else if (action == "link make") {
      service_->make_link(game_mode_.empty() ? "zm" : game_mode_,
                          report(owner_->tr("Invite link made. Copy it and "
                                            "share it.")));
    } else if (action == "link stop") {
      service_->stop_link(report(owner_->tr("The invite link stopped "
                                            "working.")));
    } else if (action == "link copy") {
      if (!link_url_.empty()) {
        owner_->copy_text(link_url_);
        owner_->notify(owner_->tr("Invite link copied."), false,
                       "invite link copied");
      }
    } else if (action == "link join") {
      const std::string text = trimmed(link_input_);
      const auto code = account::link_code_from(text);
      if (!code) {
        owner_->notify(owner_->tr("That is not an invite link."), true);
        return;
      }
      link_input_.clear();
      handle_.DirtyVariable("link_input");
      open_link(*code);
    } else if (action == "discord") {
      services &svc = owner_->svc();
      auto &config = svc.config();
      config.friends_discord = !config.friends_discord;
      if (config.friends_discord) {
        start_discord();
      } else {
        discord_.stop(true);
        party_id_.clear();
        link_asked_ = false;
      }
      if (!svc.save_config()) {
        owner_->notify(owner_->tr(svc.config_error()), true);
      }
      refresh_settings();
    } else if (action == "lobby guests" || action == "follow") {
      services &svc = owner_->svc();
      auto &config = svc.config();
      if (action == "follow") {
        config.friends_follow = !config.friends_follow;
        service_->set_follow(config.friends_follow);
      } else {
        config.friends_invite_only = !config.friends_invite_only;
        service_->set_invite_only(config.friends_invite_only);
      }
      if (!svc.save_config()) {
        owner_->notify(owner_->tr(svc.config_error()), true);
      }
      refresh_settings();
    } else if (action == "links open") {
      const auto &paths = owner_->svc().paths();
      std::string error;
      const bool here = link_protocol::current(paths.program, paths.home) ==
                        link_protocol::state::here;
      const bool ok =
          here ? link_protocol::turn_off(&error)
               : link_protocol::turn_on(paths.program, paths.home, &error);
      if (!ok) {
        owner_->notify(owner_->tr("Windows did not take the change:") + " " +
                           error,
                       true, "link protocol change failed");
      } else {
        log_info("launcher.friends.link_protocol",
                 here ? "xerces:// links no longer open Xerces"
                      : "xerces:// links open this Xerces");
        owner_->notify(here ? owner_->tr("Invite links no longer open "
                                         "Xerces.")
                            : owner_->tr("Invite links now open this "
                                         "Xerces."));
      }
      refresh_settings();
    }
  }

  template <class Rows>
  const typename Rows::value_type *at(const Rows &rows, int index) const {
    return index >= 0 && std::size_t(index) < rows.size() ? &rows[index]
                                                          : nullptr;
  }

  void row(const std::string &action, int index) {
    if (action == "mod page") {
      if (join_ && index >= 0 && std::size_t(index) < join_->missing.size() &&
          join_->missing[index].workshop_id) {
        owner_->open_url(
            core::workshop_page_url(*join_->missing[index].workshop_id));
      }
      return;
    }
    if (busy_) {
      return;
    }
    if (action == "accept" || action == "decline") {
      if (const auto *r = at(incoming_, index)) {
        if (action == "accept") {
          service_->accept_request(r->id, report(owner_->tr("Friend added.")));
        } else {
          service_->decline_request(r->id, report({}));
        }
      }
    } else if (action == "cancel") {
      if (const auto *r = at(outgoing_, index)) {
        service_->cancel_request(r->id, report({}));
      }
    } else if (action == "invite") {
      if (const auto *r = at(friends_, index)) {
        service_->invite(
            r->id, current_pack_json(),
            report(owner_->tr("Invite sent to") + " " + r->name + "."));
      }
    } else if (action == "remove" || action == "block") {
      const auto *r = at(friends_, index);
      if (!r) {
        return;
      }
      const std::string id = r->id;
      const std::string name = r->name;
      const bool block = action == "block";
      owner_->dialog(
          block ? "Block this player?" : "Remove this friend?",
          block ? "They stop being your friend, cannot see you online, and "
                  "cannot send you requests or invites. You can unblock them "
                  "later under Blocked."
                : "You stop seeing each other online and cannot invite each "
                  "other until one of you sends a new request.",
          {block ? "Block" : "Remove", "Cancel"},
          [this, id, name, block](int choice) {
            if (choice != 0) {
              return;
            }
            if (block) {
              service_->block(name, report(owner_->tr("Blocked.")));
            } else {
              service_->remove_friend(id, report(owner_->tr("Removed.")));
            }
          });
    } else if (action == "unblock") {
      if (const auto *r = at(blocks_, index)) {
        service_->unblock(r->id, report(owner_->tr("Unblocked.")));
      }
    } else if (action == "join") {
      if (const auto *r = at(invites_, index)) {
        if (join_) {
          owner_->notify(owner_->tr("Another join is under way. Cancel it "
                                    "first."),
                         true);
          return;
        }
        join_invite(r->id, true);
      }
    } else if (action == "ignore") {
      if (const auto *r = at(invites_, index)) {
        service_->decline_invite(r->id, report({}));
      }
    } else if (action == "let in") {
      const auto &knocks = service_->snapshot().knocks;
      if (index >= 0 && std::size_t(index) < knocks.size()) {
        const auto &knock = knocks[index];
        service_->let_in(
            knock.id, knock.from_user_id, current_pack_json(),
            report(owner_->tr("Let in:") + " " + knock.from_username + "."));
      }
    } else if (action == "knock ignore") {
      if (const auto *r = at(knocks_, index)) {
        service_->ignore_knock(r->id, report({}));
      }
    }
  }

  app *owner_ = nullptr;
  Rml::DataModelHandle handle_;
  std::unique_ptr<friends_service> service_;
  std::uint64_t seen_rev_ = 0;
  std::uint64_t seen_link_rev_ = 0;
  double next_game_check_ = 0;
  bool was_running_ = false;
  std::string game_mode_ = "zm";
  std::optional<pending_join> join_;
  std::optional<last_join> last_;
  workshop_download download_;
  discord_presence discord_;
  std::int64_t game_started_at_ = 0;
  std::string party_id_;
  bool link_asked_ = false;
  std::vector<double> discord_accepts_; // until when each Discord yes holds

  bool available_ = true;
  bool signed_in_ = false;
  bool restoring_ = false;
  Rml::String username_;
  Rml::String connection_;
  bool connected_ = false;
  bool busy_ = false;
  Rml::String login_name_;
  Rml::String login_password_;
  Rml::String form_problem_;
  Rml::String add_name_;
  Rml::String add_placeholder_;
  bool hosting_ = false;
  Rml::String invite_note_;
  Rml::Vector<friend_row> friends_;
  Rml::String friend_count_;
  Rml::Vector<person_row> incoming_;
  Rml::Vector<person_row> outgoing_;
  Rml::Vector<invite_row> invites_;
  Rml::Vector<person_row> blocks_;
  Rml::String privacy_requests_;
  Rml::String privacy_presence_;
  Rml::String privacy_invites_;
  bool join_active_ = false;
  Rml::String join_title_;
  Rml::String join_status_;
  bool join_ready_ = false;
  Rml::Vector<mod_row> join_mods_;
  bool join_pages_ = false;
  bool join_can_download_ = false;
  bool rejoin_ = false;
  Rml::String rejoin_text_;
  Rml::Vector<person_row> knocks_;
  bool has_link_ = false;
  Rml::String link_url_;
  Rml::String link_input_;
  Rml::String link_placeholder_;
  Rml::String links_open_text_;
  Rml::String lobby_guests_;
  Rml::String follow_text_;
  Rml::String discord_text_;
};
} // namespace

XERCES_VIEW(friends_view, "friends", 50, true);
} // namespace xerces_launcher
