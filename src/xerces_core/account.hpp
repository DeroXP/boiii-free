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
  std::string session;   // session_text()
  std::string pack_code; // XRC-XXXXXXXX: the host's mods, or ""
  std::string status;    // "pending", "accepted", "declined"
};
struct invites {
  std::vector<invite> incoming;
  std::vector<invite> outgoing;
};

// What a game invite carries in its session field: where the host's lobby
// is and what it plays. "xerces/1 <address> <mode> <modded: 0|1>", or with
// the host's ticket for this one invite (DESIGN.md 10.13)
// "xerces/2 <address> <mode> <modded> <ticket>".
struct game_session {
  std::string address; // host:port (valid_server_address)
  std::string mode = "zm";
  bool modded = true;
  std::string ticket{}; // 32 lower-case hex digits, or "" (version 1)
};
std::string session_text(const game_session &value);
std::optional<game_session> parse_session_text(std::string_view text);

// A fresh ticket: 16 random bytes as 32 lower-case hex digits.
std::string new_ticket();

// Invite links (DESIGN.md 10.12). The code in its canonical form,
// "XJN-XXXXXXXX", from what a player pastes or a link opens: the code in any
// case, without its dash, Crockford look-alikes (O, I, L), or a link that
// ends in one (https://.../join/<code>, xerces://join/<code>).
std::optional<std::string> link_code_from(std::string_view text);
// The same for a pack code, "XRC-XXXXXXXX".
std::optional<std::string> pack_code_from(std::string_view text);

// Discord's join secret for an invite link (Ask to Join), "xerces1
// XJN-XXXXXXXX", and the link's code back from one; nothing else is taken.
std::string discord_join_secret(std::string_view link_code);
std::optional<std::string> link_from_discord_secret(std::string_view secret);

struct invite_link {
  std::string code;
  std::string path; // "/join/<code>", on the backend
  std::string mode;
};
struct link_owner {
  std::string code;
  std::string host_user_id;
  std::string host_username;
  std::string mode;
};
struct knock_info {
  std::string id;
  std::string from_user_id;
  std::string from_username;
  std::string to_user_id;
  std::string to_username;
  std::string mode;
  std::string status; // "pending", "accepted" (a declined one reads pending
                      // to whoever knocked)
};
struct knock_list {
  std::vector<knock_info> incoming;
  std::vector<knock_info> outgoing;
};

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
  // out: the invite as the backend made it (its id, for the hole punch).
  outcome send_invite(std::string_view to_user_id, const game_session &game,
                      std::string_view pack_code = {}, invite *out = nullptr);
  outcome accept_invite(std::string_view invite_id, invite *out = nullptr);
  outcome decline_invite(std::string_view invite_id);
  // Your public address as the backend sees it (GET /v1/me/address).
  outcome own_address(std::string &out);

  // Mod packs on the backend (POST /v1/packs, GET /v1/packs/<code>):
  // pack_json is a pack's JSON (pack_to_json).
  outcome create_pack(const std::string &pack_json, std::string &code);
  outcome fetch_pack(std::string_view code, std::string &pack_json);

  // Hole punching (DESIGN.md 10.13): this joiner's game listens on port;
  // delivered says whether the host's launcher was there to take it.
  outcome punch(std::string_view invite_id, std::uint16_t port,
                bool *delivered = nullptr);

  // Invite links and knocks (DESIGN.md 10.12).
  outcome create_link(std::string_view mode, invite_link &out);
  outcome own_link(std::optional<invite_link> &out);
  outcome delete_link();
  outcome link(std::string_view code, link_owner &out);
  outcome knock(std::string_view code, knock_info *out = nullptr);
  outcome knocks(knock_list &out);
  outcome accept_knock(std::string_view knock_id);
  outcome decline_knock(std::string_view knock_id);

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
                    // invite, knock, punch, auth_ok, pong, error
  std::string user_id;
  std::string username;
  presence now;                     // presence
  std::vector<friend_info> friends; // hello: friends' presence now
  invite received;                  // invite
  knock_info knocked;               // knock
  std::string invite_id;            // punch: the invite it is for
  std::string from_user_id;         // punch: the joiner
  std::string address;              // punch: the joiner's ip:port
  std::string code;                 // error
};
std::optional<push> parse_push(std::string_view text);
} // namespace xerces_core::account
