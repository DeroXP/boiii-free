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

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
  bool hosting = false; // the game hosts a lobby others can join by address
};

class friends_service {
public:
  using poster = std::function<void(std::function<void()>)>;
  // On the UI thread.
  using done = std::function<void(const account::outcome &)>;
  struct join_plan {
    account::game_session game;
    bool joined_running_game = false; // the running game took the join
  };
  using joined =
      std::function<void(const account::outcome &, std::optional<join_plan>)>;

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
  // An invite came in; a line for the footer (a request, a new friend).
  std::function<void(const account::invite &)> on_invite;
  std::function<void(const std::string &)> on_news;

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
  // Invites a friend to the lobby the running game hosts.
  void invite(std::string user_id, done finished);
  // Accepts an invite and, when the game runs at its main menu, has it join
  // there; otherwise finished gets the game to start with the join.
  void accept_invite(std::string id, joined finished);
  void decline_invite(std::string id, done finished);
  // The game the launcher started or saw, from the UI thread's update().
  void set_game(bool running, std::string mode);

private:
  struct ui_side {
    friends_snapshot snapshot;
    std::uint64_t rev = 1;
    std::function<void(const account::invite &)> *on_invite = nullptr;
    std::function<void(const std::string &)> *on_news = nullptr;
  };

  void enqueue(std::function<void()> job);
  void act(std::function<account::outcome()> call, done finished);
  void work();
  void listen();
  void tick(); // worker: timers (the game's hosting, pings, renewal)
  void publish();
  void answer(const done &finished, account::outcome result);
  void refresh_all();
  void handle(const std::string &message);
  void after_sign_in(const account::outcome &result);
  void ended(); // worker: the sign-in is gone
  void send_presence(bool force);
  struct host_answer {
    bool reached = false;
    bool open = false;
    int port = 0;
    std::string mapped;
    std::string error;
  };
  host_answer ask_game_host();
  bool call_game(const char *method, const std::string &params,
                 std::string *error);

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

  // Shared.
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_ = false;
  bool want_socket_ = false; // signed in: the socket thread connects
  bool game_running_ = false;
  std::string game_mode_ui_;
  xerces_core::websocket socket_;
  std::atomic<bool> socket_up_{false};
  std::thread worker_;
  std::thread listener_;
};
} // namespace xerces_launcher
