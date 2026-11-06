#pragma once

// Flip-model presentation (docs/DESIGN.md 12.4 item 4), on only when the game
// starts with -xerces-flip-model (the launcher's "Flip-model presentation
// (experimental)"). The rules are xerces_core/flip_model.hpp. Every hook is on
// DXGI itself, found through COM vtables of objects made for the purpose, so
// no game offsets:
//
// - IDXGIFactory::CreateSwapChain (vtable slot 10), detoured at post_unpack,
//   before the engine's renderer starts. The engine (BlackOps3.exe imports
//   CreateDXGIFactory1 and D3D11CreateDevice only) makes its one chain there,
//   from its window procedure; the chain becomes FLIP_DISCARD when the
//   engine's description allows it.
// - IDXGISwapChain::ResizeBuffers (slot 13), detoured through the vtable of
//   download_overlay.cpp's dummy chain, keeps the flags fixed at creation.
// - Present stays download_overlay.cpp's one detour (MinHook takes one per
//   address). For the flipped chain it calls before_present and
//   after_present: the tearing flag when allowed, and the render targets
//   bound before the flip bound again after it, because a flip-model Present
//   unbinds the back buffer and the engine skips binding a target it thinks
//   is still bound.
//
// The engine never uses IDXGIFactory2, IDXGISwapChain1 or Present1 (their
// interface ids are not in the executable), so they are not hooked.

#include <std_include.hpp>

#include <atomic>

#include <xerces_core/perf_bench.hpp>

namespace xerces_flip {
namespace detail {
extern std::atomic<IDXGISwapChain *> flipped;
} // namespace detail

// The chain this component made a flip chain. Without the flag, or for any
// other chain, false at the cost of one atomic load (acquire, so the chain's
// creation flags stored before it are seen; a plain load on x64).
inline bool is_flipped(IDXGISwapChain *chain) {
  return chain != nullptr &&
         detail::flipped.load(std::memory_order_acquire) == chain;
}

// download_overlay.cpp makes its dummy device and chain inside this scope,
// so the CreateSwapChain detour leaves them alone (same thread only).
class own_chain_scope {
public:
  own_chain_scope();
  ~own_chain_scope();
  own_chain_scope(const own_chain_scope &) = delete;
  own_chain_scope &operator=(const own_chain_scope &) = delete;
};

// download_overlay.cpp, on the async thread, once its Present detour is in
// place: the dummy chain whose vtable it used. With the flag this detours
// ResizeBuffers through the same vtable; chains are flipped only after it.
void on_present_hooked(IDXGISwapChain *dummy);

// One Present of the flipped chain (the thread that presents).
struct present_call {
  UINT flags = 0; // what to pass to the original Present
  bool tearing_added = false;
  ID3D11DeviceContext *context = nullptr;
  ID3D11RenderTargetView *targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
  ID3D11DepthStencilView *depth = nullptr;
  UINT target_count = 0;
};
// Before the original Present: the flags to pass, and the targets bound now.
void before_present(IDXGISwapChain *chain, UINT sync_interval, UINT flags,
                    present_call &call);
// Present refused the tearing flag (DXGI_ERROR_INVALID_CALL): present again
// with the engine's own flags. The flag is not added again on this chain.
bool tearing_refused(present_call &call, HRESULT result);
// After it: the targets bound again and the references let go.
void after_present(present_call &call);

// perf.swapchain: what this component did with the chain.
void describe(xerces_core::bench::swapchain_info &info);
} // namespace xerces_flip
