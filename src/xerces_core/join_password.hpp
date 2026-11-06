#pragma once

// The network password, checked by the host (docs/DESIGN.md 11.4, 11.10).
//
// A host with net_password set says so in getinfo. Before a join, the joiner
// asks it for a challenge and gets a fresh random nonce; it answers with
// HMAC-SHA256, keyed by a hash of the password, over the nonce, the host's
// address as the joiner dialled it and the joiner's session id. The password
// never travels. The host checks the answer in constant time, limits wrong
// answers per address, and then lets that address into its lobby and match.
// Every way in that Xerces handles asks the same question (host_guard::
// admitted): the lobby join (nat.cpp) and the connect into a running match
// (network.cpp's guard on "connect").
//
// Headless and pure: the game role (src/client/xerces/join_password.cpp)
// sends the messages, keeps the clock and draws the random bytes.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sha256.hpp"

namespace xerces_core::join_password {
// ------------------------------------------------------ the key and proof
// The version of this check, in getinfo and in every message.
inline constexpr std::string_view protocol = "1";

// key = PBKDF2-HMAC-SHA256(password, key_salt, key_rounds). Both sides
// derive it from net_password; someone who records an exchange can still
// test guesses offline, the rounds only make each guess slower (the PAKE in
// DESIGN.md 11.3 removes that). An empty password has no key.
inline constexpr std::string_view key_salt = "xerces net_password v1";
inline constexpr std::uint32_t key_rounds = 10000;
using key = sha256::digest;
key derive_key(std::string_view password);

// The answer to a nonce: 64 lower-case hex digits of
// HMAC-SHA256(key, "xerces-join-v1\n" nonce "\n" host_address "\n" session).
std::string proof(const key &password_key, std::string_view nonce,
                  std::string_view host_address, std::string_view session);
// Compares in constant time; anything that is not 64 hex digits is false.
bool proof_matches(const key &password_key, std::string_view nonce,
                   std::string_view host_address, std::string_view session,
                   std::string_view proof_hex);

// ------------------------------------------------------- what getinfo says
// getinfo's key while the host checks a password: protocol ("1").
inline constexpr std::string_view info_key = "xercesPassword";
// What such a host puts in net_password_hash, where older Xerces builds and
// BOIII look: their FNV-1a hash keeps 63 bits, so no password gives this
// value, and their own check stops them with their own message ("Server
// requires a network password." or "Network password mismatch."). The host
// no longer publishes a hash of its password.
inline constexpr std::string_view legacy_hash_sentinel = "18446744073709551615";

enum class host_need {
  none,        // no password
  legacy_hash, // an older host: compare net_password_hash as before
  checked,     // this check
  newer,       // a later version of this check than this build knows
};
host_need host_needs(std::string_view xerces_password,
                     std::string_view net_password_hash);

// -------------------------------------------------------------- messages
// Connectionless commands, each starting with protocol. The session and the
// nonce are token_bytes random bytes as lower-case hex.
inline constexpr std::string_view challenge_command = "xercesJoinChallenge";
inline constexpr std::string_view nonce_command = "xercesJoinNonce";
inline constexpr std::string_view proof_command = "xercesJoinProof";
inline constexpr std::string_view result_command = "xercesJoinResult";
inline constexpr std::size_t token_bytes = 16;
// Longer payloads are not looked at.
inline constexpr std::size_t max_payload = 256;

enum class result {
  ok,      // the password matched: this address may join
  open,    // the host has no password
  wrong,   // the answer did not match
  needed,  // the host has a password and this address has not passed
  wait,    // too many wrong answers from this address lately
  stale,   // no such challenge (expired, used, or from another address)
  address, // the host address in the answer is not this host's
};
std::string_view result_name(result code);
std::optional<result> parse_result_name(std::string_view text);

// "1 <session>"
struct challenge_message {
  std::string session;
};
// "1 <session> <nonce>"
struct nonce_message {
  std::string session;
  std::string nonce;
};
// "1 <session> <nonce> <host address> <proof>"
struct proof_message {
  std::string session;
  std::string nonce;
  std::string host_address; // as the joiner dialled it, "a.b.c.d:port"
  std::string proof;
};
// "1 <session> <code> [<seconds>]"; the session is "-" when the host
// refuses a lobby join or connect that brought none.
struct result_message {
  std::string session;
  result code = result::needed;
  std::uint32_t wait_seconds = 0; // for wait
};

std::string format(const challenge_message &message);
std::string format(const nonce_message &message);
std::string format(const proof_message &message);
std::string format(const result_message &message);
// nullopt for anything else: another version, a field too many or too few,
// a token that is not token_bytes of hex, an address that is not IPv4.
std::optional<challenge_message> parse_challenge(std::string_view payload);
std::optional<nonce_message> parse_nonce(std::string_view payload);
std::optional<proof_message> parse_proof(std::string_view payload);
std::optional<result_message> parse_result(std::string_view payload);

bool valid_token(std::string_view token);
// token_bytes from the system's random source (std::random_device, which is
// rand_s, RtlGenRandom, on Windows), as lower-case hex.
std::string random_token();

// "a.b.c.d:port" with each part in range and a port above 0.
struct ipv4_endpoint {
  std::uint8_t octets[4]{};
  std::uint16_t port = 0;
  std::string ip() const;
};
std::optional<ipv4_endpoint> parse_endpoint(std::string_view text);
// The IP part of "a.b.c.d:port" ("" when it does not parse).
std::string endpoint_ip(std::string_view text);

// ------------------------------------------------- the host's addresses
// What the host knows of its own addresses: its IPv4 addresses on this PC
// (every network adapter, VPNs included), the UDP port it is bound to, and
// the router's outside address while UPnP forwards the port ("ip:port", ""
// when it does not or did not say).
struct own_addresses {
  std::vector<std::string> ips;
  std::uint16_t port = 0;
  std::string mapped;
};
// Whether the address the joiner says it dialled can be this host. An
// address of this PC (or 127.x) needs the host's port; another LAN address
// is someone else's. A shared or public address must be the router's
// outside address while the router reported a public one; when it did not,
// the host does not know its outside address, and any shared or public
// address passes (a port forwarded by hand may also change the port).
bool is_own_address(std::string_view claimed, const own_addresses &own);

// --------------------------------------------------------- the host's state
// What one host remembers: the challenges it gave, the addresses that may
// join, and the wrong answers per IP address. Not thread-safe; the game
// role holds a lock.
class host_guard {
public:
  using clock = std::chrono::steady_clock;

  // A challenge holds for this long, and once.
  static constexpr std::chrono::seconds challenge_lifetime{20};
  static constexpr std::size_t max_challenges = 64;
  // Wrong answers per IP address (every port counts) in a sliding minute;
  // the next answer waits until the oldest is a minute old.
  static constexpr std::size_t max_wrong_per_minute = 5;
  // Challenges per IP address in a sliding minute.
  static constexpr std::size_t max_challenges_per_minute = 12;
  static constexpr std::size_t max_tracked_ips = 256;
  // Addresses that may join, oldest dropped first.
  static constexpr std::size_t max_admitted = 128;

  struct challenge_answer {
    result code = result::stale; // ok: send nonce; wait: wait_seconds
    std::string nonce;
    std::uint32_t wait_seconds = 0;
  };
  // A joiner at peer ("a.b.c.d:port") asks for a challenge; nonce is a fresh
  // random_token() from the caller. The same session again gets the nonce
  // it got before (a resend); another session from the same peer replaces
  // the older challenge.
  challenge_answer challenge(std::string_view peer, std::string_view session,
                             std::string nonce, clock::time_point now);

  // A joiner answers. The challenge is used up whatever the outcome. ok
  // admits peer.
  result_message check(std::string_view peer, const proof_message &answer,
                       const key &password_key, const own_addresses &own,
                       clock::time_point now);

  // Whether peer may join: it passed check, or it joined while the host
  // had no password (admit). A password set or changed later does not
  // lock out who is already in; nothing does until the game quits.
  bool admitted(std::string_view peer) const;
  void admit(std::string_view peer);

  // Seconds until another wrong answer from peer's IP address would be
  // looked at (0 when it would be now).
  std::uint32_t wait_seconds(std::string_view peer,
                             clock::time_point now) const;

  std::size_t challenge_count() const { return challenges_.size(); }
  std::size_t admitted_count() const { return admitted_.size(); }

private:
  struct issued {
    std::string peer;
    std::string session;
    std::string nonce;
    clock::time_point expires;
  };
  // Events per IP address in the last minute, oldest first.
  struct window {
    std::string ip;
    std::deque<clock::time_point> events;
  };
  static window *find_window(std::vector<window> &windows, std::string_view ip);
  static const window *find_window(const std::vector<window> &windows,
                                   std::string_view ip);
  static window &touch_window(std::vector<window> &windows, std::string_view ip,
                              clock::time_point now);
  static void forget_old(window &entry, clock::time_point now);
  static std::uint32_t window_wait(const window *entry, std::size_t limit,
                                   clock::time_point now);

  std::vector<issued> challenges_;
  std::vector<window> wrong_;
  std::vector<window> asked_;
  std::deque<std::string> admitted_;
};

// ------------------------------------------------------------ the words
// What the joiner's game shows (a popup); each starts with the words the
// player looks for.
std::string needed_message();
std::string wrong_message();
std::string wait_message(std::uint32_t seconds);
std::string stale_message();
std::string address_message();
std::string newer_message();
std::string timeout_message();
// The host refused a lobby join from an address that did not pass its
// check (a modified client, or the host's game restarted).
std::string host_refused_message();
// What the host sends a connect it refuses, as the engine's own "error"
// message, which every client shows (also BOIII and older Xerces builds).
std::string refused_connect_message();
} // namespace xerces_core::join_password
