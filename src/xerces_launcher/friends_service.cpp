#include "friends_service.hpp"

#include <algorithm>
#include <chrono>
#include <future>

#include <xerces_core/ipc.hpp>
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

account::outcome failed(std::string message) {
  account::outcome out;
  out.message = std::move(message);
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
} // namespace

friends_service::friends_service(std::optional<std::string> base_url,
                                 fs::path home, poster post)
    : base_url_(std::move(base_url)), home_(std::move(home)),
      post_(std::move(post)), ui_(std::make_shared<ui_side>()) {
  ui_->on_invite = &on_invite;
  ui_->on_news = &on_news;
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
        // Signed out while it connected: that socket is not wanted.
        std::scoped_lock lock(mutex_);
        if (!want_socket_) {
          socket_.close();
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
  {
    std::scoped_lock lock(mutex_);
    running = game_running_;
    mode = game_mode_ui_;
  }
  if (running != work_.game_running || mode != game_mode_) {
    work_.game_running = running;
    game_mode_ = mode;
    if (!running) {
      work_.hosting = false;
    }
    next_game_check_ = 0;
    publish();
  }
  if (running && now >= next_game_check_) {
    next_game_check_ = now + game_check_every;
    const host_answer host = ask_game_host();
    const bool hosting = host.reached && host.open;
    if (hosting != work_.hosting) {
      work_.hosting = hosting;
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

void friends_service::answer(const done &finished, account::outcome result) {
  if (!finished) {
    return;
  }
  std::weak_ptr<ui_side> weak = ui_;
  post_([weak, finished, result = std::move(result)] {
    if (weak.lock()) {
      finished(result);
    }
  });
}

// ------------------------------------------------------------- the data
void friends_service::refresh_all() {
  if (!client_ || !client_->signed_in()) {
    return;
  }
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
  if (!client_->signed_in()) {
    return; // the sign-in ended on the way (ended() published)
  }
  account::friend_requests requests;
  if (client_->requests(requests).ok) {
    work_.requests = std::move(requests);
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
  std::vector<account::block_info> blocks;
  if (client_->blocks(blocks).ok) {
    work_.blocks = std::move(blocks);
  }
  account::privacy_settings privacy;
  if (client_->privacy(privacy).ok) {
    work_.privacy = privacy;
  }
  if (client_->signed_in()) {
    work_.username = client_->current()->username;
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
    std::weak_ptr<ui_side> weak = ui_;
    post_([weak, news] {
      if (auto ui = weak.lock(); ui && *ui->on_news) {
        (*ui->on_news)(news);
      }
    });
  } else if (push->type == "invite") {
    const account::invite &received = push->received;
    if (received.status == "pending" &&
        std::ranges::find(work_.invites, received.id, &account::invite::id) ==
            work_.invites.end()) {
      work_.invites.insert(work_.invites.begin(), received);
      std::weak_ptr<ui_side> weak = ui_;
      post_([weak, received] {
        if (auto ui = weak.lock(); ui && *ui->on_invite) {
          (*ui->on_invite)(received);
        }
      });
      // The player is likely in the game: say it there too.
      if (work_.game_running) {
        const auto game = account::parse_session_text(received.session);
        std::string response;
        // The toast shows about 36 characters of title and 26 of text, one
        // line each (seen in the game): "Invite from" and a 24-character
        // name, then "Multiplayer, join in Xerces" at most.
        const bool reached = call_game(
            "notify",
            "{\"text\":" +
                json::quote(std::string(mode_name(game ? game->mode : "zm")) +
                            ", join in Xerces") +
                ",\"title\":" +
                json::quote("Invite from " + received.from_username) + "}",
            &response);
        const auto parsed =
            reached ? ipc::parse_response(response) : std::nullopt;
        if (parsed && parsed->ok) {
          log_info("launcher.friends.game_toast", "the game showed the invite");
        } else {
          log_warn("launcher.friends.game_toast",
                   "the game did not show the invite: " +
                       (parsed ? parsed->message : response));
        }
      }
    }
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

void friends_service::ended() {
  work_ = friends_snapshot{};
  work_.available = base_url_.has_value();
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
  if (!call_game("host", "{}", &response)) {
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
  if (const auto *open = json::member(doc, "open"); open && open->IsBool()) {
    out.open = open->GetBool();
  }
  if (const auto *port = json::member(doc, "port"); port && port->IsInt()) {
    out.port = port->GetInt();
  }
  out.mapped = json::get_string(doc, "mapped").value_or("");
  return out;
}

// Calls the running game's pipe; response holds the whole answer, or why
// there is none.
bool friends_service::call_game(const char *method, const std::string &params,
                                std::string *response) {
  for (const auto &entry : ipc::live_run_entries(home_ / "run")) {
    if (entry.role != "game") {
      continue;
    }
    const ipc::call_result result = ipc::call(entry, method, params, 3000);
    *response = result.ok ? result.response : result.error;
    return result.ok;
  }
  *response = "the game is not running";
  return false;
}

// ----------------------------------------------------------- actions
void friends_service::set_game(bool running, std::string mode) {
  std::scoped_lock lock(mutex_);
  game_running_ = running;
  game_mode_ui_ = std::move(mode);
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

void friends_service::invite(std::string user_id, done finished) {
  enqueue([this, user_id = std::move(user_id), finished] {
    if (!client_ || !client_->signed_in()) {
      answer(finished, failed("Sign in first."));
      return;
    }
    if (!work_.game_running) {
      answer(finished, failed("Start the game and open your lobby to direct "
                              "connect first."));
      return;
    }
    const host_answer host = ask_game_host();
    if (!host.reached) {
      answer(finished, failed("The game did not answer: " + host.error));
      return;
    }
    if (!host.open) {
      answer(finished, failed("Open your lobby to direct connect first: DIRECT "
                              "CONNECT in the lobby's menu."));
      return;
    }
    std::string address = host.mapped;
    if (address.empty()) {
      // No UPnP answer: the address the backend sees, and the game's port
      // (forwarded on the router by hand, as for direct connect).
      std::string seen;
      const account::outcome where = client_->own_address(seen);
      if (!where.ok) {
        answer(finished, failed("Xerces could not tell your internet "
                                "address: " +
                                where.message));
        return;
      }
      address = with_port(seen, host.port);
    }
    account::game_session game;
    game.address = address;
    game.mode = game_mode_.empty() ? "zm" : game_mode_;
    game.modded = true;
    const account::outcome result = client_->send_invite(user_id, game);
    if (result.ok) {
      log_info("launcher.friends.invited", "sent a game invite");
    }
    answer(finished, result);
  });
}

void friends_service::accept_invite(std::string id, joined finished) {
  enqueue([this, id = std::move(id), finished] {
    const auto it = std::ranges::find(work_.invites, id, &account::invite::id);
    const auto game = it != work_.invites.end()
                          ? account::parse_session_text(it->session)
                          : std::nullopt;
    auto reply = [this, finished](account::outcome result,
                                  std::optional<join_plan> plan) {
      std::weak_ptr<ui_side> weak = ui_;
      post_(
          [weak, finished, result = std::move(result), plan = std::move(plan)] {
            if (weak.lock() && finished) {
              finished(result, plan);
            }
          });
    };
    if (!client_ || !client_->signed_in()) {
      reply(failed("Sign in first."), std::nullopt);
      return;
    }
    if (!game) {
      reply(failed("This invite does not name a lobby Xerces can join."),
            std::nullopt);
      return;
    }
    join_plan plan;
    plan.game = *game;
    if (work_.game_running) {
      // The running game joins first, so an invite the game could not take
      // yet (not at its main menu) stays to try again.
      std::string response;
      const bool reached =
          call_game("join", "{\"address\":" + json::quote(game->address) + "}",
                    &response);
      const auto parsed =
          reached ? ipc::parse_response(response) : std::nullopt;
      if (!parsed || !parsed->ok) {
        reply(failed(parsed && parsed->code == ipc::refused
                         ? "Go to the game's main menu, then Join again."
                         : "The game did not take the join: " +
                               (parsed ? parsed->message : response)),
              std::nullopt);
        return;
      }
      plan.joined_running_game = true;
    }
    const account::outcome result = client_->accept_invite(id);
    std::erase_if(work_.invites,
                  [&](const account::invite &item) { return item.id == id; });
    publish();
    if (!result.ok && !plan.joined_running_game) {
      reply(result, std::nullopt); // gone (10 minutes) or no longer a friend
      return;
    }
    log_info("launcher.friends.invite_accepted", "accepted a game invite");
    account::outcome joined_ok = result;
    joined_ok.ok = true;
    reply(joined_ok, plan);
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
} // namespace xerces_launcher
