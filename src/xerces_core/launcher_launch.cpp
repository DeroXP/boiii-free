#include "launcher_launch.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>

#include "ipc.hpp"
#include "journal.hpp"
#include "json.hpp"
#include "launch.hpp"
#include "version.hpp"

namespace xerces_core {
namespace fs = std::filesystem;

plan_request profile_request(const launcher_config &config,
                             const std::string &mode,
                             const std::optional<std::uint64_t> &steam_build,
                             const std::string &launcher_settings) {
  plan_request request;
  request.mode = mode;
  request.launcher_settings = launcher_settings;
  if (const auto it = config.profiles.find(mode); it != config.profiles.end()) {
    request.mods = it->second.mods;
    request.map = it->second.map;
    request.pins = it->second.pins;
    request.allowed_conflicts = it->second.allowed_conflicts;
    request.settings = it->second.settings;
  }
  request.saved_positions = config.saved_positions;
  if (steam_build) {
    request.game_build = std::to_string(*steam_build);
  }
  request.xerces_version =
      version::parse(xerces_version_text).value_or(version{});
  request.protocol_version = protocol_version;
  return request;
}

plan resolve_profile(const std::vector<installed_content> &catalog,
                     const launcher_config &config, const std::string &mode,
                     const std::optional<std::uint64_t> &steam_build,
                     const std::vector<std::string> *mods,
                     const std::string &launcher_settings) {
  plan_request request =
      profile_request(config, mode, steam_build, launcher_settings);
  if (mods) {
    request.mods = *mods;
  }
  return resolve_plan(catalog, request);
}

launch_outcome preview_launch(const launch_inputs &in) {
  launch_outcome out;
  const launcher_config &config = *in.config;
  launch_options options;
  options.connect = in.connect;
  options.nointro = config.nointro;
  options.mods_off = in.mods_off;
  options.extra_flags = config.extra_flags;
  for (const auto &[name, value] : config.dvars) {
    options.dvars.emplace_back(name, value);
  }
  if (in.modded && !in.mods_off) {
    out.resolved =
        resolve_profile(*in.catalog, config, in.mode, in.game->steam_build,
                        in.mods, read_launcher_settings(*in.game));
    out.plan_id = out.resolved->plan_id;
    if (!out.resolved->ok()) {
      out.diags.append(out.resolved->diags);
      out.message = "The plan has problems. Open Mods to fix them.";
      out.refusal = launch_refusal::plan;
      return out;
    }
    if (out.resolved->stack.empty()) {
      out.message = "No mods are enabled for this mode. Enable some in Mods, "
                    "or launch vanilla.";
      out.diags.error("launch.plan.empty", "", out.message);
      out.refusal = launch_refusal::plan;
      return out;
    }
  }
  const launch_command command =
      build_launch_command(out.resolved ? &*out.resolved : nullptr, options);
  out.diags.append(command.diags);
  out.command_line = command.command_line;
  if (!command.ok()) {
    out.message = command.diags.items().empty()
                      ? "The launch command could not be built."
                      : command.diags.items().front().message;
    out.refusal = launch_refusal::command;
  }
  return out;
}

bool game_in_run_folder(const fs::path &home) {
  const auto entries = ipc::live_run_entries(home / "run");
  return std::ranges::any_of(
      entries, [](const ipc::run_entry &e) { return e.role == "game"; });
}

bool game_process_running(const fs::path &client) {
  const std::wstring client_name = client.filename().wstring();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return false;
  }
  const auto named = [](const wchar_t *exe, const std::wstring &name) {
    return !name.empty() &&
           CompareStringOrdinal(exe, -1, name.c_str(), -1, TRUE) == CSTR_EQUAL;
  };
  bool found = false;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Process32FirstW(snapshot, &entry); more && !found;
       more = Process32NextW(snapshot, &entry)) {
    found = named(entry.szExeFile, client_name) ||
            named(entry.szExeFile, L"BlackOps3.exe");
  }
  CloseHandle(snapshot);
  return found;
}

namespace {
std::wstring upper(std::wstring_view text) {
  std::wstring out(text);
  for (wchar_t &c : out) {
    c = static_cast<wchar_t>(std::towupper(c));
  }
  return out;
}

// The name of "NAME=value"; a drive entry ("=C:=C:\x") keeps its first '='.
std::wstring_view entry_name(std::wstring_view entry) {
  const std::size_t eq = entry.find(L'=', 1);
  return eq == std::wstring_view::npos ? entry : entry.substr(0, eq);
}

std::wstring utf16(std::string_view text) {
  return path_from_utf8(text).wstring();
}
} // namespace

std::wstring child_environment(const wchar_t *block,
                               const environment_changes &changes) {
  std::vector<std::wstring> drives;
  std::vector<std::wstring> entries;
  for (const wchar_t *at = block; at && *at;) {
    const std::wstring entry(at);
    at += entry.size() + 1;
    if (entry.starts_with(L'=')) {
      drives.push_back(entry);
      continue;
    }
    const std::wstring name = upper(entry_name(entry));
    const bool changed = std::ranges::any_of(changes, [&](const auto &change) {
      return upper(change.first) == name;
    });
    if (!changed) {
      entries.push_back(entry);
    }
  }
  for (const auto &[name, value] : changes) {
    if (value) {
      entries.push_back(name + L"=" + *value);
    }
  }
  std::ranges::stable_sort(entries,
                           [](const std::wstring &a, const std::wstring &b) {
                             return upper(entry_name(a)) < upper(entry_name(b));
                           });
  std::wstring out;
  for (const auto &entry : drives) {
    out += entry;
    out.push_back(L'\0');
  }
  for (const auto &entry : entries) {
    out += entry;
    out.push_back(L'\0');
  }
  if (out.empty()) {
    out.push_back(L'\0'); // an empty block is two NULs
  }
  out.push_back(L'\0');
  return out;
}

launch_outcome launch_game(const launch_inputs &in,
                           const std::function<bool()> &game_running,
                           const event_log &log, void **process) {
  const launcher_paths &paths = *in.paths;
  const game_state &game = *in.game;
  launch_outcome out = preview_launch(in);
  if (out.diags.has_errors()) {
    emit(log, log_level::warn, "launch.refused", out.message);
    return out;
  }
  const auto refuse = [&](launch_refusal why, const char *code,
                          const std::string &message) {
    out.message = message;
    out.diags.error(code, "", message);
    out.refusal = why;
    emit(log, log_level::warn, "launch.refused", message);
    return out;
  };
  // The game must be found and supported, dry or not: Home, Servers and a
  // culprit test refuse there, and a dry launch shows the same refusal.
  if (!game.can_launch()) {
    const std::string why =
        !game.dir ? "Black Ops III was not found. Set its folder in Settings."
        : game.preflight && !game.preflight->message.empty()
            ? game.preflight->message
        : !game.error.empty()
            ? game.error
            : std::string("This game build is not supported.");
    return refuse(launch_refusal::game, "launch.game", why);
  }
  if (game_running && game_running()) {
    return refuse(launch_refusal::running, "launch.running",
                  "The game is already running.");
  }
  // A missing client only stops a real launch; a dry one starts nothing, so
  // a checkout without boiii.exe can still check its command lines, and it
  // says what a real launch would refuse.
  std::error_code ec;
  const std::string client_name = path_utf8(paths.client.filename());
  const std::string program_name = paths.program.empty()
                                       ? std::string("Xerces.exe")
                                       : path_utf8(paths.program.filename());
  std::string would_stop;
  if (!fs::exists(paths.client, ec)) {
    if (!in.dry_launch) {
      return refuse(launch_refusal::client, "launch.client",
                    "The game client (" + client_name +
                        ") is missing next to " + program_name + ".");
    }
    would_stop = client_name + " is missing";
  }

  // The plan file the client reads (5.4) and the journal (5.6).
  launch_journal journal;
  journal.phase = launch_phase::launching;
  journal.started = journal.updated = json_log::now_utc();
  journal.safe_mode = in.mods_off;
  journal.mode = in.mode;
  journal.launch_id = new_launch_id();
  if (out.resolved) {
    const plan &resolved = *out.resolved;
    out.plan_file = paths.plans / (plan_file_id(resolved.plan_id) + ".json");
    if (!write_text_file(out.plan_file, plan_to_json(resolved, true))) {
      out.diags.warning("launch.plan_file", path_utf8(out.plan_file),
                        "The plan file could not be written.");
      emit(log, log_level::warn, "launch.plan_file",
           "The plan file could not be written: " + path_utf8(out.plan_file));
    }
    journal.plan_id = resolved.plan_id;
    for (const auto &entry : resolved.stack) {
      journal.stack.push_back(entry.id);
    }
  } else if (in.mods_off) {
    if (const auto text = read_text_file(paths.journal_file())) {
      if (const auto last = parse_journal(*text)) {
        journal.plan_id = last->plan_id;
        journal.stack = last->stack;
      }
    }
  }
  if (in.dry_launch) {
    out.dry_run = true;
    out.message = would_stop.empty()
                      ? "Dry launch: the command was written to the log."
                      : "Dry launch: the command was written to the log (a "
                        "real launch would stop: " +
                            would_stop + ").";
    emit(log, log_level::info, "launch.dry", out.command_line,
         "{\"plan_id\":" + json::quote(out.plan_id) + "}");
    return out; // no journal: nothing ran that could crash
  }
  // Kept so a launch Windows refuses can put it back (below).
  const std::optional<std::string> previous_journal =
      read_text_file(paths.journal_file());
  write_text_file(paths.journal_file(), journal_to_json(journal));

  // The game role finds <home> and, in a development build, its data folder
  // through the environment it inherits, and the launch id that lets it take
  // this journal over. Only the child gets them: nothing else this program
  // opens (Steam, Explorer) may carry the launch id.
  environment_changes changes = {
      {L"XERCES_HOME", paths.home.wstring()},
      {L"XERCES_LAUNCH_ID", utf16(journal.launch_id)}};
  if (paths.dev_data) {
    changes.emplace_back(L"XERCES_DATA_PATH", paths.dev_data->wstring());
  }
  std::wstring environment;
  if (wchar_t *current = GetEnvironmentStringsW()) {
    environment = child_environment(current, changes);
    FreeEnvironmentStringsW(current);
  } else {
    environment = child_environment(nullptr, changes);
  }
  std::wstring command = L"\"" + paths.client.wstring() + L"\" ";
  command += utf16(out.command_line);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION started{};
  const std::wstring cwd = game.dir->wstring();
  const BOOL created =
      CreateProcessW(paths.client.wstring().c_str(), command.data(), nullptr,
                     nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                     environment.data(), cwd.c_str(), &startup, &started);
  const DWORD error = GetLastError();
  if (!created) {
    // Nothing ran, so nothing crashed: put the journal back as it was. Left
    // as "launching" it would read as a crash next time, and it would have
    // replaced the record of a real crash.
    if (previous_journal) {
      write_text_file(paths.journal_file(), *previous_journal);
    } else {
      fs::remove(paths.journal_file(), ec);
    }
    out.message = "Windows could not start the game (error " +
                  std::to_string(error) + ").";
    out.diags.error("launch.start", "", out.message);
    out.refusal = launch_refusal::start;
    emit(log, log_level::error, "launch.failed", out.message);
    return out;
  }
  CloseHandle(started.hThread);
  if (process) {
    *process = started.hProcess;
  } else {
    CloseHandle(started.hProcess);
  }
  out.started = true;
  out.pid = started.dwProcessId;
  out.message = in.modded && !in.mods_off ? "Starting Black Ops III with mods."
                                          : "Starting Black Ops III.";
  emit(log, log_level::info, "launch.started", out.command_line,
       "{\"pid\":" + std::to_string(out.pid) +
           ",\"plan_id\":" + json::quote(out.plan_id) + "}");
  return out;
}
} // namespace xerces_core
