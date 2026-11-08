#pragma once

// Friends (docs/DESIGN.md 7.3, 10): the launcher's side of Xerces accounts.
//
// One worker thread makes every call: to the backend (xerces_core::account,
// over WinHTTP) and to the running game's pipe (whether it hosts an open
// lobby, a join, a toast). A second thread only waits on the account socket
// (/v1/ws) and hands each message to the worker. The UI thread reads a
// snapshot, which the worker replaces through the app's post() with a new
// revision, so the screen never waits on the network. Actions queue work and
// answer through post() too.
//
// Presence: while Xerces is open and signed in, friends see "online", or
// "in a game" with its mode, and whether its lobby can be joined (the game
// says so over its pipe, every few seconds while it runs). Nothing else
// about the player or the PC is sent.
//
// Joining (10.12, 10.13): an invite carries the host's address, the host's
// mods as a pack code, and a ticket for that one invite. While signed in,
// the host's game lets in only players with a ticket (invite only, a
// setting). When the joiner is about to connect, its launcher tells the
// backend its game's port and the host's launcher has its game send a few
// packets there (hole punching), so routers without a forwarded port let
// the join in. Invite links let someone who is not a friend ask to join
// ("knock"); a host who lets them in sends them a normal invite.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <xerces_core/account.hpp>
#include <xerces_core/websocket.hpp>

namespace xerces_launcher {
namespace fs = std::filesystem;
namespace account = xerces_core::account;

struct friends_snapshot {
  bool available = true; // there is a backend to talk to
  bool signed_in = false;
  bool restoring = false; // a saved sign-in is being checked
  std::string username;
  bool connected = false; // the account socket is up: presence works
  std::vector<account::friend_info> friends;
  account::friend_requests requests;
  std::vector<account::block_info> blocks;
  account::privacy_settings privacy;
  std::vector<account::invite> invites; // incoming and pending
  bool game_running = false;
  bool hosting = false;  // the game hosts a lobby others can join by address
  bool in_match = false; // the game plays a match (not its menus)
  // Players who joined this game's lobby through an invite from here (their
  // hole punch came), for Discord's party size.
  int joined = 0;
  // Invite links (10.12): this player's live link, players asking to join
  // through it, and the hosts this player asked.
  std::optional<account::invite_link> link;
  std::vector<account::knock_info> knocks;  // incoming, pending
  std::vector<account::knock_info> knocked; // outgoing, until they expire
};

class friends_service {
public:
  using poster = std::function<void(std::function<void()>)>;
  // On the UI thread.
  using done = std::function<void(const account::outcome &)>;
  struct join_plan {
    account::game_session game;
    std::string invite_id;
    std::string host_user_id;
    std::string host_username;
    // The host's mods (a pack's JSON), or "" for none or a vanilla lobby.
    std::string pack_json;
    // The invite named a pack that could not be fetched: the join goes on
    // with this player's own mods, as before packs.
    bool pack_missing = false;
  };
  using joined =
      std::function<void(const account::outcome &, std::optional<join_plan>)>;
  using owner_found = std::function<void(const account::outcome &,
                                         std::optional<account::link_owner>)>;

  // base_url: crash_base_url() (nothing when the override is unusable);
  // home: the launcher's home (account.dat, run\).
  friends_service(std::optional<std::string> base_url, fs::path home,
                  poster post);
  ~friends_service();
  friends_service(const friends_service &) = delete;
  friends_service &operator=(const friends_service &) = delete;

  // UI thread.
  const friends_snapshot &snapshot() const { return ui_->snapshot; }
  std::uint64_t rev() const { return ui_->rev; }
  // An invite came in.
  std::function<void(const account::invite &)> on_invite;
  // An invite from a host this player asked to join (a knock) or joined
  // lately, with follow on: the screen joins it at once.
  std::function<void(const account::invite &)> on_auto_join;
  // Someone knocked on this player's invite link.
  std::function<void(const account::knock_info &)> on_knock;
  // The game restarted and hosts again: these players joined its last
  // lobby (user ids). The screen invites them again with the new mods.
  std::function<void(const std::vector<std::string> &)> on_party_back;
  // A line for the footer (a request, a new friend).
  std::function<void(const std::string &)> on_news;
  // The full link to share: the backend's address and the link's path.
  std::string link_url(const account::invite_link &link) const;

  void sign_in(std::string username, std::string password, done finished);
  void sign_up(std::string username, std::string password, done finished);
  void sign_out();
  void refresh();
  void add_friend(std::string username, done finished);
  void accept_request(std::string id, done finished);
  void decline_request(std::string id, done finished);
  void cancel_request(std::string id, done finished);
  void remove_friend(std::string user_id, done finished);
  void block(std::string username, done finished);
  void unblock(std::string user_id, done finished);
  void set_privacy(account::privacy_settings value, done finished);
  // Invites a player to the lobby the running game hosts. pack_json: the
  // game's mods as a pack (shared on the backend; the invite carries its
  // code), or "" for a vanilla game.
  void invite(std::string user_id, std::string pack_json, done finished);
  // Accepts an invite and fetches the host's pack. Nothing joins yet: the
  // screen checks the mods, then calls join_running or launches the game
  // and calls watch_join.
  void accept_invite(std::string id, joined finished);
  void decline_invite(std::string id, done finished);
  // The running game joins the plan's lobby from its main menu, after the
  // hole punch.
  void join_running(join_plan plan, done finished);
  // The game was started with the plan's join: the hole punch once its
  // main menu is up (within 10 minutes).
  void watch_join(join_plan plan);
  // Asks the running game to quit (the player agreed: a join that needs
  // other mods restarts it).
  void quit_game(done finished);
  // The game the launcher started or saw, from the UI thread's update().
  void set_game(bool running, std::string mode);
  // Settings (launcher_config friends_invite_only, friends_follow).
  void set_invite_only(bool on);
  void set_follow(bool on);

  // Invite links and knocks (10.12).
  void make_link(std::string mode, done finished);
  void stop_link(done finished);
  // Whose link a code (or a pasted link) is.
  void look_up_link(std::string code, owner_found finished);
  // Asks the link's owner to let this player join.
  void knock(std::string code, std::string host_user_id, done finished);
  // Lets a knocking player in: accepts the knock and invites them.
  void let_in(std::string knock_id, std::string user_id, std::string pack_json,
              done finished);
  void ignore_knock(std::string knock_id, done finished);

private:
  struct ui_side {
    friends_snapshot snapshot;
    std::uint64_t rev = 1;
  };
  struct host_answer {
    bool reached = false;
    bool open = false;
    bool main_menu = false;
    bool in_match = false;
    int port = 0;
    std::uint32_t pid = 0;
    std::string mapped;
    std::string error;
  };

  void enqueue(std::function<void()> job);
  void act(std::function<account::outcome()> call, done finished);
  void work();
  void listen();
  void tick(); // worker: timers (the game's hosting, pings, renewal)
  void publish();
  void answer(const done &finished, account::outcome result);
  // Runs on the UI thread, unless this service is gone by then.
  void post_ui(std::function<void()> run);
  void refresh_all();
  void handle(const std::string &message);
  void after_sign_in(const account::outcome &result);
  void ended(); // worker: the sign-in is gone
  void send_presence(bool force);
  void tell_game_guests(bool force); // invite_only to the running game
  void toast(const std::string &title, const std::string &text);
  account::outcome send_game_invite(const std::string &user_id,
                                    const std::string &pack_json);
  account::outcome punch_for(const join_plan &plan, const host_answer &host);
  host_answer ask_game_host();
  bool call_game(const char *method, const std::string &params,
                 std::string *response, std::uint32_t *pid = nullptr);

  std::optional<std::string> base_url_;
  fs::path home_;
  poster post_;
  std::shared_ptr<ui_side> ui_;

  // Worker thread only.
  std::unique_ptr<account::client> client_;
  friends_snapshot work_;
  std::string game_mode_;
  account::presence sent_presence_;
  bool presence_sent_ = false;
  std::string token_sent_;
  double next_game_check_ = 0;
  double next_ping_ = 0;
  double next_lists_ = 0;
  // Packs shared for invites: pack JSON -> code, kept for this session (a
  // code lasts 90 days on the backend; at most 20 new ones a day).
  std::map<std::string, std::string> pack_codes_;
  // Invites sent this session, for their hole punches.
  std::set<std::string> sent_invites_;
  // Hosts this player knocked on or joined, with when (follow).
  std::map<std::string, double> follow_hosts_;
  // Players who joined this player's lobby (their punch came), with when
  // and which game process hosted it.
  struct party_member {
    double when = 0;
    std::uint32_t pid = 0;
  };
  std::map<std::string, party_member> party_;
  std::uint32_t game_pid_ = 0;
  bool guests_told_ = false;
  bool guests_invite_only_ = false;
  std::optional<join_plan> watching_;
  double watch_until_ = 0;

  // Shared.
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_ = false;
  bool want_socket_ = false; // signed in: the socket thread connects
  bool game_running_ = false;
  std::string game_mode_ui_;
  bool invite_only_ = true;
  bool follow_ = true;
  xerces_core::websocket socket_;
  std::atomic<bool> socket_up_{false};
  std::thread worker_;
  std::thread listener_;
};
} // namespace xerces_launcher
