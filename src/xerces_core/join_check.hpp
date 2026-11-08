#pragma once

// Joining another player's self-hosted game (docs/DESIGN.md 11.4, 11.7, 11.10):
// what a host says about the mods and map it runs, the check of that against
// this PC, and the words the game shows. Headless and pure: the game role
// supplies what is installed (src/client/xerces/join.cpp).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "journal.hpp"
#include "planner.hpp"

namespace xerces_core {
// ------------------------------------------------------------ content ids
// An item's short content id: the first 16 hex digits of its content_hash
// ("sha256:<64 hex>", planner.hpp), or "" when the text is not such a hash.
// Enough to tell two versions apart; it proves nothing about the files.
std::string short_content_id(std::string_view content_hash);

struct content_id {
  std::string key; // what the host names the item by: a folder or Workshop id
  std::string id;  // short_content_id
};

// The host's list, "key=id,key=id", in getinfo's contentIds and as the sixth
// field of the lobby content. Hosts before 2026-11-05 send neither, and
// older clients ignore both. Keys are what getinfo already names the item by
// (modId, the modStack entries, usermapId or mapname), so a key never holds
// ',' or ':', and an id is 16 hex digits. At most max_content_ids entries.
inline constexpr std::size_t max_content_ids = 64;
std::string format_content_ids(const std::vector<content_id> &ids);
// Entries that do not parse are skipped; a key seen twice keeps its first id.
std::vector<content_id> parse_content_ids(std::string_view text);
std::optional<std::string> find_content_id(const std::vector<content_id> &ids,
                                           std::string_view key);

// ---------------------------------------------------- the installed index
// <home>\config\installed.json: every item of the launcher's catalog by its
// folder, written at each launch (launch_game), so the game can say what its
// own copy of a host's item is. It is as fresh as the plan file: a Workshop
// update after the launcher's last scan is not in it.
struct installed_entry {
  std::string folder; // what mod-stack loads: Workshop id or folder name
  std::string id;     // the manifest id (ws.<id> for legacy Workshop)
  std::string name;
  std::string kind;       // "mod" or "map"
  std::string content_id; // short_content_id of the content hash
};
inline constexpr std::string_view installed_index_file = "installed.json";
std::string
installed_index_to_json(const std::vector<installed_content> &catalog);
// Broken or foreign text gives an empty index.
std::vector<installed_entry> parse_installed_index(std::string_view text);
// By folder, ASCII letters in any case (Windows folder names).
const installed_entry *find_installed(const std::vector<installed_entry> &index,
                                      std::string_view folder);

// -------------------------------------------------------------- the check
enum class host_item_kind { main_mod, stacked_mod, map };

struct host_item {
  host_item_kind kind = host_item_kind::stacked_mod;
  std::string key;         // modId, a modStack entry, or the map's name
  std::string workshop_id; // digits when the Workshop page is known
  std::string name;        // what the host calls it, when it says
  // Other keys the host's content ids may use for it (a map is listed under
  // its Workshop id or its name).
  std::vector<std::string> other_keys;
};

// What the host runs, in load order: its main mod (none for "" and
// "usermaps"), its stacked mods (modStack, ',' separated; duplicates and the
// main mod left out), then its custom map ("" for a stock map or none). At
// most max_host_mods mods: the host sends the list, and a longer one is
// broken or hostile (the game's zone limit stops a real stack long before).
inline constexpr std::size_t max_host_mods = max_content_ids;
std::vector<host_item> host_items(std::string_view main_mod,
                                  std::string_view main_title,
                                  std::string_view stack, std::string_view map,
                                  std::string_view map_workshop_id);

// One text per offer, the same whichever way the host said it: getinfo names
// a custom map without a mod "usermaps", the lobby content "". A "Join
// anyway" holds for both checks of one join (src/client/xerces/join.cpp).
std::string join_offer_key(std::string_view main_mod, std::string_view stack,
                           std::string_view map, std::string_view content_ids);

// This PC's copy of a host item.
struct own_item {
  bool installed = false;
  std::string content_id; // "" when unknown
  std::string name;       // its title here, when known
};
using own_lookup = std::function<own_item(const host_item &)>;

struct join_item {
  host_item item;
  std::string host_id; // "" when the host did not say
  std::string own_id;
};

struct join_check {
  std::vector<join_item> missing; // not installed here
  // Installed, but both sides know the content id and the ids differ.
  std::vector<join_item> differ;
  bool ok() const { return missing.empty() && differ.empty(); }
};

join_check check_join(const std::vector<host_item> &items,
                      const std::vector<content_id> &host_ids,
                      const own_lookup &own);

// What the game shows. workshop_pages: the Workshop ids whose pages the
// "Yes" button opens in Steam; with none there is nothing to offer.
struct join_notice {
  std::string title;
  std::string message;
  std::vector<std::string> workshop_pages;
  // Every missing Workshop item, for Steam to get (DESIGN.md 9.2)
  std::vector<std::string> workshop_ids;
};

// A dialog names at most max_listed_items items, then says how many more.
// "Yes" opens at most max_workshop_pages pages.
inline constexpr std::size_t max_listed_items = 12;
inline constexpr std::size_t max_workshop_pages = 10;

// Everything missing at once, each with its Workshop page when it has one.
join_notice missing_notice(const join_check &check);
// The items whose files differ; "Yes" joins anyway.
join_notice differ_notice(const join_check &check);

// "" when both run the same Xerces version or the host does not say (BOIII
// and Xerces builds before 2026-11-05 do not).
std::string xerces_version_warning(std::string_view host_version,
                                   std::string_view own_version);

// ----------------------------------------------------------- the messages
// Why the engine's lobby join failed (nat.cpp maps its JoinResult here).
enum class lobby_failure {
  full,
  busy,
  closed,
  network_mode,
  version,
  in_progress,
  refused,
  other,
};
std::string lobby_failure_message(lobby_failure failure, std::uint32_t code);

// The host did not answer the join at all.
std::string unreachable_message(std::uint16_t port);

// The host is in the menus and its lobby is not open to direct connect.
inline constexpr std::string_view host_in_menus_title = "Host in the menus";
std::string host_in_menus_message();

// The server changed to a custom map this PC does not have.
std::string missing_map_message(std::string_view map,
                                std::string_view workshop_id);

// ------------------------------------------------- open to direct connect
// The host's side: its lobby answers joins by address while it is open, and
// the game asks the router to forward its UDP port (UPnP) meanwhile.
enum class ipv4_scope { invalid, loopback, private_net, shared, public_net };
// shared: 100.64.0.0/10, the carrier-grade NAT range; private_net: 10/8,
// 172.16/12, 192.168/16 and 169.254/16.
ipv4_scope classify_ipv4(std::string_view address);

enum class port_map_result {
  pending,   // asked the router, no answer yet
  mapped,    // the router forwards the port
  no_router, // no UPnP router answered (off, or not supported)
  refused,   // the router said no (taken by another PC, or not allowed)
  closed,    // the lobby closed and the mapping was removed
};
// What a forwarded port means for players on the internet, by the outside
// address the router reported. unknown: the router forwards the port but
// gave no usable address ("", 0.0.0.0, loopback), which says nothing about
// a second router; the host gives their public address themselves.
enum class mapped_reach { public_address, unknown, shared, private_address };
mapped_reach classify_mapped(std::string_view external_ip);

// external_ip: the router's outside address for mapped; for closed, anything
// non-empty when the router had forwarded the port.
join_notice direct_connect_notice(port_map_result result,
                                  std::string_view external_ip,
                                  std::uint16_t port);

// ------------------------------------------------------------ the journal
// The host's mods as the journal records them after a join: each folder's
// manifest id from the index when it is there, else the folder itself.
std::vector<std::string>
joined_stack_ids(const std::vector<std::string> &folders,
                 const std::vector<installed_entry> &index);

// How a culprit-search test launch ended, by its journal (journal.hpp). A
// test that joined another player's game with other mods ran the host's
// mods, not the ones under test, so its crash or clean run counts for
// nothing: the launcher records no result and the same test runs again.
enum class culprit_test_end { crashed, ran_fine, not_counted };
culprit_test_end
culprit_test_result(const std::optional<launch_journal> &journal);
} // namespace xerces_core
