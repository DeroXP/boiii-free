#include "launch.hpp"

#include <algorithm>

namespace xerces_core {
namespace {
bool safe_flag(std::string_view flag) {
  if (flag.size() < 2 || flag.size() > 64 || flag[0] != '-') {
    return false;
  }
  return std::ranges::all_of(flag.substr(1), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

bool safe_dvar_name(std::string_view name) {
  if (name.empty() || name.size() > 64) {
    return false;
  }
  return std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  });
}

// A value the engine reads back unchanged: no '+' (a new command), no '"'
// (ends the quoted value), no control characters, and not empty.
bool safe_value(std::string_view value) {
  if (value.empty() || value.size() > 1024) {
    return false;
  }
  return std::ranges::none_of(value, [](char c) {
    return c == '+' || c == '"' || static_cast<unsigned char>(c) < 0x20 ||
           c == 0x7F;
  });
}

bool hex_id(std::string_view text) {
  return !text.empty() && text.size() <= 128 &&
         std::ranges::all_of(text, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
} // namespace

std::string quote_argument(std::string_view arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) {
    return std::string(arg);
  }
  std::string out = "\"";
  std::size_t backslashes = 0;
  for (const char c : arg) {
    if (c == '\\') {
      ++backslashes;
      continue;
    }
    if (c == '"') {
      out.append(backslashes * 2 + 1, '\\');
    } else {
      out.append(backslashes, '\\');
    }
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2, '\\');
  out.push_back('"');
  return out;
}

launch_command build_launch_command(const plan *resolved,
                                    const launch_options &options) {
  launch_command out;
  out.args.push_back("-launch");
  if (options.nointro) {
    out.args.push_back("-nointro");
  }
  if (options.safe) {
    out.args.push_back("-safe");
  }
  for (const std::string &flag : options.extra_flags) {
    if (!safe_flag(flag)) {
      out.diags.error("launch.flag.invalid", "",
                      "\"" + flag +
                          "\" is not a launch flag: flags are a '-' then "
                          "letters, digits, '-' or '_'.");
      continue;
    }
    if (flag == "-launch" || flag == "-xerces-plan") {
      continue; // set by the launcher itself
    }
    out.args.push_back(flag);
  }

  const bool with_mods = resolved != nullptr && !options.mods_off;
  if (with_mods) {
    if (!resolved->ok()) {
      out.diags.error("launch.plan.errors", "",
                      "The plan has errors; fix them before launching.");
    } else if (resolved->stack.empty()) {
      out.diags.error("launch.plan.empty", "",
                      "The plan has no mods; launch vanilla instead.");
    }
    const std::string id = plan_file_id(resolved->plan_id);
    if (!hex_id(id)) {
      out.diags.error("launch.plan.id", "", "The plan has no valid plan id.");
    } else {
      out.args.push_back("-xerces-plan");
      out.args.push_back(id);
    }
    for (const plan_entry &entry : resolved->stack) {
      if (!safe_value(entry.folder) ||
          entry.folder.find(',') != std::string::npos) {
        out.diags.error("launch.folder.invalid", entry.id,
                        "The folder name \"" + entry.folder + "\" of " +
                            entry.id +
                            " cannot be passed to the game (it holds '+', "
                            "'\"', ',' or control characters). Rename the "
                            "folder.");
      }
    }
  }

  if (with_mods && out.ok()) {
    out.args.push_back("+set");
    out.args.push_back("fs_game");
    out.args.push_back(resolved->fs_game());
    const std::string extra = resolved->fs_extramods();
    if (!extra.empty()) {
      out.args.push_back("+set");
      out.args.push_back("fs_extramods");
      out.args.push_back(extra);
    }
  }
  for (const auto &[name, value] : options.dvars) {
    if (!safe_dvar_name(name) || !safe_value(value)) {
      out.diags.error("launch.dvar.invalid", name,
                      "The setting " + name +
                          " cannot be passed to the game (its value holds "
                          "'+', '\"' or control characters, or is empty).");
      continue;
    }
    if (name == "fs_game" || name == "fs_extramods") {
      continue; // only the plan sets these
    }
    out.args.push_back("+set");
    out.args.push_back(name);
    out.args.push_back(value);
  }

  if (options.connect) {
    if (!valid_server_address(*options.connect)) {
      out.diags.error("launch.connect.invalid", "",
                      "\"" + *options.connect +
                          "\" is not a server address (host:port).");
    } else {
      out.args.push_back("+connect");
      out.args.push_back(*options.connect);
    }
  }

  if (!out.ok()) {
    out.args.clear();
    return out;
  }
  for (std::size_t i = 0; i < out.args.size(); ++i) {
    if (i != 0) {
      out.command_line.push_back(' ');
    }
    out.command_line += quote_argument(out.args[i]);
  }
  return out;
}

bool valid_server_address(std::string_view address) {
  if (address.empty() || address.size() > 260) {
    return false;
  }
  std::string_view host = address;
  std::string_view port;
  if (address.front() == '[') {
    const std::size_t close = address.find(']');
    if (close == std::string_view::npos || close < 3) {
      return false;
    }
    host = address.substr(1, close - 1);
    const std::string_view rest = address.substr(close + 1);
    if (!rest.empty()) {
      if (rest.front() != ':') {
        return false;
      }
      port = rest.substr(1);
      if (port.empty()) {
        return false;
      }
    }
    if (!std::ranges::all_of(host, [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                 (c >= 'A' && c <= 'F') || c == ':' || c == '.';
        })) {
      return false;
    }
  } else {
    const std::size_t colon = address.rfind(':');
    if (colon != std::string_view::npos) {
      host = address.substr(0, colon);
      port = address.substr(colon + 1);
      if (port.empty()) {
        return false;
      }
    }
    if (host.empty() || host.front() == '-' || host.front() == '.' ||
        !std::ranges::all_of(host, [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '-';
        })) {
      return false;
    }
  }
  if (!port.empty()) {
    if (port.size() > 5 || !std::ranges::all_of(port, [](char c) {
          return c >= '0' && c <= '9';
        })) {
      return false;
    }
    const int value = std::stoi(std::string(port));
    if (value < 1 || value > 65535) {
      return false;
    }
  }
  return true;
}

std::string plan_file_id(std::string_view plan_id) {
  constexpr std::string_view prefix = "sha256:";
  if (plan_id.starts_with(prefix)) {
    plan_id.remove_prefix(prefix.size());
  }
  return std::string(plan_id);
}

std::optional<std::string> flag_value(const std::vector<std::string> &args,
                                      std::string_view flag) {
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (!args[i].empty() && args[i][0] == '+') {
      break; // flags only come before the first command
    }
    if (args[i] == flag) {
      return args[i + 1];
    }
  }
  return std::nullopt;
}
} // namespace xerces_core
