// The upscaler at the final composite (upscale.hpp, xerces_core/upscale.hpp,
// docs/DESIGN.md 13.3).
//
// The engine's final composite (signature r.final_composite) draws the whole
// scene texture into the whole back buffer through a bilinear sampler while
// applying the display encoding. The detour lets it run, then draws the frame
// again from what the composite left bound:
//
//   1. the composite's own pixel shader, constants and sampler, on a
//      full-screen triangle, 1:1: from the scene texture into a texture the
//      scene's size (AMD FSR 1), or from this frame's DLSS output at the back
//      buffer's size (NVIDIA DLSS, temporal.cpp), so whichever composite
//      variant the game picked does the encoding;
//   2. FSR 1: EASU from that texture to the back buffer's size;
//   3. RCAS (Upscaler Sharpness) into the back buffer, or the last pass
//      straight into it when sharpening is 0.
//
// Everything the passes change on the context is put back as it was, so the
// engine's own state tracking still holds. The HUD is drawn after the
// composite, at full size, as before. Anything unexpected (no back buffer
// bound, no scene texture, a failed resource) leaves the game's picture as it
// is.

#include <std_include.hpp>

#include "runtime.hpp"
#include "signatures.hpp"
#include "upscale.hpp"
#include "upscale_gpu.hpp"

#include "upscale_shaders/fsr1_easu_ps.h"
#include "upscale_shaders/fsr1_rcas_ps.h"

#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>
#include <utils/hook.hpp>

#include <xerces_core/json.hpp>
#include <xerces_core/sigruntime.hpp>
#include <xerces_core/upscale.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace xerces_upscale {
namespace {
namespace core = xerces_core::upscale;
namespace ids = xerces_core::upscale_signatures;

const game::DvarFlags saved{.archive = 1};
game::EngineDependentDvarMut mode_dvar;
game::EngineDependentDvarMut sharpness_dvar;

utils::hook::detour composite_hook;
std::atomic<ID3D11DeviceContext *> context{nullptr};
std::atomic<std::uint64_t> display{0}; // width << 32 | height
std::atomic<std::uint32_t> presents{0};
bool device_kept = false; // Present thread only

// The render thread's own (the composite runs there).
struct fsr_objects {
  gpu::shared_objects shared;
  ID3D11PixelShader *easu = nullptr;
  ID3D11PixelShader *rcas = nullptr;
  ID3D11Buffer *easu_cb = nullptr;
  ID3D11Buffer *rcas_cb = nullptr;
  std::array<std::uint32_t, 16> easu_values{};
  std::array<std::uint32_t, 8> rcas_values{};
  bool easu_set = false;
  bool rcas_set = false;
};

fsr_objects objects;
gpu::target scene_target;    // pass 1's output when another pass follows
gpu::target upscaled_target; // EASU's output when RCAS follows
bool broken = false;         // a resource failed: off for this run
bool logged_active[3]{};
bool logged_skip = false;

void log_off(const std::string &message) {
  xerces_runtime::log_warn("xerces.upscale.off", message);
}

bool make_objects(ID3D11Device *device) {
  bool ok = objects.shared.make(device) &&
            SUCCEEDED(device->CreatePixelShader(
                fsr1_easu_ps, sizeof(fsr1_easu_ps), nullptr, &objects.easu)) &&
            SUCCEEDED(device->CreatePixelShader(
                fsr1_rcas_ps, sizeof(fsr1_rcas_ps), nullptr, &objects.rcas));
  D3D11_BUFFER_DESC buffer{};
  buffer.Usage = D3D11_USAGE_DEFAULT;
  buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  buffer.ByteWidth = sizeof(objects.easu_values);
  ok =
      ok && SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &objects.easu_cb));
  buffer.ByteWidth = sizeof(objects.rcas_values);
  ok =
      ok && SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &objects.rcas_cb));
  return ok;
}

// The back buffer's size when `rtv` is a swap chain buffer.
bool back_buffer_size(ID3D11RenderTargetView *rtv, UINT &width, UINT &height) {
  if (!rtv) {
    return false;
  }
  ID3D11Resource *resource = nullptr;
  rtv->GetResource(&resource);
  if (!resource) {
    return false;
  }
  bool ok = false;
  IDXGIResource *dxgi = nullptr;
  DXGI_USAGE usage = 0;
  if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
    ok = SUCCEEDED(dxgi->GetUsage(&usage)) &&
         (usage & DXGI_USAGE_BACK_BUFFER) != 0;
    dxgi->Release();
  }
  ID3D11Texture2D *texture = nullptr;
  if (ok && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    texture->Release();
    width = desc.Width;
    height = desc.Height;
  } else {
    ok = false;
  }
  resource->Release();
  return ok;
}

void skip_once(const std::string &why) {
  if (!logged_skip) {
    logged_skip = true;
    xerces_runtime::log_info("xerces.upscale.skipped",
                             "Upscaler: the game's own stretch stays (" + why +
                                 ")");
  }
}

void log_active(const core::mode chosen, const core::passes &passes,
                const UINT source_width, const UINT source_height,
                const UINT display_width, const UINT display_height,
                const int sharpness) {
  const auto index = static_cast<std::size_t>(chosen);
  if (index >= std::size(logged_active) || logged_active[index]) {
    return;
  }
  logged_active[index] = true;
  const std::string sizes =
      std::to_string(source_width) + "x" + std::to_string(source_height) +
      (passes.easu ? " to " + std::to_string(display_width) + "x" +
                         std::to_string(display_height)
                   : std::string());
  const std::string sharpening =
      passes.rcas ? ", sharpness " + std::to_string(sharpness) + "/10"
                  : ", no sharpening";
  xerces_runtime::log_info(
      "xerces.upscale.active",
      chosen == core::mode::dlss
          ? "Upscaler: the composite draws NVIDIA DLSS's " +
                std::to_string(display_width) + "x" +
                std::to_string(display_height) + " output" + sharpening
          : "Upscaler: AMD FSR 1 " +
                std::string(passes.easu ? "upscales " : "sharpens ") + sizes +
                sharpening);
}

void upscale(ID3D11DeviceContext *ctx) {
  const core::mode chosen = chosen_mode();
  if (chosen == core::mode::off) {
    return;
  }
  const int sharpening = sharpness();

  gpu::saved_state saved;
  saved.capture(ctx);
  UINT display_width = 0;
  UINT display_height = 0;
  UINT source_width = 0;
  UINT source_height = 0;
  if (!back_buffer_size(saved.rtvs[0], display_width, display_height)) {
    skip_once("the composite did not draw into the back buffer");
    return;
  }
  // One view per frame: in split screen every view's composite comes here,
  // and each was drawn over the whole screen. The game's own picture then,
  // from the frame in which a second view shows up until one frame after the
  // last (review, 2026-11-13). Render thread only.
  {
    static std::uint32_t counted_present = 0;
    static int views = 0;
    static bool several = false;
    const std::uint32_t now = presents.load(std::memory_order_acquire);
    if (now != counted_present) {
      several = views > 1;
      views = 0;
      counted_present = now;
    }
    if (++views > 1 || several) {
      skip_once("more than one view is drawn in a frame (split screen)");
      return;
    }
  }
  if (!saved.ps ||
      !gpu::view_size(saved.srvs[0], source_width, source_height)) {
    skip_once("no scene texture is bound");
    return;
  }

  // DLSS: its output at the back buffer's size replaces the scene; a frame it
  // did not run for keeps the game's own picture.
  ID3D11ShaderResourceView *source = saved.srvs[0];
  bool from_dlss = false;
  if (chosen == core::mode::dlss) {
    ID3D11ShaderResourceView *const output = temporal::display_output();
    if (output) {
      source = output;
      source_width = display_width;
      source_height = display_height;
      from_dlss = true;
    } else if (source_width != display_width ||
               source_height != display_height) {
      skip_once("DLSS has not run for this frame; it needs SMAA T2x or "
                "Filmic SMAA T2x (Graphics > Advanced) and the DLSS module");
      return;
    }
  }
  core::passes passes =
      core::plan(core::mode::fsr1, sharpening, source_width, source_height,
                 display_width, display_height);
  if (from_dlss) {
    passes.active = true; // the DLSS output must replace the stretch
  }
  if (!passes.active) {
    return;
  }

  if (!objects.shared.device) {
    ID3D11Device *device = nullptr;
    ctx->GetDevice(&device);
    const bool made = device && make_objects(device);
    if (device) {
      device->Release();
    }
    if (!made) {
      broken = true;
      log_off("Upscaler: its shaders or states could not be made; the game's "
              "own stretch stays");
      return;
    }
  }
  ID3D11Device *const device = objects.shared.device;
  const bool last_is_pass1 = !passes.easu && !passes.rcas;
  if ((!last_is_pass1 &&
       !scene_target.ensure(device, source_width, source_height,
                            DXGI_FORMAT_R10G10B10A2_UNORM)) ||
      (passes.easu && passes.rcas &&
       !upscaled_target.ensure(device, display_width, display_height,
                               DXGI_FORMAT_R10G10B10A2_UNORM))) {
    broken = true;
    log_off("Upscaler: its textures could not be made; the game's own "
            "stretch stays");
    return;
  }

  ID3D11RenderTargetView *const back_buffer = saved.rtvs[0];
  objects.shared.bind(ctx);

  // 1. The composite's own shader, 1:1. Its sampler and constants stay bound;
  // only the texture changes for DLSS.
  ID3D11RenderTargetView *const first =
      last_is_pass1 ? back_buffer : scene_target.rtv;
  ctx->OMSetRenderTargets(1, &first, nullptr);
  gpu::set_viewport(ctx, source_width, source_height);
  if (from_dlss) {
    ctx->PSSetShaderResources(0, 1, &source);
  }
  ctx->Draw(3, 0);

  ID3D11ShaderResourceView *next = scene_target.srv;
  if (passes.easu) {
    // 2. EASU to the back buffer's size.
    const auto values = core::easu_constants(
        static_cast<float>(source_width), static_cast<float>(source_height),
        static_cast<float>(display_width), static_cast<float>(display_height));
    if (!objects.easu_set || values != objects.easu_values) {
      objects.easu_values = values;
      objects.easu_set = true;
      ctx->UpdateSubresource(objects.easu_cb, 0, nullptr, values.data(), 0, 0);
    }
    ID3D11RenderTargetView *const out =
        passes.rcas ? upscaled_target.rtv : back_buffer;
    ctx->OMSetRenderTargets(1, &out, nullptr);
    gpu::set_viewport(ctx, display_width, display_height);
    ctx->PSSetShader(objects.easu, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &next);
    ctx->PSSetSamplers(0, 1, &objects.shared.linear_clamp);
    ctx->PSSetConstantBuffers(0, 1, &objects.easu_cb);
    ctx->Draw(3, 0);
    next = upscaled_target.srv;
  }
  if (passes.rcas) {
    // 3. RCAS into the back buffer.
    const auto con = core::rcas_constants(core::rcas_stops(sharpening));
    std::array<std::uint32_t, 8> values{};
    std::memcpy(values.data(), con.data(), sizeof(con));
    values[4] = display_width - 1;
    values[5] = display_height - 1;
    if (!objects.rcas_set || values != objects.rcas_values) {
      objects.rcas_values = values;
      objects.rcas_set = true;
      ctx->UpdateSubresource(objects.rcas_cb, 0, nullptr, values.data(), 0, 0);
    }
    // The texture read here is no render target any more.
    ctx->OMSetRenderTargets(1, &back_buffer, nullptr);
    gpu::set_viewport(ctx, display_width, display_height);
    ctx->PSSetShader(objects.rcas, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &next);
    ctx->PSSetConstantBuffers(0, 1, &objects.rcas_cb);
    ctx->Draw(3, 0);
  }

  saved.restore(ctx);
  log_active(chosen, passes, source_width, source_height, display_width,
             display_height, sharpening);
}

void composite_stub(const std::uint64_t view, void *const state,
                    void *const view_data) {
  composite_hook.invoke<void>(view, state, view_data);
  if (broken || !mode_dvar) {
    return;
  }
  ID3D11DeviceContext *const ctx = context.load(std::memory_order_acquire);
  if (ctx) {
    upscale(ctx);
  }
}

std::string rva(const std::uintptr_t address) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%llX",
                static_cast<unsigned long long>(address - 0x140000000));
  return text;
}

void install() {
  if (!xerces_signatures::feature_enabled(core::feature)) {
    log_off(xerces_signatures::feature_message(
        core::feature, "The upscaler",
        "The game draws the scene with its own stretch."));
    return;
  }
  const auto at = xerces_signatures::address(ids::final_composite);
  if (!at) {
    log_off("Upscaler is off: the signature registry has no address for " +
            std::string(ids::final_composite));
    return;
  }
  std::string why;
  if (!xerces_signatures::code_matches(ids::final_composite,
                                       core::composite_entry, &why)) {
    log_off("Upscaler is off: " + why);
    return;
  }
  try {
    composite_hook.create(reinterpret_cast<void *>(*at),
                          reinterpret_cast<void *>(&composite_stub));
  } catch (const std::exception &e) {
    composite_hook.clear();
    log_off(std::string("Upscaler is off: its hook could not be installed (") +
            e.what() + ")");
    return;
  }
  xerces_runtime::log_info(
      "xerces.upscale.installed",
      "Upscaler: the final composite is detoured (AMD FSR 1 when "
      "xerces_upscaler is 1, the DLSS output when it is 2)",
      "{\"final_composite\":" + xerces_core::json::quote(rva(*at)) + "}");
}
} // namespace

core::mode chosen_mode() {
  if (!mode_dvar) {
    return core::mode::off;
  }
  const int value = mode_dvar.get_int();
  return value >= 0 && value <= core::mode_max ? static_cast<core::mode>(value)
                                               : core::mode::off;
}

int sharpness() {
  return sharpness_dvar ? sharpness_dvar.get_int() : core::sharpness_default;
}

ID3D11DeviceContext *game_context() {
  return context.load(std::memory_order_acquire);
}

std::pair<UINT, UINT> display_size() {
  const std::uint64_t value = display.load(std::memory_order_acquire);
  return {static_cast<UINT>(value >> 32), static_cast<UINT>(value)};
}

std::uint32_t present_count() {
  return presents.load(std::memory_order_acquire);
}

void on_present(IDXGISwapChain *chain) {
  DXGI_SWAP_CHAIN_DESC desc{};
  if (SUCCEEDED(chain->GetDesc(&desc))) {
    display.store((static_cast<std::uint64_t>(desc.BufferDesc.Width) << 32) |
                      desc.BufferDesc.Height,
                  std::memory_order_release);
  }
  presents.fetch_add(1, std::memory_order_acq_rel);
  if (device_kept) {
    return;
  }
  device_kept = true;
  ID3D11Device *device = nullptr;
  if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || !device) {
    device_kept = false; // tried again on the next Present
    return;
  }
  ID3D11DeviceContext *ctx = nullptr;
  device->GetImmediateContext(&ctx); // kept for the life of the game
  device->Release();
  context.store(ctx, std::memory_order_release);
  temporal::on_first_present(chain);
}

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_upscale");

public:
  void post_unpack() override {
    mode_dvar = game::register_dvar_int(
        "xerces_upscaler", 0, 0, core::mode_max, saved,
        "How the 3D scene is enlarged when Scene Resolution Ratio is below "
        "100%: 0 the game's own stretch, 1 AMD FidelityFX Super Resolution 1 "
        "(sharper edges and detail; the HUD is not touched), 2 NVIDIA DLSS "
        "(DLAA at 100%; needs the DLSS module, an RTX GPU and SMAA T2x).");
    sharpness_dvar = game::register_dvar_int(
        "xerces_upscalerSharpness", core::sharpness_default, 0,
        core::sharpness_max, saved,
        "The upscaler's sharpening (AMD FSR 1's RCAS, also after DLSS), 0 "
        "(none) to 10 (strongest). It also works at a Scene Resolution Ratio "
        "of 100%.");
    install();
  }
};
} // namespace xerces_upscale

REGISTER_COMPONENT(xerces_upscale::component)
