#include <std_include.hpp>

// The network password checked by the host (join_password.hpp, docs/DESIGN.md
// 11.10). The password and its key stay in this process; the game log gets
// the outcome of each check, never an address or the password (DESIGN.md
// 15.1).

#include "join_password.hpp"

#include <component/nat.hpp>
#include <component/network.hpp>
#include <component/network_password.hpp>
#include <component/scheduler.hpp>
#include <component/upnp.hpp>

#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include <xerces_core/join_password.hpp>
#include <xerces_core/json.hpp>

#include "runtime.hpp"

namespace xerces_join_password {
namespace {
namespace core = xerces_core::join_password;
using core::result;
using steady = std::chrono::steady_clock;

// ------------------------------------------------------------ the key
// derive_key takes a few milliseconds; both sides keep the last one.
std::mutex key_mutex;
std::string key_password;
std::string key_salt;
core::key key_value{};

core::key password_key(const std::string &password,
                       const std::string &host_salt) {
  std::lock_guard lock(key_mutex);
  if (password != key_password || host_salt != key_salt) {
    key_value = core::derive_key(password, host_salt);
    key_password = password;
    key_salt = host_salt;
  }
  return key_value;
}

// The payload without a line end or terminator a sender may add.
std::string payload_text(const network::data_view &data) {
  std::string text(reinterpret_cast<const char *>(data.data()), data.size());
  while (!text.empty() &&
         (text.back() == 0 || text.back() == 10 || text.back() == 13)) {
    text.pop_back();
  }
  return text;
}

// A joiner's address: one the internet or the LAN can reach, or this PC
// (two games on one PC).
bool joiner_address(const game::net::netadr_t &sender) {
  return network::is_connectable_address(sender) ||
         (network::is_ip_address(sender) && sender.ipv4.a == 127);
}

void send(const game::net::netadr_t &to, const std::string_view command,
          const std::string &payload) {
  network::send(to, std::string(command), payload);
}

void log_check(const std::string_view event, const std::string_view message,
               const std::string_view outcome) {
  xerces_runtime::log_info(
      event, message, "{\"result\":" + xerces_core::json::quote(outcome) + "}");
}

// ----------------------------------------------------------- the host
std::mutex host_mutex;
core::host_guard guard;

// Invite tickets and invite_only (join_password.hpp). Each ticket's key is
// derived once, with this host's salt, when it is added.
struct ticket_entry {
  core::key key{};
  steady::time_point expires{};
};
constexpr std::size_t max_tickets = 64;
std::atomic_bool invite_only_on{false};
std::vector<ticket_entry> tickets; // host_mutex

// This host's salt, drawn once per start of the game (join_password.hpp,
// derive_key).
const std::string &host_salt() {
  static const std::string salt = core::random_token();
  return salt;
}

// This PC's IPv4 addresses, read again at most every 30 s.
std::vector<std::string> own_ips;
steady::time_point own_ips_time{};
bool own_ips_read = false;

void add_ip(std::vector<std::string> &ips, const sockaddr *address) {
  if (!address || address->sa_family != AF_INET) {
    return;
  }
  char text[INET_ADDRSTRLEN]{};
  if (inet_ntop(AF_INET,
                &reinterpret_cast<const sockaddr_in *>(address)->sin_addr, text,
                sizeof(text)) &&
      std::ranges::find(ips, std::string(text)) == ips.end()) {
    ips.emplace_back(text);
  }
}

// Every adapter's IPv4 address, VPN adapters included: the socket layer's
// own list, and what this PC's name resolves to.
std::vector<std::string> read_own_ips() {
  std::vector<std::string> ips;
  if (const SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      probe != INVALID_SOCKET) {
    std::vector<char> buffer(16 * 1024);
    DWORD size = 0;
    if (WSAIoctl(probe, SIO_ADDRESS_LIST_QUERY, nullptr, 0, buffer.data(),
                 static_cast<DWORD>(buffer.size()), &size, nullptr,
                 nullptr) == 0) {
      const auto *list =
          reinterpret_cast<const SOCKET_ADDRESS_LIST *>(buffer.data());
      for (INT i = 0; i < list->iAddressCount; ++i) {
        add_ip(ips, list->Address[i].lpSockaddr);
      }
    }
    closesocket(probe);
  }
  char name[256]{};
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo *found = nullptr;
  if (gethostname(name, sizeof(name) - 1) == 0 &&
      getaddrinfo(name, nullptr, &hints, &found) == 0) {
    for (const addrinfo *entry = found; entry; entry = entry->ai_next) {
      add_ip(ips, entry->ai_addr);
    }
    freeaddrinfo(found);
  }
  return ips;
}

uint16_t bound_port() {
  const uint16_t port = network::get_bound_port();
  return port ? port : game::port();
}

// Call with host_mutex held.
core::own_addresses own_addresses() {
  const auto now = steady::now();
  if (!own_ips_read || now - own_ips_time >= 30s) {
    own_ips = read_own_ips();
    own_ips_time = now;
    own_ips_read = true;
  }
  core::own_addresses own;
  own.ips = own_ips;
  own.port = bound_port();
  own.mapped = upnp::external_endpoint();
  return own;
}

// Whether this game can be joined now: a match runs here, or the lobby is
// open to direct connect. Nothing else answers a challenge.
bool joinable() {
  return game::server_running() || !nat::joinable_lobby().empty();
}

void receive_challenge(const game::net::netadr_t &sender,
                       const network::data_view &data, game::LocalClientNum_t) {
  const std::optional<core::challenge_message> message =
      core::parse_challenge(payload_text(data));
  if (!message || !joiner_address(sender) || !joinable()) {
    return;
  }
  const std::string peer = network::address_to_string(sender);
  if (!locked()) {
    send(sender, core::result_command,
         core::format(core::result_message{message->session, result::open}));
    return;
  }
  core::host_guard::challenge_answer answer;
  {
    std::lock_guard lock(host_mutex);
    answer = guard.challenge(peer, message->session, core::random_token(),
                             steady::now());
  }
  if (answer.code == result::ok) {
    send(sender, core::nonce_command,
         core::format(
             core::nonce_message{message->session, answer.nonce, host_salt()}));
  } else if (core::carries_seconds(answer.code)) {
    send(sender, core::result_command,
         core::format(core::result_message{message->session, answer.code,
                                           answer.wait_seconds}));
  }
}

void receive_proof(const game::net::netadr_t &sender,
                   const network::data_view &data, game::LocalClientNum_t) {
  const std::optional<core::proof_message> message =
      core::parse_proof(payload_text(data));
  if (!message || !joiner_address(sender) || !joinable()) {
    return;
  }
  const std::string password = network_password::get_password();
  if (password.empty() && !invite_only()) {
    send(sender, core::result_command,
         core::format(core::result_message{message->session, result::open}));
    return;
  }
  std::vector<core::key> keys;
  if (!password.empty()) {
    keys.push_back(password_key(password, host_salt()));
  }
  core::result_message reply;
  {
    std::lock_guard lock(host_mutex);
    const auto now = steady::now();
    std::erase_if(tickets, [now](const ticket_entry &entry) {
      return entry.expires <= now;
    });
    for (const ticket_entry &entry : tickets) {
      keys.push_back(entry.key);
    }
    reply = guard.check(network::address_to_string(sender), *message,
                        std::span<const core::key>(keys), own_addresses(), now);
  }
  send(sender, core::result_command, core::format(reply));
  switch (reply.code) {
  case result::ok:
    printf("[Join] A joiner passed the check\n");
    log_check("join.password.host", "a joiner passed the check", "ok");
    break;
  case result::wrong:
    printf("[Join] A joiner gave a wrong password\n");
    log_check("join.password.host", "a joiner gave a wrong password", "wrong");
    break;
  case result::address:
    printf("[Join] A joiner's answer names another host's address\n");
    log_check("join.password.host",
              "a joiner's answer names an address that is not this host's",
              "address");
    break;
  default:
    break;
  }
}

// A refused connect hears why, at most every 2 s per address: the engine's
// "error" message, which BOIII and older Xerces builds show too.
std::mutex refused_mutex;
std::vector<std::pair<std::string, steady::time_point>> refused_recently;
steady::time_point refused_logged{};

void refuse_connect(const game::net::netadr_t &sender) {
  const std::string peer = network::address_to_string(sender);
  const auto now = steady::now();
  bool log = false;
  {
    std::lock_guard lock(refused_mutex);
    std::erase_if(refused_recently, [now](const auto &entry) {
      return now - entry.second >= 2s;
    });
    if (std::ranges::any_of(refused_recently, [&](const auto &entry) {
          return entry.first == peer;
        })) {
      return;
    }
    if (refused_recently.size() >= 64) {
      return;
    }
    refused_recently.emplace_back(peer, now);
    if (now - refused_logged >= 10s) {
      refused_logged = now;
      log = true;
    }
  }
  network::send(sender, "error", core::refused_connect_message());
  if (log) {
    printf("[Join] Refused a connect that did not pass the password check\n");
    log_check("join.password.refused",
              "refused a connect that did not pass the password check",
              "needed");
  }
}

// ----------------------------------------------------------- the joiner
struct attempt {
  bool active = false;
  game::net::netadr_t host{};
  std::string host_text;
  std::string session;
  bool answered = false; // the proof went out
  int challenges_sent = 0;
  int restarts = 0;
  steady::time_point next_send{};
  steady::time_point deadline{};
  done_callback done;
};

constexpr int max_challenge_sends = 3;
constexpr std::chrono::milliseconds resend_gap = 1500ms;
constexpr std::chrono::seconds attempt_time = 6s;

std::mutex joiner_mutex;
attempt joining;

// The invite's ticket for one host (set_join_ticket). joiner_mutex.
struct join_ticket {
  game::net::netadr_t host{};
  std::string ticket;
  steady::time_point expires{};
};
join_ticket ticket_for_join;

// Whether ticket_for_join is for host. Call with joiner_mutex held.
bool ticket_matches(const game::net::netadr_t &host) {
  return !ticket_for_join.ticket.empty() &&
         ticket_for_join.expires > steady::now() &&
         network::are_addresses_equal(host, ticket_for_join.host);
}

// What this joiner answers host's check with: the invite's ticket for it,
// else net_password. Call with joiner_mutex held.
std::string secret_for(const game::net::netadr_t &host) {
  return ticket_matches(host) ? ticket_for_join.ticket
                              : network_password::get_password();
}

// Call with joiner_mutex held.
void send_challenge(const steady::time_point now) {
  ++joining.challenges_sent;
  joining.next_send = now + resend_gap;
  send(joining.host, core::challenge_command,
       core::format(core::challenge_message{joining.session}));
}

// Call with joiner_mutex held.
void start_exchange(const steady::time_point now) {
  joining.session = core::random_token();
  joining.answered = false;
  joining.challenges_sent = 0;
  joining.deadline = now + attempt_time;
  send_challenge(now);
}

// Call with joiner_mutex held.
void finish(const outcome result, const std::uint32_t wait_seconds) {
  done_callback done = std::move(joining.done);
  joining = {};
  static constexpr const char *names[] = {
      "passed", "open",  "needed",  "wrong",   "wait",  "busy",
      "paused", "stale", "address", "timeout", "newer", "refused"};
  static_assert(std::size(names) ==
                static_cast<std::size_t>(outcome::refused) + 1);
  log_check("join.password.joiner", "the host's password check ended",
            names[static_cast<int>(result)]);
  if (done) {
    scheduler::once(
        [done, result, wait_seconds] { done(result, wait_seconds); },
        scheduler::main);
  }
}

bool from_host(const game::net::netadr_t &sender) {
  return joining.active && network::are_addresses_equal(sender, joining.host);
}

void receive_nonce(const game::net::netadr_t &sender,
                   const network::data_view &data, game::LocalClientNum_t) {
  const std::optional<core::nonce_message> message =
      core::parse_nonce(payload_text(data));
  if (!message) {
    return;
  }
  std::lock_guard lock(joiner_mutex);
  if (!from_host(sender) || message->session != joining.session ||
      joining.answered) {
    return;
  }
  const std::string password = secret_for(joining.host);
  if (password.empty()) {
    finish(outcome::needed, 0);
    return;
  }
  joining.answered = true;
  joining.deadline = steady::now() + attempt_time;
  const core::proof_message answer{
      joining.session, message->nonce, joining.host_text,
      core::proof(password_key(password, message->host_salt), message->nonce,
                  joining.host_text, joining.session)};
  send(joining.host, core::proof_command, core::format(answer));
}

void receive_result(const game::net::netadr_t &sender,
                    const network::data_view &data, game::LocalClientNum_t) {
  const std::optional<core::result_message> message =
      core::parse_result(payload_text(data));
  if (!message) {
    return;
  }
  std::lock_guard lock(joiner_mutex);
  if (!from_host(sender) || message->session != joining.session) {
    return;
  }
  switch (message->code) {
  case result::ok:
    finish(outcome::passed, 0);
    return;
  case result::open:
    finish(outcome::open, 0);
    return;
  case result::wrong:
    finish(outcome::wrong, 0);
    return;
  case result::needed:
    finish(outcome::needed, 0);
    return;
  case result::wait:
    finish(outcome::wait, message->wait_seconds);
    return;
  case result::busy:
    finish(outcome::busy, message->wait_seconds);
    return;
  case result::paused:
    finish(outcome::paused, message->wait_seconds);
    return;
  case result::address:
    finish(outcome::address, 0);
    return;
  case result::stale:
    // The challenge was gone (it crossed a resend, or the host restarted):
    // once more from the start.
    if (joining.restarts++ < 1) {
      start_exchange(steady::now());
    } else {
      finish(outcome::stale, 0);
    }
    return;
  }
}

void update_joiner() {
  std::lock_guard lock(joiner_mutex);
  if (!joining.active) {
    return;
  }
  const auto now = steady::now();
  if (now >= joining.deadline) {
    // No answer to the proof: once more from the start (the challenge is
    // used up); no answer to any challenge: the host did not answer.
    if (joining.answered && joining.restarts++ < 1) {
      start_exchange(now);
    } else {
      finish(outcome::timeout, 0);
    }
    return;
  }
  if (!joining.answered && now >= joining.next_send &&
      joining.challenges_sent < max_challenge_sends) {
    send_challenge(now);
  }
}
} // namespace

void set_invite_only(const bool on) {
  if (invite_only_on.exchange(on) != on) {
    xerces_runtime::log_info("join.invite_only",
                             on ? "only invited players can join"
                                : "anyone with the address can join");
  }
}

bool invite_only() { return invite_only_on.load(); }

bool locked() { return network_password::is_password_set() || invite_only(); }

bool add_ticket(const std::string &ticket,
                const std::chrono::seconds lifetime) {
  if (!core::valid_token(ticket) || lifetime <= 0s || lifetime > 24h) {
    return false;
  }
  const core::key key = core::derive_key(ticket, host_salt());
  std::lock_guard lock(host_mutex);
  const auto now = steady::now();
  std::erase_if(tickets, [now](const ticket_entry &entry) {
    return entry.expires <= now;
  });
  if (tickets.size() >= max_tickets) {
    tickets.erase(tickets.begin());
  }
  tickets.push_back(ticket_entry{key, now + lifetime});
  return true;
}

void set_join_ticket(const std::string &host, const std::string &ticket) {
  // A numeric IPv4 address and port only: a name would be looked up here.
  const std::size_t colon = host.rfind(':');
  in_addr numeric{};
  if (!core::valid_token(ticket) || colon == std::string::npos ||
      inet_pton(AF_INET, host.substr(0, colon).c_str(), &numeric) != 1) {
    return;
  }
  const game::net::netadr_t address = network::address_from_string(host);
  if (!network::is_ip_address(address)) {
    return;
  }
  std::lock_guard lock(joiner_mutex);
  ticket_for_join = join_ticket{address, ticket, steady::now() + 24h};
}

bool has_ticket_for(const game::net::netadr_t &host) {
  std::lock_guard lock(joiner_mutex);
  return ticket_matches(host);
}

bool admits(const game::net::netadr_t &sender) {
  using game::net::netadrtype_t;
  const bool locked = xerces_join_password::locked();
  // Without the check: the engine's own local players and bots, and this
  // game's own socket (DESIGN.md 11.10). A second game on this PC, or a
  // relay or tunnel agent that hands players from elsewhere to this game,
  // sends from 127.x or this PC's address on another port, and answers the
  // check like anyone.
  core::sender_kind kind = core::sender_kind::other;
  if (sender.type == netadrtype_t::LOOPBACK ||
      sender.type == netadrtype_t::BOT) {
    kind = core::sender_kind::engine_local;
  } else if (network::is_ip_address(sender)) {
    kind = core::sender_kind::ip;
  }
  const std::string peer = kind == core::sender_kind::ip
                               ? network::address_to_string(sender)
                               : std::string{};
  std::lock_guard lock(host_mutex);
  return guard.lets_in(kind, peer, locked,
                       locked && kind == core::sender_kind::ip
                           ? own_addresses()
                           : core::own_addresses{});
}

void prove(const game::net::netadr_t &host, done_callback done) {
  std::lock_guard lock(joiner_mutex);
  joining = {};
  joining.active = true;
  joining.host = host;
  joining.host_text = network::address_to_string(host);
  joining.done = std::move(done);
  start_exchange(steady::now());
}

std::string message(const outcome result, const std::uint32_t wait_seconds) {
  switch (result) {
  case outcome::passed:
  case outcome::open:
    return {};
  case outcome::needed:
    return core::needed_message();
  case outcome::wrong:
    return core::wrong_message();
  case outcome::wait:
    return core::wait_message(wait_seconds);
  case outcome::busy:
    return core::busy_message(wait_seconds);
  case outcome::paused:
    return core::paused_message(wait_seconds);
  case outcome::stale:
    return core::stale_message();
  case outcome::address:
    return core::address_message();
  case outcome::timeout:
    return core::timeout_message();
  case outcome::newer:
    return core::newer_message();
  case outcome::refused:
    return core::host_refused_message();
  }
  return {};
}

void show(const outcome result, const std::uint32_t wait_seconds) {
  const std::string text = message(result, wait_seconds);
  if (text.empty()) {
    return;
  }
  printf("Connect failed: %s\n", text.c_str());
  scheduler::once(
      [text] {
        game::ui::UI_OpenErrorPopupWithMessage(
            game::LOCAL_CLIENT_0, game::errorCode::UI, text.c_str());
      },
      scheduler::main);
}

namespace {
game::EngineDependentDvarMut skip_dvar;
} // namespace

bool skip_check_for_test() { return skip_dvar && skip_dvar.get_bool(); }

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_join_password");

public:
  void post_unpack() override {
    network::on(std::string(core::challenge_command), receive_challenge);
    network::on(std::string(core::proof_command), receive_proof);
    // A connect into the match this game runs (auth.cpp's handler comes
    // after); a game that runs no match passes it on unchanged.
    network::guard("connect", [](const game::net::netadr_t &sender) {
      if (!game::server_running() || admits(sender)) {
        return true;
      }
      if (network::is_ip_address(sender)) {
        refuse_connect(sender);
      }
      return false;
    });

    if (game::is_server()) {
      return;
    }
    network::on(std::string(core::nonce_command), receive_nonce);
    network::on(std::string(core::result_command), receive_result);
    scheduler::loop(update_joiner, scheduler::main, 250ms);
    scheduler::once(
        [] {
          skip_dvar = game::register_dvar_bool(
              "xerces_join_skip_password_check", false, game::dvarFlags_e::NONE,
              "Testing: join without answering the host's password check, as "
              "a modified client would (the host refuses)");
        },
        scheduler::main);
  }
};
} // namespace xerces_join_password

REGISTER_COMPONENT(xerces_join_password::component)
