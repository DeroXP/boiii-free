#include "game_files.hpp"

#include <algorithm>
#include <map>
#include <sstream>

#include "json.hpp"

namespace xerces_core {
namespace {
std::string trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

std::string lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  });
  return text;
}
} // namespace

bool valid_bind_key(std::string_view key) {
  if (key.empty() || key.size() > 32) {
    return false;
  }
  return std::ranges::none_of(key, [](unsigned char c) {
    return c <= ' ' || c == '"' || c == ';' || c == 0x7F;
  });
}

bool valid_bind_command(std::string_view command) {
  return !command.empty() && command.size() <= 512 &&
         command.find_first_of("\r\n\"") == std::string_view::npos;
}

std::vector<game_bind> parse_binds(std::string_view text) {
  std::map<std::string, std::string> binds;
  std::istringstream stream{std::string(text)};
  for (std::string line; std::getline(stream, line);) {
    line = trim(line);
    if (line.size() < 7 || lower(line.substr(0, 5)) != "bind ") {
      continue;
    }
    const std::string rest = line.substr(5);
    const auto separator = rest.find(' ');
    if (separator == std::string::npos) {
      continue;
    }
    const std::string key = lower(trim(rest.substr(0, separator)));
    std::string command = trim(rest.substr(separator + 1));
    if (command.size() >= 2 && command.front() == '"' && command.back() == '"') {
      command = command.substr(1, command.size() - 2);
    }
    if (!key.empty() && !command.empty()) {
      binds[key] = command;
    }
  }
  std::vector<game_bind> out;
  for (auto &[key, command] : binds) {
    out.push_back({key, command});
  }
  return out;
}

std::string binds_to_text(const std::vector<game_bind> &binds) {
  std::map<std::string, std::string> sorted;
  for (const auto &bind : binds) {
    const std::string key = lower(bind.key);
    // What parse_binds reads back from the line written below.
    const bool reads_back =
        !key.empty() && key == trim(key) &&
        key.find_first_of(" \r\n") == std::string::npos &&
        !bind.command.empty() &&
        bind.command.find_first_of("\r\n") == std::string::npos;
    if (reads_back) {
      sorted[key] = bind.command;
    }
  }
  std::string out;
  for (const auto &[key, command] : sorted) {
    out += "bind " + key + " \"" + command + "\"\n";
  }
  return out;
}

bool asset_pools_enabled(std::string_view launcher_settings_json) {
  const auto parsed = json::parse(launcher_settings_json);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return true; // the client's default
  }
  const auto enabled = json::get_string(parsed.document, "asset_limits_enabled");
  if (enabled && !enabled->empty() && *enabled != "1") {
    return false;
  }
  return json::get_string(parsed.document, "disable_asset_pools").value_or("") !=
         "1";
}

std::string set_asset_pools_enabled(std::string_view launcher_settings_json,
                                    bool enabled) {
  auto parsed = json::parse(launcher_settings_json);
  rapidjson::Document doc;
  if (parsed.ok && parsed.document.IsObject()) {
    doc.Swap(parsed.document);
  } else {
    doc.SetObject();
  }
  doc.RemoveMember("disable_asset_pools");
  json::set(doc, "asset_limits_enabled", enabled ? "1" : "0",
            doc.GetAllocator());
  return json::pretty(doc);
}
} // namespace xerces_core
