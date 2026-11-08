#pragma once

// Xerces accounts (docs/DESIGN.md 10, 7.3 Friends): sign-up and sign-in, the
// friends list, friend requests, blocks, privacy settings and game invites,
// through the backend's REST API (backend/README.md) over crash_outbox.hpp's
// transport, and the messages of its socket (/v1/ws, websocket.hpp).
//
// The sign-in is kept in <home>\config\account.dat: the account's id, name
// and refresh token, encrypted for this Windows user (DPAPI), so other users
// of the PC and copies of the file elsewhere cannot use it. The access token
// lives in memory only. No password is ever stored.
//
// A client is used from one thread at a time.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crash_outbox.hpp"

namespace xerces_core::account {
// --------------------------------------------------------------- sign-in
struct session {
  std::string user_id;
  std::string username;
  std::string refresh_token;
};

std::filesystem::path session_file(const std::filesystem::path &home);
std::optional<session> load_session(const std::filesystem::path &home);
bool save_session(const std::filesystem::path &home, const session &value);
void forget_session(const std::filesystem::path &home);
// The text inside the file before encryption (tests).
std::string session_to_json(const session &value);
std::optional<session> session_from_json(std::string_view text);

// The backend's rules for new accounts (backend/app/schemas.py), checked
// before asking so the answer can say what to fix.
inline constexpr std::size_t username_min = 3;
inline constexpr std::size_t username_max = 24;
inline constexpr std::size_t password_min = 10;
inline constexpr std::size_t password_max = 128;
// "" when it may be tried, else why not.
std::string check_username(std::string_view name);
std::string check_new_password(std::string_view password);

// ------------------------------------------------------------- the data
struct presence {
  std::string status; // "online", "away", "in_game"; "" when offline
  std::string mode;   // "zm", "mp", "cp" or ""
  bool joinable = false;
  bool operator==(const presence &) const = default;
};
struct friend_info {
  std::string user_id;
  std::string username;
  presence now;
};
struct friend_request {
  std::string id;
  std::string user_id;
  std::string username;
};
struct friend_requests {
  std::vector<friend_request> incoming;
  std::vector<friend_request> outgoing;
};
struct block_info {
  std::string user_id;
  std::string username;
};
struct privacy_settings {
  std::string friend_requests = "everyone"; // "everyone" or "nobody"
  std::string presence = "friends";         // "friends" or "nobody"
  std::string invites = "friends";          // "friends" or "nobody"
};
struct invite {
  std::string id;
  std::string from_user_id;
  std::string from_username;
  std::string to_user_id;
  std::string to_username;
  std::string session; // session_text()
  std::string status;  // "pending", "accepted", "declined"
};
struct invites {
  std::vector<invite> incoming;
  std::vector<invite> outgoing;
};

// What a game invite carries in its session field: where the host's lobby
// is and what it plays. "xerces/1 <address> <mode> <modded: 0|1>".
struct game_session {
  std::string address; // host:port (valid_server_address)
  std::string mode = "zm";
  bool modded = true;
};
std::string session_text(const game_session &value);
std::optional<game_session> parse_session_text(std::string_view text);

// ------------------------------------------------------------ the calls
// ok: the backend did it. Otherwise reached says whether it answered; code
// and message are its error (backend/app/errors.py), or a transport error.
struct outcome {
  bool ok = false;
  bool reached = false;
  int status = 0;
  std::string code;
  std::string message;
};

class client {
public:
  // base_url: crash_base_url(), the backend; transport: winhttp_send in the
  // launcher, a fake in tests.
  client(std::string base_url, crash_transport transport);

  const std::string &base_url() const { return base_url_; }
  bool signed_in() const { return session_.has_value(); }
  const std::optional<session> &current() const { return session_; }
  // A saved sign-in (load_session); the access token comes on first need.
  void resume(std::optional<session> saved);
  // Called with the session to save whenever it changes (sign-in, a new
  // refresh token), and with nullopt when it ends (sign-out, or the backend
  // no longer takes it).
  std::function<void(const std::optional<session> &)> on_session;
  // Lets tests move the clock.
  std::function<std::chrono::steady_clock::time_point()> now =
      std::chrono::steady_clock::now;

  outcome sign_up(std::string_view username, std::string_view password);
  outcome sign_in(std::string_view username, std::string_view password);
  // Revokes the refresh token on the backend (best effort) and forgets it.
  void sign_out();

  // An access token valid for at least another minute, refreshed when
  // needed; "" when signed out or the refresh failed (error says why).
  std::string access_token(outcome *error = nullptr);

  outcome friends(std::vector<friend_info> &out);
  outcome requests(friend_requests &out);
  outcome send_request(std::string_view username);
  outcome accept_request(std::string_view request_id);
  outcome decline_request(std::string_view request_id);
  outcome cancel_request(std::string_view request_id);
  outcome remove_friend(std::string_view user_id);
  outcome blocks(std::vector<block_info> &out);
  outcome block(std::string_view username);
  outcome unblock(std::string_view user_id);
  outcome privacy(privacy_settings &out);
  outcome set_privacy(const privacy_settings &value);
  outcome list_invites(invites &out);
  outcome send_invite(std::string_view to_user_id, const game_session &game);
  outcome accept_invite(std::string_view invite_id, invite *out = nullptr);
  outcome decline_invite(std::string_view invite_id);
  // Your public address as the backend sees it (GET /v1/me/address).
  outcome own_address(std::string &out);

private:
  outcome call(std::string_view method, const std::string &path,
               const std::string &body, std::string *response, bool authorized);
  outcome take_tokens(const http_response &response, bool with_user);
  outcome refresh();
  void end_session();

  std::string base_url_;
  crash_transport transport_;
  std::optional<session> session_;
  std::string access_token_;
  std::chrono::steady_clock::time_point access_expires_{};
};

// -------------------------------------------------------- the socket
// The first message (auth) and presence updates.
std::string auth_message(std::string_view access_token);
std::string presence_message(const presence &value);
inline constexpr std::string_view ping_message = R"({"type":"ping"})";

// A message from the backend's socket (backend/app/routers/ws.py).
struct push {
  std::string type; // hello, presence, offline, friend_added, friend_request,
                    // invite, auth_ok, pong, error
  std::string user_id;
  std::string username;
  presence now;                     // presence
  std::vector<friend_info> friends; // hello: friends' presence now
  invite received;                  // invite
  std::string code;                 // error
};
std::optional<push> parse_push(std::string_view text);
} // namespace xerces_core::account
