#include "launch.hpp"

#include <algorithm>

#include "json.hpp"

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

bool equals_case_insensitive(std::string_view a, std::string_view b) {
  return std::ranges::equal(a, b, [](char x, char y) {
    const auto fold = [](char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
    };
    return fold(x) == fold(y);
  });
}

// The engine names a dvar by a hash of its lower-cased name, and reads a name
// that starts with "0x" as that hash written out (Dvar_GenerateHash). So
// "FS_GAME" and "0x<hash of fs_game>" both reach fs_game; a hashed name could
// be any dvar, and the launcher cannot tell which, so it is refused.
bool hashed_dvar_name(std::string_view name) {
  return name.size() >= 2 && name[0] == '0' &&
         (name[1] == 'x' || name[1] == 'X');
}

bool safe_dvar_name(std::string_view name) {
  if (name.empty() || name.size() > 64 || hashed_dvar_name(name)) {
    return false;
  }
  return std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  });
}

// Only the plan sets the mod dvars, in any case the engine accepts.
bool plan_dvar_name(std::string_view name) {
  return equals_case_insensitive(name, "fs_game") ||
         equals_case_insensitive(name, "fs_extramods");
}

// A value the engine reads back unchanged: no '+' (a new command), no '"'
// (ends the quoted value), no ';' (the engine's command buffer runs what
// follows it as another command), no "//" (its tokenizer drops the rest of
// the line as a comment), no control characters, and not empty. Values are
// not quoted to protect a ';' instead: how the engine treats quotes inside a
// "+set" value has not been checked in the game.
bool safe_value(std::string_view value) {
  if (value.empty() || value.size() > 1024 ||
      value.find("//") != std::string_view::npos) {
    return false;
  }
  return std::ranges::none_of(value, [](char c) {
    return c == '+' || c == '"' || c == ';' ||
           static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
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
                            "'\"', ',', ';' or control characters). Rename "
                            "the folder.");
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
    if (hashed_dvar_name(name)) {
      out.diags.error("launch.dvar.invalid", name,
                      "The setting " + name +
                          " cannot be passed to the game: a name starting "
                          "with 0x is a hashed dvar name, and it could be "
                          "any dvar. Use the dvar's real name.");
      continue;
    }
    if (!safe_dvar_name(name) || !safe_value(value)) {
      out.diags.error("launch.dvar.invalid", name,
                      "The setting " + name +
                          " cannot be passed to the game (its value holds "
                          "'+', '\"', ';', '//' or control characters, or "
                          "is empty).");
      continue;
    }
    if (plan_dvar_name(name)) {
      continue; // only the plan sets these, also in safe mode (mods_off)
    }
    out.args.push_back("+set");
    out.args.push_back(name);
    out.args.push_back(value);
  }

  if (options.connect) {
    // valid_server_address allows only host, IP and port characters, so no
    // ';', '+', '"' or '/' reaches the command line through it either.
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

std::optional<plan_mod_dvars> read_plan_mod_dvars(std::string_view plan_json) {
  const auto parsed = json::parse(plan_json);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  const auto *stack = json::member(parsed.document, "stack");
  if (stack == nullptr || !stack->IsArray()) {
    return std::nullopt;
  }
  plan_mod_dvars out;
  out.plan_id = json::get_string(parsed.document, "plan_id").value_or("");
  bool first = true;
  for (const auto &entry : stack->GetArray()) {
    if (!entry.IsObject()) {
      return std::nullopt;
    }
    const auto folder = json::get_string(entry, "folder");
    if (!folder || folder->empty()) {
      return std::nullopt;
    }
    if (first) {
      out.fs_game = *folder;
      first = false;
      continue;
    }
    if (!out.fs_extramods.empty()) {
      out.fs_extramods.push_back(',');
    }
    out.fs_extramods += *folder;
  }
  return out;
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

bool has_launch_flag(const std::vector<std::string> &args,
                     std::string_view flag) {
  for (const std::string &arg : args) {
    if (!arg.empty() && arg[0] == '+') {
      return false; // flags only come before the first command
    }
    if (arg == flag) {
      return true;
    }
  }
  return false;
}
} // namespace xerces_core
