#include "join_password.hpp"

#include <algorithm>
#include <charconv>
#include <random>

#include "join_check.hpp"

namespace xerces_core::join_password {
namespace {
constexpr std::string_view proof_label = "xerces-join-v1";
constexpr std::chrono::seconds minute{60};

bool is_lower_hex(const char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

int hex_value(const char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Fields split on single spaces; an empty field (two spaces, or a space at
// either end) makes the message invalid.
std::optional<std::vector<std::string_view>> split_fields(std::string_view text,
                                                          std::size_t count) {
  if (text.size() > max_payload) {
    return std::nullopt;
  }
  std::vector<std::string_view> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find(' ', start);
    const std::string_view field = text.substr(
        start,
        end == std::string_view::npos ? std::string_view::npos : end - start);
    if (field.empty()) {
      return std::nullopt;
    }
    out.push_back(field);
    if (out.size() > count) {
      return std::nullopt;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  if (out.size() != count || out[0] != protocol) {
    return std::nullopt;
  }
  return out;
}

bool parse_u32(std::string_view text, std::uint32_t &value) {
  if (text.empty() || text.size() > 10) {
    return false;
  }
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

// "1 second", "30 seconds"; 0 shows as 1.
std::string seconds_text(const std::uint32_t seconds) {
  const std::uint32_t shown = seconds == 0 ? 1 : seconds;
  return std::to_string(shown) + (shown == 1 ? " second" : " seconds");
}

std::string proof_input(std::string_view nonce, std::string_view host_address,
                        std::string_view session) {
  std::string input(proof_label);
  input.push_back('\n');
  input.append(nonce);
  input.push_back('\n');
  input.append(host_address);
  input.push_back('\n');
  input.append(session);
  return input;
}
} // namespace

// ------------------------------------------------------ the key and proof
key derive_key(std::string_view password, std::string_view host_salt) {
  std::string salt(key_label);
  salt.push_back('\n');
  salt.append(host_salt);
  return pbkdf2_sha256(password, salt, key_rounds);
}

std::string proof(const key &password_key, std::string_view nonce,
                  std::string_view host_address, std::string_view session) {
  const std::string_view key_text(
      reinterpret_cast<const char *>(password_key.data()), password_key.size());
  return sha256::to_hex(
      hmac_sha256(key_text, proof_input(nonce, host_address, session)));
}

bool proof_matches(const key &password_key, std::string_view nonce,
                   std::string_view host_address, std::string_view session,
                   std::string_view proof_hex) {
  sha256::digest given{};
  if (proof_hex.size() != given.size() * 2) {
    return false;
  }
  for (std::size_t i = 0; i < given.size(); ++i) {
    const int high = hex_value(proof_hex[i * 2]);
    const int low = hex_value(proof_hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    given[i] = static_cast<std::uint8_t>(high * 16 + low);
  }
  const std::string_view key_text(
      reinterpret_cast<const char *>(password_key.data()), password_key.size());
  const sha256::digest expected =
      hmac_sha256(key_text, proof_input(nonce, host_address, session));
  return equal_constant_time(expected, given);
}

// ------------------------------------------------------- what getinfo says
host_need host_needs(std::string_view xerces_password,
                     std::string_view net_password_hash) {
  if (!xerces_password.empty()) {
    return xerces_password == protocol ? host_need::checked : host_need::newer;
  }
  if (net_password_hash.empty() || net_password_hash == "0") {
    return host_need::none;
  }
  return host_need::legacy_hash;
}

// -------------------------------------------------------------- messages
std::string_view result_name(const result code) {
  switch (code) {
  case result::ok:
    return "ok";
  case result::open:
    return "open";
  case result::wrong:
    return "wrong";
  case result::needed:
    return "needed";
  case result::wait:
    return "wait";
  case result::busy:
    return "busy";
  case result::paused:
    return "paused";
  case result::stale:
    return "stale";
  case result::address:
    return "address";
  }
  return "needed";
}

std::optional<result> parse_result_name(std::string_view text) {
  for (const result code :
       {result::ok, result::open, result::wrong, result::needed, result::wait,
        result::busy, result::paused, result::stale, result::address}) {
    if (text == result_name(code)) {
      return code;
    }
  }
  return std::nullopt;
}

bool carries_seconds(const result code) {
  return code == result::wait || code == result::busy || code == result::paused;
}

std::string format(const challenge_message &message) {
  return std::string(protocol) + " " + message.session;
}

std::string format(const nonce_message &message) {
  return std::string(protocol) + " " + message.session + " " + message.nonce +
         " " + message.host_salt;
}

std::string format(const proof_message &message) {
  return std::string(protocol) + " " + message.session + " " + message.nonce +
         " " + message.host_address + " " + message.proof;
}

std::string format(const result_message &message) {
  std::string out = std::string(protocol) + " " + message.session + " " +
                    std::string(result_name(message.code));
  if (carries_seconds(message.code)) {
    out += " " + std::to_string(message.wait_seconds);
  }
  return out;
}

std::optional<challenge_message> parse_challenge(std::string_view payload) {
  const auto fields = split_fields(payload, 2);
  if (!fields || !valid_token((*fields)[1])) {
    return std::nullopt;
  }
  return challenge_message{std::string((*fields)[1])};
}

std::optional<nonce_message> parse_nonce(std::string_view payload) {
  const auto fields = split_fields(payload, 4);
  if (!fields || !valid_token((*fields)[1]) || !valid_token((*fields)[2]) ||
      !valid_token((*fields)[3])) {
    return std::nullopt;
  }
  return nonce_message{std::string((*fields)[1]), std::string((*fields)[2]),
                       std::string((*fields)[3])};
}

std::optional<proof_message> parse_proof(std::string_view payload) {
  const auto fields = split_fields(payload, 5);
  if (!fields || !valid_token((*fields)[1]) || !valid_token((*fields)[2]) ||
      !parse_endpoint((*fields)[3]) || (*fields)[4].size() != 64 ||
      !std::all_of((*fields)[4].begin(), (*fields)[4].end(), is_lower_hex)) {
    return std::nullopt;
  }
  return proof_message{std::string((*fields)[1]), std::string((*fields)[2]),
                       std::string((*fields)[3]), std::string((*fields)[4])};
}

std::optional<result_message> parse_result(std::string_view payload) {
  std::optional<std::vector<std::string_view>> fields =
      split_fields(payload, 3);
  if (!fields) {
    fields = split_fields(payload, 4);
  }
  if (!fields) {
    return std::nullopt;
  }
  const std::string_view session = (*fields)[1];
  if (session != "-" && !valid_token(session)) {
    return std::nullopt;
  }
  const std::optional<result> code = parse_result_name((*fields)[2]);
  if (!code) {
    return std::nullopt;
  }
  result_message message{std::string(session), *code, 0};
  if (carries_seconds(*code)) {
    if (fields->size() != 4 || !parse_u32((*fields)[3], message.wait_seconds)) {
      return std::nullopt;
    }
  } else if (fields->size() != 3) {
    return std::nullopt;
  }
  return message;
}

bool valid_token(std::string_view token) {
  return token.size() == token_bytes * 2 &&
         std::all_of(token.begin(), token.end(), is_lower_hex);
}

std::string random_token() {
  std::random_device device; // rand_s on Windows
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(token_bytes * 2);
  for (std::size_t i = 0; i < token_bytes; i += 4) {
    const std::uint32_t value = device();
    for (int shift = 0; shift < 32; shift += 8) {
      const unsigned byte = (value >> shift) & 0xFF;
      out.push_back(digits[byte >> 4]);
      out.push_back(digits[byte & 15]);
    }
  }
  return out;
}

std::string ipv4_endpoint::ip() const {
  return std::to_string(octets[0]) + "." + std::to_string(octets[1]) + "." +
         std::to_string(octets[2]) + "." + std::to_string(octets[3]);
}

std::optional<ipv4_endpoint> parse_endpoint(std::string_view text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos || colon == 0 ||
      text.find(':', colon + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  ipv4_endpoint out;
  std::string_view ip = text.substr(0, colon);
  for (int part = 0; part < 4; ++part) {
    const std::size_t dot = ip.find('.');
    if ((part < 3) == (dot == std::string_view::npos)) {
      return std::nullopt;
    }
    const std::string_view digits = ip.substr(0, dot);
    std::uint32_t value = 0;
    if (digits.size() > 3 || !parse_u32(digits, value) || value > 255) {
      return std::nullopt;
    }
    out.octets[part] = static_cast<std::uint8_t>(value);
    ip =
        dot == std::string_view::npos ? std::string_view{} : ip.substr(dot + 1);
  }
  std::uint32_t port = 0;
  const std::string_view port_text = text.substr(colon + 1);
  if (port_text.size() > 5 || !parse_u32(port_text, port) || port == 0 ||
      port > 65535) {
    return std::nullopt;
  }
  out.port = static_cast<std::uint16_t>(port);
  return out;
}

std::string endpoint_ip(std::string_view text) {
  const std::optional<ipv4_endpoint> endpoint = parse_endpoint(text);
  return endpoint ? endpoint->ip() : std::string{};
}

// ------------------------------------------------- the host's addresses
bool is_own_address(std::string_view claimed, const own_addresses &own) {
  const std::optional<ipv4_endpoint> endpoint = parse_endpoint(claimed);
  if (!endpoint) {
    return false;
  }
  if (endpoint->octets[0] == 0 || endpoint->octets[0] >= 224) {
    return false; // nobody dials these
  }
  const std::string ip = endpoint->ip();
  const ipv4_scope scope = classify_ipv4(ip);
  const bool this_pc =
      scope == ipv4_scope::loopback ||
      std::find(own.ips.begin(), own.ips.end(), ip) != own.ips.end();
  if (this_pc) {
    return endpoint->port == own.port;
  }
  if (scope != ipv4_scope::shared && scope != ipv4_scope::public_net) {
    return false; // another PC's LAN address, or not an address at all
  }
  if (const std::optional<ipv4_endpoint> mapped = parse_endpoint(own.mapped);
      mapped && classify_ipv4(mapped->ip()) == ipv4_scope::public_net) {
    return mapped->ip() == ip && mapped->port == endpoint->port;
  }
  return true;
}

bool is_own_socket(std::string_view peer, const own_addresses &own) {
  const std::optional<ipv4_endpoint> endpoint = parse_endpoint(peer);
  if (!endpoint || own.port == 0 || endpoint->port != own.port) {
    return false;
  }
  const std::string ip = endpoint->ip();
  return classify_ipv4(ip) == ipv4_scope::loopback ||
         std::find(own.ips.begin(), own.ips.end(), ip) != own.ips.end();
}

// --------------------------------------------------------- the host's state
host_guard::window *host_guard::find_window(std::vector<window> &windows,
                                            std::string_view ip) {
  for (window &entry : windows) {
    if (entry.ip == ip) {
      return &entry;
    }
  }
  return nullptr;
}

const host_guard::window *
host_guard::find_window(const std::vector<window> &windows,
                        std::string_view ip) {
  for (const window &entry : windows) {
    if (entry.ip == ip) {
      return &entry;
    }
  }
  return nullptr;
}

void host_guard::forget_old(window &entry, const clock::time_point now) {
  while (!entry.events.empty() && now - entry.events.front() >= minute) {
    entry.events.pop_front();
  }
}

host_guard::window *host_guard::touch_window(std::vector<window> &windows,
                                             std::string_view ip,
                                             const clock::time_point now,
                                             const bool evict) {
  if (window *entry = find_window(windows, ip)) {
    forget_old(*entry, now);
    return entry;
  }
  if (windows.size() >= max_tracked_ips) {
    // An address with nothing in the last minute is the same as none.
    for (window &entry : windows) {
      forget_old(entry, now);
    }
    std::erase_if(windows,
                  [](const window &entry) { return entry.events.empty(); });
  }
  if (windows.size() >= max_tracked_ips) {
    if (!evict) {
      return nullptr;
    }
    // The address seen least recently goes.
    const auto last = [](const window &entry) {
      return entry.events.empty() ? clock::time_point::min()
                                  : entry.events.back();
    };
    const auto oldest = std::min_element(
        windows.begin(), windows.end(),
        [&](const window &a, const window &b) { return last(a) < last(b); });
    windows.erase(oldest);
  }
  windows.push_back(window{std::string(ip), {}});
  return &windows.back();
}

std::uint32_t host_guard::window_wait(const window *entry,
                                      const std::size_t limit,
                                      const clock::time_point now) {
  if (!entry || entry->events.size() < limit) {
    return 0;
  }
  // The oldest event that still counts must leave the minute first.
  const clock::time_point frees =
      entry->events[entry->events.size() - limit] + minute;
  if (frees <= now) {
    return 0;
  }
  const auto left =
      std::chrono::duration_cast<std::chrono::milliseconds>(frees - now)
          .count();
  return static_cast<std::uint32_t>((left + 999) / 1000);
}

std::uint32_t host_guard::wait_seconds(std::string_view peer,
                                       const clock::time_point now) const {
  return window_wait(find_window(wrong_, endpoint_ip(peer)),
                     max_wrong_per_minute, now);
}

std::uint32_t host_guard::paused_seconds(const clock::time_point now) const {
  return window_wait(&wrong_all_, max_wrong_per_minute_all, now);
}

host_guard::challenge_answer
host_guard::challenge(std::string_view peer, std::string_view session,
                      std::string nonce, const clock::time_point now) {
  challenge_answer answer;
  const std::string ip = endpoint_ip(peer);
  if (ip.empty() || !valid_token(session) || !valid_token(nonce)) {
    answer.code = result::stale;
    return answer;
  }
  // Say so now rather than after the joiner's answer.
  if (const std::uint32_t wait = wait_seconds(peer, now)) {
    answer.code = result::wait;
    answer.wait_seconds = wait;
    return answer;
  }
  if (const std::uint32_t wait = paused_seconds(now)) {
    answer.code = result::paused;
    answer.wait_seconds = wait;
    return answer;
  }
  // Challenges cost the host little and test no password, so a full table
  // may drop the address seen least recently; wrong answers have their own
  // limits.
  window &asked = *touch_window(asked_, ip, now, true);
  if (const std::uint32_t wait =
          window_wait(&asked, max_challenges_per_minute, now)) {
    answer.code = result::busy;
    answer.wait_seconds = wait;
    return answer;
  }
  asked.events.push_back(now);

  // The same challenge again (its answer was lost, or crossed a resend):
  // the same nonce, so the answer to either reply matches.
  for (const issued &entry : challenges_) {
    if (entry.peer == peer && entry.session == session && entry.expires > now) {
      answer.code = result::ok;
      answer.nonce = entry.nonce;
      return answer;
    }
  }
  // Another session from the same peer does not replace an older challenge:
  // someone who sends with a joiner's address cannot take the joiner's away.
  // Each IP address holds at most max_challenges_per_minute of them.
  std::erase_if(challenges_,
                [&](const issued &entry) { return entry.expires <= now; });
  if (challenges_.size() >= max_challenges) {
    challenges_.erase(std::min_element(challenges_.begin(), challenges_.end(),
                                       [](const issued &a, const issued &b) {
                                         return a.expires < b.expires;
                                       }));
  }
  challenges_.push_back(issued{std::string(peer), std::string(session), nonce,
                               now + challenge_lifetime});
  answer.code = result::ok;
  answer.nonce = std::move(nonce);
  return answer;
}

result_message host_guard::check(std::string_view peer,
                                 const proof_message &answer,
                                 const key &password_key,
                                 const own_addresses &own,
                                 const clock::time_point now) {
  result_message reply{answer.session, result::stale, 0};
  const auto found = std::find_if(
      challenges_.begin(), challenges_.end(), [&](const issued &entry) {
        return entry.peer == peer && entry.session == answer.session;
      });
  // A session or nonce that was not given to peer tests no password (the
  // proof needs the nonce), so it changes nothing: someone who sends with a
  // joiner's address cannot use the joiner's challenge up.
  if (found == challenges_.end() || found->nonce != answer.nonce) {
    return reply;
  }
  const bool live = found->expires > now;
  challenges_.erase(found); // used up, whatever comes next
  if (!live) {
    return reply;
  }
  if (const std::uint32_t wait = wait_seconds(peer, now)) {
    reply.code = result::wait;
    reply.wait_seconds = wait;
    return reply;
  }
  if (const std::uint32_t wait = paused_seconds(now)) {
    reply.code = result::paused;
    reply.wait_seconds = wait;
    return reply;
  }
  if (!is_own_address(answer.host_address, own)) {
    reply.code = result::address;
    return reply;
  }
  // A wrong answer must be counted before it is looked at. The table always
  // has room (max_wrong_per_minute_all < max_tracked_ips); were it full,
  // the answer waits rather than an address's count starting again.
  window *wrong = touch_window(wrong_, endpoint_ip(peer), now, false);
  if (!wrong) {
    reply.code = result::paused;
    reply.wait_seconds = 60;
    return reply;
  }
  if (!proof_matches(password_key, answer.nonce, answer.host_address,
                     answer.session, answer.proof)) {
    wrong->events.push_back(now);
    forget_old(wrong_all_, now);
    wrong_all_.events.push_back(now);
    reply.code = result::wrong;
    return reply;
  }
  admit(peer);
  reply.code = result::ok;
  return reply;
}

bool host_guard::lets_in(const sender_kind kind, std::string_view peer,
                         const bool password_set, const own_addresses &own) {
  if (!password_set) {
    // As before this check existed; the sender is remembered.
    if (kind == sender_kind::ip) {
      admit(peer);
    }
    return true;
  }
  switch (kind) {
  case sender_kind::engine_local:
    return true;
  case sender_kind::ip:
    return is_own_socket(peer, own) || admitted(peer);
  case sender_kind::other:
    break;
  }
  return false;
}

bool host_guard::admitted(std::string_view peer) const {
  return std::find(admitted_.begin(), admitted_.end(), peer) != admitted_.end();
}

void host_guard::admit(std::string_view peer) {
  if (peer.empty() || admitted(peer)) {
    return;
  }
  if (admitted_.size() >= max_admitted) {
    admitted_.pop_front();
  }
  admitted_.emplace_back(peer);
}

// ------------------------------------------------------------ the words
std::string needed_message() {
  return "This game needs a password. Ask the host for it, type set "
         "net_password followed by the password in the game's console, then "
         "join again.";
}

std::string wrong_message() {
  return "Wrong password. The password in net_password is not the host's. "
         "Ask the host for it, type set net_password followed by the password "
         "in the game's console, then join again.";
}

std::string wait_message(const std::uint32_t seconds) {
  return "Wrong password too many times. The host's game takes no more "
         "answers from your address for " +
         seconds_text(seconds) + ". Check net_password, then join again.";
}

std::string busy_message(const std::uint32_t seconds) {
  return "Too many join attempts from your address. The host's game takes no "
         "more from it for " +
         seconds_text(seconds) +
         ". Join again then. Players who share your internet connection "
         "count too.";
}

std::string paused_message(const std::uint32_t seconds) {
  return "The host's game has had too many wrong passwords in the last "
         "minute, from anyone, and checks none for " +
         seconds_text(seconds) + ". Join again then.";
}

std::string stale_message() {
  return "The password check did not finish (it took too long, or the host's "
         "game restarted). Join again.";
}

std::string address_message() {
  return "The host's game did not recognise the address you joined as its "
         "own, so it did not check the password. Join with the address the "
         "host gave you.";
}

std::string newer_message() {
  return "This game needs a password, and the host's Xerces checks it in a "
         "newer way than this build knows. Update Xerces, then join again.";
}

std::string timeout_message() {
  return "The host's game did not answer the password check. Join again; if "
         "it keeps happening, the host's game may have closed.";
}

std::string host_refused_message() {
  return "This game needs a password, and the host's game refused the join: "
         "this game did not pass its password check. Type set net_password "
         "followed by the password in the game's console, then join again.";
}

std::string refused_connect_message() {
  return "This game needs a password, and the host's game refused the "
         "connect: it did not pass the password check. Type set net_password "
         "followed by the password in the game's console, then join again "
         "from Xerces.";
}
} // namespace xerces_core::join_password
