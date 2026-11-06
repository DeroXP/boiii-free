// The network password checked by the host (docs/DESIGN.md 11.10): HMAC and
// PBKDF2 against published vectors, the messages, the host's challenges,
// checks, address rule and wrong-answer limit, and the words.

#include "harness.hpp"

#include <cctype>
#include <chrono>
#include <string>

#include <xerces_core/join_password.hpp>
#include <xerces_core/sha256.hpp>

using namespace xerces_core;
using namespace xerces_core::join_password;
using clock_type = host_guard::clock;
using namespace std::chrono_literals;

namespace {
const std::string session_a = "ffeeddccbbaa99887766554433221100";
const std::string nonce_a = "00112233445566778899aabbccddeeff";
const std::string nonce_b = "0123456789abcdef0123456789abcdef";
const std::string joiner = "203.0.113.50:27017";
const std::string host_lan = "192.168.1.20:27017";

own_addresses lan_host() {
  own_addresses own;
  own.ips = {"192.168.1.20", "26.10.20.30"};
  own.port = 27017;
  return own;
}

bool contains(const std::string &text, const std::string &part) {
  return text.find(part) != std::string::npos;
}

std::string hex(const sha256::digest &digest) { return sha256::to_hex(digest); }

// A joiner's answer to the challenge the guard gave it.
proof_message answer(const std::string &password, const std::string &nonce,
                     const std::string &host_address = host_lan,
                     const std::string &session = session_a) {
  return proof_message{
      session, nonce, host_address,
      proof(derive_key(password), nonce, host_address, session)};
}
} // namespace

TEST_CASE("join password: HMAC-SHA256 matches RFC 4231") {
  CHECK_EQ(hex(hmac_sha256(std::string(20, '\x0b'), "Hi There")),
           std::string("b0344c61d8db38535ca8afceaf0bf12b"
                       "881dc200c9833da726e9376c2e32cff7"));
  CHECK_EQ(hex(hmac_sha256("Jefe", "what do ya want for nothing?")),
           std::string("5bdcc146bf60754e6a042426089575c7"
                       "5a003f089d2739839dec58b964ec3843"));
  CHECK_EQ(hex(hmac_sha256(std::string(20, '\xaa'), std::string(50, '\xdd'))),
           std::string("773ea91e36800e46854db8ebd09181a7"
                       "2959098b3ef8c122d9635514ced565fe"));
  // Keys longer than a block are hashed first.
  CHECK_EQ(hex(hmac_sha256(
               std::string(131, '\xaa'),
               "Test Using Larger Than Block-Size Key - Hash Key First")),
           std::string("60e431591ee0b67f0d8a26aacbf5b77f"
                       "8e0bc6213728c5140546040f0ee37f54"));
  CHECK_EQ(
      hex(hmac_sha256(std::string(131, '\xaa'),
                      "This is a test using a larger than block-size key and a "
                      "larger than block-size data. The key needs to be hashed "
                      "before being used by the HMAC algorithm.")),
      std::string("9b09ffa71b942fcb27635fbcd5b0e944"
                  "bfdc63644f0713938a7f51535c3a35e2"));
  CHECK_EQ(hex(hmac_sha256("", "")),
           std::string("b613679a0814d9ec772f95d778c35fc5"
                       "ff1697c493715653c6c712144292c5ad"));
}

TEST_CASE("join password: PBKDF2-HMAC-SHA256 matches the known vectors") {
  CHECK_EQ(hex(pbkdf2_sha256("password", "salt", 1)),
           std::string("120fb6cffcf8b32c43e7225256c4f837"
                       "a86548c92ccc35480805987cb70be17b"));
  CHECK_EQ(hex(pbkdf2_sha256("password", "salt", 2)),
           std::string("ae4d0c95af6b46d32d0adff928f06dd0"
                       "2a303f8ef3c251dfd6e2d85a95474c43"));
  CHECK_EQ(hex(pbkdf2_sha256("password", "salt", 4096)),
           std::string("c5e478d59288c841aa530db6845c4c8d"
                       "962893a001ce4e11a4963873aa98134a"));
  CHECK_EQ(hex(pbkdf2_sha256("password", "salt", 0)),
           hex(pbkdf2_sha256("password", "salt", 1)));
}

TEST_CASE("join password: the constant-time compare") {
  const std::uint8_t a[] = {1, 2, 3, 4};
  const std::uint8_t b[] = {1, 2, 3, 4};
  const std::uint8_t c[] = {1, 2, 3, 5};
  const std::uint8_t d[] = {1, 2, 3};
  CHECK(equal_constant_time(a, b));
  CHECK(!equal_constant_time(a, c));
  CHECK(!equal_constant_time(a, d));
  CHECK(equal_constant_time(std::span<const std::uint8_t>{},
                            std::span<const std::uint8_t>{}));
}

TEST_CASE("join password: the key and the proof both sides compute") {
  // The same values Python's hashlib.pbkdf2_hmac and hmac give.
  CHECK_EQ(hex(derive_key("hunter2")),
           std::string("0904e7efa10bd6b354e844bb6e8f6aad"
                       "287c91b9b86cc2deb263a076965f2729"));
  const std::string expected = "2908827158d8a8b4bfd1c56b663fb094"
                               "8b723d56c4ee8011e9e7fafd945b1ae7";
  const key password_key = derive_key("hunter2");
  CHECK_EQ(proof(password_key, nonce_a, host_lan, session_a), expected);
  CHECK(proof_matches(password_key, nonce_a, host_lan, session_a, expected));
  // Upper-case hex is the same answer.
  std::string upper = expected;
  for (char &c : upper) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  CHECK(proof_matches(password_key, nonce_a, host_lan, session_a, upper));
  // Every input is bound.
  CHECK(!proof_matches(derive_key("hunter3"), nonce_a, host_lan, session_a,
                       expected));
  CHECK(!proof_matches(password_key, nonce_b, host_lan, session_a, expected));
  CHECK(!proof_matches(password_key, nonce_a, "192.168.1.21:27017", session_a,
                       expected));
  CHECK(!proof_matches(password_key, nonce_a, host_lan, nonce_b, expected));
  CHECK(!proof_matches(password_key, nonce_a, host_lan, session_a,
                       expected.substr(0, 62)));
  CHECK(!proof_matches(password_key, nonce_a, host_lan, session_a,
                       expected.substr(0, 62) + "zz"));
}

TEST_CASE("join password: what getinfo says, for new and older hosts") {
  CHECK(host_needs("", "") == host_need::none);
  CHECK(host_needs("", "0") == host_need::none);
  CHECK(host_needs("", "1234567") == host_need::legacy_hash);
  CHECK(host_needs("1", std::string(legacy_hash_sentinel)) ==
        host_need::checked);
  CHECK(host_needs("2", std::string(legacy_hash_sentinel)) == host_need::newer);
  // Older clients compare the FNV-1a hash they compute, masked to 63 bits,
  // with net_password_hash: the host's value is above every such hash.
  const unsigned long long sentinel =
      std::stoull(std::string(legacy_hash_sentinel));
  CHECK(sentinel > 0x7FFFFFFFFFFFFFFFULL);
}

TEST_CASE("join password: the messages round trip") {
  CHECK_EQ(format(challenge_message{session_a}), "1 " + session_a);
  const auto challenge = parse_challenge("1 " + session_a);
  REQUIRE(challenge.has_value());
  CHECK_EQ(challenge->session, session_a);

  const nonce_message nonce{session_a, nonce_a};
  const auto nonce_back = parse_nonce(format(nonce));
  REQUIRE(nonce_back.has_value());
  CHECK_EQ(nonce_back->nonce, nonce_a);

  const proof_message sent = answer("hunter2", nonce_a);
  const auto proof_back = parse_proof(format(sent));
  REQUIRE(proof_back.has_value());
  CHECK_EQ(proof_back->host_address, host_lan);
  CHECK_EQ(proof_back->proof, sent.proof);

  const auto ok = parse_result(format(result_message{session_a, result::ok}));
  REQUIRE(ok.has_value());
  CHECK(ok->code == result::ok);
  const auto wait =
      parse_result(format(result_message{session_a, result::wait, 42}));
  REQUIRE(wait.has_value());
  CHECK(wait->code == result::wait);
  CHECK_EQ(wait->wait_seconds, 42u);
  const auto needed = parse_result("1 - needed");
  REQUIRE(needed.has_value());
  CHECK(needed->code == result::needed);
  CHECK_EQ(needed->session, std::string("-"));
  for (const result code :
       {result::ok, result::open, result::wrong, result::needed, result::wait,
        result::stale, result::address}) {
    CHECK(parse_result_name(result_name(code)) == code);
  }
}

TEST_CASE("join password: broken or foreign messages do not parse") {
  CHECK(!parse_challenge(""));
  CHECK(!parse_challenge("1"));
  CHECK(!parse_challenge("2 " + session_a));       // another version
  CHECK(!parse_challenge("1 " + session_a + " ")); // trailing space
  CHECK(!parse_challenge("1  " + session_a));      // empty field
  CHECK(!parse_challenge("1 " + session_a + " x"));
  CHECK(!parse_challenge("1 " + session_a.substr(1)));           // too short
  CHECK(!parse_challenge("1 FFEEDDCCBBAA99887766554433221100")); // upper case
  CHECK(!parse_challenge("1 " + std::string(300, 'a')));
  CHECK(!parse_nonce("1 " + session_a));
  CHECK(!parse_nonce("1 " + session_a + " " + nonce_a + "x"));
  const proof_message good = answer("hunter2", nonce_a);
  CHECK(parse_proof(format(good)).has_value());
  proof_message bad = good;
  bad.host_address = "[::1]:27017";
  CHECK(!parse_proof(format(bad)));
  bad.host_address = "192.168.1.20";
  CHECK(!parse_proof(format(bad)));
  bad.host_address = "192.168.1.256:27017";
  CHECK(!parse_proof(format(bad)));
  bad.host_address = "192.168.1.20:0";
  CHECK(!parse_proof(format(bad)));
  bad = good;
  bad.proof = good.proof.substr(2);
  CHECK(!parse_proof(format(bad)));
  CHECK(!parse_result("1 " + session_a + " wait"));    // no seconds
  CHECK(!parse_result("1 " + session_a + " ok 3"));    // seconds for ok
  CHECK(!parse_result("1 " + session_a + " maybe"));   // unknown code
  CHECK(!parse_result("1 nobody needed"));             // bad session
  CHECK(!parse_result("1 " + session_a + " wait -1")); // not a count
  CHECK(!parse_result("1 " + session_a + " wait 1x"));
}

TEST_CASE("join password: tokens are 16 random bytes in hex") {
  const std::string a = random_token();
  const std::string b = random_token();
  CHECK(valid_token(a));
  CHECK(valid_token(b));
  CHECK(a != b);
}

TEST_CASE("join password: endpoints parse strictly") {
  const auto endpoint = parse_endpoint("10.0.0.5:27018");
  REQUIRE(endpoint.has_value());
  CHECK_EQ(endpoint->ip(), std::string("10.0.0.5"));
  CHECK_EQ(endpoint->port, static_cast<std::uint16_t>(27018));
  CHECK(!parse_endpoint("10.0.0:27018"));
  CHECK(!parse_endpoint("10.0.0.5.6:27018"));
  CHECK(!parse_endpoint("10.0.0.5:"));
  CHECK(!parse_endpoint("10.0.0.5:65536"));
  CHECK(!parse_endpoint("10.0.0.5:27018:1"));
  CHECK(!parse_endpoint("a.b.c.d:27018"));
  CHECK(!parse_endpoint(":27018"));
  CHECK_EQ(endpoint_ip("203.0.113.50:40000"), std::string("203.0.113.50"));
  CHECK_EQ(endpoint_ip("nope"), std::string());
}

TEST_CASE("join password: the host knows its addresses as far as it can") {
  own_addresses own = lan_host();
  // This PC, on its port: the LAN adapter, a VPN adapter, loopback.
  CHECK(is_own_address("192.168.1.20:27017", own));
  CHECK(is_own_address("26.10.20.30:27017", own));
  CHECK(is_own_address("127.0.0.1:27017", own));
  CHECK(!is_own_address("192.168.1.20:27018", own)); // another port
  CHECK(!is_own_address("192.168.1.21:27017", own)); // another PC's LAN
  CHECK(!is_own_address("10.1.2.3:27017", own));
  CHECK(!is_own_address("0.0.0.0:27017", own));
  CHECK(!is_own_address("239.1.2.3:27017", own));
  CHECK(!is_own_address("garbage", own));
  // No outside address known: any public or shared one passes, any port.
  CHECK(is_own_address("203.0.113.7:27017", own));
  CHECK(is_own_address("203.0.113.7:30000", own));
  CHECK(is_own_address("100.64.1.2:27017", own));
  // The router reported its public outside address: only that one.
  own.mapped = "203.0.113.7:27017";
  CHECK(is_own_address("203.0.113.7:27017", own));
  CHECK(!is_own_address("203.0.113.8:27017", own));
  CHECK(!is_own_address("203.0.113.7:27018", own));
  CHECK(is_own_address("192.168.1.20:27017", own)); // LAN still works
  // A private "outside" address (a second router) says nothing.
  own.mapped = "192.168.0.10:27017";
  CHECK(is_own_address("203.0.113.8:27017", own));
}

TEST_CASE("join password: the right answer admits the address, once") {
  host_guard guard;
  const auto now = clock_type::now();
  const auto given = guard.challenge(joiner, session_a, nonce_a, now);
  REQUIRE(given.code == result::ok);
  CHECK_EQ(given.nonce, nonce_a);
  CHECK(!guard.admitted(joiner));

  const key password_key = derive_key("hunter2");
  const result_message reply = guard.check(joiner, answer("hunter2", nonce_a),
                                           password_key, lan_host(), now + 1s);
  CHECK(reply.code == result::ok);
  CHECK_EQ(reply.session, session_a);
  CHECK(guard.admitted(joiner));
  CHECK(!guard.admitted("203.0.113.50:27018")); // the port counts
  CHECK(!guard.admitted("203.0.113.51:27017"));
  // The challenge is used up: the same answer again is stale.
  CHECK(guard
            .check(joiner, answer("hunter2", nonce_a), password_key, lan_host(),
                   now + 2s)
            .code == result::stale);
  CHECK_EQ(guard.challenge_count(), static_cast<std::size_t>(0));
}

TEST_CASE("join password: wrong, foreign and late answers are refused") {
  host_guard guard;
  const auto now = clock_type::now();
  const key password_key = derive_key("hunter2");
  const own_addresses own = lan_host();

  guard.challenge(joiner, session_a, nonce_a, now);
  CHECK(guard.check(joiner, answer("hunter3", nonce_a), password_key, own, now)
            .code == result::wrong);
  CHECK(!guard.admitted(joiner));

  // An answer from another address than the one challenged.
  guard.challenge(joiner, session_a, nonce_a, now);
  CHECK(guard
            .check("203.0.113.51:27017", answer("hunter2", nonce_a),
                   password_key, own, now)
            .code == result::stale);
  // Another nonce or session than the one given (and the challenge is gone).
  guard.challenge(joiner, session_a, nonce_a, now);
  CHECK(guard.check(joiner, answer("hunter2", nonce_b), password_key, own, now)
            .code == result::stale);
  guard.challenge(joiner, session_a, nonce_a, now);
  CHECK(guard.check(joiner, answer("hunter2", nonce_a), password_key, own, now)
            .code == result::ok);

  // Too late.
  host_guard late;
  late.challenge(joiner, session_a, nonce_a, now);
  CHECK(late.check(joiner, answer("hunter2", nonce_a), password_key, own,
                   now + host_guard::challenge_lifetime)
            .code == result::stale);

  // A resent challenge (same session) gets the same nonce, so the answer
  // to either reply matches.
  host_guard resent;
  resent.challenge(joiner, session_a, nonce_a, now);
  CHECK_EQ(resent.challenge(joiner, session_a, nonce_b, now + 1s).nonce,
           nonce_a);
  CHECK(resent
            .check(joiner, answer("hunter2", nonce_a), password_key, own,
                   now + 2s)
            .code == result::ok);

  // A challenge for another session replaces the older one.
  const std::string session_b = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  host_guard twice;
  twice.challenge(joiner, session_a, nonce_a, now);
  CHECK_EQ(twice.challenge(joiner, session_b, nonce_b, now + 1s).nonce,
           nonce_b);
  CHECK_EQ(twice.challenge_count(), static_cast<std::size_t>(1));
  CHECK(twice
            .check(joiner, answer("hunter2", nonce_a), password_key, own,
                   now + 2s)
            .code == result::stale);

  // The host address the joiner proved is not this host's.
  host_guard relayed;
  relayed.challenge(joiner, session_a, nonce_a, now);
  CHECK(relayed
            .check(joiner, answer("hunter2", nonce_a, "192.168.1.99:27017"),
                   password_key, own, now)
            .code == result::address);
  CHECK(!relayed.admitted(joiner));
}

TEST_CASE("join password: a few wrong answers a minute per IP address") {
  host_guard guard;
  const auto start = clock_type::now();
  const key password_key = derive_key("hunter2");
  const own_addresses own = lan_host();
  auto now = start;
  for (std::size_t i = 0; i < host_guard::max_wrong_per_minute; ++i) {
    // Each guess from another port of the same IP address.
    const std::string peer = "203.0.113.50:" + std::to_string(40000 + i);
    REQUIRE(guard.challenge(peer, session_a, nonce_a, now).code == result::ok);
    CHECK(guard
              .check(peer, answer("guess" + std::to_string(i), nonce_a),
                     password_key, own, now)
              .code == result::wrong);
    now += 2s;
  }
  // The next challenge from that IP address waits until the first wrong
  // answer (at 0 s) is a minute old: it is 10 s now, so 50 s.
  const auto refused = guard.challenge(joiner, session_a, nonce_a, now);
  CHECK(refused.code == result::wait);
  CHECK_EQ(refused.wait_seconds, 50u);
  CHECK_EQ(guard.wait_seconds(joiner, now), 50u);
  // Another IP address is not affected.
  CHECK(guard.challenge("198.51.100.9:27017", session_a, nonce_a, now).code ==
        result::ok);
  // A minute after the first wrong answer, one more answer is looked at.
  now = start + 60s;
  REQUIRE(guard.challenge(joiner, session_a, nonce_a, now).code == result::ok);
  CHECK(guard.check(joiner, answer("hunter2", nonce_a), password_key, own, now)
            .code == result::ok);
  CHECK(guard.admitted(joiner));
}

TEST_CASE("join password: an answer after the limit waits, right or not") {
  host_guard guard;
  const auto now = clock_type::now();
  const key password_key = derive_key("hunter2");
  const own_addresses own = lan_host();
  // Challenge first, then use up the wrong answers from other ports.
  REQUIRE(guard.challenge(joiner, session_a, nonce_a, now).code == result::ok);
  for (std::size_t i = 0; i < host_guard::max_wrong_per_minute; ++i) {
    const std::string peer = "203.0.113.50:" + std::to_string(40000 + i);
    guard.challenge(peer, session_a, nonce_b, now);
    guard.check(peer, answer("guess", nonce_b), password_key, own, now);
  }
  const result_message reply = guard.check(joiner, answer("hunter2", nonce_a),
                                           password_key, own, now + 1s);
  CHECK(reply.code == result::wait);
  CHECK_EQ(reply.wait_seconds, 59u);
  CHECK(!guard.admitted(joiner));
}

TEST_CASE("join password: challenges per IP address are limited too") {
  host_guard guard;
  const auto now = clock_type::now();
  for (std::size_t i = 0; i < host_guard::max_challenges_per_minute; ++i) {
    CHECK(guard.challenge(joiner, session_a, nonce_a, now).code == result::ok);
  }
  CHECK(guard.challenge(joiner, session_a, nonce_a, now).code == result::wait);
  CHECK(guard.challenge(joiner, session_a, nonce_a, now + 60s).code ==
        result::ok);
  // Broken input gets no challenge.
  CHECK(guard.challenge("not an address", session_a, nonce_a, now).code ==
        result::stale);
  CHECK(guard.challenge(joiner, "short", nonce_a, now).code == result::stale);
}

TEST_CASE("join password: the tables stay bounded") {
  host_guard guard;
  const auto now = clock_type::now();
  for (int i = 0; i < 300; ++i) {
    const std::string peer = "198.51." + std::to_string(i / 200) + "." +
                             std::to_string(i % 200 + 1) + ":27017";
    guard.challenge(peer, session_a, nonce_a, now);
    guard.admit(peer);
  }
  CHECK_EQ(guard.challenge_count(), host_guard::max_challenges);
  CHECK_EQ(guard.admitted_count(), host_guard::max_admitted);
  // The oldest admitted address went first.
  CHECK(!guard.admitted("198.51.0.1:27017"));
  CHECK(guard.admitted("198.51.1.100:27017"));
  // Admitting twice keeps one entry.
  guard.admit("198.51.1.100:27017");
  CHECK_EQ(guard.admitted_count(), host_guard::max_admitted);
}

TEST_CASE("join password: the words the player looks for") {
  CHECK(needed_message().starts_with("This game needs a password."));
  CHECK(wrong_message().starts_with("Wrong password."));
  CHECK(contains(needed_message(), "set net_password"));
  CHECK(contains(wrong_message(), "set net_password"));
  CHECK(contains(wait_message(30), "30 seconds"));
  CHECK(contains(wait_message(1), "1 second."));
  CHECK(contains(wait_message(0), "1 second."));
  CHECK(refused_connect_message().starts_with("This game needs a password"));
  CHECK(contains(refused_connect_message(), "refused"));
  CHECK(host_refused_message().starts_with("This game needs a password"));
  CHECK(contains(host_refused_message(), "refused the join"));
  CHECK(contains(newer_message(), "Update Xerces"));
  CHECK(!stale_message().empty());
  CHECK(!address_message().empty());
  CHECK(!timeout_message().empty());
}
