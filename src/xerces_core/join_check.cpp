#include "join_check.hpp"

#include <algorithm>
#include <charconv>

#include "json.hpp"

namespace xerces_core {
namespace {
constexpr std::size_t short_id_length = 16;
constexpr std::size_t max_key_length = 64;

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

bool all_digits(std::string_view text) {
  return !text.empty() && text.size() <= 20 &&
         std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

// A key the list can carry: no separator of the list or of the lobby
// content, no space, no control character.
bool valid_key(std::string_view key) {
  return !key.empty() && key.size() <= max_key_length &&
         std::ranges::none_of(key, [](char c) {
           const auto u = static_cast<unsigned char>(c);
           return u <= ' ' || u == 0x7F || c == ',' || c == '=' || c == ':' ||
                  c == '\\' || c == '"';
         });
}

bool valid_id(std::string_view id) {
  return id.size() == short_id_length && std::ranges::all_of(id, is_hex);
}

std::string trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> out;
  while (true) {
    const std::size_t at = text.find(separator);
    out.push_back(text.substr(0, at));
    if (at == std::string_view::npos) {
      return out;
    }
    text.remove_prefix(at + 1);
  }
}

// Text a host sent, made safe to show: printable ASCII only (the game's
// popups read '^' as a colour code), at most max bytes.
std::string shown(std::string_view text, std::size_t max = 48) {
  std::string out;
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (u < ' ' || u >= 0x7F || c == '^') {
      continue;
    }
    if (out.size() == max) {
      out += "...";
      break;
    }
    out.push_back(c);
  }
  return out;
}

std::string describe(const join_item &entry) {
  const host_item &item = entry.item;
  const std::string what = item.kind == host_item_kind::map ? "Map: " : "Mod: ";
  // A map's name says more than its Workshop id; a mod's key often is it.
  const std::string name = shown(
      item.name.empty() && item.key != item.workshop_id ? item.key : item.name);
  const std::string key = shown(item.key);
  if (!item.workshop_id.empty()) {
    return what + (name.empty()
                       ? "Workshop item " + item.workshop_id
                       : name + " (Workshop item " + item.workshop_id + ")");
  }
  return what + (name.empty() || name == key ? key : name + " (" + key + ")") +
         ", not a Workshop item";
}

// One line per item, at most max_listed_items, then how many more.
std::string list(const std::vector<join_item> &items) {
  std::string out;
  for (std::size_t i = 0; i < items.size() && i < max_listed_items; ++i) {
    out += "- " + describe(items[i]) + "\n";
  }
  if (items.size() > max_listed_items) {
    out +=
        "- and " + std::to_string(items.size() - max_listed_items) + " more\n";
  }
  return out;
}
} // namespace

std::string short_content_id(std::string_view content_hash) {
  constexpr std::string_view tag = "sha256:";
  if (!content_hash.starts_with(tag)) {
    return {};
  }
  content_hash.remove_prefix(tag.size());
  if (content_hash.size() != 64 || !std::ranges::all_of(content_hash, is_hex)) {
    return {};
  }
  return std::string(content_hash.substr(0, short_id_length));
}

std::string format_content_ids(const std::vector<content_id> &ids) {
  std::string out;
  std::size_t count = 0;
  for (const auto &[key, id] : ids) {
    if (!valid_key(key) || !valid_id(id) || count == max_content_ids) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out += key + "=" + id;
    ++count;
  }
  return out;
}

std::vector<content_id> parse_content_ids(std::string_view text) {
  std::vector<content_id> out;
  if (text.empty()) {
    return out;
  }
  for (const std::string_view part : split(text, ',')) {
    if (out.size() == max_content_ids) {
      break;
    }
    const std::size_t eq = part.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    const std::string key = trim(part.substr(0, eq));
    std::string id = trim(part.substr(eq + 1));
    std::ranges::transform(id, id.begin(), lower);
    if (!valid_key(key) || !valid_id(id) ||
        std::ranges::any_of(
            out, [&](const content_id &e) { return e.key == key; })) {
      continue;
    }
    out.push_back({key, std::move(id)});
  }
  return out;
}

std::optional<std::string> find_content_id(const std::vector<content_id> &ids,
                                           std::string_view key) {
  for (const auto &entry : ids) {
    if (entry.key == key) {
      return entry.id;
    }
  }
  return std::nullopt;
}

std::string
installed_index_to_json(const std::vector<installed_content> &catalog) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "schema", "xerces.installed/1", alloc);
  rapidjson::Value items(rapidjson::kArrayType);
  for (const auto &content : catalog) {
    const std::string id = short_content_id(content.content_hash);
    if (content.folder.empty() || id.empty()) {
      continue;
    }
    rapidjson::Value item(rapidjson::kObjectType);
    json::set(item, "folder", content.folder, alloc);
    json::set(item, "id", content.info.id, alloc);
    json::set(item, "name", json::valid_utf8(content.info.name, 200), alloc);
    json::set(item, "kind", to_string(content.info.kind), alloc);
    json::set(item, "content_id", id, alloc);
    items.PushBack(item, alloc);
  }
  json::set(doc, "items", std::move(items), alloc);
  return json::canonical(doc);
}

std::vector<installed_entry> parse_installed_index(std::string_view text) {
  std::vector<installed_entry> out;
  const auto parsed = json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject() ||
      json::get_string(parsed.document, "schema").value_or("") !=
          "xerces.installed/1") {
    return out;
  }
  const auto *items = json::member(parsed.document, "items");
  if (items == nullptr || !items->IsArray()) {
    return out;
  }
  for (const auto &item : items->GetArray()) {
    if (!item.IsObject()) {
      continue;
    }
    installed_entry entry;
    entry.folder = json::get_string(item, "folder").value_or("");
    entry.id = json::get_string(item, "id").value_or("");
    entry.name = json::get_string(item, "name").value_or("");
    entry.kind = json::get_string(item, "kind").value_or("");
    entry.content_id = json::get_string(item, "content_id").value_or("");
    if (entry.folder.empty() || !valid_id(entry.content_id)) {
      continue;
    }
    out.push_back(std::move(entry));
  }
  return out;
}

const installed_entry *find_installed(const std::vector<installed_entry> &index,
                                      std::string_view folder) {
  const auto same = [folder](const std::string &other) {
    return other.size() == folder.size() &&
           std::ranges::equal(other, folder, [](char a, char b) {
             return lower(a) == lower(b);
           });
  };
  for (const auto &entry : index) {
    if (same(entry.folder)) {
      return &entry;
    }
  }
  return nullptr;
}

std::vector<host_item> host_items(std::string_view main_mod,
                                  std::string_view main_title,
                                  std::string_view stack, std::string_view map,
                                  std::string_view map_workshop_id) {
  std::vector<host_item> out;
  const std::string main = trim(main_mod);
  if (!main.empty() && main != "usermaps") {
    host_item item;
    item.kind = host_item_kind::main_mod;
    item.key = main;
    item.workshop_id = all_digits(main) ? main : std::string();
    if (const std::string title = trim(main_title); title != main) {
      item.name = title;
    }
    out.push_back(std::move(item));
  }
  for (const std::string_view part : split(stack, ',')) {
    if (out.size() == max_host_mods) {
      break;
    }
    const std::string id = trim(part);
    if (id.empty() || id == main ||
        std::ranges::any_of(out,
                            [&](const host_item &e) { return e.key == id; })) {
      continue;
    }
    host_item item;
    item.kind = host_item_kind::stacked_mod;
    item.key = id;
    item.workshop_id = all_digits(id) ? id : std::string();
    out.push_back(std::move(item));
  }
  const std::string name = trim(map);
  if (!name.empty()) {
    const std::string workshop = trim(map_workshop_id);
    host_item item;
    item.kind = host_item_kind::map;
    item.key = name;
    item.workshop_id = all_digits(name)       ? name
                       : all_digits(workshop) ? workshop
                                              : std::string();
    if (!workshop.empty() && workshop != name) {
      item.other_keys.push_back(workshop);
    }
    out.push_back(std::move(item));
  }
  return out;
}

std::string join_offer_key(std::string_view main_mod, std::string_view stack,
                           std::string_view map, std::string_view content_ids) {
  std::string out;
  for (const host_item &item : host_items(main_mod, {}, stack, map, {})) {
    out += item.kind == host_item_kind::main_mod      ? "main:"
           : item.kind == host_item_kind::stacked_mod ? "mod:"
                                                      : "map:";
    out += item.key + ",";
  }
  return out + "|" + format_content_ids(parse_content_ids(content_ids));
}

join_check check_join(const std::vector<host_item> &items,
                      const std::vector<content_id> &host_ids,
                      const own_lookup &own) {
  join_check out;
  for (const host_item &item : items) {
    join_item entry;
    entry.item = item;
    std::optional<std::string> host_id = find_content_id(host_ids, item.key);
    for (const auto &key : item.other_keys) {
      if (!host_id) {
        host_id = find_content_id(host_ids, key);
      }
    }
    entry.host_id = host_id.value_or("");
    const own_item mine = own ? own(item) : own_item{};
    if (!mine.name.empty()) {
      entry.item.name = mine.name;
    }
    if (!mine.installed) {
      out.missing.push_back(std::move(entry));
      continue;
    }
    entry.own_id = mine.content_id;
    if (!entry.host_id.empty() && !entry.own_id.empty() &&
        entry.host_id != entry.own_id) {
      out.differ.push_back(std::move(entry));
    }
  }
  return out;
}

join_notice missing_notice(const join_check &check) {
  join_notice out;
  if (check.missing.empty()) {
    return out;
  }
  std::size_t maps = 0;
  std::size_t without_page = 0;
  std::vector<std::string> pages;
  for (const auto &entry : check.missing) {
    if (entry.item.kind == host_item_kind::map) {
      ++maps;
    }
    if (entry.item.workshop_id.empty()) {
      ++without_page;
    } else if (std::ranges::find(pages, entry.item.workshop_id) ==
               pages.end()) {
      pages.push_back(entry.item.workshop_id);
    }
  }
  out.workshop_pages.assign(pages.begin(),
                            pages.begin() +
                                std::min(pages.size(), max_workshop_pages));
  const std::size_t mods = check.missing.size() - maps;
  out.title = mods && maps ? "Mods and map not installed"
              : maps       ? "Map not installed"
              : mods == 1  ? "Mod not installed"
                           : "Mods not installed";
  const std::string what = mods && maps ? "mods and a map"
                           : maps       ? "a map"
                           : mods == 1  ? "a mod"
                                        : "mods";
  out.message =
      "This server uses " + what + " you don't have:\n" + list(check.missing);
  if (pages.empty()) {
    out.message += "\nAsk the host where to get " +
                   std::string(check.missing.size() == 1 ? "it" : "them") +
                   ", then join again.";
    return out;
  }
  out.workshop_ids = pages;
  out.message +=
      "\nSteam can subscribe you to " +
      std::string(pages.size() == 1 ? "it" : "them") + " and download " +
      std::string(pages.size() == 1 ? "it" : "them") + "; join again once " +
      std::string(pages.size() == 1 ? "it is" : "they are") + " here.";
  if (without_page) {
    out.message += without_page == 1
                       ? " Ask the host where to get the one that is not a "
                         "Workshop item."
                       : " Ask the host where to get the ones that are not "
                         "Workshop items.";
  }
  out.message += pages.size() == 1
                     ? "\n\nGet it through Steam now?"
                     : "\n\nGet the " + std::to_string(pages.size()) +
                           " Workshop items through Steam now?";
  return out;
}

join_notice differ_notice(const join_check &check) {
  join_notice out;
  if (check.differ.empty()) {
    return out;
  }
  out.title = "Different versions";
  out.message =
      (check.differ.size() == 1 ? "Your copy differs from the host's:\n"
                                : "Your copies differ from the host's:\n") +
      list(check.differ);
  out.message +=
      "\nOne of you has another version. Steam updates subscribed items while "
      "the game is closed: both of you quit, let Steam finish, then try "
      "again. Joining with different files can fail or play "
      "differently.\n\nJoin anyway?";
  return out;
}

std::string xerces_version_warning(std::string_view host_version,
                                   std::string_view own_version) {
  if (host_version.empty() || host_version == own_version) {
    return {};
  }
  const bool plain = host_version.size() <= 32 &&
                     std::ranges::all_of(host_version, [](char c) {
                       return (c >= '0' && c <= '9') ||
                              (c >= 'a' && c <= 'z') ||
                              (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ||
                              c == '+' || c == '_';
                     });
  return "The host runs Xerces " +
         (plain ? std::string(host_version) : std::string("of another kind")) +
         ", you run " + std::string(own_version) +
         ". If joining fails or the game behaves oddly, both of you should "
         "use the same Xerces build.";
}

std::string lobby_failure_message(lobby_failure failure, std::uint32_t code) {
  switch (failure) {
  case lobby_failure::full:
    return "The host's lobby is full.";
  case lobby_failure::busy:
    return "The host's lobby is busy starting or changing a match. Try again "
           "in a few seconds.";
  case lobby_failure::closed:
    return "The host's lobby is closed. Ask them to open it with the DIRECT "
           "CONNECT button in their lobby, then join again.";
  case lobby_failure::network_mode:
    return "One of you is in LAN or offline mode. Both of you need to be in "
           "online play.";
  case lobby_failure::version:
    return "Your game doesn't match the host's. Both of you need the same "
           "Xerces build and the same Black Ops III update.";
  case lobby_failure::in_progress:
    return "Already joining a lobby. Please wait.";
  case lobby_failure::refused:
    return "The host's lobby refused the join. Try again in a few seconds.";
  case lobby_failure::other:
    break;
  }
  return "The host's lobby stopped responding while joining (code " +
         std::to_string(code) +
         "). Try again. If it keeps failing, a firewall is blocking UDP "
         "traffic: allow boiii.exe through Windows Firewall on both PCs.";
}

std::string unreachable_message(std::uint16_t port) {
  const std::string udp = "UDP port " + std::to_string(port);
  return "Could not reach the host.\n\nCheck the address and port, and that "
         "the host is in a match or has opened their lobby with its DIRECT "
         "CONNECT button. Over the internet their router has to let " +
         udp +
         " through: their game asks it to with UPnP while the lobby is open, "
         "and a router with UPnP off needs " +
         udp +
         " forwarded to their PC. Both of you allow boiii.exe through Windows "
         "Firewall. Swapping who hosts also works.";
}

std::string host_in_menus_message() {
  return "They are in the menus and their lobby is closed. Ask them to open "
         "it with the DIRECT CONNECT button in their lobby, then join again.";
}

std::string missing_map_message(std::string_view map,
                                std::string_view workshop_id) {
  const std::string name = shown(map, 64);
  if (all_digits(workshop_id)) {
    return "You don't have the map " + name + " (Workshop item " +
           std::string(workshop_id) +
           "). Subscribe to it on the Steam Workshop; Steam downloads it. "
           "Then join again.";
  }
  return "You don't have the map " + name +
         ". Ask the host for its Workshop page, subscribe to it in Steam, "
         "then join again.";
}

ipv4_scope classify_ipv4(std::string_view address) {
  std::uint32_t parts[4] = {};
  for (int i = 0; i < 4; ++i) {
    const std::size_t dot = address.find('.');
    const std::string_view part = i < 3 ? address.substr(0, dot) : address;
    if ((i < 3 && dot == std::string_view::npos) || part.empty() ||
        part.size() > 3 || !all_digits(part)) {
      return ipv4_scope::invalid;
    }
    std::from_chars(part.data(), part.data() + part.size(), parts[i]);
    if (parts[i] > 255) {
      return ipv4_scope::invalid;
    }
    if (i < 3) {
      address.remove_prefix(dot + 1);
    }
  }
  const std::uint32_t a = parts[0], b = parts[1];
  if (a == 127 || a == 0) {
    return ipv4_scope::loopback;
  }
  if (a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
      (a == 169 && b == 254)) {
    return ipv4_scope::private_net;
  }
  if (a == 100 && b >= 64 && b <= 127) {
    return ipv4_scope::shared;
  }
  return ipv4_scope::public_net;
}

mapped_reach classify_mapped(std::string_view external_ip) {
  switch (classify_ipv4(external_ip)) {
  case ipv4_scope::public_net:
    return mapped_reach::public_address;
  case ipv4_scope::shared:
    return mapped_reach::shared;
  case ipv4_scope::private_net:
    return mapped_reach::private_address;
  case ipv4_scope::invalid:
  case ipv4_scope::loopback: // 0.0.0.0 and 127/8 are no outside address
    break;
  }
  return mapped_reach::unknown;
}

join_notice direct_connect_notice(port_map_result result,
                                  std::string_view external_ip,
                                  std::uint16_t port) {
  const std::string udp = "UDP port " + std::to_string(port);
  const std::string ip = shown(external_ip, 15);
  join_notice out;
  switch (result) {
  case port_map_result::pending:
    out.title = "Open to direct connect";
    out.message = "Others can join with your address and port " +
                  std::to_string(port) + ". Asking your router to forward " +
                  udp + " (UPnP)...";
    return out;
  case port_map_result::mapped:
    switch (classify_mapped(external_ip)) {
    case mapped_reach::public_address:
      out.title = "Router port open";
      out.message = "Players on the internet join with " + ip + ":" +
                    std::to_string(port) +
                    ". The port closes when you close the lobby or quit.";
      return out;
    case mapped_reach::unknown:
      out.title = "Router port open";
      out.message = "Your router forwards " + udp +
                    " but did not say its internet address. Players on the "
                    "internet join with your public IP address and port " +
                    std::to_string(port) +
                    " (a web search for \"what is my IP\" shows it). The port "
                    "closes when you close the lobby or quit.";
      return out;
    case mapped_reach::shared:
      out.title = "Router port open, address shared";
      out.message = "Your router's internet address (" + ip +
                    ") is shared with other customers of your provider, so "
                    "players on the internet probably cannot reach you. "
                    "Players on your network can. Let someone else host.";
      return out;
    case mapped_reach::private_address:
      out.title = "Router port open, another router in front";
      out.message = "Your router's outside address (" + ip +
                    ") is not public: another router is in front of it. "
                    "Players on your network can join. For the internet, "
                    "forward " +
                    udp + " on that router too, or let someone else host.";
      return out;
    }
    return out;
  case port_map_result::no_router:
    out.title = "No UPnP router found";
    out.message = "Players on your network can join with your address and "
                  "port " +
                  std::to_string(port) +
                  ". For the internet, turn UPnP on in your router or "
                  "forward " +
                  udp + " to this PC.";
    return out;
  case port_map_result::refused:
    out.title = "Router refused the port";
    out.message = "Your router did not forward " + udp +
                  " (another PC may have it). Players on your network can "
                  "join. For the internet, forward the port by hand, or let "
                  "someone else host.";
    return out;
  case port_map_result::closed:
    out.title = "Closed to direct connect";
    out.message = "Others can no longer join your lobby from the menus.";
    if (!ip.empty()) {
      out.message += " Your router no longer forwards " + udp + ".";
    }
    return out;
  }
  return out;
}

std::vector<std::string>
joined_stack_ids(const std::vector<std::string> &folders,
                 const std::vector<installed_entry> &index) {
  std::vector<std::string> out;
  for (const auto &folder : folders) {
    if (folder.empty()) {
      continue;
    }
    const installed_entry *entry = find_installed(index, folder);
    out.push_back(entry && !entry->id.empty() ? entry->id : folder);
  }
  return out;
}

culprit_test_end
culprit_test_result(const std::optional<launch_journal> &journal) {
  if (journal_ran_host_mods(journal)) {
    return culprit_test_end::not_counted;
  }
  return journal_crashed(journal) ? culprit_test_end::crashed
                                  : culprit_test_end::ran_fine;
}
} // namespace xerces_core
