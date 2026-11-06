#include <std_include.hpp>

#include "game/impl/ugc/ugc.hpp"
#include "loader/component_loader.hpp"

#include "game/utils.hpp"

#include "auth.hpp"
#include "friends.hpp"
#include "getinfo.hpp"
#include "mod_stack.hpp"
#include "nat.hpp"
#include "network.hpp"
#include "party.hpp"
#include "profile_infos.hpp"
#include "scheduler.hpp"
#include "server_list.hpp"
#include "toast.hpp"
#include "upnp.hpp"
#include "workshop/workshop.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>

#include <xerces/join.hpp>
#include <xerces/join_password.hpp>
#include <xerces/runtime.hpp>
#include <xerces/switches.hpp>
#include <xerces_core/join_check.hpp>

#include <random>
#include <sstream>

namespace nat {
namespace {
constexpr std::chrono::seconds REGISTER_INTERVAL = 5s;
constexpr std::chrono::seconds JOIN_TIMEOUT = 12s;
constexpr size_t MAX_CANDIDATES = 16;
constexpr const char *FRIEND_REJOIN_ROUTE = "192.0.2.254:40000";

bool open_to_friends{};
// Xerces: update_hosting checks that an open lobby is still there; after a
// few checks without one it closes it to direct connect.
int lobby_gone_checks{};
// Xerces: one automatic rejoin per host after loading its mods, so a stack
// that never loads here cannot rejoin in a loop.
std::optional<game::net::netadr_t> rejoined_host;
std::chrono::steady_clock::time_point rejoined_at{};
bool registration_confirmed{};
bool identity_warning_shown{};
std::string hosting_token;
std::string reflected_endpoint;

struct join_attempt {
  bool active{};
  bool retried{};
  uint64_t friend_id{};
  std::string token;
  std::string fallback;
  std::vector<game::net::netadr_t> candidates;
  std::chrono::steady_clock::time_point expires;
  std::chrono::steady_clock::time_point next_rendezvous;
};

struct host_probe {
  std::string token;
  game::net::netadr_t candidate{};
  std::chrono::steady_clock::time_point expires;
};

join_attempt joining;
std::vector<host_probe> host_probes;

struct joined_match {
  std::string token;
  game::net::netadr_t host{};
};

std::optional<joined_match> joined;

struct lobby_request {
  bool active{};
  std::string token;
  game::net::netadr_t host{};
  std::chrono::steady_clock::time_point expires;
  std::function<void()> fallback;
};

lobby_request lobby_joining;
std::function<void()> game_join_fallback;
std::optional<game::net::netadr_t> server_lobby;
std::mutex peers_mutex;
typedef std::unordered_map<std::string, game::net::netadr_t> lobbyPeerMap_t;
lobbyPeerMap_t lobby_peers;
utils::hook::detour dw_common_addr_to_netadr_hook;
utils::hook::detour dw_get_connection_status_hook;
utils::hook::detour send_join_response_hook;

struct lookup_state {
  std::unordered_set<uint64_t> pending;
  std::vector<friend_presence> results;
  lookup_callback done;
};

std::mutex lookup_mutex;
typedef std::unordered_map<std::string, lookup_state> lookupMap_t;
static lookupMap_t lookup_requests;

void finish_lookup(const std::string &request) {
  lookup_callback done;
  std::vector<friend_presence> results;
  {
    std::scoped_lock lock(lookup_mutex);
    const lookupMap_t::iterator found = lookup_requests.find(request);
    if (found == lookup_requests.end())
      return;
    results = std::move(found->second.results);
    done = std::move(found->second.done);
    lookup_requests.erase(found);
  }
  if (done)
    done(std::move(results));
}

std::string payload_string(const network::data_view &data) {
  return {reinterpret_cast<const char *>(data.data()), data.size()};
}

std::vector<std::string> fields(const std::string &value) {
  std::istringstream input(value);
  std::vector<std::string> result;
  for (std::string field; input >> field;)
    result.emplace_back(std::move(field));
  return result;
}

bool valid_token(const std::string &token) {
  if (token.size() < 8 || token.size() > 64)
    return false;

  return std::all_of(token.begin(), token.end(), [](const unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_';
  });
}

std::string new_token() {
  static constexpr char alphabet[] = "0123456789abcdef";
  std::random_device entropy;
  std::mt19937 generator(entropy());
  std::uniform_int_distribution<unsigned int> pick(0, 15);
  std::string result(16, '0');
  for (auto &character : result)
    character = alphabet[pick(generator)];
  return result;
}

uint16_t local_port() {
  const uint16_t port = party::get_local_port();
  return port >= 1024 ? port : 3074;
}

std::string local_endpoint() {
  static const std::string local_ip = [] {
    const SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET)
      return std::string{};

    sockaddr_in route{};
    route.sin_family = AF_INET;
    route.sin_port = htons(53);
    inet_pton(AF_INET, "1.1.1.1", &route.sin_addr);

    std::string result;
    if (connect(socket_handle, reinterpret_cast<sockaddr *>(&route),
                sizeof(route)) == 0) {
      sockaddr_in local{};
      int size = sizeof(local);
      if (getsockname(socket_handle, reinterpret_cast<sockaddr *>(&local),
                      &size) == 0) {
        char ip[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip)))
          result = ip;
      }
    }
    closesocket(socket_handle);
    return result;
  }();

  return local_ip.empty()
             ? std::string{}
             : utils::string::va("%s:%hu", local_ip.c_str(), local_port());
}

void send_rendezvous(const std::string &command, const std::string &token) {
  // Xerces (D14): every rendezvous, presence and friend-code message to Ezz's
  // masters goes through here; Xerces builds send none of them.
  if constexpr (!xerces_switches::ezz_social) {
    return;
  }
  const std::vector<game::net::netadr_t> &masters =
      server_list::get_master_servers();
  if (masters.empty())
    return;

  std::string message = token;
  if (const std::string local = local_endpoint(); !local.empty())
    message.append(" ").append(local);
  for (const game::net::netadr_t &master : masters)
    network::send(master, command, message);
}

void send_friend_publish() {
  const game::XUID friend_code = auth::get_guid();
  if (!friend_code) {
    if (!identity_warning_shown) {
      identity_warning_shown = true;
      toast::error("Friends unavailable",
                   "BOIII Friend Code could not be generated.");
    }
    return;
  }
  if (hosting_token.empty())
    return;
  const char *payload =
      utils::string::va("1 %llu %s", friend_code, hosting_token.c_str());
  send_rendezvous("friendPublish", payload);
}

void add_join_candidate(const std::string &text) {
  if (joining.candidates.size() >= MAX_CANDIDATES)
    return;

  const game::net::netadr_t candidate = network::address_from_string(text);
  if (!network::is_connectable_address(candidate))
    return;

  const bool duplicate =
      std::any_of(joining.candidates.begin(), joining.candidates.end(),
                  [&candidate](const game::net::netadr_t &existing) {
                    return network::are_addresses_equal(existing, candidate);
                  });
  if (!duplicate)
    joining.candidates.push_back(candidate);
}

std::string to_hex(const void *data, const size_t size) {
  std::string result;
  for (size_t i = 0; i < size; ++i)
    result += std::format("{:02x}", static_cast<const uint8_t *>(data)[i]);
  return result;
}

bool from_hex(const std::string &text, void *out, const size_t size) {
  if (text.size() != size * 2)
    return false;
  uint8_t *bytes = static_cast<uint8_t *>(out);
  for (size_t i = 0; i < size; ++i) {
    if (std::from_chars(text.data() + i * 2, text.data() + i * 2 + 2, bytes[i],
                        16)
            .ec != std::errc{})
      return false;
  }
  return true;
}

std::string peer_key(const game::net::XNADDR &address) {
  return std::string(reinterpret_cast<const char *>(address.addrBuff),
                     sizeof(game::net::XNADDR));
}

void add_peer(const game::net::XNADDR &address,
              const game::net::netadr_t &endpoint) {
  {
    std::scoped_lock lock(peers_mutex);
    lobby_peers.insert_or_assign(peer_key(address), endpoint);
  }
  profile_infos::distribute_profile_infos_to_user(endpoint);
}

std::optional<game::net::netadr_t> find_peer(const game::net::XNADDR &address) {
  constexpr size_t ENDPOINT_SIZE = 6;
  std::scoped_lock lock(peers_mutex);
  if (const lobbyPeerMap_t::iterator peer = lobby_peers.find(peer_key(address));
      peer != lobby_peers.end())
    return peer->second;

  std::optional<game::net::netadr_t> match;
  for (const auto &[key, endpoint] : lobby_peers) {
    if (std::memcmp(key.data(), address, ENDPOINT_SIZE) == 0) {
      if (match)
        return std::nullopt;
      match = endpoint;
    }
  }
  return match;
}

bool dw_common_addr_to_netadr_stub(game::net::netadr_t *netadr,
                                   const game::net::XNADDR &common_addr,
                                   const game::net::bdSecurityID *sec_id) {
  if (const std::optional<game::net::netadr_t> peer = find_peer(common_addr)) {
    *netadr = *peer;
    return true;
  }
  return dw_common_addr_to_netadr_hook.invoke<bool>(netadr, common_addr,
                                                    sec_id);
}

int32_t dw_get_connection_status_stub(const game::net::netadr_t *netadr) {
  constexpr int32_t CONNECTED = 2;
  return is_lobby_peer(*netadr)
             ? CONNECTED
             : dw_get_connection_status_hook.invoke<int32_t>(netadr);
}

game::net::HostInfo *lobby_host_info(
    const game::lobby::LobbyType type = game::lobby::LobbyType::PRIVATE) {
  if (game::is_legacy_client() || !game::lobby::LobbyHost_IsHost(type))
    return nullptr;
  game::lobby::session::LobbySession *session =
      game::lobby::LobbyHostData_GetSession(type);
  return session ? &session->host.info : nullptr;
}

bool is_lobby_address(const game::net::netadr_t &address) {
  return network::is_connectable_address(address) ||
         (network::is_ip_address(address) && address.ipv4.a == 127);
}

bool hosting_match() {
  return getinfo::is_host() && !game::com::Com_IsRunningUILevel();
}

std::optional<game::net::netadr_t> dedicated_server() {
  if (hosting_match() || !game::com::Com_IsInGame() ||
      !party::cl_connected_to_dedi.get_bool())
    return std::nullopt;
  const game::net::netadr_t server = party::get_connected_server();
  if (server != party::get_connect_host() ||
      !network::is_connectable_address(server))
    return std::nullopt;
  return server;
}

std::optional<std::string> joined_match_token() {
  if (!joined || hosting_match() || !game::com::Com_IsInGame() ||
      !network::are_addresses_equal(party::get_connected_server(),
                                    joined->host))
    return std::nullopt;
  return joined->token;
}

bool can_host() {
  return hosting_match() || lobby_host_info() ||
         lobby_host_info(game::lobby::LobbyType::GAME) || dedicated_server();
}

void show_error(const std::string &message) {
  game::ui::UI_OpenErrorPopupWithMessage(game::LOCAL_CLIENT_0,
                                         game::errorCode::UI, message.c_str());
}

void connect_to(const game::net::netadr_t &endpoint) {
  const std::string address = network::address_to_string(endpoint);
  if (!address.empty()) {
    party::connect(endpoint);
  }
}

void request_lobby(const game::net::netadr_t &host, const std::string &token,
                   std::function<void()> fallback = {}) {
  const game::net::HostInfo *own = lobby_host_info();
  if (!own) {
    if (fallback) {
      fallback();
      return;
    }
    show_error("Open the Multiplayer or Zombies menu first, then join again.");
    return;
  }
  lobby_joining = {true, token, host, std::chrono::steady_clock::now() + 4s,
                   std::move(fallback)};
  network::send(host, "lobbyJoin",
                token + " " +
                    to_hex(own->serializedAdr.addrBuff,
                           sizeof(own->serializedAdr.addrBuff)));
}

void complete_join(const game::net::netadr_t &endpoint) {
  if (joining.active) {
    joining.active = false;
    joined = joined_match{joining.token, endpoint};
    request_lobby(endpoint, joining.token);
  }
}

void show_join_failure(const uint16_t port) {
  // Xerces: the host's game maps its port itself while the lobby is open.
  show_error(xerces_core::unreachable_message(port ? port : local_port()));
}

void lobby_join_complete(int32_t, const game::lobby::join::JoinResult result) {
  using game::lobby::join::JoinResult;
  const std::function<void()> fallback = std::exchange(game_join_fallback, {});
  if (result != JoinResult::SUCCESS && fallback) {
    fallback();
    return;
  }
  // Xerces: the words are in xerces_core/join_check.cpp, for every build.
  using xerces_core::lobby_failure;
  lobby_failure failure = lobby_failure::other;
  switch (result) {
  case JoinResult::SUCCESS:
    return;
  case JoinResult::LOBBY_FULL:
  case JoinResult::COULD_NOT_RESERVE:
    failure = lobby_failure::full;
    break;
  case JoinResult::NOT_JOINABLE_NOT_IDLE:
  case JoinResult::MIGRATE_IN_PROGRESS:
    failure = lobby_failure::busy;
    break;
  case JoinResult::JOIN_DISABLED:
  case JoinResult::NOT_JOINABLE_NOT_HOSTING:
  case JoinResult::NOT_JOINABLE_CLOSED:
  case JoinResult::NOT_JOINABLE_INVITE_ONLY:
  case JoinResult::NOT_JOINABLE_FRIENDS_ONLY:
    failure = lobby_failure::closed;
    break;
  case JoinResult::NETWORK_MODE_MISMATCH:
    failure = lobby_failure::network_mode;
    break;
  case JoinResult::MISMATCH_PROTOCOL_VERSION:
  case JoinResult::MISMATCH_NETFIELD_CHECKSUM:
  case JoinResult::MISMATCH_FFOTD_VERSION_TO_NEW:
  case JoinResult::MISMATCH_FFOTD_VERSION_TO_OLD:
  case JoinResult::MISMATCH_PLAYLISTID:
  case JoinResult::MISMATCH_PLAYLIST_VERSION_TO_NEW:
  case JoinResult::MISMATCH_PLAYLIST_VERSION_TO_OLD:
    failure = lobby_failure::version;
    break;
  case JoinResult::JOIN_ALREADY_IN_PROGRESS:
    failure = lobby_failure::in_progress;
    break;
  case JoinResult::INVALID:
    failure = lobby_failure::refused;
    break;
  default:
    break;
  }
  show_error(xerces_core::lobby_failure_message(failure,
                                                static_cast<uint32_t>(result)));
}

void fail_lobby_join(const std::string &message) {
  if (const std::function<void()> fallback =
          std::exchange(game_join_fallback, {})) {
    fallback();
  } else if (!message.empty()) {
    show_error(message);
  }
}

bool can_join_lobby(const game::net::netadr_t &host) {
  const bool on_server =
      server_lobby && network::are_addresses_equal(*server_lobby, host);
  return (!game::com::Com_IsInGame() || on_server) && lobby_host_info();
}

// Xerces: after the host's mods load, join again. A friend's join goes
// through the friend list as upstream did; a direct connect has no friend id
// and connects to the same address again, once per host a minute.
void rejoin_after_mod_load(const game::net::netadr_t &host,
                           const uint64_t friend_id) {
  if (friend_id) {
    scheduler::once([friend_id] { friends::connect_to_friend(friend_id); },
                    scheduler::main, 3s);
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (rejoined_host && network::are_addresses_equal(*rejoined_host, host) &&
      now - rejoined_at < 60s) {
    rejoined_host.reset();
    show_error("The host's mods did not load here, so the join stopped. "
               "Xerces > Diagnostics shows what went wrong.");
    return;
  }
  rejoined_host = host;
  rejoined_at = now;
  scheduler::once([host] { party::connect(host); }, scheduler::main, 3s);
}

// The rest of join_lobby, once the host's content is checked: the join
// takes the host's stack and map-first rule, the host's mods load first if
// they are not loaded yet, then the engine's lobby join. Nothing of the
// host's is taken before this, so a join that stops at a dialog leaves this
// game's own mods as they were.
void finish_lobby_join(const game::net::netadr_t &host,
                       game::net::HostInfo info,
                       const game::lobby::LobbyType target,
                       const std::string &mod, const std::string &stack,
                       const std::string &map_scripts_first,
                       const bool same_mods, const uint64_t friend_id) {
  // A dialog may have been open for a while.
  if (!can_join_lobby(host)) {
    fail_lobby_join("Leave your current match first, then join again.");
    return;
  }
  mod_stack::set(stack);
  mod_stack::follow_host_map_scripts_first(map_scripts_first);
  xerces_join::note_joined(mod, stack);
  if (!same_mods) {
    game_join_fallback = {};
    workshop::setup_same_mod_as_host(game::LOCAL_CLIENT_0, {}, mod);
    rejoin_after_mod_load(host, friend_id);
    return;
  }
  rejoined_host.reset(); // the host's mods are loaded

  add_peer(info.serializedAdr.addrBuff, host);
  using game::lobby::LobbyType;
  if (!game::lobby::join::LobbyJoin_Begin(
          0,
          game::com::Com_LocalClient_GetControllerIndex(game::LOCAL_CLIENT_0),
          LobbyType::PRIVATE, target, lobby_join_complete)) {
    fail_lobby_join("Already joining a lobby. Please wait.");
    return;
  }
  game::lobby::join::LobbyJoin_Add(
      info.xuid, info.name, &info.secId, &info.secKey, &info.serializedAdr,
      target == LobbyType::GAME ? game::lobby::join::JoinType::PLAYLIST
                                : game::lobby::join::JoinType::FRIEND,
      0);
  game::lobby::join::LobbyJoin_Finalize();
}

void join_lobby(const game::net::netadr_t &host,
                const std::vector<std::string> &parts) {
  game::net::HostInfo info{};
  info.xuid = std::strtoull(parts[1].c_str(), nullptr, 16);
  info.serializedAdr.valid = 1;
  if (!info.xuid ||
      !from_hex(parts[2], info.serializedAdr.addrBuff,
                sizeof(info.serializedAdr.addrBuff)) ||
      !from_hex(parts[3], &info.secId, sizeof(info.secId)) ||
      !from_hex(parts[4], &info.secKey, sizeof(info.secKey)))
    return;

  const game::lobby::LobbyType target = parts[5] == "game"
                                            ? game::lobby::LobbyType::GAME
                                            : game::lobby::LobbyType::PRIVATE;
  std::vector<std::string> content(6);
  size_t name_index = 6;
  if (parts[6].starts_with("c:")) {
    content = utils::string::split(parts[6].substr(2), ':');
    // The fifth field is Xerces' map-first choice and the sixth its content
    // ids (xerces_core/join_check.hpp); older hosts leave them out and older
    // clients drop them.
    content.resize(6);
    name_index = 7;
  }
  std::string name;
  for (size_t i = name_index; i < parts.size(); ++i)
    name.append(name.empty() ? "" : " ").append(parts[i]);
  utils::string::copy(info.name, name.c_str());

  if (target == game::lobby::LobbyType::GAME) {
    if (!game_join_fallback) {
      game_join_fallback = [host] { connect_to(host); };
    }
  } else {
    game_join_fallback = {};
  }

  if (!can_join_lobby(host)) {
    fail_lobby_join("Leave your current match first, then join again.");
    return;
  }

  const std::string mod = content[0];
  const std::string map = content[1];
  const std::string loaded_mod = game::ugc::UGC_ActiveMod_PublisherId();
  // Xerces: compared with the host's list without taking it; the join takes
  // it in finish_lobby_join once nothing stops it.
  const bool same_mods =
      mod == (loaded_mod == "usermaps" ? std::string{} : loaded_mod) &&
      mod_stack::is_loaded(content[3]);
  if (game_join_fallback && !same_mods) {
    // The fallback connects to the host's match and loads the host's mods
    // there (party.cpp, connect_to_lobby), so it needs the host's list.
    mod_stack::set(content[3]);
    mod_stack::follow_host_map_scripts_first(content[4]);
    fail_lobby_join({});
    return;
  }

  const uint64_t friend_id = joining.friend_id;
  if constexpr (xerces_switches::upstream_downloaders) {
    // Upstream takes the host's list before its downloads.
    mod_stack::set(content[3]);
    mod_stack::follow_host_map_scripts_first(content[4]);
    if (!workshop::check_valid_mod_id(mod, {}) ||
        !workshop::check_valid_mod_stack(content[3]) ||
        (target == game::lobby::LobbyType::GAME && !map.empty() &&
         !workshop::check_valid_usermap_id(map, content[2], {}, {}))) {
      game_join_fallback = {};
      if (friend_id) {
        friends::remember_browser_route(friend_id, FRIEND_REJOIN_ROUTE);
        workshop::set_pending_download_reconnect(FRIEND_REJOIN_ROUTE);
      }
      return;
    }
    finish_lobby_join(host, info, target, mod, content[3], content[4],
                      same_mods, friend_id);
    return;
  }

  // Xerces (D15, DESIGN.md 11.10): everything the host runs that is missing
  // here is named at once, with its Workshop page, and files that differ
  // from the host's are named before the host's mods load. A stock map the
  // game lacks is DLC (workshop.cpp says so); a custom map is checked in a
  // party lobby too, because the host's match will load it.
  const bool custom_map =
      !map.empty() && map != "core_frontend" && !workshop::is_stock_map(map);
  if (target == game::lobby::LobbyType::GAME && !map.empty() && !custom_map &&
      !workshop::check_valid_usermap_id(map, content[2], {}, {})) {
    game_join_fallback = {};
    return;
  }
  xerces_join::host_offer offer;
  offer.main_mod = mod;
  offer.stack = content[3];
  offer.map = custom_map ? map : std::string{};
  offer.map_workshop_id = content[2];
  offer.content_ids = content[5];
  // Held back while a dialog may be open; the join puts it back.
  std::function<void()> fallback = std::exchange(game_join_fallback, {});
  const std::string map_scripts_first = content[4];
  xerces_join::check_host(offer, [=] {
    game_join_fallback = fallback;
    finish_lobby_join(host, info, target, mod, offer.stack, map_scripts_first,
                      same_mods, friend_id);
  });
}

void update_punching() {
  const std::chrono::steady_clock::time_point now =
      std::chrono::steady_clock::now();

  if (joining.active) {
    if (now >= joining.expires && !joining.retried) {
      printf("[Friends] Could not reach your friend yet, retrying\n");
      joining.retried = true;
      joining.candidates.clear();
      joining.expires = now + JOIN_TIMEOUT;
      joining.next_rendezvous = now;
    } else if (now >= joining.expires) {
      const game::net::netadr_t fallback =
          network::address_from_string(joining.fallback);
      joining.active = false;
      if (joining.candidates.empty())
        printf("[Friends] The master server did not send your friend's "
               "address\n");
      else
        printf("[Friends] Could not reach your friend directly (%zu "
               "addresses tried)\n",
               joining.candidates.size());
      if (network::is_connectable_address(fallback))
        request_lobby(fallback, joining.token);
      else
        show_join_failure(fallback.port);
    } else {
      if (joining.candidates.empty() && now >= joining.next_rendezvous) {
        send_rendezvous("privJoin", joining.token);
        joining.next_rendezvous = now + 1s;
      }
      for (const game::net::netadr_t &candidate : joining.candidates)
        network::send(candidate, "punch", joining.token);
    }
  }

  if (lobby_joining.active && now >= lobby_joining.expires) {
    lobby_joining.active = false;
    if (const std::function<void()> fallback =
            std::exchange(lobby_joining.fallback, {})) {
      fallback();
    } else {
      printf("[Friends] Your friend's game did not answer\n");
      show_join_failure(lobby_joining.host.port);
    }
  }

  std::erase_if(host_probes, [now](const host_probe &probe) {
    return now >= probe.expires;
  });
  for (const host_probe &probe : host_probes)
    network::send(probe.candidate, "punch", probe.token);
}

void refresh_lobby_buttons() {
  if (const uint16_t global = game::ui::UI_Model_GetGlobalModel()) {
    if (const uint16_t model = game::ui::UI_Model_CreateModelFromPath(
            global, "lobbyRoot.lobbyButtonUpdate"))
      game::ui::UI_Model_ForceNotify(model);
  }
}

void show_notice(const xerces_core::join_notice &notice, const bool warn) {
  if (warn) {
    toast::warn(notice.title, notice.message);
  } else {
    toast::success(notice.title, notice.message);
  }
}

// Xerces: what the router said about the lobby's port, on screen only (the
// address never goes into a log).
void port_mapped(const upnp::outcome &outcome) {
  scheduler::once(
      [outcome] {
        if (!open_to_friends) {
          return;
        }
        using xerces_core::mapped_reach;
        using xerces_core::port_map_result;
        const port_map_result result = outcome.value == upnp::result::mapped
                                           ? port_map_result::mapped
                                       : outcome.value == upnp::result::refused
                                           ? port_map_result::refused
                                           : port_map_result::no_router;
        const mapped_reach reach =
            xerces_core::classify_mapped(outcome.external_ip);
        const bool public_address = result == port_map_result::mapped &&
                                    reach == mapped_reach::public_address;
        show_notice(xerces_core::direct_connect_notice(
                        result, outcome.external_ip, outcome.port),
                    !public_address);
        xerces_runtime::log_info(
            "join.direct_connect.port",
            result == port_map_result::mapped
                ? (public_address ? "the router forwards the port"
                   : reach == mapped_reach::unknown
                       ? "the router forwards the port; it did not say its "
                         "outside address"
                       : "the router forwards the port; its outside address "
                         "is not public")
            : result == port_map_result::refused ? "the router refused the port"
                                                 : "no UPnP router answered",
            "{\"port\":" + std::to_string(outcome.port) + "}");
      },
      scheduler::main);
}

void set_open(const bool enabled) {
  const bool was_open = open_to_friends;
  open_to_friends = enabled;
  lobby_gone_checks = 0;
  game::Dvar_SetFromStringByName("nat_open", enabled ? "1" : "0", true);
  game::Dvar_SetFromStringByName("friends_open", enabled ? "1" : "0", true);
  refresh_lobby_buttons();
  game::Dvar_SetFromStringByName("com_pauseSupported", enabled ? "0" : "1",
                                 true);
  // Xerces: the router forwards the game's UDP port while the lobby is
  // open, and stops when it closes (or the game exits, upnp.cpp).
  if (enabled && !was_open)
    upnp::open_port(local_port(), port_mapped);
  else if (!enabled)
    upnp::close_port();
  if (!enabled) {
    registration_confirmed = false;
    identity_warning_shown = false;
    hosting_token.clear();
    reflected_endpoint.clear();
    host_probes.clear();
  }
}

// Xerces: for the "closed" notice, non-empty while the router forwards the
// port (join_check.hpp), also when it did not say its outside address.
std::string forwarded_endpoint() {
  if (std::string mapped = upnp::external_endpoint(); !mapped.empty())
    return mapped;
  return upnp::port_mapped() ? "forwarded" : std::string{};
}

// Xerces: closes the lobby to direct connect, saying so.
void close_direct_connect() {
  const std::string mapped = forwarded_endpoint();
  set_open(false);
  show_notice(xerces_core::direct_connect_notice(
                  xerces_core::port_map_result::closed, mapped, local_port()),
              true);
}

void update_hosting() {
  if constexpr (!xerces_switches::ezz_social) {
    // D14: nothing goes to Ezz's masters, so there is nothing to publish.
    // An open lobby closes once the host has had none for about 15 s in
    // the menus (a match keeps it open; a map load is not a menu).
    if (open_to_friends && !can_host() && !game::com::Com_IsInGame()) {
      if (++lobby_gone_checks >= 3) {
        close_direct_connect();
      }
    } else {
      lobby_gone_checks = 0;
    }
    return;
  }
  if (!open_to_friends) {
    if (const game::XUID friend_code = auth::get_guid())
      send_rendezvous("friendPublish",
                      utils::string::va("1 %llu -", friend_code));
    return;
  }
  if (!can_host())
    return;

  if (hosting_token.empty()) {
    hosting_token = new_token();
    registration_confirmed = false;
    identity_warning_shown = false;
  }
  send_rendezvous("privRegister", hosting_token);
  send_friend_publish();
}

void receive_lobby_join(const game::net::netadr_t &sender,
                        const network::data_view &data,
                        game::LocalClientNum_t) {
  const std::vector<std::string> parts = fields(payload_string(data));
  if (parts.size() != 2 ||
      (parts[0] == "match"
           ? joinable_lobby().empty()
           : hosting_token.empty() || parts[0] != hosting_token) ||
      !is_lobby_address(sender))
    return;

  // Xerces (DESIGN.md 11.10): with net_password set, only an address that
  // passed the host's password check hears anything of this lobby (its
  // address, security id and key, or the match to connect to); the others
  // hear why. Without a password everyone gets in as before, and is
  // remembered, so a password set later keeps them in.
  if (!xerces_join_password::admits(sender)) {
    network::send(sender, "lobbyDenied", parts[0] + " password");
    return;
  }

  if (hosting_match() && !lobby_host_info(game::lobby::LobbyType::GAME)) {
    network::send(sender, "lobbyGame", hosting_token);
    return;
  }

  if (const std::optional<game::net::netadr_t> server = dedicated_server()) {
    network::send(sender, "lobbyGame",
                  parts[0] + " " + network::address_to_string(*server));
    return;
  }

  if (const std::optional<std::string> token = joined_match_token()) {
    network::send(sender, "lobbyGame", parts[0] + " " + *token);
    return;
  }

  const game::net::HostInfo *game_lobby =
      lobby_host_info(game::lobby::LobbyType::GAME);
  const game::net::HostInfo *host = game_lobby ? game_lobby : lobby_host_info();
  game::net::XNADDR peer{};
  if (!host || !from_hex(parts[1], peer.addrBuff, sizeof(peer.addrBuff)))
    return;

  add_peer(peer, sender);
  const std::string mod = workshop::get_mod_publisher_id();
  const std::string map(game::get_dvar_string("ui_mapname").value_or(""));

  const game::ugc::OptionalUGCItemRef usermap_item =
      game::ugc::UGC_GetUsermapByInternalName(map);
  const std::optional<std::string_view> usermap_id =
      usermap_item.has_value() ? std::optional(usermap_item->get().publisherId)
                               : std::nullopt;
  // Xerces: the sixth content field is the content ids of the mods and the
  // map (xerces_core/join_check.hpp), so a joiner can say which one differs.
  const std::string usermap(usermap_id.value_or(""));
  const bool custom_map =
      !map.empty() && map != "core_frontend" && !workshop::is_stock_map(map);
  network::send(
      sender, "lobbyHost",
      std::format("{} {:x} {} {} {} {} c:{}:{}:{}:{}:{}:{} {}", parts[0],
                  host->xuid,
                  to_hex(host->serializedAdr.addrBuff,
                         sizeof(host->serializedAdr.addrBuff)),
                  to_hex(&host->secId, sizeof(host->secId)),
                  to_hex(&host->secKey, sizeof(host->secKey)),
                  game_lobby ? "game" : "party",
                  mod == "usermaps" ? std::string{} : mod, map, usermap,
                  mod_stack::serialize(), mod_stack::map_scripts_first_info(),
                  xerces_join::own_content_ids(custom_map ? map : std::string{},
                                               usermap),
                  host->name));
}

bool from_lobby_request(const game::net::netadr_t &sender,
                        const std::string &token) {
  if (!lobby_joining.active || token != lobby_joining.token ||
      !is_lobby_address(sender))
    return false;
  lobby_joining.active = false;
  game_join_fallback = std::exchange(lobby_joining.fallback, {});
  return true;
}

void receive_lobby_host(const game::net::netadr_t &sender,
                        const network::data_view &data,
                        game::LocalClientNum_t) {
  const std::vector<std::string> parts = fields(payload_string(data));
  if (parts.size() >= 7 && from_lobby_request(sender, parts[0]))
    scheduler::once([sender, parts] { join_lobby(sender, parts); },
                    scheduler::main);
}

// Xerces: the host refused the lobby join; with "password", this game has
// not passed its password check (no net_password, or the host restarted).
// The join stops here, without the "did not answer" message or a direct
// connect the host would refuse too.
void receive_lobby_denied(const game::net::netadr_t &sender,
                          const network::data_view &data,
                          game::LocalClientNum_t) {
  const std::vector<std::string> parts = fields(payload_string(data));
  if (parts.size() != 2 || parts[1] != "password" ||
      !from_lobby_request(sender, parts[0]))
    return;
  game_join_fallback = {};
  printf("[Friends] The host refused the join: no password check passed\n");
  xerces_join_password::show(xerces_join_password::outcome::refused, 0);
}

void receive_lobby_follow(const game::net::netadr_t &sender,
                          const network::data_view &data,
                          game::LocalClientNum_t) {
  if (lobby_host_info() || game::com::Com_IsInGame() || !is_lobby_peer(sender))
    return;
  const game::net::netadr_t server =
      network::address_from_string(payload_string(data));
  if (!network::is_connectable_address(server))
    return;
  {
    std::scoped_lock lock(peers_mutex);
    std::erase_if(lobby_peers, [&](const lobbyPeerMap_t::value_type &peer) {
      return network::are_addresses_equal(peer.second, sender);
    });
  }
  scheduler::once([server] { party::connect(server); }, scheduler::main);
}

void receive_lobby_game(const game::net::netadr_t &sender,
                        const network::data_view &data,
                        game::LocalClientNum_t) {
  const std::vector<std::string> parts = fields(payload_string(data));
  if (parts.empty() || !from_lobby_request(sender, parts[0]))
    return;

  const std::function<void()> fallback = std::exchange(game_join_fallback, {});
  if (parts.size() > 1) {
    const game::net::netadr_t server = network::address_from_string(parts[1]);
    if (network::is_connectable_address(server)) {
      scheduler::once([server] { connect_to(server); }, scheduler::main);
      return;
    }
    if (valid_token(parts[1])) {
      scheduler::once([token = parts[1]] { begin_join(token, {}, 0); },
                      scheduler::main);
      return;
    }
  }
  scheduler::once(
      fallback ? fallback : [sender] { connect_to(sender); }, scheduler::main);
}

void receive_register_ack(const game::net::netadr_t &sender,
                          const network::data_view &data,
                          game::LocalClientNum_t) {
  if (!is_rendezvous(sender) || hosting_token.empty())
    return;

  const game::net::netadr_t endpoint =
      network::address_from_string(payload_string(data));
  if (network::is_connectable_address(endpoint)) {
    reflected_endpoint = network::address_to_string(endpoint);
    registration_confirmed = true;
  }
}

void receive_peer(const game::net::netadr_t &sender,
                  const network::data_view &data, game::LocalClientNum_t) {
  if (!is_rendezvous(sender))
    return;

  const std::vector<std::string> parts = fields(payload_string(data));
  if (parts.size() < 2 || !valid_token(parts.front()))
    return;

  const std::basic_string<char> &token = parts.front();
  if (joining.active && token == joining.token) {
    for (size_t i = 1; i < parts.size(); ++i)
      add_join_candidate(parts[i]);
    return;
  }

  if (token != hosting_token)
    return;

  for (size_t i = 1; i < parts.size() && host_probes.size() < MAX_CANDIDATES;
       ++i) {
    const game::net::netadr_t candidate =
        network::address_from_string(parts[i]);
    if (!network::is_connectable_address(candidate))
      continue;
    const bool duplicate = std::any_of(host_probes.begin(), host_probes.end(),
                                       [&candidate](const host_probe &probe) {
                                         return network::are_addresses_equal(
                                             probe.candidate, candidate);
                                       });
    if (!duplicate)
      host_probes.push_back(
          {token, candidate, std::chrono::steady_clock::now() + 10s});
  }
}

void receive_rejection(const game::net::netadr_t &sender,
                       const network::data_view &, game::LocalClientNum_t) {
  if (!is_rendezvous(sender) || !joining.active)
    return;
  printf("[Friends] Your friend's party is no longer open on the master "
         "server\n");
  joining.expires = std::chrono::steady_clock::now();
}

void receive_punch(const game::net::netadr_t &sender,
                   const network::data_view &data, game::LocalClientNum_t) {
  const std::string token = payload_string(data);
  const bool valid_join = joining.active && token == joining.token;
  const bool valid_host = !hosting_token.empty() && token == hosting_token;
  if ((!valid_join && !valid_host) || !network::is_connectable_address(sender))
    return;

  network::send(sender, "punchAck", token);
  if (valid_join)
    complete_join(sender);
}

void receive_punch_ack(const game::net::netadr_t &sender,
                       const network::data_view &data, game::LocalClientNum_t) {
  const std::string token = payload_string(data);
  if (joining.active && token == joining.token &&
      network::is_connectable_address(sender))
    complete_join(sender);
}

void receive_friend_presence(const game::net::netadr_t &sender,
                             const network::data_view &data,
                             game::LocalClientNum_t) {
  if (!is_rendezvous(sender))
    return;
  const std::vector<std::string> parts = fields(payload_string(data));
  if (parts.size() != 5 || parts[0] != "1" || !valid_token(parts[3]))
    return;
  const game::XUID steam_id = std::strtoull(parts[2].c_str(), nullptr, 10);
  const game::net::netadr_t endpoint = network::address_from_string(parts[4]);
  bool complete{};
  {
    std::scoped_lock lock(lookup_mutex);
    const lookupMap_t::iterator request = lookup_requests.find(parts[1]);
    if (request == lookup_requests.end() ||
        !request->second.pending.erase(steam_id)) {
      return;
    }
    request->second.results.push_back(
        {steam_id, parts[3],
         network::is_connectable_address(endpoint)
             ? network::address_to_string(endpoint)
             : std::string{}});
    complete = request->second.pending.empty();
  }
  if (complete) {
    scheduler::once([request = parts[1]] { finish_lookup(request); },
                    scheduler::main);
  }
}

void receive_friend_status(const game::net::netadr_t &sender,
                           const network::data_view &data, const bool online) {
  if (!is_rendezvous(sender))
    return;
  const std::vector<std::string> parts = fields(payload_string(data));
  if (parts.size() != 3 || parts[0] != "1")
    return;
  const game::XUID steam_id = std::strtoull(parts[2].c_str(), nullptr, 10);
  bool complete{};
  {
    std::scoped_lock lock(lookup_mutex);
    const lookupMap_t::iterator request = lookup_requests.find(parts[1]);
    if (request == lookup_requests.end() ||
        !request->second.pending.erase(steam_id)) {
      return;
    }
    if (online)
      request->second.results.push_back({steam_id, {}, {}});
    complete = request->second.pending.empty();
  }
  if (complete) {
    scheduler::once([request = parts[1]] { finish_lookup(request); },
                    scheduler::main);
  }
}

void receive_friend_offline(const game::net::netadr_t &sender,
                            const network::data_view &data,
                            game::LocalClientNum_t) {
  receive_friend_status(sender, data, false);
}

void receive_friend_online(const game::net::netadr_t &sender,
                           const network::data_view &data,
                           game::LocalClientNum_t) {
  receive_friend_status(sender, data, true);
}
} // namespace

std::string get_host_token() {
  return registration_confirmed ? hosting_token : std::string{};
}

std::string get_host_endpoint() {
  if (const std::string mapped = upnp::external_endpoint(); !mapped.empty())
    return mapped;
  if (hosting_token.empty() || !registration_confirmed)
    return {};
  if (!reflected_endpoint.empty())
    return reflected_endpoint;
  return local_endpoint();
}

void begin_join(const std::string &token, const std::string &fallback_address,
                const uint64_t friend_id) {
  if (!valid_token(token)) {
    show_error(
        "Your friend's party is closed. Ask them to open it to friends.");
    return;
  }

  if (joining.active && joining.token == token)
    return;

  joining = {};
  joining.active = true;
  joining.friend_id = friend_id;
  joining.token = token;
  const game::net::netadr_t fallback =
      network::address_from_string(fallback_address);
  if (network::is_connectable_address(fallback))
    joining.fallback = network::address_to_string(fallback);
  joining.expires = std::chrono::steady_clock::now() + JOIN_TIMEOUT;
  joining.next_rendezvous = std::chrono::steady_clock::now() + 1s;
  send_rendezvous("privJoin", token);
  toast::show("Friends", "Joining your friend...", "t7_icon_connect_overlays");
}

std::string joinable_lobby() {
  if (hosting_match() || game::is_server()) {
    return lobby_host_info(game::lobby::LobbyType::GAME) ? "game" : "";
  }
  return open_to_friends && lobby_host_info() ? "party" : "";
}

void join_match_lobby(const game::net::netadr_t &host,
                      std::function<void()> fallback) {
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  scheduler::schedule(
      [=] {
        if (!lobby_host_info() && std::chrono::steady_clock::now() < deadline) {
          return scheduler::cond_continue;
        }
        request_lobby(host, "match", fallback);
        return scheduler::cond_end;
      },
      scheduler::main, 250ms);
}

bool has_party_lobby() {
  return lobby_host_info() && game::lobby::LobbyHost_GetClientCount(
                                  game::lobby::LobbyType::PRIVATE,
                                  game::lobby::LobbyClientType::LOCAL) > 0;
}

bool has_party_members() {
  return lobby_host_info() && game::lobby::LobbyHost_GetClientCount(
                                  game::lobby::LobbyType::PRIVATE,
                                  game::lobby::LobbyClientType::REMOTE) > 0;
}

void send_party_to_server(const game::net::netadr_t &server) {
  if (!has_party_members())
    return;
  std::vector<game::net::netadr_t> members;
  {
    std::scoped_lock lock(peers_mutex);
    for (const auto &[key, endpoint] : lobby_peers) {
      if (!network::are_addresses_equal(endpoint, server) &&
          std::ranges::none_of(members, [&](const game::net::netadr_t &m) {
            return network::are_addresses_equal(m, endpoint);
          }))
        members.push_back(endpoint);
    }
  }
  for (const game::net::netadr_t &member : members)
    network::send(member, "lobbyFollow", network::address_to_string(server));
}

void join_server_lobby(const game::net::netadr_t &server) {
  server_lobby = server;
  request_lobby(server, "match", [] {});
}

void leave_server_lobby() {
  if (!server_lobby) {
    return;
  }
  std::scoped_lock lock(peers_mutex);
  std::erase_if(lobby_peers, [](const lobbyPeerMap_t::value_type &peer) {
    return network::are_addresses_equal(peer.second, *server_lobby);
  });
  server_lobby.reset();
}

bool is_lobby_peer(const game::net::netadr_t &address) {
  std::scoped_lock lock(peers_mutex);
  return std::ranges::any_of(
      lobby_peers, [&address](lobbyPeerMap_t::value_type &peer) {
        return network::are_addresses_equal(peer.second, address);
      });
}

std::vector<game::net::netadr_t> get_lobby_peers() {
  std::scoped_lock lock(peers_mutex);
  std::vector<game::net::netadr_t> peers;
  for (const auto &[key, endpoint] : lobby_peers) {
    peers.push_back(endpoint);
  }
  return peers;
}

bool is_rendezvous(const game::net::netadr_t &sender) {
  const std::vector<game::net::netadr_t> &masters =
      server_list::get_master_servers();
  return std::ranges::any_of(
      masters, [&sender](const game::net::netadr_t &master) {
        return sender.addr == master.addr && sender.port == master.port;
      });
}

void send_invite(const uint64_t steam_id, const std::string &sender_name) {
  if constexpr (!xerces_switches::ezz_social) {
    return; // Xerces (D14): invites went through Ezz's masters
  }
  const game::XUID friend_code = auth::get_guid();
  if (!friend_code || !steam_id)
    return;
  const std::string payload =
      std::format("1 {} {} {}", steam_id, friend_code, sender_name);
  for (const game::net::netadr_t &master : server_list::get_master_servers())
    network::send(master, "friendInvite", payload);
}

bool set_open_to_friends(const bool enabled) {
  // Xerces (D14): friends no longer find an open party through Ezz's
  // masters; opening it lets others join by address (direct connect), and
  // update_hosting sends nothing.
  if (enabled && !can_host())
    return false;
  set_open(enabled);
  update_hosting();
  return true;
}

bool set_open_to_direct_connect(const bool enabled) {
  const std::string mapped = forwarded_endpoint();
  if (!set_open_to_friends(enabled)) {
    toast::warn("Direct connect",
                "Only the host of a lobby can open it. Open a Zombies or "
                "Multiplayer lobby first.");
    return false;
  }
  show_notice(xerces_core::direct_connect_notice(
                  enabled ? xerces_core::port_map_result::pending
                          : xerces_core::port_map_result::closed,
                  mapped, local_port()),
              !enabled);
  return true;
}

void refresh_friends(const std::vector<game::XUID> &steam_ids,
                     lookup_callback callback) {
  std::unordered_set<game::XUID> pending;
  for (const game::XUID steam_id : steam_ids) {
    if (steam_id)
      pending.insert(steam_id);
  }
  // Xerces (D14): presence came from Ezz's masters, so every friend is
  // reported offline right away instead of after a two-second wait.
  if (pending.empty() || !xerces_switches::ezz_social) {
    scheduler::once(
        [done = std::move(callback)]() mutable {
          if (done)
            done(std::vector<friend_presence>{});
        },
        scheduler::main);
    return;
  }

  std::string request;
  {
    std::scoped_lock lock(lookup_mutex);
    do {
      request = new_token();
    } while (lookup_requests.contains(request));
    lookup_requests.emplace(request,
                            lookup_state{pending, {}, std::move(callback)});
  }
  for (const game::XUID steam_id : pending)
    send_rendezvous("friendLookup",
                    utils::string::va("1 %s %llu", request.c_str(), steam_id));

  scheduler::once([request] { finish_lookup(request); }, scheduler::main, 2s);
}

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("nat");

public:
  void post_unpack() override {
    if (game::is_legacy_client()) {
      return;
    }

    dw_common_addr_to_netadr_hook.create(game::dw::dwCommonAddrToNetadr.get(),
                                         dw_common_addr_to_netadr_stub);
    dw_get_connection_status_hook.create(game::dw::dwGetConnectionStatus.get(),
                                         dw_get_connection_status_stub);
    network::on("lobbyJoin", receive_lobby_join);

    if (game::is_server()) {
      return;
    }

    scheduler::once(
        [] {
          game::register_dvar_bool("nat_open", false, game::dvarFlags_e::NONE,
                                   "Allow friends to join this match");
        },
        scheduler::main);

    network::on("privRegisterAck", receive_register_ack);
    network::on("privPeer", receive_peer);
    network::on("privReject", receive_rejection);
    network::on("punch", receive_punch);
    network::on("punchAck", receive_punch_ack);
    network::on("friendPresence", receive_friend_presence);
    network::on("friendOffline", receive_friend_offline);
    network::on("friendOnline", receive_friend_online);
    network::on("lobbyHost", receive_lobby_host);
    network::on("lobbyGame", receive_lobby_game);
    network::on("lobbyFollow", receive_lobby_follow);
    network::on("lobbyDenied", receive_lobby_denied);

    scheduler::loop(update_punching, scheduler::main, 250ms);
    scheduler::loop(update_hosting, scheduler::main, REGISTER_INTERVAL);
  }

  void pre_destroy() override {
    std::scoped_lock lock(lookup_mutex);
    lookup_requests.clear();
  }
};
} // namespace nat

REGISTER_COMPONENT(nat::component)
