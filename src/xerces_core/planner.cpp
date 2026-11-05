#include "planner.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>

#include "json.hpp"
#include "sha256.hpp"

namespace xerces_core {
namespace {
using rapidjson::Value;
using catalog_index =
    std::map<std::string, std::vector<const installed_content *>>;

struct constraint {
  std::string from;
  const dependency_ref *dep = nullptr;
};

using constraint_map = std::map<std::string, std::vector<constraint>>;

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
      return a->content_hash < b->content_hash;
    });
  }
  return index;
}

const installed_content *
choose(const catalog_index &index, const std::string &id,
       const std::vector<constraint> &constraints,
       const std::map<std::string, std::string> &pins) {
  const auto found = index.find(id);
  if (found == index.end()) {
    return nullptr;
  }
  const auto pin = pins.find(id);
  for (const auto *candidate : found->second) {
    if (pin != pins.end() && candidate->info.ver.to_string() != pin->second) {
      continue;
    }
    const bool fits =
        std::ranges::all_of(constraints, [&](const constraint &c) {
          return c.dep->range.satisfied_by(candidate->info.ver);
        });
    if (fits) {
      return candidate;
    }
  }
  return nullptr;
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

std::map<std::string, std::string>
resolve_settings(const installed_content &content, const plan_request &request,
                 diagnostics &diags) {
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
                      const plan_request &request, diagnostics &diags) {
  plan_entry entry;
  entry.id = content.info.id;
  entry.name = content.info.name;
  entry.ver = content.info.ver;
  entry.folder = content.folder;
  entry.content_hash = content.content_hash;
  entry.requested = requested;
  entry.legacy = content.info.legacy;
  entry.zones = count_zone_files(content.files);
  entry.settings = resolve_settings(content, request, diags);
  return entry;
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

std::size_t count_zone_files(const std::vector<std::string> &files) {
  std::size_t count = 0;
  for (const auto &file : files) {
    const std::string name = normalize_asset_name(file);
    if (name.starts_with("zone/") && name.ends_with(".ff") &&
        name.find('/', 5) == std::string::npos) {
      ++count;
    }
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

  // 1. Expand dependencies and pick versions, until the choice is stable.
  std::vector<std::string> roots = request.mods;
  if (request.map) {
    roots.push_back(*request.map);
  }
  std::map<std::string, const installed_content *> selected;
  constraint_map constraints;
  bool stable = false;
  for (int round = 0; round < 16 && !stable; ++round) {
    constraints.clear();
    for (const auto &[id, content] : selected) {
      for (const auto &dep : content->info.dependencies) {
        constraints[dep.id].push_back({id, &dep});
      }
    }
    std::map<std::string, const installed_content *> next;
    std::deque<std::string> queue(roots.begin(), roots.end());
    std::set<std::string> visited;
    while (!queue.empty()) {
      const std::string id = queue.front();
      queue.pop_front();
      if (!visited.insert(id).second) {
        continue;
      }
      const installed_content *chosen =
          choose(index, id, constraints[id], request.pins);
      if (chosen == nullptr) {
        continue;
      }
      next[id] = chosen;
      for (const auto &dep : chosen->info.dependencies) {
        if (dep.optional && !index.contains(dep.id)) {
          continue;
        }
        auto &list = constraints[dep.id];
        if (std::ranges::none_of(list, [&](const constraint &c) {
              return c.from == id && c.dep == &dep;
            })) {
          list.push_back({id, &dep});
        }
        queue.push_back(dep.id);
      }
    }
    stable = next == selected;
    selected = std::move(next);
  }
  if (!stable) {
    diags.error("plan.dependency.unstable", "",
                "The dependency versions did not settle; check for ranges "
                "that exclude each other.");
  }

  const std::set<std::string> requested_ids(request.mods.begin(),
                                            request.mods.end());
  for (const auto &id : roots) {
    if (selected.contains(id)) {
      continue;
    }
    if (!index.contains(id)) {
      diags.error("plan.mod.missing", "/mods",
                  "\"" + id + "\" is not installed.");
      out.fixes.push_back({"install", id, ""});
    } else {
      const auto pin = request.pins.find(id);
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
  for (const auto &[id, list] : constraints) {
    if (selected.contains(id) || requested_ids.contains(id) ||
        (request.map && *request.map == id)) {
      continue;
    }
    std::string ranges;
    bool all_optional = true;
    for (const auto &c : list) {
      if (!ranges.empty()) {
        ranges += ", ";
      }
      ranges += display_name(selected, c.from) + " needs " + c.dep->range_text;
      all_optional = all_optional && c.dep->optional;
    }
    if (all_optional) {
      diags.warning("plan.dependency.optional_skipped", "",
                    "Optional dependency \"" + id + "\" is left out (" +
                        ranges + ").");
    } else if (!index.contains(id)) {
      diags.error("plan.dependency.missing", "",
                  "\"" + id + "\" is needed but not installed (" + ranges +
                      ").");
      out.fixes.push_back({"install", id, list.front().dep->range_text});
    } else {
      diags.error("plan.dependency.unsatisfiable", "",
                  "No installed version of \"" + id + "\" fits (" + ranges +
                      ").");
      out.fixes.push_back({"update", id, list.front().dep->range_text});
    }
  }

  // Maps sit outside the stack; mods must not be maps and vice versa.
  if (request.map) {
    if (const auto it = selected.find(*request.map); it != selected.end()) {
      if (it->second->info.kind != content_kind::map) {
        diags.error("plan.map.kind", "/map",
                    "\"" + *request.map + "\" is a mod, not a map.");
      } else {
        out.map = make_entry(*it->second, true, request, diags);
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
                                   request, diags));
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
