#include "workshop_get.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <map>
#include <thread>

#include "json.hpp"

namespace xerces_core::workshop_get {
namespace {
using namespace std::chrono_literals;
using clock = std::chrono::steady_clock;

// EItemState (isteamugc.h, Steamworks SDK 1.37, the version of the game's
// own steam_api64.dll)
constexpr std::uint32_t state_subscribed = 1;
constexpr std::uint32_t state_installed = 4;
constexpr std::uint32_t state_needs_update = 8;
constexpr std::uint32_t state_downloading = 16;
constexpr std::uint32_t state_download_pending = 32;

// The Workshop interface the game's steam_api64.dll was built with: its flat
// functions call through that version's table.
constexpr const char *ugc_interface = "STEAMUGC_INTERFACE_VERSION008";

std::string field(const rapidjson::Value &doc, const char *name) {
  return json::get_string(doc, name).value_or("");
}

std::uint64_t number(const rapidjson::Value &doc, const char *name) {
  const auto *value = json::member(doc, name);
  if (value && value->IsUint64()) {
    return value->GetUint64();
  }
  if (value && value->IsString()) {
    std::uint64_t out = 0;
    const std::string text = value->GetString();
    std::from_chars(text.data(), text.data() + text.size(), out);
    return out;
  }
  return 0;
}

// The flat Steamworks functions this needs (steam_api_flat.h, SDK 1.37).
struct steam_api {
  HMODULE module = nullptr;
  bool(__cdecl *init)() = nullptr;
  void(__cdecl *shutdown)() = nullptr;
  void(__cdecl *run_callbacks)() = nullptr;
  void *(__cdecl *client)() = nullptr;
  std::int32_t(__cdecl *user)() = nullptr;
  std::int32_t(__cdecl *pipe)() = nullptr;
  void *(__cdecl *get_ugc)(std::intptr_t, std::int32_t, std::int32_t,
                           const char *) = nullptr;
  std::uint64_t(__cdecl *subscribe)(std::intptr_t, std::uint64_t) = nullptr;
  std::uint32_t(__cdecl *item_state)(std::intptr_t, std::uint64_t) = nullptr;
  bool(__cdecl *download_info)(std::intptr_t, std::uint64_t, std::uint64_t *,
                               std::uint64_t *) = nullptr;
  bool(__cdecl *download)(std::intptr_t, std::uint64_t, bool) = nullptr;

  template <class T> bool get(T &out, const char *name) {
    out = reinterpret_cast<T>(GetProcAddress(module, name));
    return out != nullptr;
  }

  bool load(const std::filesystem::path &dll, std::string *error) {
    module =
        LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
      *error = "Black Ops III's steam_api64.dll was not found in the game "
               "folder.";
      return false;
    }
    const bool ok =
        get(init, "SteamAPI_Init") && get(shutdown, "SteamAPI_Shutdown") &&
        get(run_callbacks, "SteamAPI_RunCallbacks") &&
        get(client, "SteamClient") && get(user, "SteamAPI_GetHSteamUser") &&
        get(pipe, "SteamAPI_GetHSteamPipe") &&
        get(get_ugc, "SteamAPI_ISteamClient_GetISteamUGC") &&
        get(subscribe, "SteamAPI_ISteamUGC_SubscribeItem") &&
        get(item_state, "SteamAPI_ISteamUGC_GetItemState") &&
        get(download_info, "SteamAPI_ISteamUGC_GetItemDownloadInfo") &&
        get(download, "SteamAPI_ISteamUGC_DownloadItem");
    if (!ok) {
      *error = "The game's steam_api64.dll lacks Steam's Workshop functions.";
    }
    return ok;
  }

  ~steam_api() {
    if (module) {
      FreeLibrary(module);
    }
  }
};

struct item {
  std::uint64_t id = 0;
  bool subscribe_asked = false;
  item_phase phase = item_phase::none;
  std::uint64_t done = 0;
  std::uint64_t total = 0;
  bool finished = false;
  bool failed = false;
  clock::time_point moved{};       // when it last changed
  clock::time_point download_at{}; // when DownloadItem was last asked
};
} // namespace

std::string to_json(const progress &value) {
  std::string out = "{\"id\":" + json::quote(std::to_string(value.id)) +
                    ",\"state\":" + json::quote(value.state);
  if (value.total || value.done) {
    out += ",\"done\":" + std::to_string(value.done) +
           ",\"total\":" + std::to_string(value.total);
  }
  if (!value.message.empty()) {
    out += ",\"message\":" + json::quote(value.message);
  }
  return out + "}";
}

std::optional<progress> parse_progress(std::string_view line) {
  const auto parsed = json::parse(line);
  if (!parsed.ok || !parsed.document.IsObject()) {
    return std::nullopt;
  }
  progress out;
  out.state = field(parsed.document, "state");
  if (out.state.empty()) {
    return std::nullopt;
  }
  out.id = number(parsed.document, "id");
  out.done = number(parsed.document, "done");
  out.total = number(parsed.document, "total");
  out.message = field(parsed.document, "message");
  return out;
}

item_phase phase_of(const std::uint32_t state, const bool subscribe_asked) {
  if ((state & state_installed) &&
      !(state &
        (state_needs_update | state_downloading | state_download_pending))) {
    return item_phase::installed;
  }
  if (state & state_downloading) {
    return item_phase::downloading;
  }
  if (state & (state_download_pending | state_needs_update)) {
    return item_phase::queued;
  }
  if (state & state_subscribed) {
    return item_phase::queued;
  }
  return subscribe_asked ? item_phase::subscribing : item_phase::none;
}

const char *to_string(const item_phase phase) {
  switch (phase) {
  case item_phase::subscribing:
    return "subscribing";
  case item_phase::queued:
    return "queued";
  case item_phase::downloading:
    return "downloading";
  case item_phase::installed:
    return "installed";
  case item_phase::none:
    break;
  }
  return "subscribing";
}

std::optional<std::vector<std::uint64_t>> parse_ids(std::string_view text) {
  std::vector<std::uint64_t> out;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find_first_of(", ", start);
    const std::string_view part = text.substr(
        start,
        end == std::string_view::npos ? std::string_view::npos : end - start);
    if (!part.empty()) {
      std::uint64_t id = 0;
      const auto [ptr, ec] =
          std::from_chars(part.data(), part.data() + part.size(), id);
      if (ec != std::errc() || ptr != part.data() + part.size() || id == 0) {
        return std::nullopt;
      }
      if (std::ranges::find(out, id) == out.end()) {
        out.push_back(id);
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  if (out.empty()) {
    return std::nullopt;
  }
  return out;
}

int run(const options &value) {
  const auto report = [&](progress line) {
    if (value.report) {
      value.report(line);
    }
  };
  steam_api steam;
  std::string error;
  if (!steam.load(value.game_dir / "steam_api64.dll", &error)) {
    report({0, "error", 0, 0, error});
    return 2;
  }
  // The Steamworks API starts as the app this names: Black Ops III, which
  // Steam allows only to an account that owns it. This process only.
  const std::wstring app = std::to_wstring(black_ops_3_app_id);
  SetEnvironmentVariableW(L"SteamAppId", app.c_str());
  SetEnvironmentVariableW(L"SteamGameId", app.c_str());
  if (!steam.init()) {
    report({0, "error", 0, 0,
            "Steam did not start the Workshop download. Is Steam open and "
            "signed in to the account that owns Black Ops III?"});
    return 2;
  }
  const auto client = reinterpret_cast<std::intptr_t>(steam.client());
  const auto ugc = reinterpret_cast<std::intptr_t>(
      client ? steam.get_ugc(client, steam.user(), steam.pipe(), ugc_interface)
             : nullptr);
  if (!ugc) {
    steam.shutdown();
    report({0, "error", 0, 0, "Steam's Workshop interface did not answer."});
    return 2;
  }
  report({0, "ready", 0, 0, ""});

  std::vector<item> items;
  const auto now = clock::now();
  auto any_moved = now;
  for (const std::uint64_t id : value.ids) {
    item entry;
    entry.id = id;
    entry.moved = now;
    items.push_back(entry);
  }

  for (;;) {
    steam.run_callbacks();
    const auto at = clock::now();
    bool all_done = true;
    for (item &entry : items) {
      if (entry.finished) {
        continue;
      }
      const std::uint32_t state = steam.item_state(ugc, entry.id);
      // Subscribing is what Steam's Subscribe button does; Steam then
      // downloads the item and keeps it up to date.
      if (!(state & state_subscribed) && !entry.subscribe_asked) {
        steam.subscribe(ugc, entry.id);
        entry.subscribe_asked = true;
      }
      const item_phase phase = phase_of(state, entry.subscribe_asked);
      if ((state & state_subscribed) && phase != item_phase::installed &&
          phase != item_phase::downloading && at - entry.download_at >= 10s) {
        // Ask Steam to fetch it now rather than in its own time
        steam.download(ugc, entry.id, true);
        entry.download_at = at;
      }
      std::uint64_t done = entry.done;
      std::uint64_t total = entry.total;
      if (phase == item_phase::downloading || phase == item_phase::queued) {
        steam.download_info(ugc, entry.id, &done, &total);
      }
      if (phase != entry.phase || done != entry.done || total != entry.total) {
        entry.phase = phase;
        entry.done = done;
        entry.total = total;
        entry.moved = at;
        any_moved = at;
        report({entry.id, to_string(phase), done, total, ""});
      }
      if (phase == item_phase::installed) {
        entry.finished = true;
        continue;
      }
      const bool never_subscribed = entry.phase == item_phase::subscribing &&
                                    at - entry.moved >= value.subscribe_limit;
      if (never_subscribed || at - any_moved >= value.stall_limit) {
        entry.finished = true;
        entry.failed = true;
        report({entry.id, "failed", entry.done, entry.total,
                never_subscribed
                    ? "Steam did not subscribe to it: it may have been "
                      "removed from the Workshop or made private."
                    : "Steam stopped downloading it."});
        continue;
      }
      all_done = false;
    }
    if (all_done || (value.cancelled && value.cancelled())) {
      break;
    }
    std::this_thread::sleep_for(500ms);
  }
  steam.shutdown();
  return std::ranges::any_of(
             items,
             [](const item &entry) { return entry.failed || !entry.finished; })
             ? 1
             : 0;
}

int run_helper(const std::filesystem::path &cli,
               const std::filesystem::path &game_dir,
               const std::vector<std::uint64_t> &ids,
               const std::function<void(const progress &)> &on_line,
               std::atomic<void *> *process, std::string *error) {
  std::wstring list;
  for (const std::uint64_t id : ids) {
    list += (list.empty() ? L"" : L",") + std::to_wstring(id);
  }
  std::wstring command = L"\"" + cli.wstring() + L"\" workshop get " + list +
                         L" --game-dir \"" + game_dir.wstring() + L"\"";

  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
    *error = "Windows could not start the download.";
    return -1;
  }
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit,
                           OPEN_EXISTING, 0, nullptr);
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdOutput = write_end;
  startup.StartupInfo.hStdError = nul;
  // Only these two handles are inherited. With plain inheritance the helper
  // took every inheritable handle of the program that started it (the game's
  // UDP socket, a file open at that moment) and kept them for as long as Steam
  // downloaded, past the game's own end (review, 2026-11-13).
  HANDLE inherited[2] = {write_end, nul};
  const DWORD inherited_count = nul != INVALID_HANDLE_VALUE ? 2 : 1;
  SIZE_T attributes_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes_size);
  std::vector<unsigned char> attributes_buffer(attributes_size);
  auto *attributes =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_buffer.data());
  bool have_list =
      attributes_size > 0 &&
      InitializeProcThreadAttributeList(attributes, 1, 0, &attributes_size);
  if (have_list &&
      !UpdateProcThreadAttribute(
          attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
          inherited_count * sizeof(HANDLE), nullptr, nullptr)) {
    DeleteProcThreadAttributeList(attributes);
    have_list = false;
  }
  startup.lpAttributeList = have_list ? attributes : nullptr;
  // And it ends with the program that started it: the download goes on in
  // Steam, but nobody reads the helper's lines any more.
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
      CloseHandle(job);
      job = nullptr;
    }
  }
  PROCESS_INFORMATION info{};
  const bool started =
      CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW | CREATE_SUSPENDED |
                         (have_list ? EXTENDED_STARTUPINFO_PRESENT : 0),
                     nullptr, nullptr, &startup.StartupInfo, &info);
  if (have_list) {
    DeleteProcThreadAttributeList(attributes);
  }
  CloseHandle(write_end);
  if (nul != INVALID_HANDLE_VALUE) {
    CloseHandle(nul);
  }
  if (!started) {
    CloseHandle(read_end);
    if (job) {
      CloseHandle(job);
    }
    *error = "xerces_cli.exe could not be started.";
    return -1;
  }
  if (job && !AssignProcessToJobObject(job, info.hProcess)) {
    CloseHandle(job);
    job = nullptr;
  }
  ResumeThread(info.hThread);
  CloseHandle(info.hThread);
  if (process) {
    process->store(info.hProcess);
  }
  std::string pending;
  char buffer[4096];
  DWORD got = 0;
  while (ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
    pending.append(buffer, got);
    std::size_t end = 0;
    while ((end = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, end);
      pending.erase(0, end + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (const auto parsed = parse_progress(line); parsed && on_line) {
        on_line(*parsed);
      }
    }
  }
  CloseHandle(read_end);
  WaitForSingleObject(info.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(info.hProcess, &code);
  if (process) {
    process->store(nullptr);
  }
  CloseHandle(info.hProcess);
  if (job) {
    CloseHandle(job);
  }
  return static_cast<int>(code);
}
} // namespace xerces_core::workshop_get
