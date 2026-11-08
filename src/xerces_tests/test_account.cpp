#include "harness.hpp"

#include <chrono>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <xerces_core/account.hpp>
#include <xerces_core/launcher_home.hpp>
#include <xerces_core/websocket.hpp>

using namespace xerces_core;
using namespace xerces_core::account;
using namespace std::chrono_literals;

namespace {
constexpr const char *alice_id = "8c6f2b2e-1d2a-4c55-9a8e-0e6b1f7d3a01";
constexpr const char *bob_id = "1b9c3d4e-5f60-4a7b-8c9d-0e1f2a3b4c5d";

// Answers in order and keeps every request it was given.
struct fake_backend {
  std::vector<http_request> seen;
  std::deque<http_response> answers;

  void answer(int status, std::string body) {
    http_response response;
    response.reached = true;
    response.status = status;
    response.body = std::move(body);
    answers.push_back(std::move(response));
  }
  crash_transport transport() {
    return [this](const http_request &request) {
      seen.push_back(request);
      if (answers.empty()) {
        return http_response{}; // not reached
      }
      http_response next = answers.front();
      answers.pop_front();
      return next;
    };
  }
};

std::string tokens(const std::string &access, const std::string &refresh,
                   bool with_user = true) {
  std::string body = "{\"access_token\":\"" + access +
                     "\",\"refresh_token\":\"" + refresh +
                     "\",\"token_type\":\"Bearer\",\"expires_in\":900";
  if (with_user) {
    body += std::string(",\"user\":{\"id\":\"") + alice_id +
            "\",\"username\":\"alice\",\"email\":null,\"privacy\":{"
            "\"friend_requests\":\"everyone\",\"presence\":\"friends\","
            "\"invites\":\"friends\"},\"created_at\":\"2026-11-07T00:00:00Z\"}";
  }
  return body + "}";
}

std::string header(const http_request &request, const std::string &name) {
  for (const auto &[key, value] : request.headers) {
    if (key == name) {
      return value;
    }
  }
  return {};
}
} // namespace

TEST_CASE("account: the saved sign-in is encrypted and read back") {
  const auto home =
      std::filesystem::temp_directory_path() / "xerces_tests_account";
  std::filesystem::remove_all(home);
  const session saved{alice_id, "alice", "refresh-secret-123"};
  REQUIRE(save_session(home, saved));
  const auto bytes = read_text_file(session_file(home));
  REQUIRE(bytes.has_value());
  // Encrypted for this Windows user: neither the token nor the name shows.
  CHECK(bytes->find("refresh-secret-123") == std::string::npos);
  CHECK(bytes->find("alice") == std::string::npos);
  const auto loaded = load_session(home);
  REQUIRE(loaded.has_value());
  CHECK_EQ(loaded->user_id, std::string(alice_id));
  CHECK_EQ(loaded->username, std::string("alice"));
  CHECK_EQ(loaded->refresh_token, std::string("refresh-secret-123"));
  // A damaged file is no sign-in.
  write_text_file(session_file(home), "not dpapi");
  CHECK(!load_session(home).has_value());
  forget_session(home);
  CHECK(!std::filesystem::exists(session_file(home)));
  CHECK(!load_session(home).has_value());
  std::filesystem::remove_all(home);
}

TEST_CASE("account: the session text needs every field") {
  const session value{alice_id, "alice", "r"};
  CHECK(session_from_json(session_to_json(value)).has_value());
  CHECK(!session_from_json("{}").has_value());
  CHECK(!session_from_json(R"({"schema":"xerces.account/1","user_id":"../x",)"
                           R"("username":"a","refresh_token":"r"})")
             .has_value());
}

TEST_CASE("account: usernames and passwords are checked as the backend does") {
  CHECK(check_username("alice").empty());
  CHECK(check_username("a.b-c_9").empty());
  CHECK(!check_username("al").empty());
  CHECK(!check_username(std::string(25, 'a')).empty());
  CHECK(!check_username("bad name").empty());
  CHECK(!check_username("___").empty());
  CHECK(check_new_password("correct horse").empty());
  CHECK(!check_new_password("short").empty());
  CHECK(!check_new_password(std::string(129, 'x')).empty());
}

TEST_CASE("account: an invite's session names the lobby and nothing else") {
  const game_session game{"70.246.76.146:27017", "zm", true};
  CHECK_EQ(session_text(game),
           std::string("xerces/1 70.246.76.146:27017 zm 1"));
  const auto back = parse_session_text(session_text(game));
  REQUIRE(back.has_value());
  CHECK_EQ(back->address, game.address);
  CHECK_EQ(back->mode, std::string("zm"));
  CHECK(back->modded);
  CHECK(parse_session_text("xerces/1 [2001:db8::1]:27017 mp 0").has_value());
  for (const char *bad :
       {"", "xerces/1", "xerces/2 1.2.3.4:27017 zm 1",
        "xerces/1 1.2.3.4:27017;quit zm 1", "xerces/1 1.2.3.4:27017 xx 1",
        "xerces/1 1.2.3.4:27017 zm 2", "xerces/1 1.2.3.4:27017 zm 1 extra",
        "xerces/1  1.2.3.4:27017 zm 1"}) {
    CHECK(!parse_session_text(bad).has_value());
  }
}

TEST_CASE("account: signing in keeps the tokens and says so") {
  fake_backend backend;
  client api("https://example.test/", backend.transport());
  std::vector<std::optional<session>> saved;
  api.on_session = [&](const std::optional<session> &s) { saved.push_back(s); };
  backend.answer(200, tokens("access-1", "refresh-1"));
  const outcome result = api.sign_in("alice", "correct horse battery");
  CHECK(result.ok);
  REQUIRE_EQ(backend.seen.size(), std::size_t(1));
  CHECK_EQ(backend.seen[0].url,
           std::string("https://example.test/v1/auth/login"));
  CHECK_EQ(backend.seen[0].method, std::string("POST"));
  CHECK(backend.seen[0].body.find("\"username\":\"alice\"") !=
        std::string::npos);
  REQUIRE(api.signed_in());
  CHECK_EQ(api.current()->username, std::string("alice"));
  REQUIRE_EQ(saved.size(), std::size_t(1));
  CHECK_EQ(saved[0]->refresh_token, std::string("refresh-1"));
  CHECK_EQ(api.access_token(), std::string("access-1"));
  CHECK_EQ(backend.seen.size(), std::size_t(1)); // no refresh needed yet
}

TEST_CASE("account: a refused sign-in keeps the backend's message") {
  fake_backend backend;
  client api("https://example.test", backend.transport());
  backend.answer(401, R"({"error":{"code":"invalid_credentials",)"
                      R"("message":"Username or password is incorrect."}})");
  const outcome result = api.sign_in("alice", "wrong password");
  CHECK(!result.ok);
  CHECK(result.reached);
  CHECK_EQ(result.code, std::string("invalid_credentials"));
  CHECK_EQ(result.message, std::string("Username or password is incorrect."));
  CHECK(!api.signed_in());
  // Unreachable: no answer at all.
  const outcome offline = api.sign_in("alice", "x");
  CHECK(!offline.reached);
  CHECK_EQ(offline.code, std::string("unreachable"));
}

TEST_CASE("account: sign-up is checked here before it is sent") {
  fake_backend backend;
  client api("https://example.test", backend.transport());
  const outcome result = api.sign_up("a b", "correct horse battery");
  CHECK(!result.ok);
  CHECK(!result.message.empty());
  CHECK(backend.seen.empty());
  backend.answer(201, tokens("access-1", "refresh-1"));
  CHECK(api.sign_up("alice", "correct horse battery").ok);
  CHECK_EQ(backend.seen.back().url,
           std::string("https://example.test/v1/auth/register"));
}

TEST_CASE("account: an old access token is renewed, and the refresh token "
          "rotates") {
  fake_backend backend;
  client api("https://example.test", backend.transport());
  auto clock = std::chrono::steady_clock::now();
  api.now = [&] { return clock; };
  std::vector<std::optional<session>> saved;
  api.on_session = [&](const std::optional<session> &s) { saved.push_back(s); };
  api.resume(session{alice_id, "alice", "refresh-1"});
  // First call: no access token yet, so a refresh comes first.
  backend.answer(200, tokens("access-2", "refresh-2", false));
  backend.answer(200, "[]");
  std::vector<friend_info> friends;
  CHECK(api.friends(friends).ok);
  REQUIRE_EQ(backend.seen.size(), std::size_t(2));
  CHECK_EQ(backend.seen[0].url,
           std::string("https://example.test/v1/auth/refresh"));
  CHECK(backend.seen[0].body.find("refresh-1") != std::string::npos);
  CHECK_EQ(header(backend.seen[1], "Authorization"),
           std::string("Bearer access-2"));
  REQUIRE_EQ(saved.size(), std::size_t(1));
  CHECK_EQ(saved[0]->refresh_token, std::string("refresh-2"));
  CHECK_EQ(saved[0]->username, std::string("alice")); // kept from before
  // 15 minutes later the token is too old: renewed before the call.
  clock += 15min;
  backend.answer(200, tokens("access-3", "refresh-3", false));
  backend.answer(200, "[]");
  CHECK(api.friends(friends).ok);
  CHECK_EQ(header(backend.seen.back(), "Authorization"),
           std::string("Bearer access-3"));
}

TEST_CASE("account: a 401 gets one fresh token, then the sign-in ends") {
  fake_backend backend;
  client api("https://example.test", backend.transport());
  std::vector<std::optional<session>> saved;
  api.on_session = [&](const std::optional<session> &s) { saved.push_back(s); };
  api.resume(session{alice_id, "alice", "refresh-1"});
  backend.answer(200, tokens("access-1", "refresh-2", false));
  backend.answer(401, R"({"error":{"code":"invalid_token","message":"x"}})");
  backend.answer(200, tokens("access-2", "refresh-3", false));
  backend.answer(200, "[]");
  std::vector<block_info> blocks;
  CHECK(api.blocks(blocks).ok); // refresh, 401, refresh, retry
  CHECK_EQ(backend.seen.size(), std::size_t(4));
  CHECK(api.signed_in());
  // The refresh token is refused: signed out, and the caller hears it.
  api.resume(session{alice_id, "alice", "refresh-3"});
  backend.answer(401, R"({"error":{"code":"invalid_token","message":"x"}})");
  const outcome ended = api.blocks(blocks);
  CHECK(!ended.ok);
  CHECK_EQ(ended.code, std::string("signed_out"));
  CHECK(!api.signed_in());
  REQUIRE(!saved.empty());
  CHECK(!saved.back().has_value());
}

TEST_CASE("account: lists and settings are read from the backend's shapes") {
  fake_backend backend;
  client api("https://example.test", backend.transport());
  api.resume(session{alice_id, "alice", "refresh-1"});
  backend.answer(200, tokens("a", "r", false));
  backend.answer(
      200, std::string(R"([{"user_id":")") + bob_id +
               R"(","username":"bob","since":"2026-11-07T00:00:00Z",)"
               R"("presence":{"status":"in_game","activity":{"mode":"zm",)"
               R"("joinable":true,"pack_code":null}}},)"
               R"({"user_id":"2b9c3d4e-5f60-4a7b-8c9d-0e1f2a3b4c5d",)"
               R"("username":"carol","since":"2026-11-07T00:00:00Z",)"
               R"("presence":null}])");
  std::vector<friend_info> friends;
  REQUIRE(api.friends(friends).ok);
  REQUIRE_EQ(friends.size(), std::size_t(2));
  CHECK_EQ(friends[0].username, std::string("bob"));
  CHECK_EQ(friends[0].now.status, std::string("in_game"));
  CHECK_EQ(friends[0].now.mode, std::string("zm"));
  CHECK(friends[0].now.joinable);
  CHECK(friends[1].now.status.empty()); // offline

  backend.answer(200,
                 R"({"incoming":[{"id":"aa-1","user_id":"bb-1",)"
                 R"("username":"dave","created_at":"x","expires_at":"y"}],)"
                 R"("outgoing":[]})");
  friend_requests requests;
  REQUIRE(api.requests(requests).ok);
  REQUIRE_EQ(requests.incoming.size(), std::size_t(1));
  CHECK_EQ(requests.incoming[0].username, std::string("dave"));

  backend.answer(200, std::string(R"({"id":")") + alice_id +
                          R"(","username":"alice","email":null,)"
                          R"("privacy":{"friend_requests":"nobody",)"
                          R"("presence":"friends","invites":"nobody"},)"
                          R"("created_at":"x"})");
  privacy_settings privacy;
  REQUIRE(api.privacy(privacy).ok);
  CHECK_EQ(privacy.friend_requests, std::string("nobody"));
  CHECK_EQ(privacy.invites, std::string("nobody"));

  backend.answer(200, R"({"address":"70.246.76.146"})");
  std::string address;
  REQUIRE(api.own_address(address).ok);
  CHECK_EQ(address, std::string("70.246.76.146"));
}

TEST_CASE("account: ids and invites are checked before anything is sent") {
  fake_backend backend;
  client api("https://example.test", backend.transport());
  api.resume(session{alice_id, "alice", "refresh-1"});
  CHECK(!api.remove_friend("../../admin").ok);
  CHECK(!api.accept_request("a b").ok);
  CHECK(!api.send_invite(bob_id, game_session{"x;quit", "zm", true}).ok);
  CHECK(backend.seen.empty());
  backend.answer(200, tokens("a", "r", false));
  backend.answer(201, "{}");
  CHECK(api.send_invite(bob_id, game_session{"70.246.76.146:27017", "zm", true})
            .ok);
  CHECK_EQ(backend.seen.back().url,
           std::string("https://example.test/v1/invites"));
  CHECK(backend.seen.back().body.find(
            R"("session":"xerces/1 70.246.76.146:27017 zm 1")") !=
        std::string::npos);
}

TEST_CASE("account: socket messages") {
  CHECK_EQ(auth_message("tok"),
           std::string(R"({"token":"tok","type":"auth"})"));
  CHECK_EQ(presence_message(presence{"online", "", false}),
           std::string(R"({"status":"online","type":"presence"})"));
  CHECK_EQ(presence_message(presence{"in_game", "zm", true}),
           std::string(R"({"status":"in_game","activity":{"joinable":true,)"
                       R"("mode":"zm"},"type":"presence"})"));

  const auto hello = parse_push(
      std::string(R"({"type":"hello","user_id":")") + alice_id +
      R"(","friends":[{"user_id":")" + bob_id +
      R"(","status":"in_game","activity":{"mode":"zm","joinable":false}}]})");
  REQUIRE(hello.has_value());
  CHECK_EQ(hello->type, std::string("hello"));
  REQUIRE_EQ(hello->friends.size(), std::size_t(1));
  CHECK_EQ(hello->friends[0].now.mode, std::string("zm"));

  const auto moved =
      parse_push(std::string(R"({"type":"presence","user_id":")") + bob_id +
                 R"(","status":"online","activity":null})");
  REQUIRE(moved.has_value());
  CHECK_EQ(moved->now.status, std::string("online"));
  CHECK(moved->now.mode.empty());

  const auto invited = parse_push(
      std::string(R"({"type":"invite","id":"cc-1","from_user_id":")") + bob_id +
      R"(","from_username":"bob","to_user_id":")" + alice_id +
      R"(","to_username":"alice","session":"xerces/1 1.2.3.4:27017 zm 1",)"
      R"("pack_code":null,"status":"pending","created_at":"x","expires_at":"y"})");
  REQUIRE(invited.has_value());
  CHECK_EQ(invited->received.from_username, std::string("bob"));
  CHECK(parse_session_text(invited->received.session).has_value());

  CHECK(!parse_push("not json").has_value());
  CHECK(!parse_push("{}").has_value());
}

TEST_CASE("websocket: the socket address follows the backend's") {
  CHECK_EQ(websocket_url("https://x.example/", "/v1/ws"),
           std::string("wss://x.example/v1/ws"));
  CHECK_EQ(websocket_url("http://127.0.0.1:8080", "/v1/ws"),
           std::string("ws://127.0.0.1:8080/v1/ws"));
  websocket socket;
  std::string error;
  CHECK(!socket.connect("wss://elsewhere.example/v1/ws", &error));
  CHECK(!error.empty());
  CHECK(!socket.connect("ws://example.org/v1/ws", &error));
  CHECK(!socket.open());
}
