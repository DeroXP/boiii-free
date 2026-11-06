#include "voice_default.hpp"

#include <cctype>
#include <string>
#include <system_error>

namespace xerces_core {
namespace {
bool starts_with_nocase(std::string_view text, std::string_view part) {
  if (text.size() < part.size()) {
    return false;
  }
  for (std::size_t i = 0; i < part.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[i])) !=
        std::tolower(static_cast<unsigned char>(part[i]))) {
      return false;
    }
  }
  return true;
}

bool ends_with_nocase(std::string_view text, std::string_view part) {
  return text.size() >= part.size() &&
         starts_with_nocase(text.substr(text.size() - part.size()), part);
}

// The profile's name is ASCII, so any other character can stand as '?'; no
// conversion that could fail on an odd file name.
std::string ascii_name(const std::wstring &name) {
  std::string out;
  out.reserve(name.size());
  for (const wchar_t c : name) {
    out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return out;
}
} // namespace

bool is_game_profile_file(std::string_view file_name) {
  constexpr std::string_view prefix = "user_settings_";
  constexpr std::string_view suffix = ".cgp";
  return file_name.size() > prefix.size() + suffix.size() &&
         starts_with_nocase(file_name, prefix) &&
         ends_with_nocase(file_name, suffix);
}

game_profile find_game_profile(const std::filesystem::path &players_dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::file_status status = fs::status(players_dir, ec);
  if (status.type() == fs::file_type::not_found) {
    return game_profile::none;
  }
  if (ec || status.type() != fs::file_type::directory) {
    return game_profile::unknown;
  }

  fs::directory_iterator it(players_dir, ec);
  if (ec) {
    return game_profile::unknown;
  }
  for (const fs::directory_iterator end{}; it != end;) {
    if (is_game_profile_file(ascii_name(it->path().filename().wstring()))) {
      return game_profile::found;
    }
    it.increment(ec);
    if (ec) {
      return game_profile::unknown;
    }
  }
  return game_profile::none;
}

voice_state voice_start_state(game_profile profile) {
  return profile == game_profile::none ? voice_state::due : voice_state::keep;
}
} // namespace xerces_core
