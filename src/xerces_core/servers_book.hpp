#pragma once

// The Servers screen's bookkeeping (docs/DESIGN.md 7.3, 11.1): the recent
// servers list, favourites and plain-English reasons why a typed address is
// not one the game's command line can carry. Headless and network-free: it
// only edits the lists kept in launcher.json (launcher_config).

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "launcher_config.hpp"

namespace xerces_core {
inline constexpr std::size_t max_recent_servers = 10;
inline constexpr std::size_t max_favorite_servers = 100;
inline constexpr std::size_t max_server_name = 64;

// Spaces, tabs and line breaks around a typed or pasted address removed.
std::string trim_server_address(std::string_view text);

// Why address is not a server address, as an English sentence the launcher
// translates; empty when it is valid (valid_server_address) or empty. The
// reasons follow valid_server_address's rules, so empty means it would pass.
std::string server_address_problem(std::string_view address);

// The same server: host names and IPv6 digits ignore case, so the comparison
// does too.
bool same_server(std::string_view a, std::string_view b);

// Puts address first, drops an older copy of the same server and keeps at
// most `keep` entries. Invalid or empty addresses are ignored (false).
bool remember_recent_server(std::vector<std::string> &recent,
                            std::string_view address,
                            std::size_t keep = max_recent_servers);
// Removes every copy of the server; false if it was not there.
bool forget_recent_server(std::vector<std::string> &recent,
                          std::string_view address);

// A favourite's name as shown: control characters removed, spaces trimmed,
// at most max_server_name bytes (never cutting a UTF-8 character); the
// address when nothing is left.
std::string clean_server_name(std::string_view name, std::string_view address);

std::optional<std::size_t>
find_favorite(const std::vector<favorite_server> &favorites,
              std::string_view address);

enum class favorite_status {
  saved,       // added or replaced; index is where it is
  bad_address, // the address is empty or not a server address
  duplicate,   // another favourite has this address; index is that one
  full,        // max_favorite_servers reached
  missing,     // replace pointed past the end
};

struct favorite_edit {
  favorite_status status = favorite_status::bad_address;
  std::size_t index = 0;
  bool ok() const { return status == favorite_status::saved; }
};

// Adds a favourite at the end, or replaces favorites[*replace] (a rename or a
// new address). The address is trimmed first; the name is cleaned.
favorite_edit save_favorite(std::vector<favorite_server> &favorites,
                            std::string_view name, std::string_view address,
                            std::optional<std::size_t> replace = {});
} // namespace xerces_core
