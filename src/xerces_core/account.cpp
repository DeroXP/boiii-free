#include "account.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dpapi.h>

#include <algorithm>
#include <system_error>

#include "json.hpp"
#include "launch.hpp"
#include "launcher_home.hpp"

#pragma comment(lib, "crypt32.lib")

namespace xerces_core::account {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {
// Mixed into the encryption, so another program's DPAPI blob is never taken
// for a sign-in, nor this one for anything else.
constexpr std::string_view entropy_text = "Xerces account v1";

// Access tokens are renewed this long before they run out.
constexpr auto renew_margin = 60s;

std::optional<std::string> dpapi(std::string_view data, bool protect) {
  DATA_BLOB in{static_cast<DWORD>(data.size()),
               reinterpret_cast<BYTE *>(const_cast<char *>(data.data()))};
  DATA_BLOB entropy{
      static_cast<DWORD>(entropy_text.size()),
      reinterpret_cast<BYTE *>(const_cast<char *>(entropy_text.data()))};
  DATA_BLOB out{};
  const BOOL ok =
      protect ? CryptProtectData(&in, L"Xerces account", &entropy, nullptr,
                                 nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
              : CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                                   CRYPTPROTECT_UI_FORBIDDEN, &out);
  if (!ok) {
    return std::nullopt;
  }
  std::string result(reinterpret_cast<const char *>(out.pbData), out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return result;
}

bool safe_id(std::string_view id) {
  return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F') || c == '-';
  });
}

std::string text_of(const rapidjson::Value &object, std::string_view name) {
  return object.IsObject() ? json::get_string(object, name).value_or("")
                           : std::string();
}

presence presence_of(const rapidjson::Value &object) {
  presence out;
  if (!object.IsObject()) {
    return out;
  }
  out.status = text_of(object, "status");
  if (const auto *activity = json::member(object, "activity");
      activity && activity->IsObject()) {
    out.mode = text_of(*activity, "mode");
    if (const auto *joinable = json::member(*activity, "joinable");
        joinable && joinable->IsBool()) {
      out.joinable = joinable->GetBool();
    }
  }
  return out;
}

invite invite_of(const rapidjson::Value &object) {
  invite out;
  out.id = text_of(object, "id");
  out.from_user_id = text_of(object, "from_user_id");
  out.from_username = text_of(object, "from_username");
  out.to_user_id = text_of(object, "to_user_id");
  out.to_username = text_of(object, "to_username");
  out.session = text_of(object, "session");
  out.status = text_of(object, "status");
  return out;
}

std::vector<friend_request> requests_of(const rapidjson::Value *array) {
  std::vector<friend_request> out;
  if (!array || !array->IsArray()) {
    return out;
  }
  for (const auto &item : array->GetArray()) {
    out.push_back({text_of(item, "id"), text_of(item, "user_id"),
                   text_of(item, "username")});
  }
  return out;
}

std::vector<invite> invites_of(const rapidjson::Value *array) {
  std::vector<invite> out;
  if (!array || !array->IsArray()) {
    return out;
  }
  for (const auto &item : array->GetArray()) {
    out.push_back(invite_of(item));
  }
  return out;
}

json::parse_result parse_object(const std::string &text) {
  auto parsed = json::parse(text);
  if (parsed.ok && !parsed.document.IsObject() && !parsed.document.IsArray()) {
    parsed.ok = false;
  }
  return parsed;
}

outcome not_understood() {
  outcome out;
  out.reached = true;
  out.code = "bad_answer";
  out.message = "The Xerces server's answer could not be read.";
  return out;
}

http_request json_post(const std::string &url, std::string body) {
  http_request request;
  request.method = "POST";
  request.url = url;
  request.headers = {{"Accept", "application/json"},
                     {"Content-Type", "application/json"}};
  request.body = std::move(body);
  return request;
}

// The backend's error ({"error": {"code", "message"}}), or a transport one.
outcome answer_error(const http_response &answer) {
  outcome out;
  out.reached = answer.reached;
  out.status = answer.status;
  if (!answer.reached) {
    out.code = "unreachable";
    out.message = "The Xerces server could not be reached.";
    return out;
  }
  const auto parsed = json::parse(answer.body);
  if (parsed.ok && parsed.document.IsObject()) {
    if (const auto *error = json::member(parsed.document, "error");
        error && error->IsObject()) {
      out.code = text_of(*error, "code");
      out.message = text_of(*error, "message");
    }
  }
  if (out.message.empty()) {
    out.message = answer.status == 429
                      ? "Too many tries for now. Wait a little and try again."
                      : "The Xerces server refused (HTTP " +
                            std::to_string(answer.status) + ").";
  }
  return out;
}
} // namespace

// --------------------------------------------------------------- sign-in
fs::path session_file(const fs::path &home) {
  return home / "config" / "account.dat";
}

std::string session_to_json(const session &value) {
  return "{\"refresh_token\":" + json::quote(value.refresh_token) +
         ",\"schema\":\"xerces.account/1\",\"user_id\":" +
         json::quote(value.user_id) +
         ",\"username\":" + json::quote(value.username) + "}";
}

std::optional<session> session_from_json(std::string_view text) {
  const auto parsed = json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject() ||
      text_of(parsed.document, "schema") != "xerces.account/1") {
    return std::nullopt;
  }
  session out{text_of(parsed.document, "user_id"),
              text_of(parsed.document, "username"),
              text_of(parsed.document, "refresh_token")};
  if (!safe_id(out.user_id) || out.username.empty() ||
      out.refresh_token.empty()) {
    return std::nullopt;
  }
  return out;
}

std::optional<session> load_session(const fs::path &home) {
  const auto blob = read_text_file(session_file(home));
  if (!blob || blob->empty()) {
    return std::nullopt;
  }
  const auto text = dpapi(*blob, false);
  if (!text) {
    return std::nullopt; // another Windows user's, or damaged
  }
  return session_from_json(*text);
}

bool save_session(const fs::path &home, const session &value) {
  const auto blob = dpapi(session_to_json(value), true);
  return blob && write_text_file(session_file(home), *blob);
}

void forget_session(const fs::path &home) {
  std::error_code ec;
  fs::remove(session_file(home), ec);
}

std::string check_username(std::string_view name) {
  if (name.size() < username_min || name.size() > username_max) {
    return "A username has 3 to 24 characters.";
  }
  bool alnum = false;
  for (const char c : name) {
    const bool letter_or_digit = (c >= 'a' && c <= 'z') ||
                                 (c >= 'A' && c <= 'Z') ||
                                 (c >= '0' && c <= '9');
    if (!letter_or_digit && c != '_' && c != '.' && c != '-') {
      return "A username uses only letters A-Z, digits, '_', '.' and '-'.";
    }
    alnum = alnum || letter_or_digit;
  }
  if (!alnum) {
    return "A username needs a letter or a digit.";
  }
  return {};
}

std::string check_new_password(std::string_view password) {
  const std::size_t length = json::length(password);
  if (length < password_min || length > password_max) {
    return "A password has 10 to 128 characters.";
  }
  return {};
}

// --------------------------------------------------------- invite text
std::string session_text(const game_session &value) {
  return "xerces/1 " + value.address + " " + value.mode + " " +
         (value.modded ? "1" : "0");
}

std::optional<game_session> parse_session_text(std::string_view text) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t space = text.find(' ', start);
    const std::size_t end =
        space == std::string_view::npos ? text.size() : space;
    parts.push_back(text.substr(start, end - start));
    if (space == std::string_view::npos) {
      break;
    }
    start = space + 1;
  }
  if (parts.size() != 4 || parts[0] != "xerces/1" ||
      !valid_server_address(parts[1]) ||
      (parts[2] != "zm" && parts[2] != "mp" && parts[2] != "cp") ||
      (parts[3] != "0" && parts[3] != "1")) {
    return std::nullopt;
  }
  return game_session{std::string(parts[1]), std::string(parts[2]),
                      parts[3] == "1"};
}

// ------------------------------------------------------------ the calls
client::client(std::string base_url, crash_transport transport)
    : base_url_(std::move(base_url)), transport_(std::move(transport)) {
  while (!base_url_.empty() && base_url_.back() == '/') {
    base_url_.pop_back();
  }
}

void client::resume(std::optional<session> saved) {
  session_ = std::move(saved);
  access_token_.clear();
}

void client::end_session() {
  const bool had = session_.has_value();
  session_.reset();
  access_token_.clear();
  if (had && on_session) {
    on_session(std::nullopt);
  }
}

outcome client::call(std::string_view method, const std::string &path,
                     const std::string &body, std::string *response,
                     const bool authorized) {
  for (int attempt = 0;; ++attempt) {
    http_request request;
    request.method = std::string(method);
    request.url = base_url_ + path;
    request.headers.emplace_back("Accept", "application/json");
    if (!body.empty()) {
      request.headers.emplace_back("Content-Type", "application/json");
    }
    request.body = body;
    if (authorized) {
      outcome refused;
      const std::string token = access_token(&refused);
      if (token.empty()) {
        return refused;
      }
      request.headers.emplace_back("Authorization", "Bearer " + token);
    }
    const http_response answer = transport_(request);
    // An access token can stop working early (the password changed, or the
    // server restarted with a new key): one fresh token, one more try.
    if (answer.reached && authorized && answer.status == 401 && attempt == 0) {
      access_token_.clear();
      continue;
    }
    if (answer.reached && answer.status >= 200 && answer.status < 300) {
      outcome out;
      out.ok = true;
      out.reached = true;
      out.status = answer.status;
      if (response) {
        *response = answer.body;
      }
      return out;
    }
    outcome out = answer_error(answer);
    if (answer.reached && authorized && answer.status == 401) {
      end_session(); // a fresh token did not help either
    }
    return out;
  }
}

outcome client::take_tokens(const http_response &response, bool with_user) {
  const auto parsed = parse_object(response.body);
  if (!parsed.ok) {
    return not_understood();
  }
  const auto &doc = parsed.document;
  const std::string access = text_of(doc, "access_token");
  const std::string refresh_token = text_of(doc, "refresh_token");
  const auto *expires = json::member(doc, "expires_in");
  if (access.empty() || refresh_token.empty() || !expires ||
      !expires->IsNumber()) {
    return not_understood();
  }
  session next = session_.value_or(session{});
  if (with_user) {
    const auto *user = json::member(doc, "user");
    if (!user || !user->IsObject()) {
      return not_understood();
    }
    next.user_id = text_of(*user, "id");
    next.username = text_of(*user, "username");
    if (!safe_id(next.user_id) || next.username.empty()) {
      return not_understood();
    }
  }
  next.refresh_token = refresh_token;
  session_ = next;
  access_token_ = access;
  access_expires_ = now() + std::chrono::seconds(
                                static_cast<long long>(expires->GetDouble()));
  if (on_session) {
    on_session(session_);
  }
  outcome out;
  out.ok = true;
  out.reached = true;
  out.status = response.status;
  return out;
}

outcome client::sign_up(std::string_view username, std::string_view password) {
  for (const std::string &problem :
       {check_username(username), check_new_password(password)}) {
    if (!problem.empty()) {
      outcome out;
      out.code = "invalid_request";
      out.message = problem;
      return out;
    }
  }
  const http_response answer =
      transport_(json_post(base_url_ + "/v1/auth/register",
                           "{\"password\":" + json::quote(password) +
                               ",\"username\":" + json::quote(username) + "}"));
  if (!answer.reached || answer.status != 201) {
    return answer_error(answer);
  }
  return take_tokens(answer, true);
}

outcome client::sign_in(std::string_view username, std::string_view password) {
  const http_response answer =
      transport_(json_post(base_url_ + "/v1/auth/login",
                           "{\"password\":" + json::quote(password) +
                               ",\"username\":" + json::quote(username) + "}"));
  if (!answer.reached || answer.status != 200) {
    return answer_error(answer);
  }
  return take_tokens(answer, true);
}

void client::sign_out() {
  if (session_) {
    // Best effort: the token is forgotten here either way.
    transport_(json_post(
        base_url_ + "/v1/auth/logout",
        "{\"refresh_token\":" + json::quote(session_->refresh_token) + "}"));
  }
  end_session();
}

outcome client::refresh() {
  if (!session_) {
    outcome out;
    out.code = "signed_out";
    out.message = "Not signed in.";
    return out;
  }
  const http_response answer = transport_(json_post(
      base_url_ + "/v1/auth/refresh",
      "{\"refresh_token\":" + json::quote(session_->refresh_token) + "}"));
  if (answer.reached && answer.status == 200) {
    return take_tokens(answer, false);
  }
  outcome out = answer_error(answer);
  if (answer.reached && answer.status == 401) {
    // Signed out elsewhere, the password changed, or the sign-in is too old.
    end_session();
    out.code = "signed_out";
    out.message = "Your sign-in has ended. Sign in again.";
  }
  return out;
}

std::string client::access_token(outcome *error) {
  if (!session_) {
    if (error) {
      error->code = "signed_out";
      error->message = "Not signed in.";
    }
    return {};
  }
  if (!access_token_.empty() && now() + renew_margin < access_expires_) {
    return access_token_;
  }
  const outcome renewed = refresh();
  if (!renewed.ok) {
    if (error) {
      *error = renewed;
    }
    return {};
  }
  return access_token_;
}

outcome client::friends(std::vector<friend_info> &out) {
  std::string body;
  outcome result = call("GET", "/v1/friends", {}, &body, true);
  if (!result.ok) {
    return result;
  }
  const auto parsed = parse_object(body);
  if (!parsed.ok || !parsed.document.IsArray()) {
    return not_understood();
  }
  out.clear();
  for (const auto &item : parsed.document.GetArray()) {
    friend_info entry{text_of(item, "user_id"), text_of(item, "username"), {}};
    if (const auto *now_presence = json::member(item, "presence");
        now_presence && now_presence->IsObject()) {
      entry.now = presence_of(*now_presence);
    }
    out.push_back(std::move(entry));
  }
  return result;
}

outcome client::requests(friend_requests &out) {
  std::string body;
  outcome result = call("GET", "/v1/friends/requests", {}, &body, true);
  if (!result.ok) {
    return result;
  }
  const auto parsed = parse_object(body);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return not_understood();
  }
  out.incoming = requests_of(json::member(parsed.document, "incoming"));
  out.outgoing = requests_of(json::member(parsed.document, "outgoing"));
  return result;
}

outcome client::send_request(std::string_view username) {
  return call("POST", "/v1/friends/requests",
              "{\"username\":" + json::quote(username) + "}", nullptr, true);
}

namespace {
outcome bad_id() {
  outcome out;
  out.code = "invalid_request";
  out.message = "That is not an id.";
  return out;
}
} // namespace

outcome client::accept_request(std::string_view request_id) {
  if (!safe_id(request_id)) {
    return bad_id();
  }
  return call("POST",
              "/v1/friends/requests/" + std::string(request_id) + "/accept", {},
              nullptr, true);
}

outcome client::decline_request(std::string_view request_id) {
  if (!safe_id(request_id)) {
    return bad_id();
  }
  return call("POST",
              "/v1/friends/requests/" + std::string(request_id) + "/decline",
              {}, nullptr, true);
}

outcome client::cancel_request(std::string_view request_id) {
  if (!safe_id(request_id)) {
    return bad_id();
  }
  return call("DELETE", "/v1/friends/requests/" + std::string(request_id), {},
              nullptr, true);
}

outcome client::remove_friend(std::string_view user_id) {
  if (!safe_id(user_id)) {
    return bad_id();
  }
  return call("DELETE", "/v1/friends/" + std::string(user_id), {}, nullptr,
              true);
}

outcome client::blocks(std::vector<block_info> &out) {
  std::string body;
  outcome result = call("GET", "/v1/blocks", {}, &body, true);
  if (!result.ok) {
    return result;
  }
  const auto parsed = parse_object(body);
  if (!parsed.ok || !parsed.document.IsArray()) {
    return not_understood();
  }
  out.clear();
  for (const auto &item : parsed.document.GetArray()) {
    out.push_back({text_of(item, "user_id"), text_of(item, "username")});
  }
  return result;
}

outcome client::block(std::string_view username) {
  return call("POST", "/v1/blocks",
              "{\"username\":" + json::quote(username) + "}", nullptr, true);
}

outcome client::unblock(std::string_view user_id) {
  if (!safe_id(user_id)) {
    return bad_id();
  }
  return call("DELETE", "/v1/blocks/" + std::string(user_id), {}, nullptr,
              true);
}

outcome client::privacy(privacy_settings &out) {
  std::string body;
  outcome result = call("GET", "/v1/me", {}, &body, true);
  if (!result.ok) {
    return result;
  }
  const auto parsed = parse_object(body);
  const auto *privacy =
      parsed.ok ? json::member(parsed.document, "privacy") : nullptr;
  if (!privacy || !privacy->IsObject()) {
    return not_understood();
  }
  out.friend_requests = text_of(*privacy, "friend_requests");
  out.presence = text_of(*privacy, "presence");
  out.invites = text_of(*privacy, "invites");
  return result;
}

outcome client::set_privacy(const privacy_settings &value) {
  return call("PATCH", "/v1/me/privacy",
              "{\"friend_requests\":" + json::quote(value.friend_requests) +
                  ",\"invites\":" + json::quote(value.invites) +
                  ",\"presence\":" + json::quote(value.presence) + "}",
              nullptr, true);
}

outcome client::list_invites(invites &out) {
  std::string body;
  outcome result = call("GET", "/v1/invites", {}, &body, true);
  if (!result.ok) {
    return result;
  }
  const auto parsed = parse_object(body);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return not_understood();
  }
  out.incoming = invites_of(json::member(parsed.document, "incoming"));
  out.outgoing = invites_of(json::member(parsed.document, "outgoing"));
  return result;
}

outcome client::send_invite(std::string_view to_user_id,
                            const game_session &game) {
  if (!safe_id(to_user_id)) {
    return bad_id();
  }
  if (!parse_session_text(session_text(game))) {
    outcome out;
    out.code = "invalid_request";
    out.message = "The game to invite to is not a lobby address.";
    return out;
  }
  return call("POST", "/v1/invites",
              "{\"session\":" + json::quote(session_text(game)) +
                  ",\"to_user_id\":" + json::quote(to_user_id) + "}",
              nullptr, true);
}

outcome client::accept_invite(std::string_view invite_id, invite *out) {
  if (!safe_id(invite_id)) {
    return bad_id();
  }
  std::string body;
  outcome result =
      call("POST", "/v1/invites/" + std::string(invite_id) + "/accept", {},
           &body, true);
  if (result.ok && out) {
    const auto parsed = parse_object(body);
    if (!parsed.ok || !parsed.document.IsObject()) {
      return not_understood();
    }
    *out = invite_of(parsed.document);
  }
  return result;
}

outcome client::decline_invite(std::string_view invite_id) {
  if (!safe_id(invite_id)) {
    return bad_id();
  }
  return call("POST", "/v1/invites/" + std::string(invite_id) + "/decline", {},
              nullptr, true);
}

outcome client::own_address(std::string &out) {
  std::string body;
  outcome result = call("GET", "/v1/me/address", {}, &body, true);
  if (!result.ok) {
    return result;
  }
  const auto parsed = parse_object(body);
  out = parsed.ok ? text_of(parsed.document, "address") : std::string();
  if (out.empty()) {
    return not_understood();
  }
  return result;
}

// -------------------------------------------------------- the socket
std::string auth_message(std::string_view access_token) {
  return "{\"token\":" + json::quote(access_token) + ",\"type\":\"auth\"}";
}

std::string presence_message(const presence &value) {
  std::string out = "{\"status\":" + json::quote(value.status);
  if (!value.mode.empty() || value.joinable) {
    out += ",\"activity\":{\"joinable\":";
    out += value.joinable ? "true" : "false";
    if (!value.mode.empty()) {
      out += ",\"mode\":" + json::quote(value.mode);
    }
    out += "}";
  }
  return out + ",\"type\":\"presence\"}";
}

std::optional<push> parse_push(std::string_view text) {
  const auto parsed = json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  const auto &doc = parsed.document;
  push out;
  out.type = text_of(doc, "type");
  if (out.type.empty()) {
    return std::nullopt;
  }
  out.user_id = text_of(doc, "user_id");
  out.username = text_of(doc, "username");
  out.code = text_of(doc, "code");
  if (out.type == "presence") {
    out.now = presence_of(doc);
  } else if (out.type == "invite") {
    out.received = invite_of(doc);
  } else if (out.type == "hello") {
    if (const auto *friends = json::member(doc, "friends");
        friends && friends->IsArray()) {
      for (const auto &item : friends->GetArray()) {
        out.friends.push_back({text_of(item, "user_id"),
                               text_of(item, "username"), presence_of(item)});
      }
    }
  }
  return out;
}
} // namespace xerces_core::account
