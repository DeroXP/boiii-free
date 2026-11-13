// NVIDIA DLSS and DLAA at the SMAA T2x resolve (upscale.hpp,
// xerces_core/upscale.hpp, docs/DESIGN.md 13.4). Open source like the rest
// of the client: NVIDIA's code is only in the optional module
// (src/xerces_dlss, D10), which this loads at run time when the player picks
// NVIDIA DLSS (xerces_upscaler 2).
//
// The temporal inputs (feature upscale_temporal):
//   - jitter: SMAA T2x moves the projection by a quarter pixel, two ways, by
//     frame parity (r.t2x_jitter_projection), and gives the same offset to
//     the passes that undo it (r.t2x_jitter_offset). With DLSS on, both
//     detours give a Halton(2, 3) sequence instead, for the same frame number
//     the engine passes (J1) or reads (J2), so the two always agree.
//   - the resolve (r.t2x_resolve.draw_call): when the game's own resolve has
//     drawn, its inputs are still bound: t0 the jittered colour (linear times
//     32768), t11 this frame's velocity (16-bit SNORM, nonlinear), t15 depth
//     (reversed). A pass turns them into DLSS's inputs, the module runs DLSS,
//     and its output goes back in the game's encoding: into the resolve's own
//     colour output for DLAA, or into a texture at the back buffer's size that
//     the final composite draws (upscale.cpp).
// Everything changed on the context is put back. Any failure turns DLSS off
// for the run, logged, and the game's own resolve and stretch stay.

#include <std_include.hpp>

#include "runtime.hpp"
#include "signatures.hpp"
#include "upscale.hpp"
#include "upscale_gpu.hpp"

#include "upscale_shaders/temporal_pack_ps.h"
#include "upscale_shaders/temporal_prepare_ps.h"

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>
#include <utils/hook.hpp>

#include <xerces_core/json.hpp>
#include <xerces_core/sigruntime.hpp>
#include <xerces_core/upscale.hpp>

#include <xerces_dlss/xerces_dlss.h>

#include <DirectXPackedVector.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

namespace xerces_upscale::temporal {
namespace {
namespace core = xerces_core::upscale;
namespace ids = xerces_core::upscale_temporal_signatures;

game::EngineDependentDvarMut available_dvar;
game::EngineDependentDvarMut jitter_debug_dvar;

utils::hook::detour jitter_offset_hook;
utils::hook::detour jitter_projection_hook;
using draw_fn = void (*)(void *);
draw_fn resolve_draw_original = nullptr;

// From r.t2x_jitter_projection's own code (its pattern fixes these bytes).
const std::uint8_t *const *frame_data = nullptr; // the global it reads
std::uint32_t frame_field = 0;                   // +0x8A364
std::uint8_t view_index_field = 0;               // [view + 0x24]
using size_fn = unsigned (*)(int);
size_fn render_width_fn = nullptr;
size_fn render_height_fn = nullptr;

std::atomic_bool jitter_on{false};
std::atomic<std::uint32_t> phases{8};
core::jitter last_jitter{};              // render thread (J1, then the resolve)
std::atomic<std::uint32_t> j1_thread{0}; // for the one-time check below
std::atomic<std::uint32_t> j1_frame{0};
std::atomic<std::uint32_t> j2_frame{0};

// The module.
HMODULE module = nullptr;
const xerces_dlss_api *api = nullptr;
bool started = false;
bool broken = false; // off for this run
std::filesystem::path module_dir;

// The render thread's own.
gpu::shared_objects objects;
ID3D11PixelShader *prepare_ps = nullptr;
ID3D11PixelShader *pack_ps = nullptr;
gpu::target in_color;   // RGBA16F, render size
gpu::target in_motion;  // RG16F
gpu::target in_depth;   // R32F
gpu::target out_color;  // RGBA16F with UAV, output size
gpu::target out_packed; // RGBA16F, back buffer size, the game's encoding
std::uint32_t last_present = 0;
std::uint32_t packed_present = 0xFFFFFFFF;
std::uint32_t frames_on = 0;
LARGE_INTEGER last_time{};
std::string logged_mode; // the DLSS mode last logged
bool logged_skip = false;
bool logged_threads = false;

void log_off(const std::string &message) {
  xerces_runtime::log_warn("xerces.upscale.dlss_off", message);
}

void turn_off(const std::string &message) {
  broken = true;
  jitter_on.store(false, std::memory_order_release);
  log_off(message + "; the game's own anti-aliasing stays");
}

void skip_once(const std::string &why) {
  if (!logged_skip) {
    logged_skip = true;
    xerces_runtime::log_info("xerces.upscale.dlss_skipped",
                             "DLSS: not this frame (" + why + ")");
  }
}

std::uint32_t read_frame() {
  const std::uint8_t *const data = frame_data ? *frame_data : nullptr;
  if (!data) {
    return 0;
  }
  std::uint32_t frame = 0;
  std::memcpy(&frame, data + frame_field, sizeof(frame));
  return frame;
}

// --- jitter ---

// xerces_dlssJitterSign's 4: one fixed jitter, to see which way DLSS takes it
// (with the right signs its output lines up with the game's own picture).
bool held_jitter() {
  return jitter_debug_dvar && (jitter_debug_dvar.get_int() & 4) != 0;
}

void jitter_offset_stub(float *out, const int frame) {
  jitter_offset_hook.invoke<void>(out, frame);
  if (!jitter_on.load(std::memory_order_acquire) || !out) {
    return;
  }
  const core::jitter j =
      held_jitter()
          ? core::jitter{0.4f, 0.3f}
          : core::temporal_jitter(static_cast<std::uint32_t>(frame),
                                  phases.load(std::memory_order_relaxed));
  out[0] = j.x;
  out[1] = j.y;
  last_jitter = j;
  j1_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
  j1_frame.store(static_cast<std::uint32_t>(frame), std::memory_order_relaxed);
}

void jitter_projection_stub(void *view, float *projection) {
  jitter_projection_hook.invoke<void>(view, projection);
  if (!jitter_on.load(std::memory_order_acquire) || !view || !projection) {
    return;
  }
  int index = 0;
  std::memcpy(&index,
              static_cast<const std::uint8_t *>(view) + view_index_field,
              sizeof(index));
  const unsigned width = render_width_fn(index) & 0xFFFF;
  const unsigned height = render_height_fn(index) & 0xFFFF;
  if (width == 0 || height == 0) {
    return;
  }
  const std::uint32_t frame = read_frame();
  j2_frame.store(frame, std::memory_order_relaxed);
  const core::jitter j =
      held_jitter() ? core::jitter{0.4f, 0.3f}
                    : core::temporal_jitter(
                          frame, phases.load(std::memory_order_relaxed));
  // [projection + 30h] and [+34h], as the engine writes them.
  projection[12] = j.x * 2.0f / static_cast<float>(width);
  projection[13] = j.y * 2.0f / static_cast<float>(height);
}

// --- the module ---

bool start_module(ID3D11Device *device) {
  if (started) {
    return api != nullptr;
  }
  started = true;
  const std::filesystem::path path = module_dir / "xerces_dlss.dll";
  module = LoadLibraryW(path.c_str());
  if (!module) {
    turn_off("DLSS: xerces_dlss.dll could not be loaded from " +
             module_dir.string());
    return false;
  }
  const auto get = reinterpret_cast<xerces_dlss_get_fn>(
      GetProcAddress(module, "xerces_dlss_get"));
  const xerces_dlss_api *candidate = get ? get() : nullptr;
  if (!candidate || candidate->abi != XERCES_DLSS_ABI) {
    turn_off("DLSS: xerces_dlss.dll is not this client's version");
    return false;
  }
  std::filesystem::path data = xerces_runtime::home();
  data = data.empty() ? module_dir : data / "logs" / "ngx";
  std::error_code ec;
  std::filesystem::create_directories(data, ec);
  char error[512]{};
  if (!candidate->start(device, module_dir.c_str(), data.c_str(), error,
                        sizeof(error))) {
    turn_off(std::string("DLSS: ") + error);
    return false;
  }
  api = candidate;
  xerces_runtime::log_info("xerces.upscale.dlss_started",
                           "DLSS: the module started NVIDIA NGX; DLSS is "
                           "available on this GPU");
  return true;
}

// What the compute stage had bound, which NGX's work may change.
struct compute_state {
  static constexpr UINT kSrvs = 16;
  static constexpr UINT kUavs = 8;
  static constexpr UINT kCbs = 14;
  static constexpr UINT kSamplers = 16;
  ID3D11ComputeShader *cs = nullptr;
  ID3D11ShaderResourceView *srvs[kSrvs]{};
  ID3D11UnorderedAccessView *uavs[kUavs]{};
  ID3D11Buffer *cbs[kCbs]{};
  ID3D11SamplerState *samplers[kSamplers]{};

  compute_state() = default;
  compute_state(const compute_state &) = delete;
  compute_state &operator=(const compute_state &) = delete;

  void capture(ID3D11DeviceContext *ctx) {
    ctx->CSGetShader(&cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, kSrvs, srvs);
    ctx->CSGetUnorderedAccessViews(0, kUavs, uavs);
    ctx->CSGetConstantBuffers(0, kCbs, cbs);
    ctx->CSGetSamplers(0, kSamplers, samplers);
  }

  void restore(ID3D11DeviceContext *ctx) const {
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetShaderResources(0, kSrvs, srvs);
    const UINT keep[kUavs] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    ctx->CSSetUnorderedAccessViews(0, kUavs, uavs, keep);
    ctx->CSSetConstantBuffers(0, kCbs, cbs);
    ctx->CSSetSamplers(0, kSamplers, samplers);
  }

  ~compute_state() {
    auto drop = [](IUnknown *object) {
      if (object) {
        object->Release();
      }
    };
    drop(cs);
    for (auto *o : srvs) {
      drop(o);
    }
    for (auto *o : uavs) {
      drop(o);
    }
    for (auto *o : cbs) {
      drop(o);
    }
    for (auto *o : samplers) {
      drop(o);
    }
  }
};

// xerces_dlssJitterSign's 16: every 30th frame, the mean of the motion
// vectors handed to DLSS over a 64x64 patch at the centre, in the log (render
// pixels, this frame to the last).
void log_motion(ID3D11DeviceContext *ctx, ID3D11Device *device,
                const UINT width, const UINT height) {
  static std::uint32_t count = 0;
  if (++count % 30 != 0 || width < 64 || height < 64) {
    return;
  }
  D3D11_TEXTURE2D_DESC desc{};
  in_motion.texture->GetDesc(&desc);
  desc.Width = 64;
  desc.Height = 64;
  desc.BindFlags = 0;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *staging = nullptr;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
    return;
  }
  D3D11_BOX box{width / 2 - 32, height / 2 - 32, 0,
                width / 2 + 32, height / 2 + 32, 1};
  ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, in_motion.texture, 0, &box);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (UINT y = 0; y < 64; ++y) {
      const auto *row = reinterpret_cast<const std::uint16_t *>(
          static_cast<const std::uint8_t *>(mapped.pData) +
          y * mapped.RowPitch);
      for (UINT x = 0; x < 64; ++x) {
        sum_x += DirectX::PackedVector::XMConvertHalfToFloat(row[x * 2]);
        sum_y += DirectX::PackedVector::XMConvertHalfToFloat(row[x * 2 + 1]);
      }
    }
    ctx->Unmap(staging, 0);
    char text[96];
    std::snprintf(text, sizeof(text), "DLSS motion at the centre: %+.2f, %+.2f",
                  sum_x / 4096.0, sum_y / 4096.0);
    xerces_runtime::log_info("xerces.upscale.dlss_motion", text);
  }
  staging->Release();
}

ID3D11ShaderResourceView *slot(ID3D11DeviceContext *ctx, const UINT index) {
  ID3D11ShaderResourceView *view = nullptr;
  ctx->PSGetShaderResources(index, 1, &view);
  return view;
}

void run(ID3D11DeviceContext *ctx) {
  // One view per frame: two views' resolves (split screen) would share one
  // DLSS history and one output (review, 2026-11-13). Off from the frame in
  // which a second view shows up until one frame after the last.
  {
    static std::uint32_t counted_present = 0;
    static int views = 0;
    static bool several = false;
    const std::uint32_t now = present_count();
    if (now != counted_present) {
      several = views > 1;
      views = 0;
      counted_present = now;
    }
    if (++views > 1 || several) {
      skip_once("more than one view is drawn in a frame (split screen)");
      jitter_on.store(false, std::memory_order_release);
      return;
    }
  }
  const auto [display_width, display_height] = display_size();
  gpu::saved_state saved;
  saved.capture(ctx);
  ID3D11ShaderResourceView *const color = saved.srvs[0];
  ID3D11ShaderResourceView *const velocity = slot(ctx, 11);
  ID3D11ShaderResourceView *const depth = slot(ctx, 15);
  struct release_views {
    ID3D11ShaderResourceView *a;
    ID3D11ShaderResourceView *b;
    ~release_views() {
      if (a) {
        a->Release();
      }
      if (b) {
        b->Release();
      }
    }
  } views{velocity, depth};

  UINT width = 0;
  UINT height = 0;
  UINT check_width = 0;
  UINT check_height = 0;
  if (!saved.rtvs[0] || !gpu::view_size(color, width, height) ||
      !gpu::view_size(velocity, check_width, check_height) ||
      check_width != width || check_height != height || !depth) {
    skip_once("the resolve's colour, velocity or depth is not where expected");
    jitter_on.store(false, std::memory_order_release);
    return;
  }
  const core::temporal_plan plan = core::plan_temporal(
      core::mode::dlss, width, height, display_width, display_height);
  if (!plan.active) {
    skip_once("the scene is larger than the screen");
    jitter_on.store(false, std::memory_order_release);
    return;
  }

  if (!objects.device) {
    ID3D11Device *device = nullptr;
    ctx->GetDevice(&device);
    const bool made =
        device && objects.make(device) &&
        SUCCEEDED(device->CreatePixelShader(temporal_prepare_ps,
                                            sizeof(temporal_prepare_ps),
                                            nullptr, &prepare_ps)) &&
        SUCCEEDED(device->CreatePixelShader(
            temporal_pack_ps, sizeof(temporal_pack_ps), nullptr, &pack_ps));
    if (device) {
      device->Release();
    }
    if (!made) {
      turn_off("DLSS: its shaders could not be made");
      return;
    }
  }
  ID3D11Device *const device = objects.device;
  if (!start_module(device)) {
    return;
  }
  const UINT out_width = plan.dlaa ? width : display_width;
  const UINT out_height = plan.dlaa ? height : display_height;
  if (!in_color.ensure(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
      !in_motion.ensure(device, width, height, DXGI_FORMAT_R16G16_FLOAT) ||
      !in_depth.ensure(device, width, height, DXGI_FORMAT_R32_FLOAT) ||
      !out_color.ensure(device, out_width, out_height,
                        DXGI_FORMAT_R16G16B16A16_FLOAT, true) ||
      (!plan.dlaa && !out_packed.ensure(device, display_width, display_height,
                                        DXGI_FORMAT_R16G16B16A16_FLOAT))) {
    turn_off("DLSS: its textures could not be made");
    return;
  }

  // The inputs.
  objects.bind(ctx);
  ID3D11RenderTargetView *const inputs[] = {in_color.rtv, in_motion.rtv,
                                            in_depth.rtv};
  ctx->OMSetRenderTargets(3, inputs, nullptr);
  gpu::set_viewport(ctx, width, height);
  ctx->PSSetShader(prepare_ps, nullptr, 0);
  ID3D11ShaderResourceView *const sources[] = {color, velocity, depth};
  ctx->PSSetShaderResources(0, 3, sources);
  ctx->Draw(3, 0);
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
  ID3D11ShaderResourceView *const none[3]{};
  ctx->PSSetShaderResources(0, 3, none);
  if (jitter_debug_dvar && (jitter_debug_dvar.get_int() & 16) != 0) {
    log_motion(ctx, device, width, height);
  }

  // DLSS.
  const std::uint32_t present = present_count();
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  const float frame_ms =
      last_time.QuadPart
          ? static_cast<float>(
                1000.0 *
                static_cast<double>(now.QuadPart - last_time.QuadPart) /
                static_cast<double>(frequency.QuadPart))
          : 16.6f;
  last_time = now;
  const int debug = jitter_debug_dvar ? jitter_debug_dvar.get_int() : 0;
  xerces_dlss_frame frame{};
  frame.color = in_color.texture;
  frame.depth = in_depth.texture;
  frame.motion = in_motion.texture;
  frame.output = out_color.texture;
  frame.render_width = width;
  frame.render_height = height;
  frame.output_width = out_width;
  frame.output_height = out_height;
  // DLSS counts pixels down; the engine moves the projection up for +y
  // (measured 2026-11-08: with the jitter held at (0.4, 0.3), only (x, -y)
  // lines DLSS's output up with the game's own picture).
  frame.jitter_x = (debug & 1) ? -last_jitter.x : last_jitter.x;
  frame.jitter_y = (debug & 2) ? last_jitter.y : -last_jitter.y;
  frame.motion_scale = (debug & 8) ? -1.0f : 1.0f;
  frame.frame_ms = frame_ms;
  // A cut: the first frames with the new jitter, or frames that went by
  // without a resolve (menus, a load).
  frame.reset = (frames_on < 2 || present > last_present + 2) ? 1 : 0;
  last_present = present;
  char error[512]{};
  bool ok = false;
  {
    compute_state compute;
    compute.capture(ctx);
    ok = api->evaluate(ctx, &frame, error, sizeof(error)) == 1;
    compute.restore(ctx);
  }
  if (!ok) {
    saved.restore(ctx);
    turn_off(std::string("DLSS: ") + error);
    return;
  }

  // Back in the game's encoding.
  objects.bind(ctx);
  ID3D11RenderTargetView *const out =
      plan.dlaa ? saved.rtvs[0] : out_packed.rtv;
  ctx->OMSetRenderTargets(1, &out, nullptr);
  gpu::set_viewport(ctx, out_width, out_height);
  ctx->PSSetShader(pack_ps, nullptr, 0);
  ctx->PSSetShaderResources(0, 1, &out_color.srv);
  ctx->Draw(3, 0);
  saved.restore(ctx);

  packed_present = plan.dlaa ? 0xFFFFFFFF : present;
  phases.store(core::jitter_phases(width, height, out_width, out_height),
               std::memory_order_relaxed);
  jitter_on.store(true, std::memory_order_release);
  ++frames_on;

  if (logged_mode != api->mode() + std::string(plan.dlaa ? "/dlaa" : "")) {
    logged_mode = api->mode() + std::string(plan.dlaa ? "/dlaa" : "");
    xerces_runtime::log_info(
        "xerces.upscale.dlss_running",
        "DLSS: " + std::string(api->mode()) + ", " + std::to_string(width) +
            "x" + std::to_string(height) +
            (plan.dlaa ? std::string(" (DLAA)")
                       : " to " + std::to_string(out_width) + "x" +
                             std::to_string(out_height)) +
            ", " + std::to_string(phases.load(std::memory_order_relaxed)) +
            " jitter positions");
  }
  if (!logged_threads && frames_on == 120) {
    logged_threads = true;
    const std::uint32_t a = j1_frame.load(std::memory_order_relaxed);
    const std::uint32_t b = j2_frame.load(std::memory_order_relaxed);
    xerces_runtime::log_info(
        "xerces.upscale.dlss_frames",
        "DLSS: the jitter's frame numbers: offset " + std::to_string(a) +
            ", projection " + std::to_string(b) + "; the offset " +
            (j1_thread.load(std::memory_order_relaxed) == GetCurrentThreadId()
                 ? "on the resolve's thread"
                 : "on another thread than the resolve"));
  }
}

void resolve_draw_stub(void *state) {
  resolve_draw_original(state);
  if (broken || chosen_mode() != core::mode::dlss) {
    if (jitter_on.exchange(false, std::memory_order_acq_rel)) {
      frames_on = 0;
    }
    return;
  }
  ID3D11DeviceContext *const ctx = game_context();
  if (ctx) {
    run(ctx);
  }
}

std::string rva(const std::uintptr_t address) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%llX",
                static_cast<unsigned long long>(address - 0x140000000));
  return text;
}

bool bytes_at(const std::uintptr_t at, std::initializer_list<std::uint8_t> b) {
  return std::memcmp(reinterpret_cast<const void *>(at), std::data(b),
                     b.size()) == 0;
}

std::uintptr_t call_target(const std::uintptr_t call) {
  std::int32_t rel = 0;
  std::memcpy(&rel, reinterpret_cast<const void *>(call + 1), sizeof(rel));
  return call + 5 + rel;
}

void install() {
  if (!xerces_signatures::feature_enabled(core::temporal_feature)) {
    log_off(xerces_signatures::feature_message(
        core::temporal_feature, "DLSS", "The game's own anti-aliasing stays."));
    return;
  }
  const auto offset_at = xerces_signatures::address(ids::jitter_offset);
  const auto projection_at = xerces_signatures::address(ids::jitter_projection);
  const auto draw_at = xerces_signatures::address(ids::resolve_draw_call);
  if (!offset_at || !projection_at || !draw_at) {
    log_off("DLSS is off: the signature registry has no address for its "
            "jitter or resolve");
    return;
  }
  std::string why;
  if (!xerces_signatures::code_matches(ids::jitter_offset,
                                       core::jitter_offset_entry, &why) ||
      !xerces_signatures::code_matches(ids::jitter_projection,
                                       core::jitter_projection_entry, &why)) {
    log_off("DLSS is off: " + why);
    return;
  }
  // What J2's detour reads from J2's own code (its pattern fixes these).
  const std::uintptr_t j2 = *projection_at;
  if (!bytes_at(j2 + 0x40, {0x48, 0x8B, 0x05}) ||
      !bytes_at(j2 + 0x56, {0x8B, 0x90}) ||
      !bytes_at(j2 + 0xA0, {0x8B, 0x4F, 0x24, 0xE8}) ||
      !bytes_at(j2 + 0xBA, {0x8B, 0x4F, 0x24, 0x0F, 0x5B, 0xC0, 0xF3, 0x0F,
                            0x5E, 0xF0, 0xE8}) ||
      !bytes_at(*draw_at, {0xE8})) {
    log_off("DLSS is off: the jitter or resolve code is not as expected");
    return;
  }
  std::int32_t disp = 0;
  std::memcpy(&disp, reinterpret_cast<const void *>(j2 + 0x43), sizeof(disp));
  frame_data = reinterpret_cast<const std::uint8_t *const *>(j2 + 0x47 + disp);
  std::memcpy(&frame_field, reinterpret_cast<const void *>(j2 + 0x58),
              sizeof(frame_field));
  view_index_field = *reinterpret_cast<const std::uint8_t *>(j2 + 0xA2);
  render_width_fn = reinterpret_cast<size_fn>(call_target(j2 + 0xA3));
  render_height_fn = reinterpret_cast<size_fn>(call_target(j2 + 0xC4));
  resolve_draw_original = reinterpret_cast<draw_fn>(call_target(*draw_at));
  try {
    jitter_offset_hook.create(reinterpret_cast<void *>(*offset_at),
                              reinterpret_cast<void *>(&jitter_offset_stub));
    jitter_projection_hook.create(
        reinterpret_cast<void *>(j2),
        reinterpret_cast<void *>(&jitter_projection_stub));
    utils::hook::call(*draw_at, &resolve_draw_stub);
  } catch (const std::exception &e) {
    jitter_offset_hook.clear();
    jitter_projection_hook.clear();
    log_off(std::string("DLSS is off: its hooks could not be installed (") +
            e.what() + ")");
    return;
  }
  xerces_runtime::log_info(
      "xerces.upscale.temporal_installed",
      "DLSS: the T2x jitter and resolve are detoured (used when "
      "xerces_upscaler is 2)",
      "{\"jitter_offset\":" + xerces_core::json::quote(rva(*offset_at)) +
          ",\"jitter_projection\":" + xerces_core::json::quote(rva(j2)) +
          ",\"resolve_draw_call\":" + xerces_core::json::quote(rva(*draw_at)) +
          "}");
}
} // namespace

void on_first_present(IDXGISwapChain *chain) {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  module_dir = std::filesystem::path(path).parent_path();
  std::error_code ec;
  const bool present =
      std::filesystem::exists(module_dir / "xerces_dlss.dll", ec) &&
      std::filesystem::exists(module_dir / "nvngx_dlss.dll", ec);
  bool nvidia = false;
  ID3D11Device *device = nullptr;
  if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device))) && device) {
    IDXGIDevice *dxgi = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
      IDXGIAdapter *adapter = nullptr;
      if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc{};
        nvidia = SUCCEEDED(adapter->GetDesc(&desc)) && desc.VendorId == 0x10DE;
        adapter->Release();
      }
      dxgi->Release();
    }
    device->Release();
  }
  const bool usable = present && nvidia;
  scheduler::once(
      [usable] {
        if (available_dvar) {
          game::set_dvar_int("xerces_dlssAvailable", usable ? 1 : 0, false);
        }
      },
      scheduler::main);
  if (present) {
    xerces_runtime::log_info(
        "xerces.upscale.dlss_module",
        nvidia ? "DLSS: the module and nvngx_dlss.dll are next to the client; "
                 "NVIDIA DLSS can be picked"
               : "DLSS: the module is there, but the GPU is not NVIDIA's");
  }
}

ID3D11ShaderResourceView *display_output() {
  return packed_present == present_count() ? out_packed.srv : nullptr;
}

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_upscale_temporal");

public:
  void post_unpack() override {
    available_dvar = game::register_dvar_int(
        "xerces_dlssAvailable", 0, 0, 1, game::DvarFlags{},
        "1 when NVIDIA DLSS can be picked: the optional DLSS module and "
        "NVIDIA's nvngx_dlss.dll are next to the client and the GPU is "
        "NVIDIA's. Set by Xerces.");
    jitter_debug_dvar = game::register_dvar_int(
        "xerces_dlssJitterSign", 0, 0, 31, game::DvarFlags{},
        "Developer setting: flips what Xerces hands DLSS against what was "
        "measured right (1 the jitter's x, 2 its y, 8 the motion vectors); 4 "
        "holds the jitter at (0.4, 0.3) pixels; 16 logs the motion vectors.");
    install();
  }
};
} // namespace xerces_upscale::temporal

REGISTER_COMPONENT(xerces_upscale::temporal::component)
