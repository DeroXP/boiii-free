#include "out_of_memory.hpp"

#include <algorithm>

#include "json.hpp"

namespace xerces_core {
namespace {
constexpr std::string_view loading_marker = "Loading fastfile '";
constexpr std::size_t max_zone_bytes = 128;
constexpr std::size_t max_name_bytes = 100;

std::string_view trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t");
  return text.substr(first, last - first + 1);
}

std::string lower_ascii(std::string_view text) {
  std::string out(text);
  for (char &c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return out;
}

std::string join(std::span<const std::string> items) {
  std::string out;
  for (const auto &item : items) {
    if (!out.empty()) {
      out += ", ";
    }
    out += item;
  }
  return out;
}
} // namespace

std::string last_loading_zone(std::span<const std::string> console_lines) {
  for (auto line = console_lines.rbegin(); line != console_lines.rend();
       ++line) {
    const std::string_view text(*line);
    const auto start = text.rfind(loading_marker);
    if (start == std::string_view::npos) {
      continue;
    }
    const auto name_start = start + loading_marker.size();
    const auto end = text.find('\'', name_start);
    if (end == std::string_view::npos || end == name_start) {
      continue; // cut off or empty: look further back
    }
    return json::valid_utf8(text.substr(name_start, end - name_start),
                            max_zone_bytes);
  }
  return {};
}

std::map<std::string, std::string>
read_plan_mod_names(std::string_view plan_json) {
  std::map<std::string, std::string> names;
  const auto parsed = json::parse(plan_json);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return names;
  }
  const auto *stack = json::member(parsed.document, "stack");
  if (stack == nullptr || !stack->IsArray()) {
    return names;
  }
  for (const auto &entry : stack->GetArray()) {
    if (!entry.IsObject()) {
      continue;
    }
    const auto folder = json::get_string(entry, "folder");
    const auto name = json::get_string(entry, "name");
    if (folder && !folder->empty() && name && !trim(*name).empty()) {
      names.emplace(lower_ascii(*folder), std::string(trim(*name)));
    }
  }
  return names;
}

std::vector<std::string>
loaded_mods(std::string_view fs_game, std::string_view fs_extramods,
            const std::map<std::string, std::string> &names) {
  std::vector<std::string> folders;
  const auto add = [&](std::string_view folder) {
    folder = trim(folder);
    if (folder.empty()) {
      return;
    }
    const std::string key = lower_ascii(folder);
    if (std::ranges::any_of(folders, [&](const std::string &seen) {
          return lower_ascii(seen) == key;
        })) {
      return;
    }
    folders.emplace_back(folder);
  };
  add(fs_game);
  std::size_t start = 0;
  while (start <= fs_extramods.size()) {
    const auto comma = fs_extramods.find(',', start);
    const auto end =
        comma == std::string_view::npos ? fs_extramods.size() : comma;
    add(fs_extramods.substr(start, end - start));
    start = end + 1;
  }

  std::vector<std::string> shown;
  shown.reserve(folders.size());
  for (const auto &folder : folders) {
    const auto name = names.find(lower_ascii(folder));
    shown.push_back(json::valid_utf8(
        name != names.end() ? name->second : folder, max_name_bytes));
  }
  return shown;
}

out_of_memory_message out_of_memory_text(std::string_view zone,
                                         std::span<const std::string> mods) {
  out_of_memory_message out;
  out.title = "Black Ops III ran out of memory";

  const std::string mod_list = mods.empty() ? "none" : join(mods);
  out.body = "The game ran out of the memory it keeps for loaded content";
  out.summary = "Out of memory";
  if (!zone.empty()) {
    out.body += " while loading " + std::string(zone);
    out.summary += " while loading " + std::string(zone);
  }
  out.body += ".\n\nMods loaded: " + mod_list + "\n\n";
  if (mods.empty()) {
    out.body += "No mods are loaded, so this map alone needs more of that "
                "memory than the game has. Play another map.";
    out.summary += " (no mods)";
  } else {
    out.body += "Maps and mods share that memory, so remove a mod (Xerces' "
                "Mods screen lists which custom maps fit with your mods) or "
                "play another map.";
    out.summary += " (mods: " + mod_list + ")";
  }
  out.body += "\n\nThe details are in Xerces' game log.";
  return out;
}
} // namespace xerces_core
