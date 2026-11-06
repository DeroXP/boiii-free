#include <std_include.hpp>

// Flip-model presentation, opt-in with -xerces-flip-model (docs/DESIGN.md
// 12.4 item 4). See flip_model.hpp for the hooks and why each is needed, and
// xerces_core/flip_model.hpp for the rules.
//
// What the engine does (read on the 2026-11-04 dump of build 0x6531394; no
// address below is used by this code): the renderer's start-up gets the
// factory from its device (IDXGIDevice1, then IDXGIAdapter1, then
// IDXGIFactory1), fills a DXGI_SWAP_CHAIN_DESC (DISCARD, R8G8B8A8_UNORM,
// usage 0x30, ALLOW_MODE_SWITCH, windowed) and sends its window the message
// WM_APP + 0x42; the window procedure calls CreateSwapChain. Present (WM_APP
// + 0x43, flags 0) and SetFullscreenState (WM_APP + 0x44) go through the
// window procedure too; ResizeBuffers is called directly with flags 0x2.
// It creates a render target view and a shader resource view on buffer 0
// once per chain or resize, and its render-target setter (0x141CF28D0 on the
// dump) skips OMSetRenderTargets when the target asked for is the one it set
// last. A flip-model Present unbinds the back buffer, so a frame that starts
// on the back buffer would draw into nothing; after_present binds the
// targets that were bound before Present again.

#include "flip_model.hpp"
#include "runtime.hpp"

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>

#include <utils/hook.hpp>

#include <xerces_core/flip_model.hpp>

#include <mutex>

namespace xerces_flip {
namespace detail {
std::atomic<IDXGISwapChain *> flipped{nullptr};
} // namespace detail

namespace {
namespace core = xerces_core::flip_model;
namespace dx = xerces_core::flip_model::dxgi;

// The rules use DXGI's numbers without its headers; these are the headers.
static_assert(dx::effect_discard == DXGI_SWAP_EFFECT_DISCARD);
static_assert(dx::effect_sequential == DXGI_SWAP_EFFECT_SEQUENTIAL);
static_assert(dx::effect_flip_sequential == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
static_assert(dx::effect_flip_discard == DXGI_SWAP_EFFECT_FLIP_DISCARD);
static_assert(dx::format_r16g16b16a16_float == DXGI_FORMAT_R16G16B16A16_FLOAT);
static_assert(dx::format_r10g10b10a2_unorm == DXGI_FORMAT_R10G10B10A2_UNORM);
static_assert(dx::format_r8g8b8a8_unorm == DXGI_FORMAT_R8G8B8A8_UNORM);
static_assert(dx::format_r8g8b8a8_unorm_srgb ==
              DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
static_assert(dx::format_b8g8r8a8_unorm == DXGI_FORMAT_B8G8R8A8_UNORM);
static_assert(dx::format_b8g8r8a8_unorm_srgb ==
              DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
static_assert(dx::chain_allow_mode_switch ==
              DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
static_assert(dx::chain_frame_latency_waitable ==
              DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
static_assert(dx::chain_allow_tearing == DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
static_assert(dx::usage_shader_input == DXGI_USAGE_SHADER_INPUT);
static_assert(dx::usage_render_target_output ==
              DXGI_USAGE_RENDER_TARGET_OUTPUT);
static_assert(dx::usage_back_buffer == DXGI_USAGE_BACK_BUFFER);
static_assert(dx::usage_unordered_access == DXGI_USAGE_UNORDERED_ACCESS);
static_assert(dx::present_test == DXGI_PRESENT_TEST);
static_assert(dx::present_allow_tearing == DXGI_PRESENT_ALLOW_TEARING);
static_assert(dx::max_buffers == DXGI_MAX_SWAP_CHAIN_BUFFERS);

// COM vtable slots: IUnknown takes 0-2 and IDXGIObject 3-6. IDXGIFactory
// then has EnumAdapters, MakeWindowAssociation, GetWindowAssociation and
// CreateSwapChain; IDXGIDeviceSubObject has GetDevice, then IDXGISwapChain
// has Present, GetBuffer, SetFullscreenState, GetFullscreenState, GetDesc
// and ResizeBuffers. The engine calls the same slots (+0x50 and +0x68).
constexpr std::size_t create_swap_chain_slot = 10;
constexpr std::size_t resize_buffers_slot = 13;

utils::hook::detour create_hook;
utils::hook::detour resize_hook;

std::atomic_bool requested{false};

// Arming (xerces_core::flip_model::arming): post_unpack (the game's start-up
// thread) and the overlay's Present hook (the async thread) can come in
// either order; the second one installs ResizeBuffers. Never held while a
// detour is made.
std::mutex arm_mutex;
core::arming armed;            // under arm_mutex
void *resize_target = nullptr; // under arm_mutex: ResizeBuffers in dxgi.dll

// The flipped chain (detail::flipped): its creation flags, whether it has
// ALLOW_TEARING, and whether Present refused the tearing flag on it.
std::atomic<std::uint32_t> created_flags{0};
std::atomic_bool chain_tears{false};
std::atomic_bool tearing_off{false};
std::atomic_bool resize_failure_logged{false};

std::atomic<std::uint32_t> chains_seen{0};
thread_local int own_chain_depth = 0;

// The last report, for perf.swapchain (written on the window thread, read on
// the thread that presents; neither is a hot path).
std::mutex report_mutex;
std::optional<core::report> last_report;
bool not_seen_logged = false; // under report_mutex

std::string hex(std::uint32_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%X", value);
  return text;
}

void log_report(const core::report &r) {
  xerces_runtime::log(
      r.applied() ? xerces_core::log_level::info : xerces_core::log_level::warn,
      "xerces.flip_model", core::message(r), core::data_json(r));
}

// Kept for perf.swapchain and logged from the async thread, never from the
// window procedure that is making the chain.
void record(const core::report &r) {
  {
    std::lock_guard lock(report_mutex);
    last_report = r;
  }
  scheduler::once([r] { log_report(r); }, scheduler::async);
}

core::chain_desc from_dxgi(const DXGI_SWAP_CHAIN_DESC &desc) {
  core::chain_desc out;
  out.width = desc.BufferDesc.Width;
  out.height = desc.BufferDesc.Height;
  out.format = static_cast<std::uint32_t>(desc.BufferDesc.Format);
  out.sample_count = desc.SampleDesc.Count;
  out.sample_quality = desc.SampleDesc.Quality;
  out.buffer_usage = desc.BufferUsage;
  out.buffer_count = desc.BufferCount;
  out.windowed = desc.Windowed != FALSE;
  out.swap_effect = static_cast<std::uint32_t>(desc.SwapEffect);
  out.flags = desc.Flags;
  return out;
}

bool tearing_supported(IDXGIFactory *factory) {
  IDXGIFactory5 *factory5 = nullptr;
  if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory5))) || !factory5) {
    return false; // DXGI before 1.5 (Windows 10 1607) has no tearing
  }
  BOOL allow = FALSE;
  const HRESULT hr = factory5->CheckFeatureSupport(
      DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow));
  factory5->Release();
  return SUCCEEDED(hr) && allow != FALSE;
}

// IDXGIFactory::CreateSwapChain. Runs on the engine's window thread, inside
// its window procedure. Chains made inside own_chain_scope pass through.
HRESULT __stdcall create_swap_chain_stub(IDXGIFactory *factory,
                                         IUnknown *device,
                                         DXGI_SWAP_CHAIN_DESC *desc,
                                         IDXGISwapChain **chain) {
  if (own_chain_depth > 0 || !desc || !chain || !device) {
    return create_hook.invoke<HRESULT>(factory, device, desc, chain);
  }
  core::report r;
  r.chain = chains_seen.fetch_add(1, std::memory_order_relaxed) + 1;
  r.asked = from_dxgi(*desc);
  // A chain made from here on is not the flipped one, whatever happens.
  detail::flipped.store(nullptr, std::memory_order_release);

  core::decision decided;
  std::optional<core::outcome> not_armed;
  {
    std::lock_guard lock(arm_mutex);
    not_armed = core::not_armed(armed);
    if (not_armed) {
      // A failed detour stays hook_failed, with its error.
      r.error = core::missing(armed);
    }
  }
  if (not_armed) {
    r.result = *not_armed;
  } else {
    r.tearing_supported = tearing_supported(factory);
    decided = core::decide(*r.asked, r.tearing_supported);
    r.result = decided.result;
  }

  if (r.result == core::outcome::applied) {
    DXGI_SWAP_CHAIN_DESC flip = *desc; // the engine's own stays as it was
    flip.SwapEffect = static_cast<DXGI_SWAP_EFFECT>(decided.desc.swap_effect);
    flip.Flags = decided.desc.flags;
    const HRESULT hr =
        create_hook.invoke<HRESULT>(factory, device, &flip, chain);
    if (SUCCEEDED(hr) && *chain) {
      created_flags.store(flip.Flags, std::memory_order_relaxed);
      chain_tears.store(decided.tearing, std::memory_order_relaxed);
      tearing_off.store(false, std::memory_order_relaxed);
      resize_failure_logged.store(false, std::memory_order_relaxed);
      detail::flipped.store(*chain, std::memory_order_release);
      r.made = decided.desc;
      r.tearing = decided.tearing;
      record(r);
      return hr;
    }
    // Older Windows (FLIP_DISCARD needs Windows 10) or a driver that says
    // no: the engine gets the chain it asked for.
    r.result = core::outcome::create_failed;
    r.flip_hr = static_cast<std::uint32_t>(hr);
  }

  const HRESULT hr = create_hook.invoke<HRESULT>(factory, device, desc, chain);
  if (SUCCEEDED(hr)) {
    r.made = r.asked;
  } else {
    r.create_hr = static_cast<std::uint32_t>(hr);
  }
  record(r);
  return hr;
}

// IDXGISwapChain::ResizeBuffers. Only the flipped chain is changed: it must
// keep ALLOW_TEARING and needs at least two buffers.
HRESULT __stdcall resize_buffers_stub(IDXGISwapChain *chain, UINT count,
                                      UINT width, UINT height,
                                      DXGI_FORMAT format, UINT flags) {
  if (!is_flipped(chain)) {
    return resize_hook.invoke<HRESULT>(chain, count, width, height, format,
                                       flags);
  }
  const core::resize_request asked{count, static_cast<std::uint32_t>(format),
                                   flags};
  const core::resize_request given =
      core::resize(asked, created_flags.load(std::memory_order_relaxed));
  const HRESULT hr = resize_hook.invoke<HRESULT>(
      chain, UINT{given.buffer_count}, width, height,
      static_cast<DXGI_FORMAT>(given.format), UINT{given.flags});
  if (FAILED(hr) && !resize_failure_logged.exchange(true)) {
    const std::string data = core::resize_failure_json(
        asked, given, width, height, static_cast<std::uint32_t>(hr));
    const std::string text = "ResizeBuffers failed on the flip-model chain (" +
                             hex(static_cast<std::uint32_t>(hr)) +
                             "). Turn -xerces-flip-model off if the picture "
                             "stops updating.";
    scheduler::once(
        [data, text] {
          xerces_runtime::log_warn("xerces.flip_model.resize_failed", text,
                                   data);
        },
        scheduler::async);
  }
  return hr;
}

void log_hook_failed(const std::string &error) {
  core::report r;
  r.result = core::outcome::hook_failed;
  r.error = error;
  {
    std::lock_guard lock(report_mutex);
    last_report = r;
  }
  log_report(r);
}

// Empty when the detour is in place, otherwise why not. A failed create
// leaves the detour empty (utils::hook::detour::create), so nothing is
// cleared here: clearing would remove another owner's hook on the address.
std::string install_create_hook() {
  // A factory made only to find CreateSwapChain: every DXGI factory shares
  // dxgi.dll's implementation, which the detour patches, and the game
  // imports dxgi.dll, so it stays loaded after this one is released.
  IDXGIFactory1 *factory = nullptr;
  const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr) || !factory) {
    return "CreateDXGIFactory1 failed (" + hex(static_cast<std::uint32_t>(hr)) +
           ")";
  }
  void *const target =
      (*reinterpret_cast<void ***>(factory))[create_swap_chain_slot];
  factory->Release();
  try {
    create_hook.create(target,
                       reinterpret_cast<void *>(&create_swap_chain_stub));
  } catch (const std::exception &e) {
    return std::string("CreateSwapChain: ") + e.what();
  }
  return {};
}

// The last arming step, on whichever thread brought it: post_unpack's or
// the async thread. Runs once (arming hands it out once).
void install_resize_hook() {
  void *target = nullptr;
  core::arm_step by = core::arm_step::create_hook;
  {
    std::lock_guard lock(arm_mutex);
    target = resize_target;
    by = armed.armed_by.value_or(by);
  }
  std::string error;
  if (!target) {
    error = "ResizeBuffers: no address from the overlay's dummy chain";
  } else {
    try {
      resize_hook.create(target,
                         reinterpret_cast<void *>(&resize_buffers_stub));
    } catch (const std::exception &e) {
      error = std::string("ResizeBuffers: ") + e.what();
    }
  }
  {
    std::lock_guard lock(arm_mutex);
    core::arm_resize_hook(armed, error.empty(), error);
  }
  if (!error.empty()) {
    log_hook_failed(error);
    return;
  }
  const std::string_view last = core::to_string(by);
  xerces_runtime::log_info(
      "xerces.flip_model.armed",
      "Flip model asked for (-xerces-flip-model): CreateSwapChain, Present "
      "and ResizeBuffers are detoured (the last step was " +
          std::string(last) + ")",
      "{\"armed_by\":\"" + std::string(last) + "\"}");
}
} // namespace

own_chain_scope::own_chain_scope() { ++own_chain_depth; }
own_chain_scope::~own_chain_scope() { --own_chain_depth; }

void on_present_hooked(IDXGISwapChain *dummy) {
  if (!dummy) {
    return;
  }
  // Kept with or without the flag (post_unpack may not have run yet): the
  // address is in dxgi.dll, which stays loaded after the dummy is released.
  void *const target =
      (*reinterpret_cast<void ***>(dummy))[resize_buffers_slot];
  bool install = false;
  {
    std::lock_guard lock(arm_mutex);
    resize_target = target;
    install = core::arm_present_hook(armed);
  }
  if (install) {
    install_resize_hook();
  }
}

void before_present(IDXGISwapChain *chain, UINT sync_interval, UINT flags,
                    present_call &call) {
  call.flags = flags;
  if (sync_interval == 0 && chain_tears.load(std::memory_order_relaxed) &&
      !tearing_off.load(std::memory_order_relaxed)) {
    // DXGI refuses the tearing flag in exclusive fullscreen; a state that
    // cannot be read counts as fullscreen.
    BOOL fullscreen = TRUE;
    if (FAILED(chain->GetFullscreenState(&fullscreen, nullptr))) {
      fullscreen = TRUE;
    }
    call.flags =
        core::present_flags(flags, sync_interval, true, fullscreen != FALSE);
    call.tearing_added = call.flags != flags;
  }
  ID3D11Device *device = nullptr;
  if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || !device) {
    return;
  }
  device->GetImmediateContext(&call.context);
  device->Release();
  if (!call.context) {
    return;
  }
  call.context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                   call.targets, &call.depth);
  for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
    if (call.targets[i]) {
      call.target_count = i + 1;
    }
  }
}

bool tearing_refused(present_call &call, HRESULT result) {
  if (!call.tearing_added || result != DXGI_ERROR_INVALID_CALL) {
    return false;
  }
  call.tearing_added = false;
  call.flags &= ~UINT{DXGI_PRESENT_ALLOW_TEARING};
  if (!tearing_off.exchange(true)) {
    scheduler::once(
        [] {
          xerces_runtime::log_warn(
              "xerces.flip_model.tearing_refused",
              "Present refused DXGI_PRESENT_ALLOW_TEARING; the game presents "
              "without it from now on (flip model stays on)",
              "{\"hr\":\"0x887A0001\"}");
        },
        scheduler::async);
  }
  return true;
}

void after_present(present_call &call) {
  if (!call.context) {
    return;
  }
  if (call.target_count > 0 || call.depth) {
    // The same targets and depth buffer as before Present; UAVs untouched.
    call.context->OMSetRenderTargetsAndUnorderedAccessViews(
        call.target_count, call.targets, call.depth,
        D3D11_KEEP_UNORDERED_ACCESS_VIEWS, 0, nullptr, nullptr);
  }
  for (ID3D11RenderTargetView *&target : call.targets) {
    if (target) {
      target->Release();
      target = nullptr;
    }
  }
  if (call.depth) {
    call.depth->Release();
    call.depth = nullptr;
  }
  call.context->Release();
  call.context = nullptr;
}

void describe(xerces_core::bench::swapchain_info &info) {
  if (!requested.load(std::memory_order_acquire)) {
    info.flip_model =
        std::string(core::to_string(core::outcome::not_requested));
    return;
  }
  bool log_not_seen = false;
  {
    std::lock_guard lock(report_mutex);
    if (!last_report) {
      // The flag was passed and the detour was in place, yet no chain was
      // made through it before the first Present.
      core::report r;
      r.result = core::outcome::not_seen;
      last_report = r;
      log_not_seen = !not_seen_logged;
      not_seen_logged = true;
    }
    info.flip_model = std::string(core::to_string(last_report->result));
    if (last_report->asked) {
      info.engine_swap_effect = last_report->asked->swap_effect;
      info.engine_flags = last_report->asked->flags;
    }
  }
  if (log_not_seen) {
    core::report r;
    r.result = core::outcome::not_seen;
    scheduler::once([r] { log_report(r); }, scheduler::async);
  }
}

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_flip_model");

public:
  void post_unpack() override {
    if (!game::is_client() ||
        !core::requested(xerces_runtime::command_line_args())) {
      return;
    }
    requested.store(true, std::memory_order_release);
    // Before the engine's renderer starts, so its CreateSwapChain is seen.
    const std::string error = install_create_hook();
    bool install = false;
    {
      std::lock_guard lock(arm_mutex);
      install = core::arm_create_hook(armed, error.empty(), error);
    }
    if (!error.empty()) {
      log_hook_failed(error);
    } else if (install) {
      // The overlay's Present hook came first (the usual order).
      install_resize_hook();
    } else {
      xerces_runtime::log_info(
          "xerces.flip_model.waiting",
          "Flip model asked for (-xerces-flip-model): CreateSwapChain is "
          "detoured; ResizeBuffers follows when the overlay's Present detour "
          "is in place");
    }
  }
};
} // namespace xerces_flip

REGISTER_COMPONENT(xerces_flip::component)
