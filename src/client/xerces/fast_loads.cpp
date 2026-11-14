#include <std_include.hpp>

// -xerces-fast-loads (opt-in; docs/research/engine-re.md, "A map's load"):
//
// 1. The loading screen draws every frame. The engine's screen update
//    (SCR_UpdateScreen, 0x1413E1570) draws a frame only once 16 ms have passed
//    since the last while the local client's connection is not active (33 ms
//    in its last two states before that), and the texture streamer asks for
//    its next reads from that screen update: the images that stream in behind
//    the loading screen come at that pace. Both waits are set to 0 (the two
//    instructions, mov ebx, 16 at 0x1413E15A4 and mov ecx, 33 at 0x1413E15A9,
//    are checked first; anything else there is left alone and logged).
//    Custom maps loaded 3 to 12 s sooner with it (docs/TESTING.md).
// 2. A match launched from a lobby with no other player in it does not wait
//    for the others' acknowledgements: lobbyLaunch_waitForClientAckDelay
//    (2,000 ms) and lobbyLaunch_gameLaunchDelay (700 ms) are 0 for that
//    launch, and back to what they were for a launch with others in the
//    lobby (they give a friend's game time to hear of the launch). About 1.9 s
//    before the level starts loading.
// 3. The game's start asks WMI, from a static initializer before WinMain
//    (0x142BC15F8, with root\CIMV2 and root\wmi: a description of the PC),
//    for BitLocker's namespace, root\CIMV2\Security\MicrosoftVolumeEncryption.
//    A user who is not an administrator gets only "access denied", and
//    Windows takes 5 s to say so (5,019 ms measured on this PC): the start
//    stood still that long. While the process is not elevated, that one
//    ConnectServer now answers WBEM_E_ACCESS_DENIED at once, the answer the
//    game got anyway, 5 s sooner. The game's import of CoCreateInstance is
//    pointed here before its entry point runs (post_load); every other COM
//    object, and every other WMI call, is the real one.
// 4. A Zombies level's first seconds run 4x on our own server while the
//    local client waits for it. The new server is up, but the client is let
//    go only once its level.time reaches about 4.95 s on Shadows of Evil,
//    4.8 s on Town Reimagined and MW2 Rust, 3.15 s on Origins: a wait on the
//    game's clock, not work (no game thread busy: only the protection's
//    worker 0, not in step with it; no dvar changes, the online service is
//    quiet; timescale 2 or 4 shortens it, 1.01 does not). Most of it is
//    sv_reconnectlimit (3 s): the server's handler of the client's join
//    ("cfl", 0x1421ECAC0) ignores it until that much of its time has passed
//    since the client's last connect. That limit is left as it is: at 0 the
//    host was in at level.time 150 (Origins) and 1,950 (Shadows of Evil),
//    sooner than the game ever lets anyone in, which the level's scripts
//    may not expect; at 4x the client arrives at the usual level.time. The
//    texture streaming that 1. speeds up used to hide in it. So while the
//    local client is in CONFIRMLOADING, the server is ours, a level's (not
//    the menus') and in GAME, the mode is Zombies, nobody else is in the
//    lobby and timescale is 1, timescale is 4; it is 1 again as soon as any
//    of that ends (20 s at most, once a wait; a timescale something else
//    changed meanwhile is left as it is). The server still steps 50 ms of
//    game time a frame, only more of them a second, and no player is in the
//    level yet. Shadows of Evil from the menus: in the level 3.3 to 4.1 s
//    sooner (8.8 to 9.6 s from `map`, against 12.9 s with 1.-3. alone and
//    14.6 s without the flag); Town Reimagined and MW2 Rust 2.3 s sooner
//    with mods off. Not without 1.: the streaming then takes its time after
//    the wait instead, and the level started 2 s later.

#include "fast_loads.hpp"

#include <wbemcli.h>

#include <cwctype>

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include <utils/hook.hpp>
#include <utils/nt.hpp>

#include <xerces_core/launch.hpp>

#include "runtime.hpp"

namespace xerces_fast_loads {
namespace {
constexpr std::string_view flag = "-xerces-fast-loads";
constexpr std::uintptr_t wait_loading = 0x1413E15A4; // mov ebx, 0x10
constexpr std::uintptr_t wait_late = 0x1413E15A9;    // mov ecx, 0x21

constexpr const char *ack_delay = "lobbyLaunch_waitForClientAckDelay";
constexpr const char *launch_delay = "lobbyLaunch_gameLaunchDelay";

bool enabled = false;
// The lobby's own values, read before each change of ours (main thread only)
std::optional<std::int32_t> ack_delay_before;
std::optional<std::int32_t> launch_delay_before;
// Our zeros are in the dvars now (put back by restore_waits)
bool zeroed = false;

// ---------------------------------------------------------------- WMI
using co_create_fn = HRESULT(WINAPI *)(REFCLSID, LPUNKNOWN, DWORD, REFIID,
                                       LPVOID *);
co_create_fn game_co_create = nullptr;
std::atomic<int> wmi_denied{0};
bool wmi_hooked = false;

bool process_elevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  const bool ok = GetTokenInformation(token, TokenElevation, &elevation,
                                      sizeof(elevation), &size) != FALSE;
  CloseHandle(token);
  return ok && elevation.TokenIsElevated != 0;
}

bool bitlocker_namespace(const BSTR resource) {
  if (!resource) {
    return false;
  }
  std::wstring lower(resource);
  for (wchar_t &c : lower) {
    c = static_cast<wchar_t>(std::towlower(c));
  }
  return lower.find(L"microsoftvolumeencryption") != std::wstring::npos;
}

// The game's WMI locator, but for BitLocker's namespace without the rights
// to it (see 3. above)
class locator_proxy final : public IWbemLocator {
public:
  explicit locator_proxy(IWbemLocator *real) : real_(real) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) {
      return E_POINTER;
    }
    if (IsEqualIID(iid, IID_IUnknown) ||
        IsEqualIID(iid, __uuidof(IWbemLocator))) {
      *out = static_cast<IWbemLocator *>(this);
      AddRef();
      return S_OK;
    }
    return real_->QueryInterface(iid, out);
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (left == 0) {
      real_->Release();
      delete this;
    }
    return left;
  }
  HRESULT STDMETHODCALLTYPE ConnectServer(const BSTR resource, const BSTR user,
                                          const BSTR password,
                                          const BSTR locale, long flags,
                                          const BSTR authority,
                                          IWbemContext *context,
                                          IWbemServices **services) override {
    if (bitlocker_namespace(resource) && !process_elevated()) {
      if (services) {
        *services = nullptr;
      }
      ++wmi_denied;
      return WBEM_E_ACCESS_DENIED;
    }
    return real_->ConnectServer(resource, user, password, locale, flags,
                                authority, context, services);
  }

private:
  IWbemLocator *real_;
  std::atomic<ULONG> refs_{1};
};

// The game asks for the locator's IUnknown and then for IWbemLocator (seen
// 2026-11-14): either way it gets the proxy, which answers both. Any other
// interface asked for gets the real object.
HRESULT WINAPI co_create_stub(REFCLSID clsid, LPUNKNOWN outer, DWORD context,
                              REFIID iid, LPVOID *out) {
  const HRESULT result = game_co_create(clsid, outer, context, iid, out);
  if (FAILED(result) || !out || !*out || outer ||
      !IsEqualCLSID(clsid, __uuidof(WbemLocator)) ||
      (!IsEqualIID(iid, IID_IUnknown) &&
       !IsEqualIID(iid, __uuidof(IWbemLocator)))) {
    return result;
  }
  auto *got = static_cast<IUnknown *>(*out);
  IWbemLocator *real = nullptr;
  if (FAILED(got->QueryInterface(__uuidof(IWbemLocator),
                                 reinterpret_cast<void **>(&real))) ||
      !real) {
    return result; // not a locator after all: left as it is
  }
  auto *proxy = new (std::nothrow) locator_proxy(real);
  if (!proxy) {
    real->Release();
    return result;
  }
  got->Release();
  *out = static_cast<IWbemLocator *>(proxy);
  return result;
}

// Before the game's entry point: its static initializers make the call
void hook_wmi_locator() {
  for (const char *module :
       {"ole32.dll", "combase.dll", "api-ms-win-core-com-l1-1-0.dll"}) {
    if (void *entry =
            utils::nt::library{}.get_iat_entry(module, "CoCreateInstance")) {
      game_co_create = *static_cast<co_create_fn *>(entry);
      utils::hook::set(entry, &co_create_stub);
      wmi_hooked = true;
      return;
    }
  }
}

bool code_is(const std::uintptr_t address, const std::uint8_t opcode,
             const std::uint32_t value) {
  const auto *code = reinterpret_cast<const std::uint8_t *>(address);
  std::uint32_t immediate = 0;
  std::memcpy(&immediate, code + 1, sizeof(immediate));
  return code[0] == opcode && immediate == value;
}

bool draw_every_frame() {
  const std::uintptr_t loading = game::relocate(wait_loading);
  const std::uintptr_t late = game::relocate(wait_late);
  if (!code_is(loading, 0xBB, 0x10) || !code_is(late, 0xB9, 0x21)) {
    xerces_runtime::log_warn("xerces.fast_loads.screen_off",
                             "The screen update's waits are not the expected "
                             "code; left as they are");
    return false;
  }
  utils::hook::set<std::uint32_t>(loading + 1, 0);
  utils::hook::set<std::uint32_t>(late + 1, 0);
  return true;
}

// The lobby's own waits back once the launch is under way: a later launch
// as a friend's guest reads lobbyLaunch_gameLaunchDelay too (a review,
// 2026-11-14). Main thread.
void restore_waits() {
  if (!zeroed || !ack_delay_before || !launch_delay_before) {
    return;
  }
  game::set_dvar_int(ack_delay, *ack_delay_before, false);
  game::set_dvar_int(launch_delay, *launch_delay_before, false);
  zeroed = false;
}
} // namespace

void before_lobby_launch() {
  if (!enabled || game::is_legacy_client()) {
    return;
  }
  // What the lobby has now, unless our zeros are still in (a mod or the
  // console may have changed them since)
  if (!zeroed) {
    ack_delay_before = game::get_dvar_int(ack_delay);
    launch_delay_before = game::get_dvar_int(launch_delay);
    if (!ack_delay_before || !launch_delay_before) {
      enabled = false; // not this game's dvars: leave the launch alone
      return;
    }
  }
  // No other player in the game lobby nor in the party (private) lobby,
  // whose members may not have moved to the game lobby yet
  const bool alone = game::lobby::LobbyHost_GetClientCount(
                         game::lobby::LobbyType::GAME,
                         game::lobby::LobbyClientType::REMOTE) == 0 &&
                     game::lobby::LobbyHost_GetClientCount(
                         game::lobby::LobbyType::PRIVATE,
                         game::lobby::LobbyClientType::REMOTE) == 0;
  game::set_dvar_int(ack_delay, alone ? 0 : *ack_delay_before, false);
  game::set_dvar_int(launch_delay, alone ? 0 : *launch_delay_before, false);
  zeroed = alone;
  if (alone) {
    // The launch reads them within its first seconds (the level starts
    // loading about 3.6 s after the command)
    scheduler::once(restore_waits, scheduler::main, 15s);
  }
  xerces_runtime::log_info(
      "xerces.fast_loads.launch",
      alone ? "No other player in the lobby: the launch does not wait for "
              "acknowledgements"
            : "Other players in the lobby: the launch waits for them as usual");
}

// ---------------------------------------------------------- level start (4.)
constexpr float level_start_speed = 4.0f;
constexpr auto level_start_longest = std::chrono::seconds(20);
bool level_start_enabled = false;
// What timescale was before our 4 (set while it is in), main thread only
std::optional<float> speed_before;
std::chrono::steady_clock::time_point speed_since{};
int speed_level_time_from = 0;
// This wait has had its 4x (main thread only)
bool level_start_done = false;

bool lobby_alone() {
  return game::lobby::LobbyHost_GetClientCount(
             game::lobby::LobbyType::GAME,
             game::lobby::LobbyClientType::REMOTE) == 0 &&
         game::lobby::LobbyHost_GetClientCount(
             game::lobby::LobbyType::PRIVATE,
             game::lobby::LobbyClientType::REMOTE) == 0;
}

bool level_start_waiting() {
  return game::cg::clientUIActives->actives[0].connectionState ==
             game::connstate_t::CONFIRMLOADING &&
         game::sv::sv->state == game::sv::serverState_t::GAME &&
         !game::sv::sv->isUILevel && // a level, not the menus' own
         game::com::Com_SessionMode_IsMode(game::eModes::ZOMBIES) &&
         lobby_alone();
}

// Every frame, main thread. Once a wait: after it ends (or after 20 s, or
// when a player joins the lobby) not again until the client leaves
// CONFIRMLOADING.
void level_start_frame() {
  const bool confirm_loading =
      game::cg::clientUIActives->actives[0].connectionState ==
      game::connstate_t::CONFIRMLOADING;
  if (!speed_before) {
    if (!confirm_loading) {
      level_start_done = false;
      return;
    }
    if (level_start_done || !level_start_waiting()) {
      return;
    }
    const std::optional<float> now = game::get_dvar_float("timescale");
    if (!now || *now != 1.0f) {
      level_start_done = true; // someone's own clock (or not this game's
      return;                  // dvar): left alone for this wait
    }
    speed_before = now;
    game::set_dvar_float("timescale", level_start_speed, true);
    speed_since = std::chrono::steady_clock::now();
    speed_level_time_from = game::sv::sv->levelTime;
    return;
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - speed_since)
                      .count();
  if (level_start_waiting() &&
      ms < std::chrono::duration_cast<std::chrono::milliseconds>(
               level_start_longest)
               .count()) {
    return;
  }
  // Ours back, unless something else changed the clock meanwhile
  const std::optional<float> now = game::get_dvar_float("timescale");
  const bool ours = now && *now == level_start_speed;
  if (ours) {
    game::set_dvar_float("timescale", *speed_before, true);
  }
  speed_before.reset();
  level_start_done = true;
  xerces_runtime::log_info(
      "xerces.fast_loads.level_start",
      std::format("The level's start ran {}x for {} ms while the client "
                  "waited (level.time {} to {}; client state {}){}",
                  level_start_speed, ms, speed_level_time_from,
                  game::sv::sv->levelTime,
                  static_cast<int>(
                      game::cg::clientUIActives->actives[0].connectionState),
                  ours ? ""
                       : "; the clock had been changed since: left as "
                         "it is"));
}

bool flag_given() {
  const auto args = xerces_runtime::command_line_args();
  return xerces_core::has_launch_flag(args, flag) ||
         xerces_core::has_launch_flag(args, "-" + std::string(flag));
}

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_fast_loads");

public:
  void post_load() override {
    if (flag_given()) {
      hook_wmi_locator();
    }
  }

  void post_unpack() override {
    if (!game::is_new_client()) {
      return;
    }
    const auto args = xerces_runtime::command_line_args();
    if (!xerces_core::has_launch_flag(args, flag) &&
        !xerces_core::has_launch_flag(args, "-" + std::string(flag))) {
      return;
    }
    enabled = true;
    // 4. only with 1.: without it the streaming comes after the wait instead
    level_start_enabled = draw_every_frame();
    if (level_start_enabled) {
      scheduler::loop(level_start_frame, scheduler::main);
    }
    xerces_runtime::log_info(
        "xerces.fast_loads.on",
        level_start_enabled
            ? "The loading screen draws every frame, a launch alone does not "
              "wait for others and a Zombies level's start runs 4x while the "
              "client waits for it (-xerces-fast-loads)"
            : "A launch alone does not wait for others; the loading screen "
              "and the level's start are as they were (-xerces-fast-loads)");
    xerces_runtime::log_info(
        "xerces.fast_loads.wmi",
        !wmi_hooked ? "The game's import of CoCreateInstance was not found: "
                      "its start asks WMI as it always did"
                    : std::format("BitLocker's WMI namespace answered at once "
                                  "{} time(s) at the start (not elevated: "
                                  "access denied, as Windows would after 5 s)",
                                  wmi_denied.load()));
  }
};
} // namespace xerces_fast_loads

REGISTER_COMPONENT(xerces_fast_loads::component)
