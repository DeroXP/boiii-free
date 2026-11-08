// AMD FSR 1 in place of the game's own scene stretch (upscale.hpp,
// xerces_core/upscale.hpp, docs/DESIGN.md 13.3).
//
// The engine's final composite (signature r.final_composite) draws the whole
// scene texture into the whole back buffer through a bilinear sampler while
// applying the display encoding. The detour lets it run, then, with FSR 1 on,
// draws the frame again from what the composite left bound:
//
//   1. the composite's own pixel shader, constants, texture and sampler, on a
//      full-screen triangle into a texture the scene's size: the encoded scene
//      1:1, whichever composite variant the game picked;
//   2. EASU from that texture to the back buffer's size;
//   3. RCAS into the back buffer (or EASU straight into it when sharpening is
//      0; RCAS alone when the scene is already the back buffer's size).
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

#include "upscale_shaders/fsr1_easu_ps.h"
#include "upscale_shaders/fsr1_rcas_ps.h"
#include "upscale_shaders/fullscreen_vs.h"

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
bool device_kept = false; // Present thread only

// The render thread's own (the composite runs there).
struct gpu_objects {
  ID3D11Device *device = nullptr;
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *easu = nullptr;
  ID3D11PixelShader *rcas = nullptr;
  ID3D11SamplerState *linear_clamp = nullptr;
  ID3D11RasterizerState *raster = nullptr;
  ID3D11Buffer *easu_cb = nullptr;
  ID3D11Buffer *rcas_cb = nullptr;
  std::array<std::uint32_t, 16> easu_values{};
  std::array<std::uint32_t, 8> rcas_values{};
  bool easu_set = false;
  bool rcas_set = false;
};

struct target {
  ID3D11Texture2D *texture = nullptr;
  ID3D11RenderTargetView *rtv = nullptr;
  ID3D11ShaderResourceView *srv = nullptr;
  UINT width = 0;
  UINT height = 0;

  void release() {
    if (srv) {
      srv->Release();
    }
    if (rtv) {
      rtv->Release();
    }
    if (texture) {
      texture->Release();
    }
    *this = {};
  }
};

gpu_objects gpu;
target scene_target;    // the encoded scene, the scene's size
target upscaled_target; // EASU's output when RCAS follows
bool broken = false;    // a resource failed: off for this run
bool logged_active = false;
bool logged_skip = false;

void log_off(const std::string &message) {
  xerces_runtime::log_warn("xerces.upscale.off", message);
}

bool make_objects(ID3D11Device *device) {
  gpu.device = device;
  device->AddRef();
  bool ok =
      SUCCEEDED(device->CreateVertexShader(fullscreen_vs, sizeof(fullscreen_vs),
                                           nullptr, &gpu.vs)) &&
      SUCCEEDED(device->CreatePixelShader(fsr1_easu_ps, sizeof(fsr1_easu_ps),
                                          nullptr, &gpu.easu)) &&
      SUCCEEDED(device->CreatePixelShader(fsr1_rcas_ps, sizeof(fsr1_rcas_ps),
                                          nullptr, &gpu.rcas));
  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  ok = ok && SUCCEEDED(device->CreateSamplerState(&sampler, &gpu.linear_clamp));
  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  ok = ok && SUCCEEDED(device->CreateRasterizerState(&raster, &gpu.raster));
  D3D11_BUFFER_DESC buffer{};
  buffer.Usage = D3D11_USAGE_DEFAULT;
  buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  buffer.ByteWidth = sizeof(gpu.easu_values);
  ok = ok && SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &gpu.easu_cb));
  buffer.ByteWidth = sizeof(gpu.rcas_values);
  ok = ok && SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &gpu.rcas_cb));
  return ok;
}

bool ensure_target(target &t, const UINT width, const UINT height) {
  if (t.texture && t.width == width && t.height == height) {
    return true;
  }
  t.release();
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, &t.texture)) ||
      FAILED(gpu.device->CreateRenderTargetView(t.texture, nullptr, &t.rtv)) ||
      FAILED(
          gpu.device->CreateShaderResourceView(t.texture, nullptr, &t.srv))) {
    t.release();
    return false;
  }
  t.width = width;
  t.height = height;
  return true;
}

// What the composite left bound, and everything the passes change.
struct saved_state {
  ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
  ID3D11DepthStencilView *dsv = nullptr;
  D3D11_VIEWPORT
  viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
  UINT viewport_count = 0;
  ID3D11RasterizerState *raster = nullptr;
  ID3D11BlendState *blend = nullptr;
  FLOAT blend_factor[4]{};
  UINT sample_mask = 0;
  ID3D11DepthStencilState *depth = nullptr;
  UINT stencil_ref = 0;
  ID3D11InputLayout *layout = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ID3D11VertexShader *vs = nullptr;
  ID3D11GeometryShader *gs = nullptr;
  ID3D11HullShader *hs = nullptr;
  ID3D11DomainShader *ds = nullptr;
  ID3D11PixelShader *ps = nullptr;
  ID3D11ShaderResourceView *srv0 = nullptr;
  ID3D11SamplerState *sampler0 = nullptr;
  ID3D11Buffer *cb0 = nullptr;

  void capture(ID3D11DeviceContext *ctx) {
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&viewport_count, viewports);
    ctx->RSGetState(&raster);
    ctx->OMGetBlendState(&blend, blend_factor, &sample_mask);
    ctx->OMGetDepthStencilState(&depth, &stencil_ref);
    ctx->IAGetInputLayout(&layout);
    ctx->IAGetPrimitiveTopology(&topology);
    ctx->VSGetShader(&vs, nullptr, nullptr);
    ctx->GSGetShader(&gs, nullptr, nullptr);
    ctx->HSGetShader(&hs, nullptr, nullptr);
    ctx->DSGetShader(&ds, nullptr, nullptr);
    ctx->PSGetShader(&ps, nullptr, nullptr);
    ctx->PSGetShaderResources(0, 1, &srv0);
    ctx->PSGetSamplers(0, 1, &sampler0);
    ctx->PSGetConstantBuffers(0, 1, &cb0);
  }

  void restore(ID3D11DeviceContext *ctx) const {
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    ctx->RSSetViewports(viewport_count, viewports);
    ctx->RSSetState(raster);
    ctx->OMSetBlendState(blend, blend_factor, sample_mask);
    ctx->OMSetDepthStencilState(depth, stencil_ref);
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(topology);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->GSSetShader(gs, nullptr, 0);
    ctx->HSSetShader(hs, nullptr, 0);
    ctx->DSSetShader(ds, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv0);
    ctx->PSSetSamplers(0, 1, &sampler0);
    ctx->PSSetConstantBuffers(0, 1, &cb0);
  }

  ~saved_state() {
    for (ID3D11RenderTargetView *rtv : rtvs) {
      if (rtv) {
        rtv->Release();
      }
    }
    IUnknown *const objects[] = {dsv, raster, blend, depth, layout,   vs, gs,
                                 hs,  ds,     ps,    srv0,  sampler0, cb0};
    for (IUnknown *object : objects) {
      if (object) {
        object->Release();
      }
    }
  }
};

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

// The scene texture's size, when the view shows the whole of a plain 2D
// texture's top mip (what the composite samples, UV 0 to 1).
bool scene_size(ID3D11ShaderResourceView *srv, UINT &width, UINT &height) {
  if (!srv) {
    return false;
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC view{};
  srv->GetDesc(&view);
  if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
      view.Texture2D.MostDetailedMip != 0) {
    return false;
  }
  ID3D11Resource *resource = nullptr;
  srv->GetResource(&resource);
  if (!resource) {
    return false;
  }
  bool ok = false;
  ID3D11Texture2D *texture = nullptr;
  if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    texture->Release();
    ok = desc.SampleDesc.Count == 1;
    width = desc.Width;
    height = desc.Height;
  }
  resource->Release();
  return ok;
}

void set_viewport(ID3D11DeviceContext *ctx, const UINT width,
                  const UINT height) {
  D3D11_VIEWPORT viewport{};
  viewport.Width = static_cast<float>(width);
  viewport.Height = static_cast<float>(height);
  viewport.MaxDepth = 1.0f;
  ctx->RSSetViewports(1, &viewport);
}

void skip_once(const std::string &why) {
  if (!logged_skip) {
    logged_skip = true;
    xerces_runtime::log_info("xerces.upscale.skipped",
                             "Upscaler: the game's own stretch stays (" + why +
                                 ")");
  }
}

void upscale(ID3D11DeviceContext *ctx) {
  const auto chosen = static_cast<core::mode>(mode_dvar.get_int());
  if (chosen == core::mode::off) {
    return;
  }
  const int sharpness =
      sharpness_dvar ? sharpness_dvar.get_int() : core::sharpness_default;

  saved_state saved;
  saved.capture(ctx);
  UINT display_width = 0;
  UINT display_height = 0;
  UINT source_width = 0;
  UINT source_height = 0;
  if (!back_buffer_size(saved.rtvs[0], display_width, display_height)) {
    skip_once("the composite did not draw into the back buffer");
    return;
  }
  if (!saved.ps || !scene_size(saved.srv0, source_width, source_height)) {
    skip_once("no scene texture is bound");
    return;
  }
  const core::passes passes =
      core::plan(chosen, sharpness, source_width, source_height, display_width,
                 display_height);
  if (!passes.active) {
    return;
  }

  if (!gpu.device) {
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
  if (!ensure_target(scene_target, source_width, source_height) ||
      (passes.easu && passes.rcas &&
       !ensure_target(upscaled_target, display_width, display_height))) {
    broken = true;
    log_off("Upscaler: its textures could not be made; the game's own "
            "stretch stays");
    return;
  }

  // Shared by the three passes.
  ID3D11RenderTargetView *const back_buffer = saved.rtvs[0];
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(gpu.vs, nullptr, 0);
  ctx->GSSetShader(nullptr, nullptr, 0);
  ctx->HSSetShader(nullptr, nullptr, 0);
  ctx->DSSetShader(nullptr, nullptr, 0);
  ctx->RSSetState(gpu.raster);
  ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(nullptr, 0);

  // 1. The composite's own shader, 1:1 into the scene-sized texture.
  ctx->OMSetRenderTargets(1, &scene_target.rtv, nullptr);
  set_viewport(ctx, source_width, source_height);
  ctx->Draw(3, 0);

  ID3D11ShaderResourceView *next = scene_target.srv;
  if (passes.easu) {
    // 2. EASU to the back buffer's size.
    const auto values = core::easu_constants(
        static_cast<float>(source_width), static_cast<float>(source_height),
        static_cast<float>(display_width), static_cast<float>(display_height));
    if (!gpu.easu_set || values != gpu.easu_values) {
      gpu.easu_values = values;
      gpu.easu_set = true;
      ctx->UpdateSubresource(gpu.easu_cb, 0, nullptr, values.data(), 0, 0);
    }
    ID3D11RenderTargetView *const out =
        passes.rcas ? upscaled_target.rtv : back_buffer;
    ctx->OMSetRenderTargets(1, &out, nullptr);
    set_viewport(ctx, display_width, display_height);
    ctx->PSSetShader(gpu.easu, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &next);
    ctx->PSSetSamplers(0, 1, &gpu.linear_clamp);
    ctx->PSSetConstantBuffers(0, 1, &gpu.easu_cb);
    ctx->Draw(3, 0);
    next = upscaled_target.srv;
  }
  if (passes.rcas) {
    // 3. RCAS into the back buffer.
    const auto con = core::rcas_constants(core::rcas_stops(sharpness));
    std::array<std::uint32_t, 8> values{};
    std::memcpy(values.data(), con.data(), sizeof(con));
    values[4] = display_width - 1;
    values[5] = display_height - 1;
    if (!gpu.rcas_set || values != gpu.rcas_values) {
      gpu.rcas_values = values;
      gpu.rcas_set = true;
      ctx->UpdateSubresource(gpu.rcas_cb, 0, nullptr, values.data(), 0, 0);
    }
    // The texture read here is no render target any more.
    ctx->OMSetRenderTargets(1, &back_buffer, nullptr);
    set_viewport(ctx, display_width, display_height);
    ctx->PSSetShader(gpu.rcas, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &next);
    ctx->PSSetConstantBuffers(0, 1, &gpu.rcas_cb);
    ctx->Draw(3, 0);
  }

  // The passes' texture must not stay bound as a shader input.
  ID3D11ShaderResourceView *const none = nullptr;
  ctx->PSSetShaderResources(0, 1, &none);
  saved.restore(ctx);

  if (!logged_active) {
    logged_active = true;
    xerces_runtime::log_info(
        "xerces.upscale.active",
        "Upscaler: AMD FSR 1 " +
            std::string(passes.easu ? "upscales " : "sharpens ") +
            std::to_string(source_width) + "x" + std::to_string(source_height) +
            (passes.easu ? " to " + std::to_string(display_width) + "x" +
                               std::to_string(display_height)
                         : std::string()) +
            (passes.rcas ? ", sharpness " + std::to_string(sharpness) + "/10"
                         : ", no sharpening"));
  }
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
      "xerces_upscaler is 1)",
      "{\"final_composite\":" + xerces_core::json::quote(rva(*at)) + "}");
}
} // namespace

void on_present(IDXGISwapChain *chain) {
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
}

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_upscale");

public:
  void post_unpack() override {
    mode_dvar = game::register_dvar_int(
        "xerces_upscaler", 0, 0, core::mode_max, saved,
        "How the 3D scene is enlarged when Scene Resolution Ratio is below "
        "100%: 0 the game's own stretch, 1 AMD FidelityFX Super Resolution 1 "
        "(sharper edges and detail; the HUD is not touched).");
    sharpness_dvar = game::register_dvar_int(
        "xerces_upscalerSharpness", core::sharpness_default, 0,
        core::sharpness_max, saved,
        "AMD FSR 1's sharpening, 0 (none) to 10 (strongest). It also works at "
        "a Scene Resolution Ratio of 100%.");
    install();
  }
};
} // namespace xerces_upscale

REGISTER_COMPONENT(xerces_upscale::component)
