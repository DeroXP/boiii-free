#include "friends_service.hpp"

#include <algorithm>
#include <chrono>
#include <future>

#include <xerces_core/ipc.hpp>
#include <xerces_core/join_check.hpp>
#include <xerces_core/join_password.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/launch.hpp>

#include "log.hpp"

namespace xerces_launcher {
namespace {
namespace json = xerces_core::json;
namespace ipc = xerces_core::ipc;
using namespace std::chrono_literals;

double seconds_now() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// While the game runs, how often it is asked whether its lobby is open.
constexpr double game_check_every = 5.0;
// The socket's keep-alive, and the check whether a fresh access token is due.
constexpr double ping_every = 25.0;
// The lists again: nothing tells a player they were removed or blocked (the
// backend keeps that quiet), and invites run out after 10 minutes.
constexpr double lists_every = 60.0;
// A ticket lets its player in for this long: time to get the host's mods,
// start the game, and come back after a crash.
constexpr int ticket_seconds = 3 * 60 * 60;
// An invite from a host this player knocked on, within the hour the backend
// allows it; or from one this player joined, within a play session.
constexpr double knock_follow_seconds = 60.0 * 60;
constexpr double join_follow_seconds = 6.0 * 60 * 60;
// Players who joined a lobby are asked again when the host's game restarts
// within this long.
constexpr double party_seconds = 6.0 * 60 * 60;
// A launch with a join waits this long for the game's main menu.
constexpr double watch_seconds = 10.0 * 60;

account::outcome failed(std::string message) {
  account::outcome out;
  out.message = std::move(message);
  return out;
}

account::outcome succeeded() {
  account::outcome out;
  out.ok = true;
  out.reached = true;
  return out;
}

const char *mode_name(const std::string &mode) {
  return mode == "mp" ? "Multiplayer" : mode == "cp" ? "Campaign" : "Zombies";
}

// host:port for an address the backend saw, with brackets for IPv6.
std::string with_port(const std::string &address, int port) {
  const bool v6 = address.find(':') != std::string::npos;
  return (v6 ? "[" + address + "]" : address) + ":" + std::to_string(port);
}

bool bool_member(const rapidjson::Value &doc, const char *name) {
  const auto *value = json::member(doc, name);
  return value && value->IsBool() && value->GetBool();
}
} // namespace

friends_service::friends_service(std::optional<std::string> base_url,
                                 fs::path home, poster post)
    : base_url_(std::move(base_url)), home_(std::move(home)),
      post_(std::move(post)), ui_(std::make_shared<ui_side>()) {
  work_.available = base_url_.has_value();
  if (base_url_) {
    client_ = std::make_unique<account::client>(*base_url_,
                                                xerces_core::winhttp_send);
    client_->on_session = [this](const std::optional<account::session> &s) {
      // Worker thread: every call runs there.
      if (s) {
        if (!account::save_session(home_, *s)) {
          log_warn("launcher.friends.save_failed",
                   "the sign-in could not be saved; it lasts until Xerces "
                   "closes");
        }
      } else {
        account::forget_session(home_);
        ended();
      }
    };
    if (auto saved = account::load_session(home_)) {
      client_->resume(std::move(saved));
      work_.signed_in = true;
      work_.restoring = true;
      work_.username = client_->current()->username;
    }
  }
  ui_->snapshot = work_;
  worker_ = std::thread([this] { work(); });
  listener_ = std::thread([this] { listen(); });
  if (work_.signed_in) {
    enqueue([this] {
      refresh_all();
      if (client_->signed_in()) {
        work_.restoring = false;
        {
          std::scoped_lock lock(mutex_);
          want_socket_ = true;
        }
        wake_.notify_all();
      }
      publish();
    });
  }
}

friends_service::~friends_service() {
  {
    std::scoped_lock lock(mutex_);
    stopping_ = true;
    jobs_.clear();
  }
  wake_.notify_all();
  socket_.close();
  if (listener_.joinable()) {
    listener_.join();
  }
  if (worker_.joinable()) {
    worker_.join();
  }
}

std::string friends_service::link_url(const account::invite_link &link) const {
  return base_url_ ? *base_url_ + link.path : link.path;
}

// ------------------------------------------------------------- threads
void friends_service::enqueue(std::function<void()> job) {
  {
    std::scoped_lock lock(mutex_);
    if (stopping_) {
      return;
    }
    jobs_.push_back(std::move(job));
  }
  wake_.notify_all();
}

void friends_service::work() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, 1s, [this] { return stopping_ || !jobs_.empty(); });
      if (stopping_) {
        return;
      }
      if (!jobs_.empty()) {
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
    }
    if (job) {
      job();
    }
    tick();
  }
}

void friends_service::listen() {
  double backoff = 2.0;
  for (;;) {
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || want_socket_; });
      if (stopping_) {
        return;
      }
    }
    // The access token comes from the worker, which owns the client.
    auto promise = std::make_shared<std::promise<std::string>>();
    std::future<std::string> token_future = promise->get_future();
    enqueue([this, promise] {
      promise->set_value(client_->signed_in() ? client_->access_token()
                                              : std::string());
    });
    std::string token;
    while (token_future.wait_for(250ms) != std::future_status::ready) {
      std::scoped_lock lock(mutex_);
      if (stopping_) {
        return;
      }
    }
    token = token_future.get();

    std::string error;
    if (!token.empty() &&
        socket_.connect(xerces_core::websocket_url(*base_url_, "/v1/ws"),
                        &error) &&
        socket_.send(account::auth_message(token))) {
      {
        // Signed out, or Xerces closing, while it connected: that socket is
        // not wanted. The destructor's close() came while connect() still
        // held the handles itself and closed nothing, so the receive below
        // would wait on this socket for ever, and the destructor's join
        // with it: a hidden Xerces.exe holding the home's mutex.
        std::scoped_lock lock(mutex_);
        if (stopping_ || !want_socket_) {
          socket_.close();
          if (stopping_) {
            return;
          }
          continue;
        }
      }
      backoff = 2.0;
      socket_up_ = true;
      enqueue([this, token] {
        token_sent_ = token;
        work_.connected = true;
        presence_sent_ = false;
        send_presence(true);
        refresh_all();
        publish();
      });
      int close_code = 0;
      while (auto message = socket_.receive(&close_code)) {
        enqueue([this, text = std::move(*message)] { handle(text); });
      }
      socket_up_ = false;
      enqueue([this] {
        work_.connected = false;
        publish();
      });
      log_info("launcher.friends.socket_closed",
               "the account socket closed (" + std::to_string(close_code) +
                   ")");
      if (close_code == 4401) {
        continue; // the token ran out: a fresh one at once
      }
    } else if (!token.empty()) {
      log_warn("launcher.friends.socket_failed", error);
    }
    // Wait before the next try, longer each time, unless asked to stop.
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, std::chrono::duration<double>(backoff),
                   [this] { return stopping_; });
    if (stopping_) {
      return;
    }
    backoff = std::min(backoff * 2, 60.0);
  }
}

void friends_service::tick() {
  if (!client_) {
    return;
  }
  const double now = seconds_now();
  bool running = false;
  std::string mode;
  bool invite_only = false;
  {
    std::scoped_lock lock(mutex_);
    running = game_running_;
    mode = game_mode_ui_;
    invite_only = invite_only_;
  }
  if (running != work_.game_running || mode != game_mode_) {
    work_.game_running = running;
    game_mode_ = mode;
    if (!running) {
      work_.hosting = false;
      work_.in_match = false;
      work_.joined = 0;
      game_pid_ = 0;
      guests_told_ = false;
    }
    next_game_check_ = 0;
    publish();
  }
  // A launch with a join: the hole punch once the game's menu is up.
  if (watching_ && running) {
    if (now >= watch_until_) {
      watching_.reset();
    } else {
      const host_answer host = ask_game_host();
      if (host.reached && host.main_menu && host.port > 0) {
        punch_for(*watching_, host);
        watching_.reset();
      }
    }
  }
  if (running && now >= next_game_check_) {
    next_game_check_ = now + game_check_every;
    const host_answer host = ask_game_host();
    if (host.reached && host.pid != game_pid_) {
      // Another game process: it knows no setting yet.
      game_pid_ = host.pid;
      guests_told_ = false;
    }
    if (host.reached) {
      const bool want = invite_only && client_->signed_in();
      if (!guests_told_ || want != guests_invite_only_) {
        guests_invite_only_ = want;
        tell_game_guests(true);
      }
    }
    const bool hosting = host.reached && host.open;
    if (hosting && !work_.hosting) {
      // The lobby opened. Players who joined this player's lobby in a game
      // that has since closed are asked again (follow the host).
      std::vector<std::string> back;
      for (auto it = party_.begin(); it != party_.end();) {
        if (now - it->second.when > party_seconds) {
          it = party_.erase(it);
        } else if (it->second.pid != host.pid) {
          back.push_back(it->first);
          it = party_.erase(it);
        } else {
          ++it;
        }
      }
      if (!back.empty()) {
        post_ui([this, back] {
          if (on_party_back) {
            on_party_back(back);
          }
        });
      }
    }
    const int joined =
        static_cast<int>(std::ranges::count_if(party_, [&](const auto &entry) {
          return entry.second.pid == host.pid;
        }));
    const bool in_match = host.reached && host.in_match;
    if (hosting != work_.hosting || in_match != work_.in_match ||
        joined != work_.joined) {
      work_.hosting = hosting;
      work_.in_match = in_match;
      work_.joined = joined;
      publish();
    }
  }
  if (client_->signed_in() && now >= next_lists_) {
    if (next_lists_ > 0) {
      refresh_all();
      publish();
    }
    next_lists_ = now + lists_every;
  }
  if (socket_up_) {
    send_presence(false);
    if (now >= next_ping_) {
      next_ping_ = now + ping_every;
      // A fresh access token keeps the socket open past the old one's end.
      const std::string token = client_->access_token();
      if (!token.empty() && token != token_sent_) {
        socket_.send(account::auth_message(token));
        token_sent_ = token;
      }
      socket_.send(account::ping_message);
    }
  }
}

void friends_service::publish() {
  std::weak_ptr<ui_side> weak = ui_;
  post_([weak, snapshot = work_] {
    if (auto ui = weak.lock()) {
      ui->snapshot = snapshot;
      ++ui->rev;
    }
  });
}

void friends_service::post_ui(std::function<void()> run) {
  std::weak_ptr<ui_side> weak = ui_;
  post_([weak, run = std::move(run)] {
    // The service (which owns ui_) is still there: run may use it.
    if (weak.lock()) {
      run();
    }
  });
}

void friends_service::answer(const done &finished, account::outcome result) {
  if (!finished) {
    return;
  }
  post_ui([finished, result = std::move(result)] { finished(result); });
}

// ------------------------------------------------------------- the data
bool friends_service::stopping() {
  std::scoped_lock lock(mutex_);
  return stopping_;
}

void friends_service::refresh_all() {
  if (!client_ || !client_->signed_in()) {
    return;
  }
  // Seven calls, each up to 15 s when the backend does not answer, and
  // closing Xerces waits for this thread: it stops between them.
  std::vector<account::friend_info> friends;
  if (client_->friends(friends).ok) {
    // The socket's presence is newer than the list's for friends it named.
    for (auto &entry : friends) {
      const auto old = std::ranges::find(work_.friends, entry.user_id,
                                         &account::friend_info::user_id);
      if (work_.connected && old != work_.friends.end() &&
          entry.now.status.empty()) {
        entry.now = old->now;
      }
    }
    std::ranges::sort(friends, [](const auto &a, const auto &b) {
      // Online first, then by name.
      if (a.now.status.empty() != b.now.status.empty()) {
        return !a.now.status.empty();
      }
      return a.username < b.username;
    });
    work_.friends = std::move(friends);
  }
  if (stopping() || !client_->signed_in()) {
    return; // the sign-in ended on the way (ended() published)
  }
  account::friend_requests requests;
  if (client_->requests(requests).ok) {
    work_.requests = std::move(requests);
  }
  if (stopping()) {
    return;
  }
  account::invites invites;
  if (client_->list_invites(invites).ok) {
    work_.invites.clear();
    for (auto &item : invites.incoming) {
      if (item.status == "pending") {
        work_.invites.push_back(std::move(item));
      }
    }
  }
  if (stopping()) {
    return;
  }
  std::vector<account::block_info> blocks;
  if (client_->blocks(blocks).ok) {
    work_.blocks = std::move(blocks);
  }
  if (stopping()) {
    return;
  }
  account::privacy_settings privacy;
  if (client_->privacy(privacy).ok) {
    work_.privacy = privacy;
  }
  if (stopping()) {
    return;
  }
  std::optional<account::invite_link> link;
  if (client_->own_link(link).ok) {
    work_.link = std::move(link);
  }
  if (stopping()) {
    return;
  }
  account::knock_list knocks;
  if (client_->knocks(knocks).ok) {
    work_.knocks = std::move(knocks.incoming);
    work_.knocked = std::move(knocks.outgoing);
  }
  if (client_->signed_in()) {
    work_.username = client_->current()->username;
  }
}

void friends_service::toast(const std::string &title, const std::string &text) {
  if (!work_.game_running) {
    return;
  }
  std::string response;
  const bool reached = call_game("notify",
                                 "{\"text\":" + json::quote(text) +
                                     ",\"title\":" + json::quote(title) + "}",
                                 &response);
  const auto parsed = reached ? ipc::parse_response(response) : std::nullopt;
  if (parsed && parsed->ok) {
    log_info("launcher.friends.game_toast", "the game showed a friends toast");
  } else {
    log_warn("launcher.friends.game_toast",
             "the game did not show a friends toast: " +
                 (parsed ? parsed->message : response));
  }
}

void friends_service::handle(const std::string &message) {
  const auto push = account::parse_push(message);
  if (!push) {
    return;
  }
  auto find = [this](const std::string &user_id) {
    return std::ranges::find(work_.friends, user_id,
                             &account::friend_info::user_id);
  };
  if (push->type == "hello") {
    for (auto &entry : work_.friends) {
      entry.now = {};
    }
    for (const auto &seen : push->friends) {
      if (const auto it = find(seen.user_id); it != work_.friends.end()) {
        it->now = seen.now;
      }
    }
  } else if (push->type == "presence") {
    if (const auto it = find(push->user_id); it != work_.friends.end()) {
      it->now = push->now;
    }
  } else if (push->type == "offline") {
    if (const auto it = find(push->user_id); it != work_.friends.end()) {
      it->now = {};
    }
  } else if (push->type == "friend_added" || push->type == "friend_request") {
    refresh_all();
    const std::string news =
        push->type == "friend_added"
            ? push->username + " is now your friend."
            : std::string("You have a new friend request.");
    post_ui([this, news] {
      if (on_news) {
        on_news(news);
      }
    });
  } else if (push->type == "invite") {
    const account::invite &received = push->received;
    if (received.status != "pending" ||
        std::ranges::find(work_.invites, received.id, &account::invite::id) !=
            work_.invites.end()) {
      return;
    }
    work_.invites.insert(work_.invites.begin(), received);
    // From a host this player asked to join, or joined lately: taken at
    // once (follow the host).
    bool follow = false;
    {
      std::scoped_lock lock(mutex_);
      follow = follow_;
    }
    const auto host = follow_hosts_.find(received.from_user_id);
    const bool auto_join = follow && host != follow_hosts_.end() &&
                           seconds_now() - host->second < join_follow_seconds;
    post_ui([this, received, auto_join] {
      if (auto_join && on_auto_join) {
        on_auto_join(received);
      } else if (on_invite) {
        on_invite(received);
      }
    });
    if (!auto_join) {
      // The player is likely in the game: say it there too. The toast shows
      // about 36 characters of title and 26 of text, one line each (seen in
      // the game): "Invite from" and a 24-character name, then "Multiplayer,
      // join in Xerces" at most.
      const auto game = account::parse_session_text(received.session);
      toast("Invite from " + received.from_username,
            std::string(mode_name(game ? game->mode : "zm")) +
                ", join in Xerces");
    }
  } else if (push->type == "knock") {
    const account::knock_info &knock = push->knocked;
    if (knock.id.empty() ||
        std::ranges::find(work_.knocks, knock.id, &account::knock_info::id) !=
            work_.knocks.end()) {
      return;
    }
    work_.knocks.insert(work_.knocks.begin(), knock);
    post_ui([this, knock] {
      if (on_knock) {
        on_knock(knock);
      }
    });
    toast(knock.from_username + " asks to join", "Let them in from Xerces");
  } else if (push->type == "punch") {
    // The player this launcher invited is about to connect: open the host's
    // router to them. Only for invites sent from here.
    if (!sent_invites_.contains(push->invite_id) ||
        !xerces_core::valid_server_address(push->address)) {
      return;
    }
    std::string response;
    std::uint32_t pid = 0;
    const bool reached =
        call_game("punch", "{\"address\":" + json::quote(push->address) + "}",
                  &response, &pid);
    const auto parsed = reached ? ipc::parse_response(response) : std::nullopt;
    if (parsed && parsed->ok) {
      // The address stays out of the log (DESIGN.md 15.1).
      log_info("launcher.friends.punch", "the game opens the router to a "
                                         "joining player");
    } else {
      log_warn("launcher.friends.punch",
               "the game did not take the hole punch: " +
                   (parsed ? parsed->message : response));
    }
    party_[push->from_user_id] = party_member{seconds_now(), pid};
    return;
  } else if (push->type == "error") {
    log_warn("launcher.friends.socket_error", push->code);
  } else {
    return; // auth_ok, pong
  }
  publish();
}

void friends_service::send_presence(bool force) {
  account::presence wanted;
  wanted.status = work_.game_running ? "in_game" : "online";
  if (work_.game_running) {
    wanted.mode = game_mode_.empty() ? "zm" : game_mode_;
    wanted.joinable = work_.hosting;
  }
  if (!force && presence_sent_ && wanted == sent_presence_) {
    return;
  }
  if (socket_.send(account::presence_message(wanted))) {
    sent_presence_ = wanted;
    presence_sent_ = true;
  }
}

void friends_service::tell_game_guests(bool force) {
  if (!force && guests_told_) {
    return;
  }
  std::string response;
  const bool reached =
      call_game("guests",
                std::string("{\"invite_only\":") +
                    (guests_invite_only_ ? "true" : "false") + "}",
                &response);
  const auto parsed = reached ? ipc::parse_response(response) : std::nullopt;
  guests_told_ = parsed && parsed->ok;
  if (reached && !guests_told_) {
    // An older game build: it knows no invite tickets, so its lobby stays
    // open to anyone with the address.
    log_warn("launcher.friends.guests",
             "the game did not take invite-only: " +
                 (parsed ? parsed->message : response));
    guests_told_ = true; // asked once per game
  }
}

void friends_service::ended() {
  work_ = friends_snapshot{};
  work_.available = base_url_.has_value();
  sent_invites_.clear();
  follow_hosts_.clear();
  party_.clear();
  watching_.reset();
  guests_told_ = false; // invite-only follows the sign-in
  {
    std::scoped_lock lock(mutex_);
    want_socket_ = false;
  }
  socket_.close();
  publish();
}

void friends_service::after_sign_in(const account::outcome &result) {
  if (!result.ok) {
    return;
  }
  work_.signed_in = true;
  work_.restoring = false;
  work_.username = client_->current()->username;
  guests_told_ = false;
  log_info("launcher.friends.signed_in", "signed in to a Xerces account");
  refresh_all();
  {
    std::scoped_lock lock(mutex_);
    want_socket_ = true;
  }
  wake_.notify_all();
  publish();
}

// ------------------------------------------------------------ the game
friends_service::host_answer friends_service::ask_game_host() {
  host_answer out;
  std::string response;
  if (!call_game("host", "{}", &response, &out.pid)) {
    out.error = response;
    return out;
  }
  const auto parsed = ipc::parse_response(response);
  if (!parsed || !parsed->ok) {
    out.error = parsed ? parsed->message : "no answer";
    return out;
  }
  const auto result = json::parse(parsed->result);
  if (!result.ok || !result.document.IsObject()) {
    out.error = "no answer";
    return out;
  }
  const auto &doc = result.document;
  out.reached = true;
  out.open = bool_member(doc, "open");
  out.main_menu = bool_member(doc, "main_menu");
  out.in_match = bool_member(doc, "in_match");
  if (const auto *port = json::member(doc, "port"); port && port->IsInt()) {
    out.port = port->GetInt();
  }
  out.mapped = json::get_string(doc, "mapped").value_or("");
  return out;
}

// Calls the running game's pipe; response holds the whole answer, or why
// there is none.
bool friends_service::call_game(const char *method, const std::string &params,
                                std::string *response, std::uint32_t *pid) {
  for (const auto &entry : ipc::live_run_entries(home_ / "run")) {
    if (entry.role != "game") {
      continue;
    }
    if (pid) {
      *pid = entry.pid;
    }
    const ipc::call_result result = ipc::call(entry, method, params, 3000);
    *response = result.ok ? result.response : result.error;
    return result.ok;
  }
  *response = "the game is not running";
  return false;
}

account::outcome friends_service::punch_for(const join_plan &plan,
                                            const host_answer &host) {
  if (plan.invite_id.empty() || host.port <= 0 || host.port > 65535) {
    return failed("no port to punch for");
  }
  bool delivered = false;
  const account::outcome result = client_->punch(
      plan.invite_id, static_cast<std::uint16_t>(host.port), &delivered);
  if (result.ok) {
    log_info("launcher.friends.punch_asked",
             delivered ? "the host's launcher was asked to open its router"
                       : "the host's launcher was not there to open its "
                         "router");
  } else {
    // Not fatal: a forwarded port or UPnP lets the join in anyway.
    log_warn("launcher.friends.punch_asked",
             "the hole punch could not be asked for: " + result.code);
  }
  return result;
}

account::outcome
friends_service::send_game_invite(const std::string &user_id,
                                  const std::string &pack_json) {
  if (!work_.game_running) {
    return failed("Start the game and open your lobby to direct connect "
                  "first.");
  }
  const host_answer host = ask_game_host();
  if (!host.reached) {
    return failed("The game did not answer: " + host.error);
  }
  if (!host.open) {
    return failed("Open your lobby to direct connect first: DIRECT CONNECT "
                  "in the lobby's menu.");
  }
  std::string address = host.mapped;
  // A router that is behind another one, or behind the provider's shared
  // address, or did not say its address, answers UPnP with an address no
  // friend can dial: the address the backend sees and the hole punch then
  // (review, 2026-11-13)
  if (xerces_core::classify_ipv4(
          xerces_core::join_password::endpoint_ip(address)) !=
      xerces_core::ipv4_scope::public_net) {
    address.clear();
  }
  if (address.empty()) {
    // No UPnP answer: the address the backend sees, and the game's port.
    // The hole punch opens the router for the one joining (most routers
    // keep the game's port); otherwise it is forwarded by hand, as for
    // direct connect.
    std::string seen;
    const account::outcome where = client_->own_address(seen);
    if (!where.ok) {
      return failed("Xerces could not tell your internet address: " +
                    where.message);
    }
    address = with_port(seen, host.port);
  }
  account::game_session game;
  game.address = address;
  game.mode = game_mode_.empty() ? "zm" : game_mode_;
  game.modded = !pack_json.empty();

  // The host's mods, shared as a pack (once per stack this session).
  std::string pack_code;
  if (!pack_json.empty()) {
    if (const auto known = pack_codes_.find(pack_json);
        known != pack_codes_.end()) {
      pack_code = known->second;
    } else {
      const account::outcome shared =
          client_->create_pack(pack_json, pack_code);
      if (shared.ok) {
        pack_codes_[pack_json] = pack_code;
      } else {
        // The invite goes without it: the friend joins with their own mods.
        log_warn("launcher.friends.pack_failed",
                 "the mods could not be shared with the invite: " +
                     shared.code);
        pack_code.clear();
      }
    }
  }

  // A ticket for this one invite, given to the game first: with invite-only
  // on, only a player with one gets in.
  game.ticket = account::new_ticket();
  std::string response;
  const bool reached =
      call_game("guests",
                "{\"seconds\":" + std::to_string(ticket_seconds) +
                    ",\"ticket\":" + json::quote(game.ticket) + "}",
                &response);
  const auto parsed = reached ? ipc::parse_response(response) : std::nullopt;
  if (!parsed || !parsed->ok) {
    if (guests_invite_only_) {
      return failed("The game did not take the invite's ticket: " +
                    (parsed ? parsed->message : response));
    }
    game.ticket.clear(); // an older game build: no ticket needed either
  }

  account::invite sent;
  const account::outcome result =
      client_->send_invite(user_id, game, pack_code, &sent);
  if (result.ok) {
    if (!sent.id.empty()) {
      sent_invites_.insert(sent.id);
    }
    log_info("launcher.friends.invited",
             pack_code.empty() ? "sent a game invite"
                               : "sent a game invite with the mods");
  }
  return result;
}

// ----------------------------------------------------------- actions
void friends_service::set_game(bool running, std::string mode) {
  std::scoped_lock lock(mutex_);
  game_running_ = running;
  game_mode_ui_ = std::move(mode);
}

void friends_service::set_invite_only(bool on) {
  std::scoped_lock lock(mutex_);
  invite_only_ = on;
}

void friends_service::set_follow(bool on) {
  std::scoped_lock lock(mutex_);
  follow_ = on;
}

void friends_service::sign_in(std::string username, std::string password,
                              done finished) {
  enqueue([this, username = std::move(username), password = std::move(password),
           finished] {
    if (!client_) {
      answer(finished, failed("Xerces has no server to sign in to."));
      return;
    }
    const account::outcome result = client_->sign_in(username, password);
    after_sign_in(result);
    answer(finished, result);
  });
}

void friends_service::sign_up(std::string username, std::string password,
                              done finished) {
  enqueue([this, username = std::move(username), password = std::move(password),
           finished] {
    if (!client_) {
      answer(finished, failed("Xerces has no server to sign up with."));
      return;
    }
    const account::outcome result = client_->sign_up(username, password);
    if (result.ok) {
      log_info("launcher.friends.signed_up", "made a Xerces account");
    }
    after_sign_in(result);
    answer(finished, result);
  });
}

void friends_service::sign_out() {
  enqueue([this] {
    if (client_) {
      client_->sign_out(); // on_session: forget the file, ended()
      log_info("launcher.friends.signed_out", "signed out");
    }
  });
}

void friends_service::refresh() {
  enqueue([this] {
    refresh_all();
    publish();
  });
}

// The usual shape of an action: the call on the worker, then the lists
// again, then the answer.
void friends_service::act(std::function<account::outcome()> call,
                          done finished) {
  enqueue([this, call = std::move(call), finished] {
    if (!client_ || !client_->signed_in()) {
      answer(finished, failed("Sign in first."));
      return;
    }
    const account::outcome result = call();
    refresh_all();
    publish();
    answer(finished, result);
  });
}

void friends_service::add_friend(std::string username, done finished) {
  act([this, username] { return client_->send_request(username); },
      std::move(finished));
}

void friends_service::accept_request(std::string id, done finished) {
  act([this, id] { return client_->accept_request(id); }, std::move(finished));
}

void friends_service::decline_request(std::string id, done finished) {
  act([this, id] { return client_->decline_request(id); }, std::move(finished));
}

void friends_service::cancel_request(std::string id, done finished) {
  act([this, id] { return client_->cancel_request(id); }, std::move(finished));
}

void friends_service::remove_friend(std::string user_id, done finished) {
  act([this, user_id] { return client_->remove_friend(user_id); },
      std::move(finished));
}

void friends_service::block(std::string username, done finished) {
  act([this, username] { return client_->block(username); },
      std::move(finished));
}

void friends_service::unblock(std::string user_id, done finished) {
  act([this, user_id] { return client_->unblock(user_id); },
      std::move(finished));
}

void friends_service::set_privacy(account::privacy_settings value,
                                  done finished) {
  act([this, value] { return client_->set_privacy(value); },
      std::move(finished));
}

void friends_service::invite(std::string user_id, std::string pack_json,
                             done finished) {
  act(
      [this, user_id = std::move(user_id), pack_json = std::move(pack_json)] {
        return send_game_invite(user_id, pack_json);
      },
      std::move(finished));
}

void friends_service::accept_invite(std::string id, joined finished) {
  enqueue([this, id = std::move(id), finished] {
    auto reply = [this, finished](account::outcome result,
                                  std::optional<join_plan> plan) {
      post_ui([finished, result = std::move(result), plan = std::move(plan)] {
        if (finished) {
          finished(result, plan);
        }
      });
    };
    if (!client_ || !client_->signed_in()) {
      reply(failed("Sign in first."), std::nullopt);
      return;
    }
    const auto it = std::ranges::find(work_.invites, id, &account::invite::id);
    const auto game = it != work_.invites.end()
                          ? account::parse_session_text(it->session)
                          : std::nullopt;
    if (!game) {
      reply(failed("This invite does not name a lobby Xerces can join."),
            std::nullopt);
      return;
    }
    join_plan plan;
    plan.game = *game;
    plan.invite_id = id;
    plan.host_user_id = it->from_user_id;
    plan.host_username = it->from_username;
    const std::string pack_code = it->pack_code;

    account::invite taken;
    const account::outcome result = client_->accept_invite(id, &taken);
    std::erase_if(work_.invites,
                  [&](const account::invite &item) { return item.id == id; });
    publish();
    if (!result.ok) {
      reply(result, std::nullopt); // gone (10 minutes) or no longer allowed
      return;
    }
    if (!pack_code.empty() && game->modded) {
      if (!client_->fetch_pack(pack_code, plan.pack_json).ok) {
        plan.pack_json.clear();
        plan.pack_missing = true;
      }
    }
    follow_hosts_[plan.host_user_id] = seconds_now();
    log_info("launcher.friends.invite_accepted", "accepted a game invite");
    reply(result, plan);
  });
}

void friends_service::decline_invite(std::string id, done finished) {
  enqueue([this, id = std::move(id), finished] {
    if (!client_ || !client_->signed_in()) {
      answer(finished, failed("Sign in first."));
      return;
    }
    const account::outcome result = client_->decline_invite(id);
    std::erase_if(work_.invites,
                  [&](const account::invite &item) { return item.id == id; });
    publish();
    answer(finished, result);
  });
}

void friends_service::join_running(join_plan plan, done finished) {
  enqueue([this, plan = std::move(plan), finished] {
    const host_answer host = ask_game_host();
    if (!host.reached) {
      answer(finished, failed("The game did not answer: " + host.error));
      return;
    }
    if (host.in_match || !host.main_menu) {
      answer(finished, failed("Go to the game's main menu, then Join again."));
      return;
    }
    if (client_ && client_->signed_in() && punch_for(plan, host).ok) {
      // The host's game starts sending within a moment; give it one.
      std::this_thread::sleep_for(750ms);
    }
    std::string params = "{\"address\":" + json::quote(plan.game.address);
    if (!plan.game.ticket.empty()) {
      params += ",\"ticket\":" + json::quote(plan.game.ticket);
    }
    params += "}";
    std::string response;
    const bool reached = call_game("join", params, &response);
    const auto parsed = reached ? ipc::parse_response(response) : std::nullopt;
    if (!parsed || !parsed->ok) {
      answer(finished,
             failed(parsed && parsed->code == ipc::refused
                        ? "Go to the game's main menu, then Join again."
                        : "The game did not take the join: " +
                              (parsed ? parsed->message : response)));
      return;
    }
    follow_hosts_[plan.host_user_id] = seconds_now();
    answer(finished, succeeded());
  });
}

void friends_service::watch_join(join_plan plan) {
  enqueue([this, plan = std::move(plan)] {
    follow_hosts_[plan.host_user_id] = seconds_now();
    watching_ = plan;
    watch_until_ = seconds_now() + watch_seconds;
  });
}

void friends_service::quit_game(done finished) {
  enqueue([this, finished] {
    std::string response;
    const bool reached = call_game("quit", "{}", &response);
    const auto parsed = reached ? ipc::parse_response(response) : std::nullopt;
    if (!parsed || !parsed->ok) {
      answer(finished, failed("The game did not quit: " +
                              (parsed ? parsed->message : response)));
      return;
    }
    log_info("launcher.friends.quit_game", "asked the game to quit for a join");
    answer(finished, succeeded());
  });
}

// ------------------------------------------------------- links, knocks
void friends_service::make_link(std::string mode, done finished) {
  act(
      [this, mode = std::move(mode)] {
        account::invite_link link;
        const account::outcome result = client_->create_link(mode, link);
        if (result.ok) {
          log_info("launcher.friends.link_made", "made an invite link");
        }
        return result;
      },
      std::move(finished));
}

void friends_service::stop_link(done finished) {
  act([this] { return client_->delete_link(); }, std::move(finished));
}

void friends_service::look_up_link(std::string code, owner_found finished) {
  enqueue([this, code = std::move(code), finished] {
    account::link_owner owner;
    const account::outcome result = client_ && client_->signed_in()
                                        ? client_->link(code, owner)
                                        : failed("Sign in first.");
    post_ui([finished, result, owner, ok = result.ok] {
      if (finished) {
        finished(result, ok ? std::optional(owner) : std::nullopt);
      }
    });
  });
}

void friends_service::knock(std::string code, std::string host_user_id,
                            done finished) {
  act(
      [this, code = std::move(code), host_user_id = std::move(host_user_id)] {
        const account::outcome result = client_->knock(code);
        if (result.ok) {
          // Their invite is taken at once, within the hour (follow).
          follow_hosts_[host_user_id] =
              seconds_now() - (join_follow_seconds - knock_follow_seconds);
          log_info("launcher.friends.knocked", "asked to join through a link");
        }
        return result;
      },
      std::move(finished));
}

void friends_service::let_in(std::string knock_id, std::string user_id,
                             std::string pack_json, done finished) {
  act(
      [this, knock_id = std::move(knock_id), user_id = std::move(user_id),
       pack_json = std::move(pack_json)] {
        const account::outcome accepted = client_->accept_knock(knock_id);
        if (!accepted.ok) {
          return accepted;
        }
        std::erase_if(work_.knocks, [&](const account::knock_info &item) {
          return item.id == knock_id;
        });
        return send_game_invite(user_id, pack_json);
      },
      std::move(finished));
}

void friends_service::ignore_knock(std::string knock_id, done finished) {
  act([this, knock_id = std::move(
                 knock_id)] { return client_->decline_knock(knock_id); },
      std::move(finished));
}
} // namespace xerces_launcher
