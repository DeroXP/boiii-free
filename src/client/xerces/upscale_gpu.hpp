#pragma once

// Direct3D 11 pieces the upscaler's passes share (upscale.cpp, the final
// composite; temporal.cpp, the SMAA T2x resolve): render targets of their own,
// a record of the context state they change so it can be put back exactly
// (the engine tracks what it bound and skips setting it again), and the
// full-screen triangle's shader and states. Render thread only.

#include <std_include.hpp>

#include "upscale_shaders/fullscreen_vs.h"

namespace xerces_upscale::gpu {
// A texture the passes draw into and read from.
struct target {
  ID3D11Texture2D *texture = nullptr;
  ID3D11RenderTargetView *rtv = nullptr;
  ID3D11ShaderResourceView *srv = nullptr;
  ID3D11UnorderedAccessView *uav = nullptr;
  UINT width = 0;
  UINT height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

  void release() {
    IUnknown *const objects[] = {uav, srv, rtv, texture};
    for (IUnknown *object : objects) {
      if (object) {
        object->Release();
      }
    }
    *this = {};
  }

  // Made again when the size or format changes; false when it cannot be.
  bool ensure(ID3D11Device *device, const UINT w, const UINT h,
              const DXGI_FORMAT f, const bool unordered = false) {
    if (texture && width == w && height == h && format == f &&
        (uav != nullptr) == unordered) {
      return true;
    }
    release();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = f;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                     (unordered ? D3D11_BIND_UNORDERED_ACCESS : 0);
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)) ||
        FAILED(device->CreateRenderTargetView(texture, nullptr, &rtv)) ||
        FAILED(device->CreateShaderResourceView(texture, nullptr, &srv)) ||
        (unordered &&
         FAILED(device->CreateUnorderedAccessView(texture, nullptr, &uav)))) {
      release();
      return false;
    }
    width = w;
    height = h;
    format = f;
    return true;
  }
};

// What a pass changes on the context. SRVs and samplers are kept for the
// first `kSlots` pixel shader slots, the constant buffer for slot 0.
struct saved_state {
  static constexpr UINT kSlots = 3;

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
  ID3D11ShaderResourceView *srvs[kSlots]{};
  ID3D11SamplerState *samplers[kSlots]{};
  ID3D11Buffer *cb0 = nullptr;

  saved_state() = default;
  saved_state(const saved_state &) = delete;
  saved_state &operator=(const saved_state &) = delete;

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
    ctx->PSGetShaderResources(0, kSlots, srvs);
    ctx->PSGetSamplers(0, kSlots, samplers);
    ctx->PSGetConstantBuffers(0, 1, &cb0);
  }

  void restore(ID3D11DeviceContext *ctx) const {
    // The passes' own textures first leave the shader inputs, then every
    // binding is what it was.
    ID3D11ShaderResourceView *const none[kSlots]{};
    ctx->PSSetShaderResources(0, kSlots, none);
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
    ctx->PSSetShaderResources(0, kSlots, srvs);
    ctx->PSSetSamplers(0, kSlots, samplers);
    ctx->PSSetConstantBuffers(0, 1, &cb0);
  }

  ~saved_state() {
    for (ID3D11RenderTargetView *rtv : rtvs) {
      if (rtv) {
        rtv->Release();
      }
    }
    for (UINT i = 0; i < kSlots; ++i) {
      if (srvs[i]) {
        srvs[i]->Release();
      }
      if (samplers[i]) {
        samplers[i]->Release();
      }
    }
    IUnknown *const objects[] = {dsv, raster, blend, depth, layout, vs,
                                 gs,  hs,     ds,    ps,    cb0};
    for (IUnknown *object : objects) {
      if (object) {
        object->Release();
      }
    }
  }
};

// The full-screen triangle and the states every pass uses, made once.
struct shared_objects {
  ID3D11Device *device = nullptr;
  ID3D11VertexShader *vs = nullptr;
  ID3D11RasterizerState *raster = nullptr;
  ID3D11SamplerState *linear_clamp = nullptr;

  bool make(ID3D11Device *d) {
    device = d;
    device->AddRef();
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    return SUCCEEDED(device->CreateVertexShader(
               fullscreen_vs, sizeof(fullscreen_vs), nullptr, &vs)) &&
           SUCCEEDED(device->CreateSamplerState(&sampler, &linear_clamp)) &&
           SUCCEEDED(device->CreateRasterizerState(&rasterizer, &raster));
  }

  // The full-screen triangle's pipeline, all but the targets and the pixel
  // shader's inputs.
  void bind(ID3D11DeviceContext *ctx) const {
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->RSSetState(raster);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
  }
};

inline void set_viewport(ID3D11DeviceContext *ctx, const UINT width,
                         const UINT height) {
  D3D11_VIEWPORT viewport{};
  viewport.Width = static_cast<float>(width);
  viewport.Height = static_cast<float>(height);
  viewport.MaxDepth = 1.0f;
  ctx->RSSetViewports(1, &viewport);
}

// The size of the 2D texture behind a view (its top mip), and whether it is a
// plain single-sample texture whose whole top mip the view shows.
inline bool view_size(ID3D11ShaderResourceView *srv, UINT &width,
                      UINT &height) {
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
} // namespace xerces_upscale::gpu
