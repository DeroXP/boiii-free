#include "conflicts.hpp"

#include <algorithm>
#include <cctype>

#include "json.hpp"
#include "sha256.hpp"

namespace xerces_core {
namespace {
using rapidjson::Value;
using asset_key = std::pair<std::string, std::string>; // type, name

std::string lower(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string extension(std::string_view name) {
  const std::size_t dot = name.rfind('.');
  const std::size_t slash = name.rfind('/');
  if (dot == std::string_view::npos ||
      (slash != std::string_view::npos && dot < slash)) {
    return {};
  }
  return lower(name.substr(dot));
}

std::string plural(std::size_t count, const std::string &one,
                   const std::string &many) {
  return std::to_string(count) + " " + (count == 1 ? one : many);
}
} // namespace

const char *to_string(asset_strategy value) {
  switch (value) {
  case asset_strategy::merge_table:
    return "merge_table";
  case asset_strategy::merge_config:
    return "merge_config";
  case asset_strategy::merge_strings:
    return "merge_strings";
  case asset_strategy::union_list:
    return "union";
  case asset_strategy::exclusive_script:
    return "exclusive_script";
  case asset_strategy::exclusive:
    return "exclusive";
  }
  return "exclusive";
}

const char *to_string(asset_outcome value) {
  switch (value) {
  case asset_outcome::added:
    return "added";
  case asset_outcome::merged:
    return "merged";
  case asset_outcome::override_allowed:
    return "override_allowed";
  case asset_outcome::override_blocked:
    return "override_blocked";
  case asset_outcome::legacy_conflict:
    return "legacy_conflict";
  case asset_outcome::identical:
    return "identical";
  case asset_outcome::overlap:
    return "overlap";
  case asset_outcome::map_priority:
    return "map_priority";
  }
  return "added";
}

asset_strategy strategy_for(std::string_view type, std::string_view name) {
  const std::string t = lower(type);
  if (t == "stringtable") {
    return asset_strategy::merge_table;
  }
  if (t == "localize" || t == "localizedstring" || t == "localize_entry") {
    return asset_strategy::merge_strings;
  }
  if (t == "bg_cache" || t == "precache") {
    return asset_strategy::union_list;
  }
  if (t == "scriptparsetree") {
    return asset_strategy::exclusive_script;
  }
  if (t == "rawfile") {
    const std::string ext = extension(name);
    if (lower(name).starts_with("ui/") && ext == ".lua") {
      return asset_strategy::exclusive;
    }
    if (ext == ".cfg" || ext == ".json" || ext == ".ini" || ext == ".kv") {
      return asset_strategy::merge_config;
    }
  }
  return asset_strategy::exclusive;
}

bool has_exact_hash(std::string_view type) {
  const std::string t = lower(type);
  return t == "scriptparsetree" || t == "rawfile" || t == "stringtable" ||
         t == "localize" || t == "localizedstring" || t == "localize_entry";
}

asset_index index_loose_files(
    const std::string &owner,
    const std::vector<std::pair<std::string, std::string>> &files) {
  asset_index index;
  index.owner = owner;
  for (const auto &[path, content] : files) {
    const std::string name = normalize_asset_name(path);
    const std::string ext = extension(name);
    std::string type;
    if (name == manifest_file_name || name.ends_with("workshop.json") ||
        name.starts_with("zone/") || ext == ".png" || ext == ".jpg" ||
        ext == ".md" || (ext == ".txt" && name.starts_with("readme"))) {
      continue; // metadata, previews and fastfiles (those come from index runs)
    }
    if (ext == ".gsc" || ext == ".csc") {
      type = "scriptparsetree";
    } else if (ext == ".csv") {
      type = "stringtable";
    } else if (ext == ".str") {
      type = "localize";
    } else {
      type = "rawfile";
    }
    index.records.push_back({type, name, "loose",
                             static_cast<std::uint64_t>(content.size()),
                             sha256_tagged(content)});
  }
  std::ranges::sort(index.records, [](const auto &a, const auto &b) {
    return std::tie(a.type, a.name) < std::tie(b.type, b.name);
  });
  return index;
}

conflict_report check_conflicts(const conflict_inputs &inputs) {
  conflict_report report;
  if (inputs.resolved == nullptr) {
    report.diags.error("conflict.no_plan", "", "There is no plan to check.");
    return report;
  }
  const plan &resolved = *inputs.resolved;

  // Plan order, highest first; the map sits below the mods except for its own
  // scripts (rule 6).
  std::vector<std::string> order;
  for (const auto &entry : resolved.stack) {
    order.push_back(entry.id);
  }
  const std::optional<std::string> map_id =
      resolved.map ? std::optional(resolved.map->id) : std::nullopt;
  if (map_id) {
    order.push_back(*map_id);
  }

  const auto display = [&](const std::string &id) -> std::string {
    if (id == "stock") {
      return "the game";
    }
    if (const auto it = inputs.manifests.find(id);
        it != inputs.manifests.end() && !it->second.name.empty()) {
      return it->second.name;
    }
    return id;
  };

  std::map<std::string, std::map<asset_key, const asset_record *>> by_owner;
  std::set<asset_key> keys;
  for (const auto &index : inputs.indexes) {
    for (const auto &record : index.records) {
      by_owner[index.owner][{record.type, record.name}] = &record;
      keys.insert({record.type, record.name});
    }
    for (const auto &tier : index.missing_tiers) {
      report.diags.info("conflict.index.partial", index.owner,
                        display(index.owner) + "'s " + tier +
                            " tier is not indexed yet.");
    }
  }
  for (const auto &id : order) {
    if (!by_owner.contains(id) &&
        std::ranges::none_of(inputs.indexes, [&](const asset_index &index) {
          return index.owner == id;
        })) {
      report.diags.warning("conflict.index.missing", id,
                           display(id) +
                               " has not been indexed yet, so its fastfile "
                               "assets are not in this report.");
    }
  }
  std::set<asset_key> stock_keys;
  if (inputs.stock) {
    for (const auto &record : inputs.stock->records) {
      stock_keys.insert({record.type, record.name});
    }
  }

  for (const auto &key : keys) {
    const auto &[type, name] = key;
    asset_ownership entry;
    entry.type = type;
    entry.name = name;
    entry.strategy = strategy_for(type, name);
    entry.in_stock = stock_keys.contains(key);
    for (const auto &id : order) {
      if (by_owner[id].contains(key)) {
        entry.owners.push_back(id);
      }
    }
    if (entry.owners.empty()) {
      continue; // indexed, but not part of this plan
    }
    bool map_moved = false;
    if (map_id && inputs.map_first_for_scripts &&
        entry.strategy == asset_strategy::exclusive_script &&
        entry.owners.back() == *map_id && entry.owners.size() > 1) {
      entry.owners.pop_back();
      entry.owners.insert(entry.owners.begin(), *map_id);
      map_moved = true;
    }
    if (entry.owners.size() == 1 && !entry.in_stock) {
      continue; // rule 1: a new asset, nothing to report
    }
    const std::string first = display(entry.owners.front());

    if (entry.strategy != asset_strategy::exclusive &&
        entry.strategy != asset_strategy::exclusive_script) {
      entry.outcome = asset_outcome::merged;
      entry.rule = 2;
      entry.winner = entry.owners.front();
      std::string who;
      for (std::size_t i = 0; i < entry.owners.size(); ++i) {
        who += (i == 0                         ? ""
                : i + 1 == entry.owners.size() ? " and "
                                               : ", ") +
               display(entry.owners[i]);
      }
      entry.message =
          entry.owners.size() == 1
              ? who + " extends the game's " + name + "."
              : who + (entry.owners.size() == 2 ? " both" : " all") +
                    " extend " + name + ". Each applies, merged by " +
                    (entry.strategy == asset_strategy::merge_table ? "row"
                                                                   : "key") +
                    "; where two change the same value, the higher one wins.";
      ++report.merges;
      report.entries.push_back(std::move(entry));
      continue;
    }

    // Exclusive: find the first owner allowed to win.
    for (std::size_t i = 0; i < entry.owners.size(); ++i) {
      const std::string &owner = entry.owners[i];
      const bool replaces_something =
          i + 1 < entry.owners.size() || entry.in_stock;
      if (!replaces_something) {
        entry.winner = owner;
        break;
      }
      const auto m = inputs.manifests.find(owner);
      const bool legacy = m == inputs.manifests.end() || m->second.legacy;
      const bool declared = m != inputs.manifests.end() &&
                            m->second.declares_override(type, name);
      const bool is_map = map_id && owner == *map_id && map_moved;
      if (!legacy && !declared && !is_map &&
          !inputs.allow_undeclared.contains(owner)) {
        entry.blocked.push_back(owner);
        continue;
      }
      entry.winner = owner;
      break;
    }
    if (entry.winner.empty()) {
      entry.winner = "stock";
    }
    const std::string winner = display(entry.winner);

    if (!entry.blocked.empty()) {
      entry.outcome = asset_outcome::override_blocked;
      entry.rule = 3;
      for (const auto &blocked : entry.blocked) {
        entry.message += display(blocked) + " replaces " + name +
                         " without declaring an override, so " + winner +
                         "'s copy is used. ";
        report.fixes.push_back({"allow_override", blocked, type + ":" + name});
      }
      entry.message += "Allow it in the pack to change that.";
      ++report.blocked;
      report.entries.push_back(std::move(entry));
      continue;
    }
    if (map_moved && entry.winner == *map_id) {
      entry.outcome = asset_outcome::map_priority;
      entry.rule = 6;
      entry.message = "The map's own " + name + " stays above " +
                      display(entry.owners[1]) +
                      "'s copy (maps rank first for their scripts).";
      report.entries.push_back(std::move(entry));
      continue;
    }

    const auto winner_it = std::ranges::find(entry.owners, entry.winner);
    std::vector<std::string> lower_owners(winner_it == entry.owners.end()
                                              ? entry.owners.begin()
                                              : std::next(winner_it),
                                          entry.owners.end());
    if (lower_owners.empty()) {
      // One owner replacing the game's copy.
      entry.outcome = asset_outcome::override_allowed;
      entry.rule = 3;
      const auto m = inputs.manifests.find(entry.winner);
      const bool legacy = m == inputs.manifests.end() || m->second.legacy;
      entry.message = winner + " replaces the game's " + name +
                      (legacy ? "." : " (declared).");
      ++report.overrides;
      report.entries.push_back(std::move(entry));
      continue;
    }

    if (!has_exact_hash(type)) {
      entry.outcome = asset_outcome::overlap;
      entry.rule = 5;
      entry.message = winner + " and " + display(lower_owners.front()) +
                      " both have " + type + " " + name + " (not compared); " +
                      winner + "'s wins by order.";
      ++report.overlaps;
      report.entries.push_back(std::move(entry));
      continue;
    }

    const std::string &winner_hash =
        entry.winner == "stock" ? std::string{}
                                : by_owner[entry.winner][key]->content_hash;
    std::vector<std::string> differing;
    for (const auto &owner : lower_owners) {
      if (by_owner[owner][key]->content_hash != winner_hash) {
        differing.push_back(owner);
      }
    }
    if (differing.empty()) {
      entry.outcome = asset_outcome::identical;
      entry.rule = 5;
      entry.message = winner + " and " + display(lower_owners.front()) +
                      " ship identical copies of " + name + ".";
      ++report.identical;
      report.entries.push_back(std::move(entry));
      continue;
    }

    const auto m = inputs.manifests.find(entry.winner);
    const bool winner_declared = m != inputs.manifests.end() &&
                                 !m->second.legacy &&
                                 m->second.declares_override(type, name);
    const std::string loser = display(differing.front());
    if (winner_declared || inputs.allow_undeclared.contains(entry.winner)) {
      entry.outcome = asset_outcome::override_allowed;
      entry.rule = 3;
      entry.message = winner + " overrides " + name + "; " + loser +
                      "'s copy will not apply.";
      ++report.overrides;
      report.entries.push_back(std::move(entry));
      continue;
    }

    entry.outcome = asset_outcome::legacy_conflict;
    entry.rule = 4;
    entry.needs_decision = !inputs.decided.contains(type + ":" + name);
    entry.message = winner + " and " + loser + " both replace " + name +
                    " with different content. " + winner +
                    " wins because it is higher in the order. " + loser +
                    "'s change will not apply.";
    if (entry.needs_decision) {
      ++report.decisions;
      report.fixes.push_back({"reorder", differing.front(), entry.winner});
      report.fixes.push_back({"keep_order", entry.winner, type + ":" + name});
      report.fixes.push_back({"remove", differing.front(), ""});
    }
    report.entries.push_back(std::move(entry));
  }
  return report;
}

std::string conflict_report_to_json(const conflict_report &report,
                                    bool pretty) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "needs_decision", report.needs_decision(), alloc);
  Value counts(rapidjson::kObjectType);
  json::set(counts, "decisions", static_cast<std::int64_t>(report.decisions),
            alloc);
  json::set(counts, "merges", static_cast<std::int64_t>(report.merges), alloc);
  json::set(counts, "blocked", static_cast<std::int64_t>(report.blocked),
            alloc);
  json::set(counts, "overlaps", static_cast<std::int64_t>(report.overlaps),
            alloc);
  json::set(counts, "identical", static_cast<std::int64_t>(report.identical),
            alloc);
  json::set(counts, "overrides", static_cast<std::int64_t>(report.overrides),
            alloc);
  json::set(doc, "counts", std::move(counts), alloc);

  Value entries(rapidjson::kArrayType);
  for (const auto &entry : report.entries) {
    Value object(rapidjson::kObjectType);
    json::set(object, "type", entry.type, alloc);
    json::set(object, "name", entry.name, alloc);
    json::set(object, "strategy", to_string(entry.strategy), alloc);
    json::set(object, "owners", json::string_array(entry.owners, alloc), alloc);
    json::set(object, "winner", entry.winner, alloc);
    json::set(object, "blocked", json::string_array(entry.blocked, alloc),
              alloc);
    json::set(object, "outcome", to_string(entry.outcome), alloc);
    json::set(object, "rule", static_cast<std::int64_t>(entry.rule), alloc);
    json::set(object, "needs_decision", entry.needs_decision, alloc);
    json::set(object, "in_stock", entry.in_stock, alloc);
    json::set(object, "message", entry.message, alloc);
    entries.PushBack(std::move(object), alloc);
  }
  json::set(doc, "entries", std::move(entries), alloc);

  Value fixes(rapidjson::kArrayType);
  for (const auto &fix : report.fixes) {
    Value object(rapidjson::kObjectType);
    json::set(object, "kind", fix.kind, alloc);
    json::set(object, "id", fix.id, alloc);
    json::set(object, "detail", fix.detail, alloc);
    fixes.PushBack(std::move(object), alloc);
  }
  json::set(doc, "fixes", std::move(fixes), alloc);

  Value diags(rapidjson::kArrayType);
  for (const auto &item : report.diags.items()) {
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

std::string conflict_report_to_text(const conflict_report &report) {
  std::string out;
  const auto section = [&](asset_outcome kind, const std::string &heading,
                           bool decisions_only) {
    std::string body;
    for (const auto &entry : report.entries) {
      if (entry.outcome != kind || (decisions_only && !entry.needs_decision)) {
        continue;
      }
      body += "  " + entry.message + "\n";
      if (entry.needs_decision) {
        body += "    [Move the other mod up] [Keep order] [Disable the other "
                "mod]\n";
      }
    }
    if (!body.empty()) {
      out += heading + "\n" + body;
    }
  };
  if (report.decisions > 0) {
    out += plural(report.decisions, "conflict needs a decision.",
                  "conflicts need a decision.") +
           "\n";
    section(asset_outcome::legacy_conflict, "", true);
  }
  if (report.blocked > 0) {
    section(asset_outcome::override_blocked,
            plural(report.blocked, "override was blocked.",
                   "overrides were blocked."),
            false);
  }
  if (report.merges > 0) {
    section(asset_outcome::merged,
            plural(report.merges, "asset merges.", "assets merge."), false);
  }
  if (report.overrides > 0) {
    section(asset_outcome::override_allowed,
            plural(report.overrides, "override.", "overrides."), false);
  }
  section(asset_outcome::map_priority, "Map scripts:", false);
  if (report.overlaps > 0) {
    section(asset_outcome::overlap,
            plural(report.overlaps, "overlap, not compared, resolved by order.",
                   "overlaps, not compared, resolved by order."),
            false);
  }
  if (report.identical > 0) {
    out += plural(report.identical, "identical copy, no conflict.",
                  "identical copies, no conflict.") +
           "\n";
  }
  if (out.empty()) {
    out = "No conflicts.\n";
  }
  for (const auto &item : report.diags.items()) {
    out += std::string(to_string(item.level)) + ": " + item.message + "\n";
  }
  return out;
}
} // namespace xerces_core
