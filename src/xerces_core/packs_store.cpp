#include "packs_store.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>

#include "json.hpp"

namespace xerces_core {
namespace fs = std::filesystem;

namespace {
std::string to_utf8(const fs::path &path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::string lower_ascii(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
         c == '\v';
}

std::optional<std::string> read_all(const fs::path &file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

// "ws.1944930083" is Workshop item 1944930083; anything else has no number.
std::optional<std::uint64_t> workshop_number(std::string_view id) {
  if (!id.starts_with("ws.") || id.size() <= 3) {
    return std::nullopt;
  }
  std::uint64_t number = 0;
  const char *begin = id.data() + 3;
  const char *end = id.data() + id.size();
  const auto parsed = std::from_chars(begin, end, number);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  return number;
}

// Sort key for the list: what the player reads, then the id for ties.
bool name_before(const library_entry &a, const library_entry &b) {
  const std::string left = lower_ascii(a.value.name);
  const std::string right = lower_ascii(b.value.name);
  if (left != right) {
    return left < right;
  }
  return a.value.id < b.value.id;
}
} // namespace

const library_entry *pack_library::find(std::string_view id) const {
  const auto it = std::ranges::find_if(
      packs, [&](const library_entry &e) { return e.value.id == id; });
  return it == packs.end() ? nullptr : &*it;
}

fs::path pack_library_file(const fs::path &folder, std::string_view id) {
  return folder / fs::path(std::u8string(
                      reinterpret_cast<const char8_t *>(id.data()), id.size()) +
                  u8".json");
}

pack_library read_pack_library(const fs::path &folder) {
  pack_library library;
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) {
    return library;
  }
  std::vector<fs::path> files;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension() == ".json") {
      files.push_back(it->path());
    }
  }
  // Sorted so that, of two files holding one id, the same one always wins.
  std::ranges::sort(files);
  for (const auto &file : files) {
    const std::string shown = to_utf8(file.filename());
    const pack_result result = read_pack_file(file);
    if (!result.ok()) {
      std::string reason = "it is not a valid pack";
      for (const auto &d : result.diags.items()) {
        if (d.level == severity::error) {
          reason = d.message;
          break;
        }
      }
      library.diags.warning("packs.file.invalid", shown,
                            shown + " was skipped: " + reason);
      continue;
    }
    if (const library_entry *existing = library.find(result.value.id)) {
      // Keep the file named after the id; the copy is only reported.
      const bool this_is_named =
          file.stem() == pack_library_file(folder, result.value.id).stem();
      if (this_is_named) {
        const std::string other = to_utf8(existing->file.filename());
        auto &slot = *std::ranges::find_if(library.packs, [&](const auto &e) {
          return e.value.id == result.value.id;
        });
        slot = {result.value, file};
        library.diags.warning("packs.file.duplicate", other,
                              other + " holds the same pack id as " + shown +
                                  " and was skipped.");
      } else {
        library.diags.warning("packs.file.duplicate", shown,
                              shown + " holds the same pack id as " +
                                  to_utf8(existing->file.filename()) +
                                  " and was skipped.");
      }
      continue;
    }
    library.packs.push_back({result.value, file});
  }
  std::ranges::sort(library.packs, name_before);
  return library;
}

bool write_pack(const fs::path &folder, const pack &value,
                diagnostics &diags) {
  const std::string text = pack_to_json(value);
  // What an importer would see: refuse to store a pack nobody could read back.
  const pack_result check = parse_pack(text);
  if (!check.ok()) {
    for (const auto &d : check.diags.items()) {
      if (d.level == severity::error) {
        diags.add(d.level, d.code, d.path, d.message);
      }
    }
    return false;
  }
  std::error_code ec;
  fs::create_directories(folder, ec);
  const fs::path file = pack_library_file(folder, value.id);
  fs::path temporary = file;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      diags.error("packs.write", to_utf8(file.filename()),
                  "The pack could not be written to the packs folder.");
      return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out.flush()) {
      out.close();
      fs::remove(temporary, ec);
      diags.error("packs.write", to_utf8(file.filename()),
                  "The pack could not be written to the packs folder.");
      return false;
    }
  }
  fs::rename(temporary, file, ec);
  if (ec) {
    fs::remove(temporary, ec);
    diags.error("packs.write", to_utf8(file.filename()),
                "The pack could not be written to the packs folder.");
    return false;
  }
  return true;
}

bool remove_pack(const fs::path &folder, std::string_view id) {
  bool removed = false;
  std::error_code ec;
  if (fs::remove(pack_library_file(folder, id), ec) && !ec) {
    removed = true;
  }
  // A copy under another file name would bring the pack back on the next
  // read; remove those too. The library lists only one file per id, so the
  // folder itself is walked to find every copy.
  std::vector<fs::path> copies;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code type_ec;
    if (!it->is_regular_file(type_ec) || it->path().extension() != ".json") {
      continue;
    }
    const pack_result result = read_pack_file(it->path());
    if (result.ok() && result.value.id == id) {
      copies.push_back(it->path());
    }
  }
  for (const auto &file : copies) {
    std::error_code remove_ec;
    if (fs::remove(file, remove_ec) && !remove_ec) {
      removed = true;
    }
  }
  return removed;
}

pack_result read_pack_text(std::string_view text) {
  if (text.starts_with("\xEF\xBB\xBF")) {
    text.remove_prefix(3); // Notepad's byte order mark
  }
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    pack_result empty;
    empty.diags.error("pack.file.empty", "", "The file is empty.");
    return empty;
  }
  if (text.starts_with(share_code_prefix)) {
    return decode_share_code(text);
  }
  return parse_pack(text);
}

pack_result read_pasted_pack(std::string_view text) {
  std::string_view rest = text;
  if (rest.starts_with("\xEF\xBB\xBF")) {
    rest.remove_prefix(3);
  }
  while (!rest.empty() && is_space(rest.front())) {
    rest.remove_prefix(1);
  }
  if (rest.starts_with('{')) {
    return read_pack_text(rest);
  }
  return decode_share_code(rest);
}

pack_result read_pack_file(const fs::path &file) {
  pack_result failed;
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    failed.diags.error("pack.file.read", "", "The file does not exist.");
    return failed;
  }
  const std::uintmax_t size = fs::file_size(file, ec);
  if (ec) {
    failed.diags.error("pack.file.read", "", "The file could not be read.");
    return failed;
  }
  if (size > pack_file_max_bytes) {
    failed.diags.error("pack.file.size", "",
                       "The file is too large to be a pack.");
    return failed;
  }
  const auto text = read_all(file);
  if (!text) {
    failed.diags.error("pack.file.read", "", "The file could not be read.");
    return failed;
  }
  return read_pack_text(*text);
}

import_placement place_import(const pack_library &library, pack incoming,
                              const std::function<std::string()> &make_id) {
  import_placement out;
  const library_entry *existing = library.find(incoming.id);
  if (existing == nullptr) {
    out.value = std::move(incoming);
    out.kind = import_kind::added;
    return out;
  }
  if (pack_to_json(existing->value) == pack_to_json(incoming)) {
    out.value = std::move(incoming);
    out.kind = import_kind::duplicate;
    return out;
  }
  std::string id = make_id();
  for (int tries = 0; library.find(id) != nullptr && tries < 16; ++tries) {
    id = make_id();
  }
  incoming.id = id;
  out.value = std::move(incoming);
  out.kind = import_kind::renumbered;
  return out;
}

std::string trim_pack_name(std::string_view name) {
  while (!name.empty() && is_space(name.front())) {
    name.remove_prefix(1);
  }
  while (!name.empty() && is_space(name.back())) {
    name.remove_suffix(1);
  }
  return std::string(name);
}

std::string pack_name_problem(std::string_view name) {
  const std::string trimmed = trim_pack_name(name);
  if (trimmed.empty()) {
    return "Give the pack a name.";
  }
  if (json::length(trimmed) > pack_name_max_chars) {
    return "Pack names are at most 80 characters.";
  }
  if (std::ranges::any_of(trimmed, [](char c) {
        return static_cast<unsigned char>(c) < 0x20;
      })) {
    return "Pack names cannot contain control characters.";
  }
  return {};
}

const library_entry *find_by_name(const pack_library &library,
                                  std::string_view name) {
  const std::string wanted = lower_ascii(trim_pack_name(name));
  const auto it = std::ranges::find_if(library.packs, [&](const auto &e) {
    return lower_ascii(trim_pack_name(e.value.name)) == wanted;
  });
  return it == library.packs.end() ? nullptr : &*it;
}

std::string strip_colour_codes(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '^' && i + 1 < text.size() && text[i + 1] >= '0' &&
        text[i + 1] <= '9') {
      ++i;
      continue;
    }
    out.push_back(text[i]);
  }
  return trim_pack_name(out);
}

std::string pack_file_stem(std::string_view name) {
  std::string out;
  for (const char c : strip_colour_codes(name)) {
    const auto u = static_cast<unsigned char>(c);
    // Bytes of UTF-8 sequences are letters in other scripts: keep them.
    if (u >= 0x80 || std::isalnum(u) || c == ' ' || c == '-' || c == '_' ||
        c == '.') {
      out.push_back(c);
    } else {
      out.push_back('_');
    }
  }
  // Windows drops trailing dots and spaces from file names.
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
    out.pop_back();
  }
  while (!out.empty() && (out.front() == '.' || out.front() == ' ')) {
    out.erase(out.begin());
  }
  if (out.empty()) {
    return "pack";
  }
  // Device names cannot be file names on Windows, with any extension.
  static constexpr std::array<std::string_view, 22> reserved = {
      "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
      "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
      "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
  const std::string base = lower_ascii(out.substr(0, out.find('.')));
  if (std::ranges::find(reserved, base) != reserved.end()) {
    out += "_pack";
  }
  return out;
}

fs::path with_pack_extension(const fs::path &file) {
  const std::string name = lower_ascii(to_utf8(file.filename()));
  fs::path out = file;
  if (name.ends_with(".json")) {
    return out;
  }
  out += name.ends_with(".xpack") ? ".json" : ".xpack.json";
  return out;
}

const char *to_string(pack_item_state state) {
  switch (state) {
  case pack_item_state::installed: return "installed";
  case pack_item_state::missing: return "missing";
  case pack_item_state::other_version: return "other_version";
  case pack_item_state::changed: return "changed";
  }
  return "missing";
}

pack_check check_pack(const pack &value,
                      const std::vector<installed_content> &catalog) {
  pack_check out;
  const auto copies_of = [&](const std::string &id) {
    std::vector<const installed_content *> found;
    for (const auto &item : catalog) {
      if (item.info.id == id) {
        found.push_back(&item);
      }
    }
    return found;
  };
  const auto count = [&](pack_item_state state) {
    switch (state) {
    case pack_item_state::missing: ++out.missing; break;
    case pack_item_state::other_version: ++out.other_version; break;
    case pack_item_state::changed: ++out.changed; break;
    case pack_item_state::installed: break;
    }
  };

  // Load order first; a mod the order leaves out (a damaged pack) still shows.
  std::vector<const pack_mod *> mods;
  for (const auto &id : value.order) {
    const auto it = std::ranges::find_if(
        value.mods, [&](const pack_mod &m) { return m.id == id; });
    if (it != value.mods.end() &&
        std::ranges::find(mods, &*it) == mods.end()) {
      mods.push_back(&*it);
    }
  }
  for (const auto &mod : value.mods) {
    if (std::ranges::find(mods, &mod) == mods.end()) {
      mods.push_back(&mod);
    }
  }

  for (const pack_mod *mod : mods) {
    pack_item_check item;
    item.id = mod->id;
    item.name = mod->id;
    item.pinned = mod->ver.to_string();
    item.workshop_id = mod->workshop_id ? mod->workshop_id
                                        : workshop_number(mod->id);
    const auto copies = copies_of(mod->id);
    if (copies.empty()) {
      item.state = pack_item_state::missing;
    } else {
      const std::string title = strip_colour_codes(copies.front()->info.name);
      item.name = title.empty() ? mod->id : title;
      if (!item.workshop_id) {
        item.workshop_id = copies.front()->info.workshop_id;
      }
      // Versions compare as text, build metadata included, the way the
      // planner matches pins: a Workshop update changes "+ws.<time>".
      std::vector<const installed_content *> pinned;
      for (const auto *copy : copies) {
        if (copy->info.ver.to_string() == item.pinned) {
          pinned.push_back(copy);
        }
      }
      if (pinned.empty()) {
        item.state = pack_item_state::other_version;
        item.installed = copies.front()->info.ver.to_string();
      } else if (std::ranges::any_of(pinned, [&](const auto *copy) {
                   return copy->content_hash == mod->content_hash;
                 })) {
        item.state = pack_item_state::installed;
      } else {
        item.state = pack_item_state::changed;
      }
    }
    count(item.state);
    out.items.push_back(std::move(item));
  }

  if (value.map) {
    pack_item_check item;
    item.id = value.map->id;
    item.name = value.map->id;
    item.is_map = true;
    item.workshop_id = workshop_number(value.map->id);
    const auto copies = copies_of(value.map->id);
    if (copies.empty()) {
      item.state = pack_item_state::missing;
    } else {
      const std::string title = strip_colour_codes(copies.front()->info.name);
      item.name = title.empty() ? value.map->id : title;
      if (!item.workshop_id) {
        item.workshop_id = copies.front()->info.workshop_id;
      }
      item.state = std::ranges::any_of(copies,
                                       [&](const auto *copy) {
                                         return copy->content_hash ==
                                                value.map->content_hash;
                                       })
                       ? pack_item_state::installed
                       : pack_item_state::changed;
    }
    count(item.state);
    out.items.push_back(std::move(item));
  }
  return out;
}
} // namespace xerces_core
