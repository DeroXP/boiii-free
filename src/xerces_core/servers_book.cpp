#include "servers_book.hpp"

#include <algorithm>

#include "launch.hpp"

namespace xerces_core {
namespace {
bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
         c == '\f';
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

bool host_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '-';
}

bool ipv6_char(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F') || c == ':' || c == '.';
}

// The port part after the colon, judged the way valid_server_address does.
std::string port_problem(std::string_view port) {
  if (port.empty()) {
    return "Add the port after the colon, for example :27017.";
  }
  if (port.size() > 5 ||
      !std::ranges::all_of(port, [](char c) { return c >= '0' && c <= '9'; })) {
    return "The port is a number from 1 to 65535.";
  }
  const int value = std::stoi(std::string(port));
  if (value < 1 || value > 65535) {
    return "The port is a number from 1 to 65535.";
  }
  return {};
}
} // namespace

std::string trim_server_address(std::string_view text) {
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

std::string server_address_problem(std::string_view address) {
  if (address.empty() || valid_server_address(address)) {
    return {};
  }
  const std::string generic =
      "Not a server address. Use host:port, for example 192.168.1.20:27017.";
  if (address.size() > 260) {
    return "The address is too long.";
  }
  if (std::ranges::any_of(address, is_space)) {
    return "Remove the spaces from the address.";
  }
  // IPv6 hints name no IPv6 example: the game cannot join one anyway
  // (ipv4_only_problem).
  if (address.front() == '[') {
    const std::size_t close = address.find(']');
    if (close == std::string_view::npos) {
      return "Close the IPv6 address with a ]. The game connects over IPv4 "
             "only, though: use the server's IPv4 address.";
    }
    const std::string_view host = address.substr(1, close - 1);
    if (close < 3 || !std::ranges::all_of(host, ipv6_char)) {
      return std::string(ipv4_only_problem);
    }
    const std::string_view rest = address.substr(close + 1);
    if (!rest.empty() && rest.front() != ':') {
      return "Put a colon between ] and the port. The game connects over IPv4 "
             "only, though: use the server's IPv4 address.";
    }
    if (!rest.empty()) {
      if (std::string problem = port_problem(rest.substr(1));
          !problem.empty()) {
        return problem;
      }
    }
    return generic;
  }
  if (std::ranges::count(address, ':') > 1) {
    return std::string(ipv4_only_problem);
  }
  std::string_view host = address;
  const std::size_t colon = address.find(':');
  if (colon != std::string_view::npos) {
    host = address.substr(0, colon);
  }
  if (host.empty()) {
    return "Add the host before the colon, for example 192.168.1.20:27017.";
  }
  if (host.front() == '-' || host.front() == '.') {
    return "The host cannot start with a dot or a dash.";
  }
  if (!std::ranges::all_of(host, host_char)) {
    return "Use only letters, digits, dots and dashes in the host.";
  }
  if (colon != std::string_view::npos) {
    if (std::string problem = port_problem(address.substr(colon + 1));
        !problem.empty()) {
      return problem;
    }
  }
  return generic;
}

std::string server_join_problem(std::string_view address) {
  std::string problem = server_address_problem(address);
  if (problem.empty() && !address.empty() && address.front() == '[') {
    problem = ipv4_only_problem;
  }
  return problem;
}

bool same_server(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
           return lower(x) == lower(y);
         });
}

bool remember_recent_server(std::vector<std::string> &recent,
                            std::string_view address, std::size_t keep) {
  const std::string trimmed = trim_server_address(address);
  if (trimmed.empty() || !valid_server_address(trimmed) || keep == 0) {
    return false;
  }
  std::erase_if(recent, [&](const std::string &entry) {
    return same_server(entry, trimmed);
  });
  recent.insert(recent.begin(), trimmed);
  if (recent.size() > keep) {
    recent.resize(keep);
  }
  return true;
}

bool forget_recent_server(std::vector<std::string> &recent,
                          std::string_view address) {
  return std::erase_if(recent, [&](const std::string &entry) {
           return same_server(entry, address);
         }) != 0;
}

std::string clean_server_name(std::string_view name, std::string_view address) {
  std::string out;
  out.reserve(name.size());
  for (const char c : name) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) {
      continue;
    }
    out.push_back(c);
  }
  if (out.size() > max_server_name) {
    // Back off to the start of a UTF-8 character (continuation bytes are
    // 10xxxxxx) so the name never ends in half a character.
    std::size_t cut = max_server_name;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    out.resize(cut);
  }
  const std::string trimmed = trim_server_address(out);
  return trimmed.empty() ? trim_server_address(address) : trimmed;
}

std::optional<std::size_t>
find_favorite(const std::vector<favorite_server> &favorites,
              std::string_view address) {
  const std::string trimmed = trim_server_address(address);
  for (std::size_t i = 0; i < favorites.size(); ++i) {
    if (same_server(favorites[i].address, trimmed)) {
      return i;
    }
  }
  return std::nullopt;
}

favorite_edit save_favorite(std::vector<favorite_server> &favorites,
                            std::string_view name, std::string_view address,
                            std::optional<std::size_t> replace) {
  const std::string trimmed = trim_server_address(address);
  if (trimmed.empty() || !valid_server_address(trimmed)) {
    return {favorite_status::bad_address, 0};
  }
  if (replace && *replace >= favorites.size()) {
    return {favorite_status::missing, 0};
  }
  for (std::size_t i = 0; i < favorites.size(); ++i) {
    if ((!replace || i != *replace) &&
        same_server(favorites[i].address, trimmed)) {
      return {favorite_status::duplicate, i};
    }
  }
  favorite_server entry{clean_server_name(name, trimmed), trimmed};
  if (replace) {
    favorites[*replace] = std::move(entry);
    return {favorite_status::saved, *replace};
  }
  if (favorites.size() >= max_favorite_servers) {
    return {favorite_status::full, 0};
  }
  favorites.push_back(std::move(entry));
  return {favorite_status::saved, favorites.size() - 1};
}
} // namespace xerces_core
