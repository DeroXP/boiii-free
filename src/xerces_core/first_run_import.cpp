#include "first_run_import.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

#include "json.hpp"
#include "launch.hpp"

namespace xerces_core {
namespace fs = std::filesystem;

namespace {
// properties.json and the server lists are a few hundred bytes; anything this
// big is not upstream's file and is not read.
constexpr std::uintmax_t max_file_size = 1024 * 1024;

std::string utf8(const fs::path &path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::string lower(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  });
  return out;
}

std::string_view trim(std::string_view text) {
  const auto blank = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (!text.empty() && blank(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && blank(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

bool contains_address(const std::vector<std::string> &list,
                      std::string_view address) {
  const std::string key = lower(address);
  return std::ranges::any_of(
      list, [&](const std::string &item) { return lower(item) == key; });
}

bool has_favorite(const launcher_config &config, std::string_view address) {
  const std::string key = lower(address);
  return std::ranges::any_of(config.favorites,
                             [&](const favorite_server &server) {
                               return lower(server.address) == key;
                             });
}

// Places left in a list that holds at most `limit` (none when a hand-edited
// or older config already holds more).
std::size_t room(std::size_t size, std::size_t limit) {
  return limit - std::min(size, limit);
}

// True (with a warning) when path is, or may be, in an identity folder.
bool refused(const fs::path &path, const std::vector<fs::path> &appdata,
             diagnostics &diags) {
  bool unknown = appdata.empty();
  for (const fs::path &dir : appdata) {
    if (dir.empty()) {
      unknown = true; // a folder nobody could name: no telling
      continue;
    }
    switch (check_boiii_identity_path(path, dir)) {
    case boiii_identity_check::outside:
      break;
    case boiii_identity_check::inside:
      diags.warning("first_run.import.identity", utf8(path),
                    "Skipped: this is inside BOIII's identity key folder.");
      return true;
    case boiii_identity_check::unknown:
      unknown = true;
      break;
    }
  }
  if (unknown) {
    diags.warning("first_run.import.unchecked", utf8(path),
                  "Skipped: Xerces could not make sure this is outside "
                  "BOIII's identity key folder.");
  }
  return unknown;
}

// Reads one small file, unless it is in the identity folder. Missing files
// are not an error; everything else that stops the read is a warning.
std::optional<std::string> read_small(const fs::path &file,
                                      const std::vector<fs::path> &appdata,
                                      diagnostics &diags) {
  if (refused(file, appdata, diags)) {
    return std::nullopt;
  }
  std::error_code ec;
  const fs::file_status status = fs::status(file, ec);
  if (ec || !fs::exists(status)) {
    return std::nullopt;
  }
  if (!fs::is_regular_file(status)) {
    diags.warning("first_run.import.not_a_file", utf8(file),
                  "Skipped: not a file.");
    return std::nullopt;
  }
  const std::uintmax_t size = fs::file_size(file, ec);
  if (ec || size > max_file_size) {
    diags.warning("first_run.import.too_big", utf8(file),
                  "Skipped: the file is larger than upstream ever writes it.");
    return std::nullopt;
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    diags.warning("first_run.import.unreadable", utf8(file),
                  "The file could not be opened.");
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}
} // namespace

std::string sanitize_boiii_player_name(std::string_view name) {
  std::string out;
  for (const char c : name) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 32 && byte <= 126) {
      out += c;
    }
  }
  std::string trimmed(trim(out));
  // Names are short; a long value is a broken file, not a name.
  if (trimmed.size() > 64) {
    trimmed.resize(64);
    trimmed = std::string(trim(trimmed));
  }
  return trimmed;
}

std::optional<std::string>
boiii_player_name_from_properties(std::string_view text) {
  if (text.starts_with("\xEF\xBB\xBF")) {
    text.remove_prefix(3);
  }
  const json::parse_result parsed = json::parse(text);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  const auto name = json::get_string(parsed.document, "playerName");
  if (!name) {
    return std::nullopt;
  }
  std::string clean = sanitize_boiii_player_name(*name);
  if (clean.empty()) {
    return std::nullopt;
  }
  return clean;
}

bool is_server_address(std::string_view address) {
  // One rule for what a server address is: launch's. An address it would
  // refuse could be saved here but never joined. Upstream always writes a
  // port, so one is required ("[v6]" alone has its colons inside brackets).
  const std::size_t colon = address.rfind(':');
  const std::size_t bracket = address.rfind(']');
  if (colon == std::string_view::npos ||
      (bracket != std::string_view::npos && colon < bracket)) {
    return false;
  }
  if (address.substr(0, colon) == "0.0.0.0") {
    return false; // what upstream writes for an address it could not parse
  }
  return valid_server_address(address);
}

std::vector<std::string> parse_boiii_server_list(std::string_view text,
                                                 std::size_t limit,
                                                 std::size_t *skipped) {
  std::vector<std::string> out;
  std::size_t bad = 0;
  if (text.starts_with("\xEF\xBB\xBF")) {
    text.remove_prefix(3);
  }
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    const std::string_view line =
        trim(text.substr(0, end == std::string_view::npos ? text.size() : end));
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    if (line.empty()) {
      continue;
    }
    if (!is_server_address(line)) {
      ++bad;
      continue;
    }
    if (out.size() < limit && !contains_address(out, line)) {
      out.emplace_back(line);
    }
  }
  if (skipped) {
    *skipped += bad;
  }
  return out;
}

std::vector<fs::path>
boiii_appdata_dirs(const std::optional<fs::path> &known_folder,
                   const std::optional<fs::path> &variable) {
  std::vector<fs::path> out;
  if (!known_folder || known_folder->empty()) {
    return out;
  }
  out.push_back(*known_folder / "boiii");
  if (variable && !variable->empty()) {
    // The same folder written another way (case, a trailing '\') adds
    // nothing; anything else is checked too.
    const auto key = [](const fs::path &dir) {
      std::string text = lower(utf8(dir.lexically_normal()));
      std::ranges::replace(text, '/', '\\');
      while (text.size() > 3 && text.back() == '\\') {
        text.pop_back();
      }
      return text;
    };
    if (key(*variable) != key(*known_folder)) {
      out.push_back(*variable / "boiii");
    }
  }
  return out;
}

boiii_import find_boiii_import(const std::vector<fs::path> &boiii_appdata,
                               const std::optional<fs::path> &game_dir) {
  boiii_import out;
  std::error_code ec;

  // %LOCALAPPDATA%\boiii: upstream's data, caches and crash dumps, and the
  // identity folder (user\). Nothing outside user\ maps onto the launcher
  // config, and user\ is never opened, so only its presence is reported.
  if (!boiii_appdata.empty() && !boiii_appdata.front().empty()) {
    out.places.push_back({"appdata", boiii_appdata.front(),
                          fs::is_directory(boiii_appdata.front(), ec)});
  }

  if (game_dir && !game_dir->empty()) {
    const fs::path players = *game_dir / "boiii_players";
    boiii_place place{"players", players, false};
    if (boiii_appdata.empty()) {
      // Without the app data folder there is no telling where the identity
      // folder is, so no path can be checked against it.
      out.diags.warning("first_run.import.no_appdata", utf8(players),
                        "Skipped: Windows did not say where the Local "
                        "AppData folder is, so Xerces cannot make sure this "
                        "is outside BOIII's identity key folder.");
    } else if (!refused(players, boiii_appdata, out.diags)) {
      place.exists = fs::is_directory(players, ec);
    }
    out.places.push_back(place);
    if (place.exists) {
      const fs::path properties = players / "properties.json";
      if (const auto text = read_small(properties, boiii_appdata, out.diags)) {
        out.player_name = boiii_player_name_from_properties(*text);
        if (out.player_name) {
          out.player_name_file = properties;
        } else if (!text->empty()) {
          out.diags.info("first_run.import.no_name", utf8(properties),
                         "properties.json has no player name.");
        }
      }
      const fs::path favorites = players / "user" / "favorite_servers.txt";
      if (const auto text = read_small(favorites, boiii_appdata, out.diags)) {
        out.favorites = parse_boiii_server_list(*text, boiii_favorite_limit,
                                                &out.skipped_lines);
        if (!out.favorites.empty()) {
          out.favorites_file = favorites;
        }
      }
      const fs::path recent = players / "user" / "recent_servers.txt";
      if (const auto text = read_small(recent, boiii_appdata, out.diags)) {
        out.recent_servers = parse_boiii_server_list(*text, boiii_recent_limit,
                                                     &out.skipped_lines);
        if (!out.recent_servers.empty()) {
          out.recent_file = recent;
        }
      }
    }
  }
  return out;
}

std::size_t new_boiii_favorites(const launcher_config &config,
                                const boiii_import &found,
                                std::size_t *no_room) {
  std::size_t fresh = 0;
  for (const std::string &address : found.favorites) {
    fresh += has_favorite(config, address) ? 0 : 1;
  }
  // apply_boiii_import stops once the list is full, so only what fits counts
  // as new; otherwise the screen would offer servers that never arrive.
  const std::size_t fits =
      std::min(fresh, room(config.favorites.size(), max_favorite_servers));
  if (no_room) {
    *no_room = fresh - fits;
  }
  return fits;
}

std::size_t new_boiii_recent_servers(const launcher_config &config,
                                     const boiii_import &found,
                                     std::size_t *no_room) {
  std::size_t fresh = 0;
  for (const std::string &address : found.recent_servers) {
    fresh += contains_address(config.recent_servers, address) ? 0 : 1;
  }
  const std::size_t fits =
      std::min(fresh, room(config.recent_servers.size(), max_recent_servers));
  if (no_room) {
    *no_room = fresh - fits;
  }
  return fits;
}

boiii_import_result apply_boiii_import(launcher_config &config,
                                       const boiii_import &found,
                                       const boiii_import_choice &choice) {
  boiii_import_result result;
  if (choice.player_name && found.player_name &&
      config.player_name != *found.player_name) {
    config.player_name = *found.player_name;
    result.player_name = true;
  }
  if (choice.favorites) {
    for (const std::string &address : found.favorites) {
      if (config.favorites.size() >= max_favorite_servers) {
        break; // the Servers screen would refuse every add past this
      }
      if (!has_favorite(config, address)) {
        // BOIII's list has addresses only; the name stays empty until the
        // player names the server.
        config.favorites.push_back({"", address});
        ++result.favorites;
      }
    }
  }
  if (choice.recent_servers) {
    for (const std::string &address : found.recent_servers) {
      if (config.recent_servers.size() >= max_recent_servers) {
        break; // the next join would cut the list back to this
      }
      if (!contains_address(config.recent_servers, address)) {
        config.recent_servers.push_back(address);
        ++result.recent_servers;
      }
    }
  }
  return result;
}
} // namespace xerces_core
