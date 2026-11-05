#include "mods_edit.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace xerces_core {
namespace {
std::string lower(std::string_view text) {
  std::string out(text);
  for (char &c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

bool word_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::string joined(const std::vector<std::string> &items) {
  std::string out;
  for (const auto &item : items) {
    out += (out.empty() ? "" : ", ") + item;
  }
  return out;
}
} // namespace

// ------------------------------------------------------------- load order
bool mods_same(const mode_profile &a, const mode_profile &b) {
  return a.mods == b.mods && a.map == b.map && a.modded == b.modded &&
         a.pins == b.pins && a.pack_name == b.pack_name &&
         a.pack_id == b.pack_id;
}

mods_snapshot mods_take(const launcher_config &config,
                        const std::string &mode) {
  mods_snapshot out;
  if (const auto it = config.profiles.find(mode); it != config.profiles.end()) {
    out.profile = it->second;
  }
  out.saved_positions = config.saved_positions;
  return out;
}

bool mods_set_enabled(mode_profile &profile, const std::string &id, bool on) {
  const auto it = std::ranges::find(profile.mods, id);
  if (on) {
    if (it != profile.mods.end() || id.empty()) {
      return false;
    }
    profile.mods.push_back(id);
    return true;
  }
  if (it == profile.mods.end()) {
    return false;
  }
  profile.mods.erase(it);
  return true;
}

bool mods_move(mode_profile &profile, std::size_t from, std::size_t to) {
  auto &mods = profile.mods;
  if (from >= mods.size() || to >= mods.size() || from == to) {
    return false;
  }
  std::string moving = std::move(mods[from]);
  mods.erase(mods.begin() + static_cast<std::ptrdiff_t>(from));
  mods.insert(mods.begin() + static_cast<std::ptrdiff_t>(to),
              std::move(moving));
  return true;
}

bool mods_move_by(mode_profile &profile, const std::string &id, int delta) {
  const auto it = std::ranges::find(profile.mods, id);
  if (it == profile.mods.end() || delta == 0) {
    return false;
  }
  const auto from = static_cast<std::ptrdiff_t>(it - profile.mods.begin());
  const auto last = static_cast<std::ptrdiff_t>(profile.mods.size()) - 1;
  const std::ptrdiff_t to = std::clamp<std::ptrdiff_t>(from + delta, 0, last);
  return mods_move(profile, static_cast<std::size_t>(from),
                   static_cast<std::size_t>(to));
}

bool mods_set_map(mode_profile &profile,
                  const std::optional<std::string> &map) {
  const std::optional<std::string> next =
      map && !map->empty() ? map : std::nullopt;
  if (profile.map == next) {
    return false;
  }
  profile.map = next;
  return true;
}

bool mods_unpin(mode_profile &profile, const std::string &id) {
  return profile.pins.erase(id) > 0;
}

std::string mods_hand_edit(launcher_config &config, const std::string &mode) {
  mode_profile &profile = profile_for(config, mode);
  std::string detached =
      !profile.pack_name.empty() ? profile.pack_name : profile.pack_id;
  profile.pins.clear();
  profile.pack_name.clear();
  profile.pack_id.clear();
  for (std::size_t i = 0; i < profile.mods.size(); ++i) {
    config.saved_positions[profile.mods[i]] = static_cast<std::int64_t>(i);
  }
  return detached;
}

void mods_history::record(const std::string &mode, const mods_snapshot &before,
                          const mods_snapshot &after) {
  // Only the positions this edit wrote; the rest belong to other edits.
  std::map<std::string, position_change> positions;
  const auto lookup = [](const std::map<std::string, std::int64_t> &map,
                         const std::string &key) {
    const auto it = map.find(key);
    return it == map.end() ? std::optional<std::int64_t>()
                           : std::optional<std::int64_t>(it->second);
  };
  for (const auto *side : {&before.saved_positions, &after.saved_positions}) {
    for (const auto &[key, value] : *side) {
      const auto was = lookup(before.saved_positions, key);
      const auto now = lookup(after.saved_positions, key);
      if (was != now) {
        positions[key] = {was, now};
      }
    }
  }
  if (mods_same(before.profile, after.profile) && positions.empty()) {
    return;
  }
  entries_.push_back(
      {mode, before.profile, after.profile, std::move(positions)});
  while (entries_.size() > limit_) {
    entries_.pop_front();
  }
}

mods_history::undo_result mods_history::undo(launcher_config &config,
                                             const std::string &mode) {
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    if (it->mode != mode) {
      continue;
    }
    if (!mods_same(mods_take(config, mode).profile, it->after)) {
      // Packs or Home changed this mode's profile since: the old states no
      // longer describe what is there, so they cannot be restored safely.
      std::erase_if(entries_, [&](const entry &e) { return e.mode == mode; });
      return undo_result::stale;
    }
    config.profiles[mode] = it->before;
    auto &saved = config.saved_positions;
    for (const auto &[key, change] : it->positions) {
      const auto current = saved.find(key);
      const std::optional<std::int64_t> now =
          current == saved.end() ? std::nullopt
                                 : std::optional<std::int64_t>(current->second);
      if (now != change.second) {
        continue; // a later edit (another mode's) wrote it; that one stays
      }
      if (change.first) {
        saved[key] = *change.first;
      } else {
        saved.erase(key);
      }
    }
    entries_.erase(std::next(it).base());
    return undo_result::done;
  }
  return undo_result::nothing;
}

std::size_t mods_history::size(const std::string &mode) const {
  return static_cast<std::size_t>(std::ranges::count_if(
      entries_, [&](const entry &e) { return e.mode == mode; }));
}

// ----------------------------------------------------------------- search
std::string mods_plain_name(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (std::size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '^' && i + 1 < name.size() && name[i + 1] >= '0' &&
        name[i + 1] <= '9') {
      ++i;
      continue;
    }
    out.push_back(name[i]);
  }
  // Codes between words leave double spaces behind.
  std::string tidy;
  for (const char c : out) {
    if (c == ' ' && (tidy.empty() || tidy.back() == ' ')) {
      continue;
    }
    tidy.push_back(c);
  }
  while (!tidy.empty() && tidy.back() == ' ') {
    tidy.pop_back();
  }
  return tidy.empty() ? std::string(name) : tidy;
}

bool mods_matches(const installed_content &item, std::string_view query) {
  std::string haystack = lower(item.info.name) + "\n" +
                         lower(mods_plain_name(item.info.name)) + "\n" +
                         lower(item.info.id) + "\n" + lower(item.folder);
  if (item.info.workshop_id) {
    haystack += "\n" + std::to_string(*item.info.workshop_id);
  }
  const std::string needle = lower(query);
  std::size_t pos = 0;
  while (pos < needle.size()) {
    const std::size_t start = needle.find_first_not_of(" \t", pos);
    if (start == std::string::npos) {
      break;
    }
    std::size_t end = needle.find_first_of(" \t", start);
    if (end == std::string::npos) {
      end = needle.size();
    }
    if (haystack.find(needle.substr(start, end - start)) == std::string::npos) {
      return false;
    }
    pos = end;
  }
  return true;
}

// ------------------------------------------------------------ diagnostics
bool mods_notes::has_error(const std::string &id) const {
  const auto it = by_id.find(id);
  return it != by_id.end() &&
         std::ranges::any_of(it->second, [](const mods_note &n) {
           return n.level == severity::error;
         });
}

bool mods_notes::has_warning(const std::string &id) const {
  const auto it = by_id.find(id);
  return it != by_id.end() &&
         std::ranges::any_of(it->second, [](const mods_note &n) {
           return n.level == severity::warning;
         });
}

mods_notes
mods_attribute(const plan &resolved,
               const std::vector<std::pair<std::string, std::string>> &names) {
  // Longest names first, so a name inside a longer one is only counted when
  // it also appears on its own.
  std::vector<std::pair<std::string, std::string>> by_length = names;
  std::ranges::stable_sort(by_length, [](const auto &a, const auto &b) {
    return a.second.size() > b.second.size();
  });

  mods_notes out;
  for (const diagnostic &item : resolved.diags.items()) {
    const mods_note note{item.level, item.code, item.message};
    std::set<std::string> ids;
    // Settings name their mod in the path: /settings/<id>/<key>.
    if (item.path.starts_with("/settings/")) {
      const std::string rest = item.path.substr(10);
      ids.insert(rest.substr(0, rest.find('/')));
    }
    const std::string &text = item.message;
    std::vector<bool> taken(text.size(), false);
    const auto claim = [&](std::size_t pos, std::size_t length) {
      for (std::size_t i = pos; i < pos + length; ++i) {
        if (taken[i]) {
          return false;
        }
      }
      std::fill(taken.begin() + static_cast<std::ptrdiff_t>(pos),
                taken.begin() + static_cast<std::ptrdiff_t>(pos + length),
                true);
      return true;
    };
    for (const auto &[id, name] : by_length) {
      const std::string quoted = "\"" + id + "\"";
      for (std::size_t pos = text.find(quoted); pos != std::string::npos;
           pos = text.find(quoted, pos + 1)) {
        claim(pos, quoted.size());
        ids.insert(id);
      }
    }
    for (const auto &[id, name] : by_length) {
      if (name.size() < 2) {
        continue;
      }
      for (std::size_t pos = text.find(name); pos != std::string::npos;
           pos = text.find(name, pos + 1)) {
        const std::size_t end = pos + name.size();
        // Whole words only, where the name starts or ends with one.
        const bool left_ok =
            pos == 0 || !word_char(name.front()) || !word_char(text[pos - 1]);
        const bool right_ok = end == text.size() || !word_char(name.back()) ||
                              !word_char(text[end]);
        if (left_ok && right_ok && claim(pos, name.size())) {
          ids.insert(id);
        }
      }
    }
    if (ids.empty()) {
      out.general.push_back(note);
      continue;
    }
    for (const auto &id : ids) {
      out.by_id[id].push_back(note);
    }
  }
  return out;
}

std::vector<mods_note> mods_folder_notes(const diagnostics &diags,
                                         const std::string &folder) {
  std::vector<mods_note> out;
  if (folder.empty()) {
    return out;
  }
  for (const diagnostic &item : diags.items()) {
    if (item.path == folder || item.path.starts_with(folder + "/")) {
      out.push_back({item.level, item.code, item.message});
    }
  }
  return out;
}

// ------------------------------------------------------------------ text
std::string mods_short_hash(std::string_view hash) {
  if (const std::size_t colon = hash.find(':');
      colon != std::string_view::npos) {
    hash.remove_prefix(colon + 1);
  }
  return std::string(hash.substr(0, 12));
}

std::vector<mods_capability> mods_capabilities(const capabilities &caps) {
  std::vector<mods_capability> out;
  if (caps.native_code) {
    out.push_back({"Native code (DLL or EXE files)", ""});
  }
  if (caps.lua_unsafe) {
    out.push_back({"Unrestricted Lua", ""});
  }
  if (!caps.lua_net.empty()) {
    out.push_back({"Lua network access", joined(caps.lua_net)});
  }
  if (!caps.lua_files.empty()) {
    out.push_back({"Lua file access", joined(caps.lua_files)});
  }
  if (caps.gsc_exec_command) {
    out.push_back({"GSC runs console commands", ""});
  }
  if (caps.gsc_files) {
    out.push_back({"GSC reads and writes files", ""});
  }
  if (caps.gsc_replacefunc) {
    out.push_back({"GSC replaces game functions", ""});
  }
  return out;
}
} // namespace xerces_core
