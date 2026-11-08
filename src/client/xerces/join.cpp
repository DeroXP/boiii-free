#include <std_include.hpp>

// Joining another player's self-hosted game (join.hpp, docs/DESIGN.md 11.10).
// What this PC has comes from the engine's mod and map pools (installed or
// not, and the item's folder) and from <home>\config\installed.json, which
// the launcher writes at each launch (content ids). Workshop ids and content
// ids go into the game log; addresses never do (DESIGN.md 15.1).

#include "join.hpp"

#include <component/download_overlay.hpp>
#include <component/mod_stack.hpp>
#include <component/scheduler.hpp>
#include <component/workshop/workshop.hpp>

#include <game/game.hpp>
#include <game/impl/ugc/ugc.hpp>

#include <utils/string.hpp>

#include <xerces_core/join_check.hpp>
#include <xerces_core/json.hpp>

#include "runtime.hpp"

namespace xerces_join {
namespace {
using xerces_core::host_item;
using xerces_core::host_item_kind;
using xerces_core::installed_entry;

// The index is read again at most every few seconds: getinfo answers every
// server browser that asks.
constexpr std::chrono::seconds index_reread = 5s;
// How long a "Join anyway" holds for the same offer.
constexpr std::chrono::seconds accepted_for = 60s;
// Workshop pages open one after another, so each request reaches Steam.
// Steam shows them in its one browser view; whether Back reaches the earlier
// ones is a check in docs/TWO_PC_TEST.md (test 2). The dialog names the
// items by Workshop id too, so the player can find each one in Steam.
constexpr std::chrono::milliseconds page_gap = 1500ms;

std::mutex index_mutex;
std::vector<installed_entry> index_cache;
std::chrono::steady_clock::time_point index_time{};
bool index_read = false;

std::vector<installed_entry> installed_index() {
  // A game started some other way may find an index from an older launch;
  // without one the join checks fall back to "installed or not".
  if (!xerces_runtime::started_by_launcher()) {
    return {};
  }
  std::lock_guard lock(index_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (!index_read || now - index_time >= index_reread) {
    index_cache = xerces_core::parse_installed_index(xerces_runtime::read_file(
        xerces_runtime::home() / "config" /
        std::string(xerces_core::installed_index_file)));
    index_time = now;
    index_read = true;
  }
  return index_cache;
}

// The engine's entry for a mod or map, by publisher id or internal name.
const game::ugc::UGCItem *find_item(const bool map,
                                    const std::vector<std::string> &keys) {
  const auto &pool = map ? game::ugc::usermapsPool : game::ugc::modsPool;
  for (uint32_t i = 0; i < pool.count; ++i) {
    const game::ugc::UGCItem *item = &pool.data[i];
    for (const std::string &key : keys) {
      if (!key.empty() &&
          (item->publisherId == key || item->internalName == key)) {
        return item;
      }
    }
  }
  return nullptr;
}

// The item's folder: the one holding its zone files, or their parent when
// they sit in zone\ (mods built with the Mod Tools).
std::string item_folder(const game::ugc::UGCItem &item) {
  const char *zone_files = item.absolutePathZoneFiles;
  std::filesystem::path path(zone_files ? zone_files : "");
  path = path.lexically_normal();
  if (!path.has_filename()) {
    path = path.parent_path();
  }
  if (utils::string::to_lower(path.filename().string()) == "zone") {
    path = path.parent_path();
  }
  return path.filename().string();
}

xerces_core::own_item own_copy(const host_item &item,
                               const std::vector<installed_entry> &index) {
  std::vector<std::string> keys{item.key};
  keys.insert(keys.end(), item.other_keys.begin(), item.other_keys.end());
  const game::ugc::UGCItem *found =
      find_item(item.kind == host_item_kind::map, keys);
  if (!found) {
    return {};
  }
  xerces_core::own_item out;
  out.installed = true;
  const char *title = found->title;
  out.name = title ? title : "";
  const installed_entry *entry =
      xerces_core::find_installed(index, item_folder(*found));
  for (std::size_t i = 0; !entry && i < keys.size(); ++i) {
    entry = xerces_core::find_installed(index, keys[i]);
  }
  if (entry) {
    out.content_id = entry->content_id;
  }
  return out;
}

std::string keys_json(const std::vector<xerces_core::join_item> &items) {
  std::vector<std::string> keys;
  for (const auto &entry : items) {
    keys.push_back(xerces_core::json::valid_utf8(entry.item.key, 64));
  }
  std::string out = "[";
  for (std::size_t i = 0; i < keys.size(); ++i) {
    out += (i ? "," : "") + xerces_core::json::quote(keys[i]);
  }
  return out + "]";
}

void show_error(const std::string &message) {
  scheduler::once(
      [message] {
        game::ui::UI_OpenErrorPopupWithMessage(
            game::LOCAL_CLIENT_0, game::errorCode::UI, message.c_str());
      },
      scheduler::main);
}

std::mutex accepted_mutex;
std::string accepted_offer;
std::chrono::steady_clock::time_point accepted_time{};

// The same for getinfo's and the lobby's view of one host (join_check.hpp).
std::string offer_key(const host_offer &offer) {
  return xerces_core::join_offer_key(offer.main_mod, offer.stack, offer.map,
                                     offer.content_ids);
}

bool accepted_recently(const std::string &key) {
  std::lock_guard lock(accepted_mutex);
  return key == accepted_offer &&
         std::chrono::steady_clock::now() - accepted_time < accepted_for;
}

void accept(const std::string &key) {
  std::lock_guard lock(accepted_mutex);
  accepted_offer = key;
  accepted_time = std::chrono::steady_clock::now();
}
} // namespace

void check_host(const host_offer &offer, std::function<void()> proceed) {
  const std::vector<host_item> items =
      xerces_core::host_items(offer.main_mod, offer.main_title, offer.stack,
                              offer.map, offer.map_workshop_id);
  const std::vector<installed_entry> index = installed_index();
  const xerces_core::join_check check = xerces_core::check_join(
      items, xerces_core::parse_content_ids(offer.content_ids),
      [&index](const host_item &item) { return own_copy(item, index); });

  xerces_runtime::log_info(
      "join.check",
      check.ok()               ? "the host's mods and map are here"
      : !check.missing.empty() ? "the host's content is missing here"
                               : "the host's content differs from this PC's",
      "{\"items\":" + std::to_string(items.size()) +
          ",\"host_ids\":" + (offer.content_ids.empty() ? "false" : "true") +
          ",\"index\":" + std::to_string(index.size()) +
          ",\"missing\":" + keys_json(check.missing) +
          ",\"differ\":" + keys_json(check.differ) + "}");

  if (!check.missing.empty()) {
    const xerces_core::join_notice notice = xerces_core::missing_notice(check);
    if (notice.workshop_pages.empty()) {
      show_error(notice.message);
    } else {
      // Yes: Steam subscribes to and downloads every one of them, through
      // the Workshop menu's queue (DESIGN.md 9.2); the player joins again
      // once they are here.
      std::vector<std::pair<std::string, game::ZoneType>> wanted;
      for (const auto &entry : check.missing) {
        if (!entry.item.workshop_id.empty()) {
          wanted.emplace_back(entry.item.workshop_id,
                              entry.item.kind ==
                                      xerces_core::host_item_kind::map
                                  ? game::ZoneType::USERMAP
                                  : game::ZoneType::MOD);
        }
      }
      download_overlay::show_confirmation(
          notice.title, notice.message, [wanted] {
            for (const auto &[id, type] : wanted) {
              workshop::queue::add(id, type);
            }
            xerces_runtime::log_info("join.missing.steam",
                                     "Steam is asked for the host's content");
          });
    }
    return;
  }

  if (!check.differ.empty()) {
    const std::string key = offer_key(offer);
    if (accepted_recently(key)) {
      proceed();
      return;
    }
    const xerces_core::join_notice notice = xerces_core::differ_notice(check);
    download_overlay::show_confirmation(
        notice.title, notice.message, [key, proceed] {
          accept(key);
          xerces_runtime::log_info("join.differ.accepted",
                                   "joining with different files");
          scheduler::once(proceed, scheduler::main);
        });
    return;
  }

  proceed();
}

std::string own_content_ids(std::string_view map,
                            std::string_view map_workshop_id) {
  const std::vector<installed_entry> index = installed_index();
  if (index.empty()) {
    return {};
  }
  const std::string main = workshop::get_mod_publisher_id();
  const std::vector<host_item> items = xerces_core::host_items(
      main, {}, utils::string::join(mod_stack::get(), ","), map,
      map_workshop_id);
  std::vector<xerces_core::content_id> ids;
  for (const host_item &item : items) {
    const xerces_core::own_item own = own_copy(item, index);
    if (!own.content_id.empty()) {
      ids.push_back({item.key, own.content_id});
    }
  }
  return xerces_core::format_content_ids(ids);
}

void note_joined(std::string_view main_mod, std::string_view stack) {
  std::vector<std::string> folders;
  if (main_mod != "usermaps") {
    folders.emplace_back(main_mod);
  }
  for (std::string id : utils::string::split(std::string(stack), ',')) {
    utils::string::trim(id);
    if (!id.empty() && std::ranges::find(folders, id) == folders.end()) {
      folders.push_back(std::move(id));
    }
  }
  const std::vector<std::string> ids =
      xerces_core::joined_stack_ids(folders, installed_index());
  xerces_runtime::journal_joined(ids);
}
} // namespace xerces_join
