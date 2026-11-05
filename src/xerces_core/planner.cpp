#include "planner.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <tuple>

#include "json.hpp"
#include "sha256.hpp"

namespace xerces_core {
namespace {
using rapidjson::Value;
using catalog_index =
    std::map<std::string, std::vector<const installed_content *>>;

constexpr std::size_t search_step_budget = 100000;

catalog_index index_catalog(const std::vector<installed_content> &catalog) {
  catalog_index index;
  for (const auto &item : catalog) {
    index[item.info.id].push_back(&item);
  }
  for (auto &[id, items] : index) {
    std::ranges::sort(items, [](const auto *a, const auto *b) {
      if (const auto c = a->info.ver <=> b->info.ver; c != 0) {
        return c > 0; // highest version first
      }
      if (a->info.ver.build != b->info.ver.build) {
        return a->info.ver.build > b->info.ver.build;
      }
      // The same version installed twice: still one fixed order (6.8).
      return std::tie(a->content_hash, a->folder, a->path) <
             std::tie(b->content_hash, b->folder, b->path);
    });
  }
  return index;
}

// Step 1 of 5.3: a version for every id the plan needs. A depth-first search
// over the installed versions, highest first. Ids are decided dependents
// first, so a mod's version is picked before its dependencies'. When the
// versions chosen so far leave an id nothing that fits, it goes back to the
// latest choice that caused that (conflict-directed backjumping), so mods that
// have nothing to do with the clash are not tried in every combination.
class version_search {
public:
  using choice = const installed_content *; // nullptr: left out

  version_search(const catalog_index &index, const plan_request &request,
                 const std::vector<std::string> &roots);

  // A choice for every id that meets every range. False when there is none,
  // or when the step budget ran out first (exhausted()).
  bool solve();
  // The same order without backtracking: what fits is kept and the rest left
  // out, so the problems can be named.
  void settle();

  bool exhausted() const { return exhausted_; }
  std::map<std::string, choice> selected() const;

private:
  enum class need { none, optional, required };
  // Positions in order_ of choices that rule something out.
  using culprits = std::set<std::size_t>;

  need need_of(const std::string &id, culprits *requirers) const;
  bool fits(const std::string &id, choice candidate, bool look_ahead,
            culprits *why) const;
  std::vector<choice> options(const std::string &id, culprits &why) const;

  std::set<std::string> roots_;
  std::map<std::string, std::vector<choice>> candidates_; // pins applied
  std::vector<std::string> order_;
  std::map<std::string, std::size_t> position_; // in order_
  std::set<std::string> revisit_; // an id decided later may depend on these
  std::map<std::string, choice> decided_;
  bool exhausted_ = false;
};

version_search::version_search(const catalog_index &index,
                               const plan_request &request,
                               const std::vector<std::string> &roots)
    : roots_(roots.begin(), roots.end()) {
  // Every id any version of a root can reach, in the order first met.
  std::vector<std::string> found;
  std::deque<std::string> queue(roots.begin(), roots.end());
  while (!queue.empty()) {
    const std::string id = queue.front();
    queue.pop_front();
    if (candidates_.contains(id)) {
      continue;
    }
    auto &list = candidates_[id];
    if (const auto it = index.find(id); it != index.end()) {
      const auto pin = request.pins.find(id);
      for (const auto *candidate : it->second) {
        if (pin == request.pins.end() ||
            candidate->info.ver.to_string() == pin->second) {
          list.push_back(candidate);
        }
      }
    }
    found.push_back(id);
    for (const auto *candidate : list) {
      for (const auto &dep : candidate->info.dependencies) {
        queue.push_back(dep.id);
      }
    }
  }

  // Dependents first; ties and cycles go by the order found.
  std::map<std::string, std::set<std::string>> uses;
  std::map<std::string, std::size_t> users;
  for (const auto &id : found) {
    for (const auto *candidate : candidates_[id]) {
      for (const auto &dep : candidate->info.dependencies) {
        if (dep.id != id && uses[id].insert(dep.id).second) {
          ++users[dep.id];
        }
      }
    }
  }
  std::set<std::string> placed;
  while (order_.size() < found.size()) {
    auto next = std::ranges::find_if(found, [&](const std::string &id) {
      return !placed.contains(id) && users[id] == 0;
    });
    if (next == found.end()) {
      next = std::ranges::find_if(
          found, [&](const std::string &id) { return !placed.contains(id); });
    }
    placed.insert(*next);
    position_[*next] = order_.size();
    order_.push_back(*next);
    for (const auto &dep : uses[*next]) {
      --users[dep];
    }
  }
  for (std::size_t i = 0; i < order_.size(); ++i) {
    for (std::size_t j = i + 1; j < order_.size(); ++j) {
      if (uses[order_[j]].contains(order_[i])) {
        revisit_.insert(order_[i]);
        break;
      }
    }
  }
}

// Roots are always required; other ids when a chosen mod needs them, and
// `requirers` gets those mods.
version_search::need version_search::need_of(const std::string &id,
                                             culprits *requirers) const {
  if (roots_.contains(id)) {
    return need::required;
  }
  need out = need::none;
  for (const auto &[other, chosen] : decided_) {
    if (chosen == nullptr) {
      continue;
    }
    for (const auto &dep : chosen->info.dependencies) {
      if (dep.id != id) {
        continue;
      }
      if (!dep.optional) {
        out = need::required;
        if (requirers != nullptr) {
          requirers->insert(position_.at(other));
        }
      } else if (out == need::none) {
        out = need::optional;
      }
    }
  }
  return out;
}

// Whether `candidate` agrees with every choice made so far, both ways. With
// look_ahead, also whether each dependency it needs still has a version left.
// When it does not fit, `why` gets the choices that rule it out (none when
// no other choice would help).
bool version_search::fits(const std::string &id, choice candidate,
                          bool look_ahead, culprits *why) const {
  culprits blame;
  const auto against = [&](const std::string &target, choice version,
                           culprits &out) {
    for (const auto &[other, chosen] : decided_) {
      if (chosen == nullptr) {
        continue;
      }
      for (const auto &dep : chosen->info.dependencies) {
        if (dep.id == target && !dep.range.satisfied_by(version->info.ver)) {
          out.insert(position_.at(other));
        }
      }
    }
  };
  against(id, candidate, blame);
  bool ok = blame.empty();
  for (const auto &dep : candidate->info.dependencies) {
    if (const auto it = decided_.find(dep.id); it != decided_.end()) {
      if (it->second == nullptr
              ? !dep.optional
              : !dep.range.satisfied_by(it->second->info.ver)) {
        ok = false;
        blame.insert(position_.at(dep.id));
      }
    } else if (look_ahead && !dep.optional) {
      culprits blocking;
      const bool open =
          std::ranges::any_of(candidates_.at(dep.id), [&](choice next) {
            if (!dep.range.satisfied_by(next->info.ver)) {
              return false;
            }
            culprits against_next;
            against(dep.id, next, against_next);
            blocking.insert(against_next.begin(), against_next.end());
            return against_next.empty();
          });
      if (!open) {
        ok = false;
        blame.insert(blocking.begin(), blocking.end());
      }
    }
  }
  if (!ok && why != nullptr) {
    why->insert(blame.begin(), blame.end());
  }
  return ok;
}

std::vector<version_search::choice>
version_search::options(const std::string &id, culprits &why) const {
  culprits requirers;
  const need wanted = need_of(id, &requirers);
  std::vector<choice> out;
  if (wanted == need::none) {
    out.push_back(nullptr); // nothing chosen so far asks for it
    if (!revisit_.contains(id)) {
      return out;
    }
  }
  for (const choice candidate : candidates_.at(id)) {
    if (fits(id, candidate, true, &why)) {
      out.push_back(candidate);
    }
  }
  if (wanted == need::optional) {
    out.push_back(nullptr);
  } else if (wanted == need::required) {
    why.insert(requirers.begin(), requirers.end()); // they rule out "left out"
  }
  return out;
}

bool version_search::solve() {
  decided_.clear();
  exhausted_ = false;
  if (order_.empty()) {
    return true;
  }
  // One level per id in order_: its options, the next one to try, and the
  // earlier choices that have ruled options out.
  struct level {
    std::vector<choice> options;
    std::size_t next = 0;
    culprits why;
  };
  std::vector<level> levels;
  levels.reserve(order_.size());
  const auto enter = [&] {
    level fresh;
    fresh.options = options(order_[levels.size()], fresh.why);
    levels.push_back(std::move(fresh));
  };
  enter();
  std::size_t steps = 0;
  while (true) {
    const std::size_t at = levels.size() - 1;
    if (level &top = levels.back(); top.next < top.options.size()) {
      if (++steps > search_step_budget) {
        exhausted_ = true;
        return false;
      }
      decided_[order_[at]] = top.options[top.next++];
      if (levels.size() == order_.size()) {
        return true;
      }
      enter();
      continue;
    }
    // Nothing left here: go back to the latest choice that caused it.
    culprits why = std::move(levels.back().why);
    decided_.erase(order_[at]);
    levels.pop_back();
    if (why.empty()) {
      return false; // no earlier choice could change this
    }
    const std::size_t back_to = *why.rbegin();
    while (levels.size() > back_to + 1) {
      decided_.erase(order_[levels.size() - 1]);
      levels.pop_back();
    }
    why.erase(back_to);
    levels.back().why.insert(why.begin(), why.end());
  }
}

void version_search::settle() {
  decided_.clear();
  for (const auto &id : order_) {
    choice pick = nullptr;
    if (need_of(id, nullptr) != need::none) {
      for (const choice candidate : candidates_.at(id)) {
        if (fits(id, candidate, false, nullptr)) {
          pick = candidate;
          break;
        }
      }
    }
    decided_[id] = pick;
  }
}

std::map<std::string, version_search::choice> version_search::selected() const {
  std::map<std::string, choice> out;
  for (const auto &[id, chosen] : decided_) {
    if (chosen != nullptr) {
      out[id] = chosen;
    }
  }
  return out;
}

std::string
display_name(const std::map<std::string, const installed_content *> &selected,
             const std::string &id) {
  const auto it = selected.find(id);
  if (it == selected.end() || it->second->info.name.empty()) {
    return id;
  }
  return it->second->info.name;
}

// "above" edges: key ranks above every id in its set.
class order_graph {
public:
  enum class reason { dependency, user, hint };

  explicit order_graph(std::vector<std::string> nodes)
      : nodes_(std::move(nodes)) {}

  bool reachable(const std::string &from, const std::string &to) const {
    std::vector<std::string> stack{from};
    std::set<std::string> seen;
    while (!stack.empty()) {
      const std::string current = stack.back();
      stack.pop_back();
      if (current == to) {
        return true;
      }
      if (!seen.insert(current).second) {
        continue;
      }
      if (const auto it = above_.find(current); it != above_.end()) {
        stack.insert(stack.end(), it->second.begin(), it->second.end());
      }
    }
    return false;
  }

  // False when the edge would contradict the edges already in place.
  bool add(const std::string &upper, const std::string &lower, reason why) {
    if (upper == lower || reachable(lower, upper)) {
      return false;
    }
    if (above_[upper].insert(lower).second) {
      reasons_[{upper, lower}] = why;
    }
    return true;
  }

  std::optional<reason> direct(const std::string &upper,
                               const std::string &lower) const {
    const auto it = reasons_.find({upper, lower});
    if (it == reasons_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  const std::map<std::pair<std::string, std::string>, reason> &edges() const {
    return reasons_;
  }

  // Kahn's algorithm; `better` picks among the nodes that are free to go next.
  std::vector<std::string>
  sort(const std::function<bool(const std::string &, const std::string &)>
           &better) const {
    std::map<std::string, int> blockers;
    for (const auto &node : nodes_) {
      blockers[node];
    }
    for (const auto &[upper, lowers] : above_) {
      for (const auto &lower : lowers) {
        ++blockers[lower];
      }
    }
    std::vector<std::string> out;
    std::set<std::string> placed;
    while (out.size() < nodes_.size()) {
      std::optional<std::string> pick;
      for (const auto &node : nodes_) {
        if (placed.contains(node) || blockers[node] != 0) {
          continue;
        }
        if (!pick || better(node, *pick)) {
          pick = node;
        }
      }
      if (!pick) {
        break; // only reachable with a cycle, which add() prevents
      }
      placed.insert(*pick);
      out.push_back(*pick);
      if (const auto it = above_.find(*pick); it != above_.end()) {
        for (const auto &lower : it->second) {
          --blockers[lower];
        }
      }
    }
    return out;
  }

private:
  std::vector<std::string> nodes_;
  std::map<std::string, std::set<std::string>> above_;
  std::map<std::pair<std::string, std::string>, reason> reasons_;
};

bool setting_value_fits(const setting_decl &setting, const Value &value) {
  if (setting.type == "bool") {
    return value.IsBool();
  }
  if (setting.type == "int" || setting.type == "float") {
    if (setting.type == "int" ? !value.IsInt64() : !value.IsNumber()) {
      return false;
    }
    const double number = value.GetDouble();
    return (!setting.min || number >= *setting.min) &&
           (!setting.max || number <= *setting.max);
  }
  if (setting.type == "string") {
    return value.IsString() && value.GetStringLength() <= 256;
  }
  if (setting.type == "enum") {
    return value.IsString() &&
           std::ranges::find(
               setting.options,
               std::string(value.GetString(), value.GetStringLength())) !=
               setting.options.end();
  }
  return false;
}

// The request's values over the declared defaults. A stored value the
// chosen version no longer accepts (its type or range changed in an update)
// is an error with a fix: drop it and use the default.
std::map<std::string, std::string>
resolve_settings(const installed_content &content, const plan_request &request,
                 diagnostics &diags, std::vector<fix_action> &fixes) {
  std::map<std::string, std::string> out;
  const auto &id = content.info.id;
  const auto requested = request.settings.find(id);
  for (const auto &setting : content.info.settings) {
    out[setting.key] = setting.default_json;
    if (requested == request.settings.end()) {
      continue;
    }
    const auto value = requested->second.find(setting.key);
    if (value == requested->second.end()) {
      continue;
    }
    const auto parsed = json::parse(value->second);
    if (!parsed.ok || !setting_value_fits(setting, parsed.document)) {
      diags.error("plan.setting.invalid", "/settings/" + id + "/" + setting.key,
                  "\"" + value->second + "\" is not a valid value for " +
                      content.info.name + "'s \"" + setting.label + "\".");
      fixes.push_back({"reset_setting", id, setting.key});
      continue;
    }
    out[setting.key] = json::canonical(parsed.document);
  }
  if (requested != request.settings.end()) {
    for (const auto &[key, value] : requested->second) {
      if (!out.contains(key)) {
        diags.warning("plan.setting.unknown", "/settings/" + id + "/" + key,
                      content.info.name + " has no setting \"" + key +
                          "\"; it is ignored.");
      }
    }
  }
  return out;
}

plan_entry make_entry(const installed_content &content, bool requested,
                      const plan_request &request, diagnostics &diags,
                      std::vector<fix_action> &fixes) {
  plan_entry entry;
  entry.id = content.info.id;
  entry.name = content.info.name;
  entry.ver = content.info.ver;
  entry.folder = content.folder;
  entry.path = content.path;
  entry.content_hash = content.content_hash;
  entry.requested = requested;
  entry.legacy = content.info.legacy;
  entry.zones = count_zone_files(content.files, request.mode);
  entry.zone_memory = zone_memory_of(content.zone_headers, request.mode,
                                     content.info.kind != content_kind::map)
                          .bytes;
  entry.settings = resolve_settings(content, request, diags, fixes);
  return entry;
}

// A custom map the mode loads: its manifest allows the mode and, when its
// zones' names say which mode it is for ("zm_backrooms"), it is this one.
// Workshop maps have no manifest, so their names decide.
bool map_for_mode(const installed_content &content, const std::string &mode) {
  if (content.info.kind != content_kind::map ||
      !content.info.supports_mode(mode)) {
    return false;
  }
  const std::set<std::string> modes = map_zone_modes(content.zone_headers);
  return modes.empty() || modes.contains(mode);
}

// Whole MiB for players, never "0 MiB over".
std::string whole_mib(double mib) {
  return std::to_string(static_cast<long long>(std::ceil(mib))) + " MiB";
}

// "A", "A and B", "A, B and C".
std::string join_and(const std::vector<std::string> &items) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += i + 1 == items.size() ? " and " : ", ";
    }
    out += items[i];
  }
  return out;
}

// Step 5's second half: zone memory, reported in no hash, with warnings for
// the installed maps that do not fit with the plan's mods, those that do not
// fit even with no mods and those whose headers were not all read, and an
// error when the plan's own map does not fit.
void check_zone_memory(
    plan &out, const catalog_index &index,
    const std::map<std::string, const installed_content *> &selected,
    const plan_request &request) {
  zone_memory_report &memory = out.zone_memory;
  diagnostics &diags = out.diags;
  memory.budget = request.zone_budget;
  const auto note_unread = [&](const plan_entry &entry, bool mod) {
    if (const auto it = selected.find(entry.id); it != selected.end()) {
      if (!zone_memory_of(it->second->zone_headers, request.mode, mod)
               .unread.empty()) {
        memory.unread.push_back(entry.id);
      }
    }
  };
  const plan_entry *biggest = nullptr;
  for (const auto &entry : out.stack) {
    memory.stack_bytes += entry.zone_memory;
    note_unread(entry, true);
    if (entry.zone_memory > 0 &&
        (!biggest || entry.zone_memory > biggest->zone_memory)) {
      biggest = &entry;
    }
  }
  if (out.map) {
    memory.map_bytes = out.map->zone_memory;
    note_unread(*out.map, false);
  }
  if (!request.zone_budget) {
    diags.info("plan.memory.unknown", "",
               "Xerces has no zone memory figures for this game build and "
               "mode (" +
                   request.mode +
                   "), so it cannot tell whether custom maps fit with these "
                   "mods.");
    return;
  }
  const zone_budget &budget = *request.zone_budget;
  const double stack_mib = to_mib(memory.stack_bytes);
  for (const auto &[id, items] : index) {
    // The plan's map in the version it picked, and checked whatever its
    // names say: it is the one that loads.
    const bool own = out.map && out.map->id == id;
    const installed_content *content =
        own && selected.contains(id) ? selected.at(id) : items.front();
    if (!own && !map_for_mode(*content, request.mode)) {
      continue;
    }
    const zone_memory_use use =
        zone_memory_of(content->zone_headers, request.mode,
                       content->info.kind != content_kind::map);
    const std::string name =
        content->info.name.empty() ? id : content->info.name;
    const double map_mib = to_mib(own ? out.map->zone_memory : use.bytes);
    const double need = budget.baseline_mib + stack_mib + map_mib;
    if (need > budget.pool_mib) {
      // Over without the zones not read, so over with them too.
      memory.maps_over.push_back(
          {id, name, need, need - budget.pool_mib,
           budget.baseline_mib + map_mib > budget.pool_mib});
    } else if (!use.unread.empty()) {
      // It takes more than map_mib, by how much is unknown.
      memory.maps_unread.push_back({id, name});
    }
  }
  std::ranges::sort(memory.maps_over, [](const auto &a, const auto &b) {
    if (a.over_by_mib != b.over_by_mib) {
      return a.over_by_mib > b.over_by_mib;
    }
    return a.id < b.id;
  });

  const std::string most =
      biggest ? " The mod that takes the most is " + biggest->name + " (" +
                    whole_mib(to_mib(biggest->zone_memory)) + ")."
              : "";
  // Maps that removing a mod would make fit, and maps too big even with no
  // mods, for which that advice would be wrong (and with no mods, every map
  // over is one of these).
  std::vector<std::string> with_mods;
  std::vector<std::string> too_big;
  for (const auto &map : memory.maps_over) {
    const std::string over = whole_mib(map.over_by_mib) + " over";
    if (out.map && map.id == out.map->id) {
      diags.error("plan.memory.over", "/map",
                  map.name + " does not fit in the game's memory" +
                      (map.over_without_mods
                           ? ", even with no mods (" + over +
                                 "): the game would close while loading it. "
                                 "Pick another map."
                           : " with these mods (" + over +
                                 "): the game would close while loading it. "
                                 "Remove a mod, or pick another map." +
                                 most));
      continue;
    }
    (map.over_without_mods ? too_big : with_mods)
        .push_back(map.name + " (" + over + ")");
  }
  if (with_mods.size() == 1) {
    diags.warning("plan.memory.maps_over", "",
                  "The map " + with_mods.front() +
                      " does not fit in the game's memory with these mods: "
                      "the game closes while loading it. Remove a mod to "
                      "play it, or play another map." +
                      most);
  } else if (!with_mods.empty()) {
    diags.warning("plan.memory.maps_over", "",
                  std::to_string(with_mods.size()) +
                      " maps do not fit in the game's memory with these "
                      "mods: " +
                      join_and(with_mods) +
                      ". The game closes while loading them. Remove a mod "
                      "to play them, or play another map." +
                      most);
  }
  if (too_big.size() == 1) {
    diags.warning("plan.memory.maps_too_big", "",
                  "The map " + too_big.front() +
                      " does not fit in the game's memory even with no "
                      "mods: the game closes while loading it. Play another "
                      "map.");
  } else if (!too_big.empty()) {
    diags.warning("plan.memory.maps_too_big", "",
                  std::to_string(too_big.size()) +
                      " maps do not fit in the game's memory even with no "
                      "mods: " +
                      join_and(too_big) +
                      ". The game closes while loading them. Play another "
                      "map.");
  }
  if (!memory.maps_unread.empty()) {
    std::vector<std::string> names;
    for (const auto &map : memory.maps_unread) {
      names.push_back(map.name);
    }
    const bool one = names.size() == 1;
    diags.warning(
        "plan.memory.maps_unread", "",
        "Xerces could not read part of " +
            (one ? "the map " : std::to_string(names.size()) + " maps, ") +
            join_and(names) + ", so it did not check whether " +
            (one ? "it fits" : "they fit") +
            " in the game's memory. If Steam is updating " +
            (one ? "it" : "them") + ", check again once Steam is done.");
  }
}

Value settings_value(const std::map<std::string, std::string> &settings,
                     const installed_content *content, bool server_only,
                     json::allocator &alloc) {
  Value object(rapidjson::kObjectType);
  for (const auto &[key, value] : settings) {
    if (server_only && content != nullptr) {
      const auto decl = std::ranges::find_if(
          content->info.settings,
          [&](const setting_decl &setting) { return setting.key == key; });
      if (decl != content->info.settings.end() && decl->scope == "client") {
        continue;
      }
    }
    auto parsed = json::parse(value);
    json::set(object, key,
              parsed.ok ? json::copy(parsed.document, alloc) : Value(), alloc);
  }
  return object;
}

Value entry_value(const plan_entry &entry, bool with_folder,
                  json::allocator &alloc) {
  Value object(rapidjson::kObjectType);
  json::set(object, "id", entry.id, alloc);
  json::set(object, "version", entry.ver.to_string(), alloc);
  json::set(object, "content_hash", entry.content_hash, alloc);
  if (with_folder) {
    json::set(object, "folder", entry.folder, alloc);
  }
  return object;
}
} // namespace

std::size_t count_zone_files(const std::vector<std::string> &files,
                             std::string_view mode) {
  // base zone -> {base file present, a language variant present}
  std::map<std::string, std::pair<bool, bool>> zones;
  for (const auto &file : files) {
    std::string name = normalize_asset_name(file);
    // Mod Tools builds keep fastfiles in zone/; Steam's Workshop folders
    // keep them at the item's root.
    if (name.starts_with("zone/")) {
      name.erase(0, 5);
    }
    if (!name.ends_with(".ff") || name.find('/') != std::string::npos) {
      continue;
    }
    name.resize(name.size() - 3);
    // "en_core_mod": the engine loads one language's copy, not all of them.
    // The mode tiers ("zm_mod", "mp_mod_patch") look the same and are not.
    const bool tier = name.starts_with("zm_mod") || name.starts_with("mp_mod") ||
                      name.starts_with("cp_mod");
    const bool language = !tier && name.size() > 3 && name[2] == '_' &&
                          std::islower(static_cast<unsigned char>(name[0])) &&
                          std::islower(static_cast<unsigned char>(name[1]));
    const std::string base = language ? name.substr(3) : name;
    // Another mode's tier (mp_mod in Zombies) is not loaded.
    if (!mode.empty()) {
      bool other_mode = false;
      for (const std::string_view tier : {"zm_mod", "mp_mod", "cp_mod"}) {
        const std::string own = std::string(mode) + "_mod";
        if (tier != own && (base == tier || base == std::string(tier) + "_patch")) {
          other_mode = true;
        }
      }
      if (other_mode) {
        continue;
      }
    }
    auto &seen = zones[base];
    (language ? seen.second : seen.first) = true;
  }
  std::size_t count = 0;
  for (const auto &[base, seen] : zones) {
    count += (seen.first ? 1 : 0) + (seen.second ? 1 : 0);
  }
  return count;
}

std::string plan::fs_game() const {
  return stack.empty() ? std::string{} : stack.front().folder;
}

std::string plan::fs_extramods() const {
  std::string out;
  for (std::size_t i = 1; i < stack.size(); ++i) {
    if (!out.empty()) {
      out.push_back(',');
    }
    out += stack[i].folder;
  }
  return out;
}

plan resolve_plan(const std::vector<installed_content> &catalog,
                  const plan_request &request) {
  plan out;
  out.mode = request.mode;
  diagnostics &diags = out.diags;
  if (!valid_mode(request.mode)) {
    diags.error("plan.mode.invalid", "/mode",
                "Modes are \"zm\", \"mp\" and \"cp\".");
  }
  const catalog_index index = index_catalog(catalog);

  // 1. Expand dependencies and pick versions.
  std::vector<std::string> roots = request.mods;
  if (request.map) {
    roots.push_back(*request.map);
  }
  version_search search(index, request, roots);
  if (!search.solve()) {
    search.settle(); // keep what fits, so the problems below can be named
  }
  if (search.exhausted()) {
    diags.error("plan.dependency.too_complex", "",
                "Trying the installed versions took more than " +
                    std::to_string(search_step_budget) +
                    " steps without finding a set that fits; the problems "
                    "below are from the first versions tried.");
  }
  const std::map<std::string, const installed_content *> selected =
      search.selected();

  // What the chosen mods ask of an id: "Name needs range, ...".
  struct asks {
    std::string text;
    std::string range; // the first range that is not optional
    bool optional_only = true;
  };
  const auto asks_of = [&](const std::string &id) {
    asks out;
    for (const auto &[other, content] : selected) {
      for (const auto &dep : content->info.dependencies) {
        if (dep.id != id) {
          continue;
        }
        out.text += (out.text.empty() ? "" : ", ") +
                    display_name(selected, other) + " needs " + dep.range_text;
        if (!dep.optional && out.optional_only) {
          out.range = dep.range_text;
          out.optional_only = false;
        }
      }
    }
    return out;
  };
  // The pinned version, when it is installed and the ranges left it out.
  const auto pin_left_out =
      [&](const std::string &id) -> std::optional<std::string> {
    const auto pin = request.pins.find(id);
    const auto found = index.find(id);
    if (pin == request.pins.end() || found == index.end() ||
        std::ranges::none_of(found->second, [&](const auto *candidate) {
          return candidate->info.ver.to_string() == pin->second;
        })) {
      return std::nullopt;
    }
    return pin->second;
  };

  const std::set<std::string> requested_ids(request.mods.begin(),
                                            request.mods.end());
  const std::set<std::string> root_ids(roots.begin(), roots.end());
  std::set<std::string> reported;
  for (const auto &id : roots) {
    if (selected.contains(id) || !reported.insert(id).second) {
      continue;
    }
    const auto pin = request.pins.find(id);
    if (!index.contains(id)) {
      diags.error("plan.mod.missing", "/mods",
                  "\"" + id + "\" is not installed.");
      out.fixes.push_back({"install", id, ""});
    } else if (const auto pinned = pin_left_out(id)) {
      const std::string text = asks_of(id).text;
      diags.error("plan.mod.version", "/mods",
                  "Version " + *pinned + " of \"" + id + "\" is pinned, but " +
                      (text.empty() ? "it does not fit the other mods" : text) +
                      ".");
      out.fixes.push_back({"unpin", id, *pinned});
    } else {
      diags.error("plan.mod.version", "/mods",
                  pin != request.pins.end()
                      ? "Version " + pin->second + " of \"" + id +
                            "\" is not installed."
                      : "No installed version of \"" + id +
                            "\" fits what the other mods need.");
      out.fixes.push_back(
          {"install", id, pin != request.pins.end() ? pin->second : ""});
    }
  }
  std::set<std::string> asked;
  for (const auto &[id, content] : selected) {
    for (const auto &dep : content->info.dependencies) {
      asked.insert(dep.id);
    }
  }
  for (const auto &id : asked) {
    if (selected.contains(id) || root_ids.contains(id)) {
      continue;
    }
    const asks need = asks_of(id);
    if (need.optional_only) {
      diags.warning("plan.dependency.optional_skipped", "",
                    "Optional dependency \"" + id + "\" is left out (" +
                        need.text + ").");
    } else if (!index.contains(id)) {
      diags.error("plan.dependency.missing", "",
                  "\"" + id + "\" is needed but not installed (" + need.text +
                      ").");
      out.fixes.push_back({"install", id, need.range});
    } else if (const auto pinned = pin_left_out(id)) {
      diags.error("plan.dependency.unsatisfiable", "",
                  "Version " + *pinned + " of \"" + id + "\" is pinned, but " +
                      need.text + ".");
      out.fixes.push_back({"unpin", id, *pinned});
    } else {
      diags.error("plan.dependency.unsatisfiable", "",
                  "No installed version of \"" + id + "\" fits (" + need.text +
                      ").");
      out.fixes.push_back({"update", id, need.range});
    }
  }

  // Maps sit outside the stack; mods must not be maps and vice versa.
  if (request.map) {
    if (const auto it = selected.find(*request.map); it != selected.end()) {
      if (it->second->info.kind != content_kind::map) {
        diags.error("plan.map.kind", "/map",
                    "\"" + *request.map + "\" is a mod, not a map.");
      } else {
        out.map = make_entry(*it->second, true, request, diags, out.fixes);
      }
    }
  }
  std::vector<std::string> nodes;
  for (const auto &[id, content] : selected) {
    if (request.map && id == *request.map) {
      continue;
    }
    if (content->info.kind == content_kind::map) {
      diags.error("plan.map.in_stack", "/mods",
                  "\"" + content->info.name +
                      "\" is a map; a plan holds at most one, as its map.");
      continue;
    }
    nodes.push_back(id);
  }

  // 2. Declared conflicts block the plan unless the user allowed them.
  for (auto a = selected.begin(); a != selected.end(); ++a) {
    for (auto b = std::next(a); b != selected.end(); ++b) {
      const auto declares = [](const installed_content *from,
                               const std::string &other) {
        return std::ranges::find_if(
            from->info.conflicts,
            [&](const conflict_ref &c) { return c.id == other; });
      };
      auto found = declares(a->second, b->first);
      const bool from_a = found != a->second->info.conflicts.end();
      if (!from_a) {
        found = declares(b->second, a->first);
        if (found == b->second->info.conflicts.end()) {
          continue;
        }
      }
      const std::string reason =
          found->reason.empty() ? "" : ": " + found->reason;
      const std::string text = display_name(selected, a->first) + " and " +
                               display_name(selected, b->first) +
                               " are declared incompatible" + reason + ".";
      if (request.allowed_conflicts.contains({a->first, b->first})) {
        diags.warning("plan.conflict.allowed", "", text + " Allowed by you.");
        out.allowed_conflicts.insert({a->first, b->first});
        continue;
      }
      diags.error("plan.conflict.declared", "", text);
      out.fixes.push_back({"remove", a->first, ""});
      out.fixes.push_back({"remove", b->first, ""});
      out.fixes.push_back({"allow_conflict", a->first, b->first});
    }
  }

  // Mode support and the Xerces version each mod asks for.
  for (const auto &[id, content] : selected) {
    if (!content->info.supports_mode(request.mode)) {
      diags.error("plan.mode.unsupported", "",
                  content->info.name + " does not support " + request.mode +
                      ".");
      out.fixes.push_back({"remove", id, ""});
    }
    if (content->info.requires_xerces &&
        !content->info.requires_xerces->satisfied_by(request.xerces_version)) {
      diags.error("plan.xerces.version", "",
                  content->info.name + " needs Xerces " +
                      content->info.requires_xerces_text + "; this is " +
                      request.xerces_version.to_string() + ".");
      out.fixes.push_back(
          {"update_xerces", id, content->info.requires_xerces_text});
    }
    if (!content->valid_manifest) {
      diags.error("plan.mod.invalid_manifest", "",
                  display_name(selected, id) +
                      "'s xerces.mod.json has errors; xerces_cli mod "
                      "validate lists them.");
      out.fixes.push_back({"remove", id, ""});
    }
  }

  // 3. Order: dependents above dependencies, then the user's order, then
  // manifest hints; ties by priority, saved position, id.
  order_graph graph(nodes);
  const std::set<std::string> node_set(nodes.begin(), nodes.end());
  for (const auto &id : nodes) {
    for (const auto &dep : selected.at(id)->info.dependencies) {
      if (!node_set.contains(dep.id)) {
        continue;
      }
      if (!graph.add(id, dep.id, order_graph::reason::dependency)) {
        diags.error("plan.dependency.cycle", "",
                    display_name(selected, id) + " and " +
                        display_name(selected, dep.id) +
                        " depend on each other.");
      }
    }
  }
  std::vector<std::string> user_order;
  for (const auto &id : request.mods) {
    if (node_set.contains(id) &&
        std::ranges::find(user_order, id) == user_order.end()) {
      user_order.push_back(id);
    }
  }
  for (std::size_t i = 0; i < user_order.size(); ++i) {
    for (std::size_t j = i + 1; j < user_order.size(); ++j) {
      if (!graph.add(user_order[i], user_order[j], order_graph::reason::user)) {
        diags.error("plan.order.breaks_dependency", "/mods",
                    "You put " + display_name(selected, user_order[i]) +
                        " above " + display_name(selected, user_order[j]) +
                        ", but it is a dependency of it, so it must be "
                        "below.");
        out.fixes.push_back({"reorder", user_order[i], user_order[j]});
      }
    }
  }
  for (const auto &id : nodes) {
    const auto &hints = selected.at(id)->info.order;
    const auto hint = [&](const std::string &upper, const std::string &lower) {
      if (!node_set.contains(upper) || !node_set.contains(lower)) {
        return;
      }
      if (!graph.add(upper, lower, order_graph::reason::hint)) {
        diags.warning("plan.order.hint_ignored", "",
                      display_name(selected, id) + " asks for " +
                          display_name(selected, upper) + " above " +
                          display_name(selected, lower) +
                          ", which other rules overrule.");
      }
    };
    for (const auto &other : hints.after) {
      hint(id, other);
    }
    for (const auto &other : hints.before) {
      hint(other, id);
    }
  }

  const auto priority = [&](const std::string &id) {
    return selected.at(id)->info.order.priority;
  };
  const auto saved = [&](const std::string &id) {
    const auto it = request.saved_positions.find(id);
    return it == request.saved_positions.end()
               ? std::numeric_limits<std::int64_t>::max()
               : it->second;
  };
  const std::vector<std::string> order =
      graph.sort([&](const std::string &a, const std::string &b) {
        if (priority(a) != priority(b)) {
          return priority(a) > priority(b);
        }
        if (saved(a) != saved(b)) {
          return saved(a) < saved(b);
        }
        return a < b;
      });

  for (const auto &id : order) {
    out.stack.push_back(make_entry(*selected.at(id), requested_ids.contains(id),
                                   request, diags, out.fixes));
  }

  for (const auto &[edge, why] : graph.edges()) {
    const auto &[upper, lower] = edge;
    const std::string a = display_name(selected, upper);
    const std::string b = display_name(selected, lower);
    switch (why) {
    case order_graph::reason::dependency:
      out.explanations.push_back(
          {"order.dependency", upper, lower,
           a + " is above " + b + " because it depends on it."});
      break;
    case order_graph::reason::user:
      out.explanations.push_back(
          {"order.user", upper, lower,
           a + " is above " + b + " because you put it there."});
      break;
    case order_graph::reason::hint:
      out.explanations.push_back(
          {"order.hint", upper, lower,
           a + " is above " + b + " because a manifest asks for it."});
      break;
    }
  }
  for (std::size_t i = 0; i + 1 < order.size(); ++i) {
    const auto &a = order[i];
    const auto &b = order[i + 1];
    if (graph.reachable(a, b)) {
      continue;
    }
    std::string why;
    if (priority(a) != priority(b)) {
      why = "it has a higher priority (" + std::to_string(priority(a)) +
            " vs " + std::to_string(priority(b)) + ")";
    } else if (saved(a) != saved(b)) {
      why = "it came first in your last saved order";
    } else {
      why = "nothing else decides, so ids sort alphabetically";
    }
    out.explanations.push_back({"order.tie", a, b,
                                display_name(selected, a) + " is above " +
                                    display_name(selected, b) + ": " + why +
                                    "."});
  }

  // 4. mod-stack's limits on stacked mods.
  for (std::size_t i = 1; i < out.stack.size(); ++i) {
    const auto &entry = out.stack[i];
    if (entry.folder.size() > mod_stack_name_limit) {
      diags.error("plan.stack.name_too_long", "",
                  entry.name + "'s folder name \"" + entry.folder +
                      "\" is longer than " +
                      std::to_string(mod_stack_name_limit) +
                      " characters, so mod-stack cannot stack it.");
    }
    if (entry.folder.find(',') != std::string::npos) {
      diags.error("plan.stack.comma", "",
                  entry.name + "'s folder name contains a comma, which "
                               "fs_extramods cannot carry.");
    }
  }

  // 5. Zone budget.
  for (const auto &entry : out.stack) {
    out.zones_added += entry.zones;
  }
  if (out.map) {
    out.zones_added += out.map->zones;
  }
  if (request.stock_zones) {
    out.zones_total = *request.stock_zones + out.zones_added;
    if (*out.zones_total > zone_slot_limit) {
      diags.error("plan.budget.zones", "",
                  "This plan needs " + std::to_string(*out.zones_total) +
                      " zone slots (" + std::to_string(*request.stock_zones) +
                      " for the mode and map, " +
                      std::to_string(out.zones_added) +
                      " for mods); the engine has " +
                      std::to_string(zone_slot_limit) + ".");
    }
  } else if (out.zones_added > 0) {
    diags.info("plan.budget.stock_unknown", "",
               "Mods add " + std::to_string(out.zones_added) +
                   " zones; the stock count for this mode is not indexed "
                   "yet, so the 63-slot budget is not checked.");
  }

  // Zone memory: reported, in no hash.
  check_zone_memory(out, index, selected, request);

  // Asset pools (5.4 step 4): reported, in no hash. The client reads the pool
  // files of the main mod and the stacked ones when the mod loads
  // (UGC_LoadModByPublisherId_Impl), and the map's when the map loads
  // (UGC_LoadUsermapByPublisherId_Impl), so the map comes last.
  const pool_settings pool_config =
      parse_pool_settings(request.launcher_settings);
  out.pools.enabled = pool_config.enabled;
  std::vector<mod_pool_limits> stack_pools;
  const auto add_to_stack = [&](const plan_entry &entry) {
    if (const auto it = selected.find(entry.id); it != selected.end()) {
      stack_pools.push_back({entry.id, it->second->pool_limits});
      if (it->second->pool_file_unread) {
        out.pools.stack_unread.push_back(entry.id);
      }
    }
  };
  for (const auto &entry : out.stack) {
    add_to_stack(entry);
  }
  if (out.map) {
    add_to_stack(*out.map);
  }
  out.pools.stack = largest_limits(stack_pools, pool_config);
  // Every mod and map in the catalog, sorted by id so a tie names the same
  // one whatever order the catalog is in.
  std::vector<const installed_content *> pool_items;
  for (const auto &content : catalog) {
    if (!content.pool_limits.empty()) {
      pool_items.push_back(&content);
    }
    if (content.pool_file_unread) {
      out.pools.installed_unread.push_back(content.info.id);
    }
  }
  std::ranges::sort(pool_items, [](const auto *a, const auto *b) {
    return a->info.id < b->info.id;
  });
  std::vector<mod_pool_limits> installed_pools;
  for (const auto *content : pool_items) {
    installed_pools.push_back({content->info.id, content->pool_limits});
  }
  out.pools.installed = largest_limits(installed_pools, pool_config);
  // Two versions of one id may both be unread; name it once.
  std::ranges::sort(out.pools.installed_unread);
  const auto repeated = std::ranges::unique(out.pools.installed_unread);
  out.pools.installed_unread.erase(repeated.begin(), repeated.end());

  // 6. Hashes over canonical JSON.
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  rapidjson::Document content_doc(rapidjson::kObjectType);
  auto &content_alloc = content_doc.GetAllocator();
  json::set(doc, "schema", std::int64_t{1}, alloc);
  json::set(content_doc, "schema", std::int64_t{1}, content_alloc);
  json::set(doc, "mode", out.mode, alloc);
  json::set(content_doc, "mode", out.mode, content_alloc);
  Value stack(rapidjson::kArrayType);
  Value content_stack(rapidjson::kArrayType);
  Value settings(rapidjson::kObjectType);
  Value content_settings(rapidjson::kObjectType);
  const auto add_settings = [&](const plan_entry &entry) {
    const installed_content *content =
        selected.contains(entry.id) ? selected.at(entry.id) : nullptr;
    json::set(settings, entry.id,
              settings_value(entry.settings, content, false, alloc), alloc);
    json::set(content_settings, entry.id,
              settings_value(entry.settings, content, true, content_alloc),
              content_alloc);
  };
  for (const auto &entry : out.stack) {
    stack.PushBack(entry_value(entry, true, alloc), alloc);
    content_stack.PushBack(entry_value(entry, false, content_alloc),
                           content_alloc);
    add_settings(entry);
  }
  json::set(doc, "stack", std::move(stack), alloc);
  json::set(content_doc, "stack", std::move(content_stack), content_alloc);
  if (out.map) {
    json::set(doc, "map", entry_value(*out.map, true, alloc), alloc);
    json::set(content_doc, "map", entry_value(*out.map, false, content_alloc),
              content_alloc);
    add_settings(*out.map);
  } else {
    json::set(doc, "map", Value(), alloc);
    json::set(content_doc, "map", Value(), content_alloc);
  }
  json::set(doc, "settings", std::move(settings), alloc);
  json::set(content_doc, "settings", std::move(content_settings),
            content_alloc);
  json::set(doc, "game_build", request.game_build, alloc);
  json::set(content_doc, "game_build", request.game_build, content_alloc);
  json::set(doc, "xerces", request.xerces_version.to_string(), alloc);
  json::set(doc, "protocol", std::int64_t{request.protocol_version}, alloc);
  json::set(content_doc, "protocol", std::int64_t{request.protocol_version},
            content_alloc);

  out.canonical = json::canonical(doc);
  out.plan_id = sha256_tagged(out.canonical);
  out.content_id = sha256_tagged(json::canonical(content_doc));
  return out;
}

std::string plan_to_json(const plan &value, bool pretty) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "ok", value.ok(), alloc);
  json::set(doc, "mode", value.mode, alloc);
  json::set(doc, "plan_id", value.plan_id, alloc);
  json::set(doc, "content_id", value.content_id, alloc);
  json::set(doc, "fs_game", value.fs_game(), alloc);
  json::set(doc, "fs_extramods", value.fs_extramods(), alloc);

  const auto entry_json = [&](const plan_entry &entry) {
    Value object(rapidjson::kObjectType);
    json::set(object, "id", entry.id, alloc);
    json::set(object, "name", entry.name, alloc);
    json::set(object, "version", entry.ver.to_string(), alloc);
    json::set(object, "folder", entry.folder, alloc);
    json::set(object, "content_hash", entry.content_hash, alloc);
    json::set(object, "requested", entry.requested, alloc);
    json::set(object, "legacy", entry.legacy, alloc);
    json::set(object, "zones", static_cast<std::int64_t>(entry.zones), alloc);
    json::set(object, "zone_mib", Value(round_mib(to_mib(entry.zone_memory))),
              alloc);
    Value settings(rapidjson::kObjectType);
    for (const auto &[key, raw] : entry.settings) {
      auto parsed = json::parse(raw);
      json::set(settings, key,
                parsed.ok ? json::copy(parsed.document, alloc) : Value(),
                alloc);
    }
    json::set(object, "settings", std::move(settings), alloc);
    return object;
  };
  Value stack(rapidjson::kArrayType);
  for (const auto &entry : value.stack) {
    stack.PushBack(entry_json(entry), alloc);
  }
  json::set(doc, "stack", std::move(stack), alloc);
  json::set(doc, "map", value.map ? entry_json(*value.map) : Value(), alloc);

  Value zones(rapidjson::kObjectType);
  json::set(zones, "added", static_cast<std::int64_t>(value.zones_added),
            alloc);
  json::set(zones, "limit", static_cast<std::int64_t>(zone_slot_limit), alloc);
  json::set(zones, "total",
            value.zones_total
                ? Value(static_cast<std::int64_t>(*value.zones_total))
                : Value(),
            alloc);
  json::set(doc, "zones", std::move(zones), alloc);

  // Not in plan_id or content_id: the game's pool sizes depend on every
  // installed mod and map and on launcher_settings.json, not only on the
  // plan.
  const auto pool_list = [&](const std::vector<pool_need> &needs) {
    Value list(rapidjson::kArrayType);
    for (const auto &need : needs) {
      const asset_pool_info *pool = find_asset_pool(need.type);
      Value object(rapidjson::kObjectType);
      json::set(object, "type", std::int64_t{need.type}, alloc);
      json::set(object, "name", pool ? pool->name : std::string_view(), alloc);
      json::set(object, "label", pool ? pool->label : std::string_view(),
                alloc);
      json::set(object, "setting_key",
                pool ? pool->setting_key : std::string_view(), alloc);
      json::set(object, "size", std::int64_t{need.size}, alloc);
      json::set(object, "mod", need.mod, alloc);
      json::set(object, "default",
                need.default_size ? Value(std::int64_t{*need.default_size})
                                  : Value(),
                alloc);
      json::set(object, "fixed", need.fixed, alloc);
      json::set(object, "raises", need.raises, alloc);
      list.PushBack(std::move(object), alloc);
    }
    return list;
  };
  Value pools(rapidjson::kObjectType);
  json::set(pools, "enabled", value.pools.enabled, alloc);
  json::set(pools, "stack", pool_list(value.pools.stack), alloc);
  json::set(pools, "installed", pool_list(value.pools.installed), alloc);
  json::set(pools, "stack_unread",
            json::string_array(value.pools.stack_unread, alloc), alloc);
  json::set(pools, "installed_unread",
            json::string_array(value.pools.installed_unread, alloc), alloc);
  json::set(doc, "asset_pools", std::move(pools), alloc);

  // Not in plan_id or content_id either: it depends on every installed map
  // and on compat/zone_memory.json. MiB to one decimal.
  const zone_memory_report &memory = value.zone_memory;
  const auto mib_value = [](std::optional<double> mib) {
    return mib ? Value(round_mib(*mib)) : Value();
  };
  const double stack_mib = to_mib(memory.stack_bytes);
  const std::optional<double> map_mib =
      memory.map_bytes ? std::optional(to_mib(*memory.map_bytes))
                       : std::nullopt;
  Value zone_memory(rapidjson::kObjectType);
  json::set(zone_memory, "checked", memory.budget.has_value(), alloc);
  json::set(zone_memory, "pool_mib",
            mib_value(memory.budget ? std::optional(memory.budget->pool_mib)
                                    : std::nullopt),
            alloc);
  json::set(zone_memory, "baseline_mib",
            mib_value(memory.budget ? std::optional(memory.budget->baseline_mib)
                                    : std::nullopt),
            alloc);
  json::set(zone_memory, "stack_mib", mib_value(stack_mib), alloc);
  json::set(zone_memory, "map_mib", mib_value(map_mib), alloc);
  json::set(zone_memory, "total_mib",
            mib_value(memory.budget
                          ? std::optional(memory.budget->baseline_mib +
                                          stack_mib + map_mib.value_or(0))
                          : std::nullopt),
            alloc);
  Value maps_over(rapidjson::kArrayType);
  for (const auto &map : memory.maps_over) {
    Value object(rapidjson::kObjectType);
    json::set(object, "id", map.id, alloc);
    json::set(object, "name", map.name, alloc);
    json::set(object, "need_mib", mib_value(map.need_mib), alloc);
    json::set(object, "over_by_mib", mib_value(map.over_by_mib), alloc);
    json::set(object, "over_without_mods", map.over_without_mods, alloc);
    maps_over.PushBack(std::move(object), alloc);
  }
  json::set(zone_memory, "maps_over", std::move(maps_over), alloc);
  Value maps_unread(rapidjson::kArrayType);
  for (const auto &map : memory.maps_unread) {
    Value object(rapidjson::kObjectType);
    json::set(object, "id", map.id, alloc);
    json::set(object, "name", map.name, alloc);
    maps_unread.PushBack(std::move(object), alloc);
  }
  json::set(zone_memory, "maps_unread", std::move(maps_unread), alloc);
  json::set(zone_memory, "unread", json::string_array(memory.unread, alloc),
            alloc);
  json::set(doc, "zone_memory", std::move(zone_memory), alloc);

  Value explanations(rapidjson::kArrayType);
  for (const auto &item : value.explanations) {
    Value object(rapidjson::kObjectType);
    json::set(object, "code", item.code, alloc);
    json::set(object, "subject", item.subject, alloc);
    json::set(object, "object", item.object, alloc);
    json::set(object, "message", item.message, alloc);
    explanations.PushBack(std::move(object), alloc);
  }
  json::set(doc, "explanations", std::move(explanations), alloc);

  Value fixes(rapidjson::kArrayType);
  for (const auto &fix : value.fixes) {
    Value object(rapidjson::kObjectType);
    json::set(object, "kind", fix.kind, alloc);
    json::set(object, "id", fix.id, alloc);
    json::set(object, "detail", fix.detail, alloc);
    fixes.PushBack(std::move(object), alloc);
  }
  json::set(doc, "fixes", std::move(fixes), alloc);

  Value diags(rapidjson::kArrayType);
  for (const auto &item : value.diags.items()) {
    Value object(rapidjson::kObjectType);
    json::set(object, "level", to_string(item.level), alloc);
    json::set(object, "code", item.code, alloc);
    json::set(object, "path", item.path, alloc);
    json::set(object, "message", item.message, alloc);
    diags.PushBack(std::move(object), alloc);
  }
  json::set(doc, "diagnostics", std::move(diags), alloc);
  return pretty ? json::pretty(doc) : json::canonical(doc);
}
} // namespace xerces_core
