// The Direct3D 11 frame tracer (gfx_trace.hpp, docs/DESIGN.md 13.3).

#include <std_include.hpp>

#include "gfx_trace.hpp"
#include "runtime.hpp"

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>
#include <utils/hook.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace xerces_gfx_trace {
namespace detail {
std::atomic_bool on{false};
} // namespace detail

namespace {
// COM vtable slots, in d3d11.h's declaration order (the COM ABI, not game
// addresses).
constexpr std::size_t dev_create_texture2d = 5;
constexpr std::size_t dev_create_vertex_shader = 12;
constexpr std::size_t dev_create_pixel_shader = 15;
constexpr std::size_t dev_create_compute_shader = 18;
constexpr std::size_t dev_create_deferred_context = 27;

constexpr std::size_t ctx_draw_indexed = 12;
constexpr std::size_t ctx_draw = 13;
constexpr std::size_t ctx_draw_indexed_instanced = 20;
constexpr std::size_t ctx_draw_instanced = 21;
constexpr std::size_t ctx_dispatch = 41;
constexpr std::size_t ctx_copy_subresource_region = 46;
constexpr std::size_t ctx_copy_resource = 47;
constexpr std::size_t ctx_clear_rtv = 50;
constexpr std::size_t ctx_clear_dsv = 53;
constexpr std::size_t ctx_resolve = 57;
constexpr std::size_t ctx_execute_command_list = 58;

constexpr std::uintptr_t game_low = 0x140000000;
constexpr std::uintptr_t game_high = 0x150000000;

utils::hook::detour create_texture2d_hook;
utils::hook::detour create_vs_hook;
utils::hook::detour create_ps_hook;
utils::hook::detour create_cs_hook;
utils::hook::detour create_deferred_hook;

utils::hook::detour draw_indexed_hook;
utils::hook::detour draw_hook;
utils::hook::detour draw_indexed_instanced_hook;
utils::hook::detour draw_instanced_hook;
utils::hook::detour dispatch_hook;
utils::hook::detour copy_region_hook;
utils::hook::detour copy_resource_hook;
utils::hook::detour clear_rtv_hook;
utils::hook::detour clear_dsv_hook;
utils::hook::detour resolve_hook;
utils::hook::detour execute_hook;

std::filesystem::path out_dir;
void *dummy_device_vtable_ps = nullptr; // CreatePixelShader on the dummy

// Shaders by object, their bytecode by id.
struct shader_ref {
  std::uint64_t id = 0;
  char kind = '?';
};
std::mutex shaders_mutex;
std::unordered_map<const void *, shader_ref> shader_by_object;
std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> code_by_id;

// Render, depth and UAV textures made since the hooks went in: where.
struct texture_made {
  D3D11_TEXTURE2D_DESC desc{};
  std::uintptr_t callers[4]{};
};
std::mutex textures_mutex;
std::unordered_map<const void *, texture_made> textures_made;
std::unordered_map<const void *, int> texture_ids; // short ids in traces
std::atomic_int deferred_contexts{0};
void *deferred_vtable_draw = nullptr;

// The capture.
std::atomic_bool capturing{false};
std::atomic_int arm_request{0}; // frames asked for, 0 = none
std::atomic_bool arm_cb{false};
bool context_hooked = false; // Present thread only
int frames_left = 0;         // Present thread only
int frame_number = 0;        // Present thread only
bool want_cb = false;

std::mutex trace_mutex;
std::string trace_text;
int event_number = 0;
std::set<std::uint64_t> frame_shaders;
std::set<const void *> frame_textures;
const void *back_buffer = nullptr;

std::uint64_t shader_id(const void *code, SIZE_T size) {
  const auto *bytes = static_cast<const std::uint8_t *>(code);
  if (size >= 20 && std::memcmp(bytes, "DXBC", 4) == 0) {
    std::uint64_t id = 0;
    std::memcpy(&id, bytes + 4, sizeof(id));
    return id;
  }
  std::uint64_t hash = 1469598103934665603ull; // FNV-1a
  for (SIZE_T i = 0; i < size; ++i) {
    hash = (hash ^ bytes[i]) * 1099511628211ull;
  }
  return hash;
}

void note_shader(const void *object, const void *code, SIZE_T size, char kind) {
  if (!object || !code || size == 0) {
    return;
  }
  const std::uint64_t id = shader_id(code, size);
  std::lock_guard lock(shaders_mutex);
  shader_by_object[object] = {id, kind};
  auto &stored = code_by_id[id];
  if (stored.empty()) {
    const auto *bytes = static_cast<const std::uint8_t *>(code);
    stored.assign(bytes, bytes + size);
  }
}

std::string hex(std::uint64_t value, int digits) {
  char text[24];
  std::snprintf(text, sizeof(text), "%0*llx", digits,
                static_cast<unsigned long long>(value));
  return text;
}

std::string where(std::uintptr_t address) {
  if (address >= game_low && address < game_high) {
    return "0x" + hex(address, 9);
  }
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &module) &&
      module) {
    wchar_t name[MAX_PATH]{};
    GetModuleFileNameW(module, name, MAX_PATH);
    const std::filesystem::path path(name);
    return path.filename().string() + "+0x" +
           hex(address - reinterpret_cast<std::uintptr_t>(module), 1);
  }
  return "?0x" + hex(address, 1);
}

// Whether an address is in the client itself (boiii.exe): the tracer's own
// frames, left out of the callers.
bool in_client(const std::uintptr_t address) {
  static const auto base =
      reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  HMODULE module = nullptr;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) &&
         reinterpret_cast<std::uintptr_t>(module) == base;
}

// The first five callers outside the client: where the game made the call.
std::string callers(int skip) {
  void *frames[12]{};
  const USHORT count = RtlCaptureStackBackTrace(skip, 12, frames, nullptr);
  std::string out;
  int written = 0;
  for (USHORT i = 0; i < count && written < 5; ++i) {
    const auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
    if (written == 0 && in_client(address)) {
      continue;
    }
    if (written++) {
      out += '<';
    }
    out += where(address);
  }
  return out;
}

std::string shader_text(const void *object) {
  if (!object) {
    return "-";
  }
  std::lock_guard lock(shaders_mutex);
  const auto found = shader_by_object.find(object);
  if (found == shader_by_object.end()) {
    return "unknown";
  }
  frame_shaders.insert(found->second.id);
  return hex(found->second.id, 16);
}

// Holds trace_mutex.
int texture_id(const void *resource) {
  std::lock_guard lock(textures_mutex);
  const auto found = texture_ids.find(resource);
  if (found != texture_ids.end()) {
    return found->second;
  }
  const int id = static_cast<int>(texture_ids.size()) + 1;
  texture_ids.emplace(resource, id);
  return id;
}

std::string resource_text(ID3D11Resource *resource, DXGI_FORMAT view_format) {
  if (!resource) {
    return "-";
  }
  if (resource == back_buffer) {
    frame_textures.insert(resource);
  }
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  resource->GetType(&dim);
  const int id = texture_id(resource);
  frame_textures.insert(resource);
  std::string out = (resource == back_buffer ? "BB" : "T") + std::to_string(id);
  if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    ID3D11Texture2D *texture = nullptr;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
      D3D11_TEXTURE2D_DESC desc{};
      texture->GetDesc(&desc);
      texture->Release();
      out += ' ' + std::to_string(desc.Width) + 'x' +
             std::to_string(desc.Height) + " f" +
             std::to_string(static_cast<int>(desc.Format));
      if (view_format != DXGI_FORMAT_UNKNOWN && view_format != desc.Format) {
        out += "/v" + std::to_string(static_cast<int>(view_format));
      }
      if (desc.SampleDesc.Count > 1) {
        out += " ms" + std::to_string(desc.SampleDesc.Count);
      }
      if (desc.ArraySize > 1) {
        out += " a" + std::to_string(desc.ArraySize);
      }
      if (desc.MipLevels > 1) {
        out += " m" + std::to_string(desc.MipLevels);
      }
    }
  } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
    ID3D11Buffer *buffer = nullptr;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))) {
      D3D11_BUFFER_DESC desc{};
      buffer->GetDesc(&desc);
      buffer->Release();
      out += " buf" + std::to_string(desc.ByteWidth);
    }
  } else {
    out += " dim" + std::to_string(static_cast<int>(dim));
  }
  return out;
}

template <typename View, typename Desc> std::string view_text(View *view) {
  if (!view) {
    return "-";
  }
  Desc desc{};
  view->GetDesc(&desc);
  ID3D11Resource *resource = nullptr;
  view->GetResource(&resource);
  std::string out = resource_text(resource, desc.Format);
  if (resource) {
    resource->Release();
  }
  return out;
}

std::string sampler_text(ID3D11SamplerState *sampler) {
  D3D11_SAMPLER_DESC desc{};
  sampler->GetDesc(&desc);
  return "f" + hex(desc.Filter, 1) + "/a" + std::to_string(desc.AddressU) +
         (desc.MipLODBias != 0.0f ? "/b" + std::to_string(desc.MipLODBias)
                                  : "");
}

// The first 256 bytes of each bound constant buffer, read back (immediate
// context only: a deferred one cannot map for reading).
std::string constant_text(ID3D11DeviceContext *ctx, ID3D11Buffer **buffers,
                          int count) {
  ID3D11Device *device = nullptr;
  ctx->GetDevice(&device);
  if (!device) {
    return {};
  }
  std::string out;
  for (int slot = 0; slot < count; ++slot) {
    ID3D11Buffer *buffer = buffers[slot];
    if (!buffer) {
      continue;
    }
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    D3D11_BUFFER_DESC staging_desc{};
    staging_desc.ByteWidth = desc.ByteWidth;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer *staging = nullptr;
    if (FAILED(device->CreateBuffer(&staging_desc, nullptr, &staging))) {
      continue;
    }
    copy_resource_hook.invoke<void>(ctx, staging, buffer);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
      const UINT floats = (std::min)(desc.ByteWidth, 256u) / 4;
      const auto *values = static_cast<const float *>(mapped.pData);
      out += "\n      cb" + std::to_string(slot) + "[" +
             std::to_string(desc.ByteWidth) + "]";
      for (UINT i = 0; i < floats; ++i) {
        char text[32];
        std::snprintf(text, sizeof(text), "%s%.6g", i % 4 ? "," : " | ",
                      values[i]);
        out += text;
      }
      ctx->Unmap(staging, 0);
    }
    staging->Release();
  }
  device->Release();
  return out;
}

void begin_event(const char *what, const std::string &detail, int skip) {
  char head[48];
  std::snprintf(head, sizeof(head), "#%05d ", ++event_number);
  trace_text += head;
  trace_text += what;
  if (!detail.empty()) {
    trace_text += ' ';
    trace_text += detail;
  }
  trace_text += " at=" + callers(skip);
}

__declspec(noinline) void record_draw(ID3D11DeviceContext *ctx,
                                      const char *what,
                                      const std::string &detail) {
  std::lock_guard lock(trace_mutex);
  if (!capturing.load(std::memory_order_relaxed)) {
    return;
  }
  const bool deferred = ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
  begin_event(what, detail + (deferred ? " ctx=D" : ""), 3);

  ID3D11VertexShader *vs = nullptr;
  ctx->VSGetShader(&vs, nullptr, nullptr);
  ID3D11PixelShader *ps = nullptr;
  ctx->PSGetShader(&ps, nullptr, nullptr);
  trace_text += " vs=" + shader_text(vs) + " ps=" + shader_text(ps);
  if (vs) {
    vs->Release();
  }
  if (ps) {
    ps->Release();
  }

  ID3D11RenderTargetView *rtvs[8]{};
  ID3D11DepthStencilView *dsv = nullptr;
  ctx->OMGetRenderTargets(8, rtvs, &dsv);
  trace_text += "\n    rt=[";
  bool first = true;
  for (int i = 0; i < 8; ++i) {
    if (rtvs[i]) {
      trace_text +=
          (first ? "" : ", ") + std::to_string(i) + ":" +
          view_text<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>(
              rtvs[i]);
      first = false;
      rtvs[i]->Release();
    }
  }
  trace_text +=
      "] ds=" +
      view_text<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>(dsv);
  if (dsv) {
    dsv->Release();
  }

  D3D11_VIEWPORT viewports[2]{};
  UINT viewport_count = 2;
  ctx->RSGetViewports(&viewport_count, viewports);
  if (viewport_count > 0) {
    char text[96];
    std::snprintf(text, sizeof(text), " vp=%g,%g,%gx%g", viewports[0].TopLeftX,
                  viewports[0].TopLeftY, viewports[0].Width,
                  viewports[0].Height);
    trace_text += text;
  }

  ID3D11ShaderResourceView *srvs[16]{};
  ctx->PSGetShaderResources(0, 16, srvs);
  trace_text += "\n    ps.t=[";
  first = true;
  for (int i = 0; i < 16; ++i) {
    if (srvs[i]) {
      trace_text +=
          (first ? "" : ", ") + std::to_string(i) + ":" +
          view_text<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>(
              srvs[i]);
      first = false;
      srvs[i]->Release();
    }
  }
  trace_text += "]";

  ID3D11SamplerState *samplers[4]{};
  ctx->PSGetSamplers(0, 4, samplers);
  trace_text += " ps.s=[";
  first = true;
  for (int i = 0; i < 4; ++i) {
    if (samplers[i]) {
      trace_text += (first ? "" : ", ") + std::to_string(i) + ":" +
                    sampler_text(samplers[i]);
      first = false;
      samplers[i]->Release();
    }
  }
  trace_text += "]";

  if (want_cb && !deferred) {
    ID3D11Buffer *buffers[4]{};
    ctx->PSGetConstantBuffers(0, 4, buffers);
    trace_text += constant_text(ctx, buffers, 4);
    for (ID3D11Buffer *buffer : buffers) {
      if (buffer) {
        buffer->Release();
      }
    }
  }
  trace_text += '\n';
}

__declspec(noinline) void record_dispatch(ID3D11DeviceContext *ctx,
                                          const std::string &detail) {
  std::lock_guard lock(trace_mutex);
  if (!capturing.load(std::memory_order_relaxed)) {
    return;
  }
  const bool deferred = ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
  begin_event("Dispatch", detail + (deferred ? " ctx=D" : ""), 3);
  ID3D11ComputeShader *cs = nullptr;
  ctx->CSGetShader(&cs, nullptr, nullptr);
  trace_text += " cs=" + shader_text(cs);
  if (cs) {
    cs->Release();
  }
  ID3D11ShaderResourceView *srvs[16]{};
  ctx->CSGetShaderResources(0, 16, srvs);
  trace_text += "\n    cs.t=[";
  bool first = true;
  for (int i = 0; i < 16; ++i) {
    if (srvs[i]) {
      trace_text +=
          (first ? "" : ", ") + std::to_string(i) + ":" +
          view_text<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>(
              srvs[i]);
      first = false;
      srvs[i]->Release();
    }
  }
  ID3D11UnorderedAccessView *uavs[8]{};
  ctx->CSGetUnorderedAccessViews(0, 8, uavs);
  trace_text += "] cs.u=[";
  first = true;
  for (int i = 0; i < 8; ++i) {
    if (uavs[i]) {
      trace_text += (first ? "" : ", ") + std::to_string(i) + ":" +
                    view_text<ID3D11UnorderedAccessView,
                              D3D11_UNORDERED_ACCESS_VIEW_DESC>(uavs[i]);
      first = false;
      uavs[i]->Release();
    }
  }
  trace_text += "]";
  if (want_cb && !deferred) {
    ID3D11Buffer *buffers[4]{};
    ctx->CSGetConstantBuffers(0, 4, buffers);
    trace_text += constant_text(ctx, buffers, 4);
    for (ID3D11Buffer *buffer : buffers) {
      if (buffer) {
        buffer->Release();
      }
    }
  }
  trace_text += '\n';
}

__declspec(noinline) void record_simple(const char *what,
                                        const std::string &detail) {
  std::lock_guard lock(trace_mutex);
  if (!capturing.load(std::memory_order_relaxed)) {
    return;
  }
  begin_event(what, detail, 3);
  trace_text += '\n';
}

// --- context detours ---

void __stdcall draw_indexed_stub(ID3D11DeviceContext *ctx, UINT count,
                                 UINT start, INT base) {
  if (capturing.load(std::memory_order_relaxed)) {
    record_draw(ctx, "DrawIndexed", "n=" + std::to_string(count));
  }
  draw_indexed_hook.invoke<void>(ctx, count, start, base);
}

void __stdcall draw_stub(ID3D11DeviceContext *ctx, UINT count, UINT start) {
  if (capturing.load(std::memory_order_relaxed)) {
    record_draw(ctx, "Draw", "n=" + std::to_string(count));
  }
  draw_hook.invoke<void>(ctx, count, start);
}

void __stdcall draw_indexed_instanced_stub(ID3D11DeviceContext *ctx, UINT count,
                                           UINT instances, UINT start, INT base,
                                           UINT start_instance) {
  if (capturing.load(std::memory_order_relaxed)) {
    record_draw(ctx, "DrawIndexedInstanced",
                "n=" + std::to_string(count) +
                    " i=" + std::to_string(instances));
  }
  draw_indexed_instanced_hook.invoke<void>(ctx, count, instances, start, base,
                                           start_instance);
}

void __stdcall draw_instanced_stub(ID3D11DeviceContext *ctx, UINT count,
                                   UINT instances, UINT start,
                                   UINT start_instance) {
  if (capturing.load(std::memory_order_relaxed)) {
    record_draw(ctx, "DrawInstanced",
                "n=" + std::to_string(count) +
                    " i=" + std::to_string(instances));
  }
  draw_instanced_hook.invoke<void>(ctx, count, instances, start,
                                   start_instance);
}

void __stdcall dispatch_stub(ID3D11DeviceContext *ctx, UINT x, UINT y, UINT z) {
  if (capturing.load(std::memory_order_relaxed)) {
    record_dispatch(ctx, std::to_string(x) + "x" + std::to_string(y) + "x" +
                             std::to_string(z));
  }
  dispatch_hook.invoke<void>(ctx, x, y, z);
}

std::string resource_pair(ID3D11Resource *dst, ID3D11Resource *src) {
  std::lock_guard lock(trace_mutex);
  return "dst=" + resource_text(dst, DXGI_FORMAT_UNKNOWN) +
         " src=" + resource_text(src, DXGI_FORMAT_UNKNOWN);
}

void __stdcall copy_region_stub(ID3D11DeviceContext *ctx, ID3D11Resource *dst,
                                UINT dst_sub, UINT x, UINT y, UINT z,
                                ID3D11Resource *src, UINT src_sub,
                                const D3D11_BOX *box) {
  if (capturing.load(std::memory_order_relaxed)) {
    std::string detail = resource_pair(dst, src) + " to=" + std::to_string(x) +
                         "," + std::to_string(y);
    if (box) {
      detail += " box=" + std::to_string(box->left) + "," +
                std::to_string(box->top) + "," + std::to_string(box->right) +
                "," + std::to_string(box->bottom);
    }
    record_simple("CopySubresourceRegion", detail);
  }
  copy_region_hook.invoke<void>(ctx, dst, dst_sub, x, y, z, src, src_sub, box);
}

void __stdcall copy_resource_stub(ID3D11DeviceContext *ctx, ID3D11Resource *dst,
                                  ID3D11Resource *src) {
  if (capturing.load(std::memory_order_relaxed)) {
    record_simple("CopyResource", resource_pair(dst, src));
  }
  copy_resource_hook.invoke<void>(ctx, dst, src);
}

void __stdcall clear_rtv_stub(ID3D11DeviceContext *ctx,
                              ID3D11RenderTargetView *view,
                              const FLOAT color[4]) {
  if (capturing.load(std::memory_order_relaxed)) {
    std::string detail;
    {
      std::lock_guard lock(trace_mutex);
      detail = view_text<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>(
          view);
    }
    record_simple("ClearRTV", detail);
  }
  clear_rtv_hook.invoke<void>(ctx, view, color);
}

void __stdcall clear_dsv_stub(ID3D11DeviceContext *ctx,
                              ID3D11DepthStencilView *view, UINT flags,
                              FLOAT depth, UINT8 stencil) {
  if (capturing.load(std::memory_order_relaxed)) {
    std::string detail;
    {
      std::lock_guard lock(trace_mutex);
      detail = view_text<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>(
                   view) +
               " depth=" + std::to_string(depth);
    }
    record_simple("ClearDSV", detail);
  }
  clear_dsv_hook.invoke<void>(ctx, view, flags, depth, stencil);
}

void __stdcall resolve_stub(ID3D11DeviceContext *ctx, ID3D11Resource *dst,
                            UINT dst_sub, ID3D11Resource *src, UINT src_sub,
                            DXGI_FORMAT format) {
  if (capturing.load(std::memory_order_relaxed)) {
    record_simple("ResolveSubresource", resource_pair(dst, src));
  }
  resolve_hook.invoke<void>(ctx, dst, dst_sub, src, src_sub, format);
}

void __stdcall execute_stub(ID3D11DeviceContext *ctx, ID3D11CommandList *list,
                            BOOL restore) {
  if (capturing.load(std::memory_order_relaxed)) {
    record_simple("ExecuteCommandList", {});
  }
  execute_hook.invoke<void>(ctx, list, restore);
}

// --- device detours ---

HRESULT __stdcall create_texture2d_stub(ID3D11Device *device,
                                        const D3D11_TEXTURE2D_DESC *desc,
                                        const D3D11_SUBRESOURCE_DATA *data,
                                        ID3D11Texture2D **out) {
  const HRESULT result =
      create_texture2d_hook.invoke<HRESULT>(device, desc, data, out);
  constexpr UINT targets = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL |
                           D3D11_BIND_UNORDERED_ACCESS;
  if (SUCCEEDED(result) && out && *out && desc && (desc->BindFlags & targets)) {
    texture_made made{};
    made.desc = *desc;
    void *frames[5]{};
    const USHORT count = RtlCaptureStackBackTrace(1, 4, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
      made.callers[i] = reinterpret_cast<std::uintptr_t>(frames[i]);
    }
    std::lock_guard lock(textures_mutex);
    textures_made[*out] = made;
  }
  return result;
}

HRESULT __stdcall create_vs_stub(ID3D11Device *device, const void *code,
                                 SIZE_T size, ID3D11ClassLinkage *linkage,
                                 ID3D11VertexShader **out) {
  const HRESULT result =
      create_vs_hook.invoke<HRESULT>(device, code, size, linkage, out);
  if (SUCCEEDED(result) && out) {
    note_shader(*out, code, size, 'v');
  }
  return result;
}

HRESULT __stdcall create_ps_stub(ID3D11Device *device, const void *code,
                                 SIZE_T size, ID3D11ClassLinkage *linkage,
                                 ID3D11PixelShader **out) {
  const HRESULT result =
      create_ps_hook.invoke<HRESULT>(device, code, size, linkage, out);
  if (SUCCEEDED(result) && out) {
    note_shader(*out, code, size, 'p');
  }
  return result;
}

HRESULT __stdcall create_cs_stub(ID3D11Device *device, const void *code,
                                 SIZE_T size, ID3D11ClassLinkage *linkage,
                                 ID3D11ComputeShader **out) {
  const HRESULT result =
      create_cs_hook.invoke<HRESULT>(device, code, size, linkage, out);
  if (SUCCEEDED(result) && out) {
    note_shader(*out, code, size, 'c');
  }
  return result;
}

HRESULT __stdcall create_deferred_stub(ID3D11Device *device, UINT flags,
                                       ID3D11DeviceContext **out) {
  const HRESULT result =
      create_deferred_hook.invoke<HRESULT>(device, flags, out);
  if (SUCCEEDED(result) && out && *out) {
    deferred_contexts.fetch_add(1);
    deferred_vtable_draw = (*reinterpret_cast<void ***>(*out))[ctx_draw];
  }
  return result;
}

template <typename Stub>
void hook_slot(utils::hook::detour &hook, void **vtable, std::size_t slot,
               Stub stub) {
  hook.create(vtable[slot], reinterpret_cast<void *>(stub));
}

// post_unpack, before the engine makes its device: the device methods through
// a WARP device's vtable (d3d11.dll's own, shared by every device; checked
// against the game's device at the first Present).
std::string hook_device() {
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  D3D_FEATURE_LEVEL level{};
  const HRESULT hr =
      D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                        D3D11_SDK_VERSION, &device, &level, &context);
  if (FAILED(hr) || !device) {
    return "D3D11CreateDevice (WARP) failed";
  }
  void **vtable = *reinterpret_cast<void ***>(device);
  dummy_device_vtable_ps = vtable[dev_create_pixel_shader];
  std::string error;
  try {
    hook_slot(create_texture2d_hook, vtable, dev_create_texture2d,
              &create_texture2d_stub);
    hook_slot(create_vs_hook, vtable, dev_create_vertex_shader,
              &create_vs_stub);
    hook_slot(create_ps_hook, vtable, dev_create_pixel_shader, &create_ps_stub);
    hook_slot(create_cs_hook, vtable, dev_create_compute_shader,
              &create_cs_stub);
    hook_slot(create_deferred_hook, vtable, dev_create_deferred_context,
              &create_deferred_stub);
  } catch (const std::exception &e) {
    error = e.what();
  }
  if (context) {
    context->Release();
  }
  device->Release();
  return error;
}

// First Present: the context methods through the game's own immediate
// context.
void hook_context(IDXGISwapChain *chain) {
  ID3D11Device *device = nullptr;
  if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || !device) {
    return;
  }
  ID3D11DeviceContext *ctx = nullptr;
  device->GetImmediateContext(&ctx);
  void **device_vtable = *reinterpret_cast<void ***>(device);
  const bool same_device =
      device_vtable[dev_create_pixel_shader] == dummy_device_vtable_ps;
  std::string error;
  if (ctx) {
    void **vtable = *reinterpret_cast<void ***>(ctx);
    try {
      hook_slot(draw_indexed_hook, vtable, ctx_draw_indexed,
                &draw_indexed_stub);
      hook_slot(draw_hook, vtable, ctx_draw, &draw_stub);
      hook_slot(draw_indexed_instanced_hook, vtable, ctx_draw_indexed_instanced,
                &draw_indexed_instanced_stub);
      hook_slot(draw_instanced_hook, vtable, ctx_draw_instanced,
                &draw_instanced_stub);
      hook_slot(dispatch_hook, vtable, ctx_dispatch, &dispatch_stub);
      hook_slot(copy_region_hook, vtable, ctx_copy_subresource_region,
                &copy_region_stub);
      hook_slot(copy_resource_hook, vtable, ctx_copy_resource,
                &copy_resource_stub);
      hook_slot(clear_rtv_hook, vtable, ctx_clear_rtv, &clear_rtv_stub);
      hook_slot(clear_dsv_hook, vtable, ctx_clear_dsv, &clear_dsv_stub);
      hook_slot(resolve_hook, vtable, ctx_resolve, &resolve_stub);
      hook_slot(execute_hook, vtable, ctx_execute_command_list, &execute_stub);
    } catch (const std::exception &e) {
      error = e.what();
    }
    const bool deferred_same = deferred_contexts.load() == 0 ||
                               deferred_vtable_draw == vtable[ctx_draw];
    xerces_runtime::log_info(
        "xerces.gfx_trace.context",
        "Frame tracer: context detours " +
            (error.empty() ? std::string("in place") : "failed: " + error) +
            "; device methods " +
            (same_device ? "shared with the WARP device"
                         : "NOT the WARP device's (shaders are not named)") +
            "; deferred contexts so far " +
            std::to_string(deferred_contexts.load()) +
            (deferred_same ? "" : " (another vtable: not traced)"));
    ctx->Release();
  }
  device->Release();
}

void write_frame(IDXGISwapChain *chain) {
  std::string text;
  std::set<std::uint64_t> shaders_used;
  std::set<const void *> textures_used;
  {
    std::lock_guard lock(trace_mutex);
    text.swap(trace_text);
    shaders_used.swap(frame_shaders);
    textures_used.swap(frame_textures);
    event_number = 0;
  }
  DXGI_SWAP_CHAIN_DESC desc{};
  chain->GetDesc(&desc);
  char head[160];
  std::snprintf(head, sizeof(head),
                "frame %d swapchain %ux%u f%d buffers %u%s\n", frame_number,
                desc.BufferDesc.Width, desc.BufferDesc.Height,
                static_cast<int>(desc.BufferDesc.Format), desc.BufferCount,
                want_cb ? " cb" : "");
  std::string textures = "textures made since the hooks went in:\n";
  {
    std::lock_guard lock(textures_mutex);
    for (const void *resource : textures_used) {
      const auto made = textures_made.find(resource);
      const auto id = texture_ids.find(resource);
      if (made == textures_made.end() || id == texture_ids.end()) {
        continue;
      }
      const D3D11_TEXTURE2D_DESC &d = made->second.desc;
      textures += "  T" + std::to_string(id->second) + " " +
                  std::to_string(d.Width) + "x" + std::to_string(d.Height) +
                  " f" + std::to_string(static_cast<int>(d.Format)) + " bind" +
                  hex(d.BindFlags, 1) + " made at=";
      for (int i = 0; i < 4 && made->second.callers[i]; ++i) {
        textures += (i ? "<" : "") + where(made->second.callers[i]);
      }
      textures += '\n';
    }
  }
  const auto path =
      out_dir / ("frame-" + std::to_string(frame_number) + ".txt");
  xerces_runtime::write_file(path, head + text + textures);

  const auto shader_dir = out_dir / "shaders";
  std::error_code ec;
  std::filesystem::create_directories(shader_dir, ec);
  std::lock_guard lock(shaders_mutex);
  for (const std::uint64_t id : shaders_used) {
    const auto code = code_by_id.find(id);
    if (code == code_by_id.end()) {
      continue;
    }
    char kind = '?';
    for (const auto &[object, ref] : shader_by_object) {
      if (ref.id == id) {
        kind = ref.kind;
        break;
      }
    }
    const std::string ext = kind == 'v' ? "vs" : kind == 'p' ? "ps" : "cs";
    const auto file = shader_dir / (hex(id, 16) + "." + ext + ".dxbc");
    if (std::filesystem::exists(file, ec)) {
      continue;
    }
    xerces_runtime::write_file(
        file,
        std::string_view(reinterpret_cast<const char *>(code->second.data()),
                         code->second.size()));
  }
}

void begin_frame(IDXGISwapChain *chain) {
  ID3D11Texture2D *buffer = nullptr;
  if (SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer))) && buffer) {
    back_buffer = buffer; // compared by address only
    buffer->Release();
  }
  ++frame_number;
  std::lock_guard lock(trace_mutex);
  trace_text.clear();
  event_number = 0;
  capturing.store(true, std::memory_order_release);
}

// Twice a second (async thread): the arm and exec.cfg files.
void poll_files() {
  std::error_code ec;
  const auto arm = out_dir / "arm";
  if (std::filesystem::exists(arm, ec)) {
    const std::string text = xerces_runtime::read_file(arm);
    std::filesystem::remove(arm, ec);
    int frames = std::atoi(text.c_str());
    frames = frames < 1 ? 1 : (frames > 8 ? 8 : frames);
    arm_cb.store(text.find("cb") != std::string::npos);
    arm_request.store(frames, std::memory_order_release);
  }
  const auto exec = out_dir / "exec.cfg";
  if (std::filesystem::exists(exec, ec)) {
    std::string text = xerces_runtime::read_file(exec);
    std::filesystem::remove(exec, ec);
    if (!text.empty()) {
      if (text.back() != '\n') {
        text += '\n';
      }
      scheduler::once(
          [text] {
            game::cbuf::Cbuf_AddText(game::LOCAL_CLIENT_0, text.c_str());
          },
          scheduler::main);
    }
  }
}
} // namespace

void on_present(IDXGISwapChain *chain) {
  if (!context_hooked) {
    context_hooked = true;
    hook_context(chain);
  }
  if (capturing.load(std::memory_order_acquire)) {
    capturing.store(false, std::memory_order_release);
    write_frame(chain);
    if (--frames_left > 0) {
      begin_frame(chain);
    }
    return;
  }
  const int asked = arm_request.exchange(0, std::memory_order_acq_rel);
  if (asked > 0) {
    frames_left = asked;
    want_cb = arm_cb.load();
    begin_frame(chain);
  }
}

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_gfx_trace");

public:
  void post_unpack() override {
    if (!game::is_client()) {
      return;
    }
    wchar_t value[8]{};
    const DWORD size = GetEnvironmentVariableW(L"XERCES_GFX_TRACE", value, 8);
    if (size != 1 || value[0] != L'1') {
      return;
    }
    out_dir = xerces_runtime::home() / "bench" / "gfx-trace";
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    const std::string error = hook_device();
    detail::on.store(true, std::memory_order_release);
    scheduler::loop(poll_files, scheduler::async,
                    std::chrono::milliseconds(500));
    xerces_runtime::log_info(
        "xerces.gfx_trace.on",
        "Frame tracer on (XERCES_GFX_TRACE=1): captures go to " +
            out_dir.string() +
            (error.empty() ? std::string()
                           : "; the device detours failed: " + error));
  }
};
} // namespace xerces_gfx_trace

REGISTER_COMPONENT(xerces_gfx_trace::component)
