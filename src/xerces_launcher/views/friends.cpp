// Friends (docs/DESIGN.md 7.3, 10): a Xerces account, the friends list with
// presence, friend requests, blocks, privacy, and game invites. The network
// work is friends_service's, on its own threads; this screen shows its
// snapshot and turns clicks into its actions.
//
// An invite carries the host's lobby address (account.hpp game_session).
// Accepting one has the running game join from its main menu, or starts the
// game with the join (as Join in Servers does).

#include <RmlUi/Core.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <xerces_core/crash_outbox.hpp>
#include <xerces_core/log.hpp>

#include "../app.hpp"
#include "../friends_service.hpp"
#include "../log.hpp"

namespace xerces_launcher {
namespace {
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
    model.RegisterArray<Rml::Vector<friend_row>>();
    model.RegisterArray<Rml::Vector<person_row>>();
    model.RegisterArray<Rml::Vector<invite_row>>();

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

    service_ = std::make_unique<friends_service>(
        xerces_core::crash_base_url(), owner.svc().paths().home,
        [&owner](std::function<void()> task) { owner.post(std::move(task)); });
    service_->on_invite = [this](const account::invite &received) {
      const auto game = account::parse_session_text(received.session);
      owner_->notify(received.from_username + " " +
                         owner_->tr("invited you to") + " " +
                         owner_->tr(mode_name(game ? game->mode : "zm")) +
                         ". " + owner_->tr("Join from Friends."),
                     false, "an invite came in");
    };
    service_->on_news = [this](const std::string &text) {
      owner_->notify(text, false, "friends news");
    };
    return true;
  }

  void update(double now) override {
    // The game's state, for presence and Invite, once a second.
    if (now >= next_game_check_) {
      next_game_check_ = now + 1.0;
      services &svc = owner_->svc();
      const bool running = svc.game_running();
      std::string mode = owner_->mode();
      if (const auto journal = svc.last_journal();
          running && journal && !journal->mode.empty()) {
        mode = journal->mode;
      }
      service_->set_game(running, mode);
    }
    if (service_->rev() != seen_rev_) {
      seen_rev_ = service_->rev();
      refresh();
    }
  }

private:
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
    invite_note_ =
        !s.game_running
            ? owner_->tr("To invite a friend, start the game and open your "
                         "lobby to direct connect.")
        : !s.hosting
            ? owner_->tr("In the game, set your lobby's DIRECT CONNECT to "
                         "open; then Invite sends a friend its address.")
            : owner_->tr("Your lobby is open: Invite sends a friend its "
                         "address. Without UPnP, forward the game's UDP port "
                         "on your router as for direct connect.");

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
      invites_.push_back(
          {received.id, received.from_username + " " +
                            owner_->tr("invited you to") + " " +
                            owner_->tr(mode_name(game ? game->mode : "zm"))});
    }
    blocks_.clear();
    for (const auto &blocked : s.blocks) {
      blocks_.push_back({blocked.user_id, blocked.username});
    }
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
    }
  }

  template <class Rows>
  const typename Rows::value_type *at(const Rows &rows, int index) const {
    return index >= 0 && std::size_t(index) < rows.size() ? &rows[index]
                                                          : nullptr;
  }

  void row(const std::string &action, int index) {
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
            r->id, report(owner_->tr("Invite sent to") + " " + r->name + "."));
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
        join_invite(r->id);
      }
    } else if (action == "ignore") {
      if (const auto *r = at(invites_, index)) {
        service_->decline_invite(r->id, report({}));
      }
    }
  }

  void join_invite(const std::string &id) {
    busy_ = true;
    handle_.DirtyVariable("busy");
    service_->accept_invite(
        id, [this](const account::outcome &result,
                   std::optional<friends_service::join_plan> plan) {
          busy_ = false;
          handle_.DirtyVariable("busy");
          if (!result.ok || !plan) {
            owner_->notify(result.message, true,
                           "invite join failed: " + result.code);
            return;
          }
          if (plan->joined_running_game) {
            owner_->notify(owner_->tr("The game is joining your friend's "
                                      "lobby."));
            return;
          }
          launch_join(plan->game);
        });
  }

  // As Join in Servers: the game starts with the join, and connects once its
  // main menu is up. The host decides which mods load.
  void launch_join(const account::game_session &game) {
    services &svc = owner_->svc();
    bool modded = game.modded;
    if (modded) {
      const xerces_core::plan plan = svc.resolve(game.mode);
      modded = plan.ok() && !plan.stack.empty();
    }
    log_info("launcher.friends.join",
             std::string(xerces_core::redacted_address) +
                 (modded ? " modded" : " vanilla"));
    const launch_outcome outcome =
        svc.launch(game.mode, modded, false, game.address);
    if (!(outcome.started || outcome.dry_run)) {
      owner_->notify(owner_->tr(outcome.message), true);
      return;
    }
    owner_->notify(owner_->tr("Starting the game to join your friend."));
    if (outcome.started && svc.config().close_on_launch) {
      owner_->win().request_close();
    }
  }

  app *owner_ = nullptr;
  Rml::DataModelHandle handle_;
  std::unique_ptr<friends_service> service_;
  std::uint64_t seen_rev_ = 0;
  double next_game_check_ = 0;

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
};
} // namespace

XERCES_VIEW(friends_view, "friends", 50, true);
} // namespace xerces_launcher
