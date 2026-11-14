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
    xerces_runtime::log_info(
        "xerces.fast_loads.on",
        draw_every_frame()
            ? "The loading screen draws every frame and a launch alone does "
              "not wait for others (-xerces-fast-loads)"
            : "A launch alone does not wait for others; the loading screen "
              "is as it was (-xerces-fast-loads)");
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
