#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "command.hpp"
#include "engine_perf.hpp"

#include <utils/hook.hpp>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_3.h>

// bo3-bundle: D3D11 renderer-side FPS investigation.
//
// Sleep skip works (5B Sleeps suppressed) but in-game FPS didn't move past
// ~200 -- meaning gameplay is NOT Sleep-bound. The cap is somewhere in the
// renderer. Three suspects this component targets:
//   1. IDXGISwapChain::Present(SyncInterval) -- even with r_vsync 0, if BO3
//      passes SyncInterval=1 we still sync to monitor refresh. Override the
//      interval via bundle_perf_present_interval.
//   2. IDXGIDevice1::SetMaximumFrameLatency -- default of 3 means up to 3
//      frames queued before CPU blocks. Lower can reduce latency / boost
//      FPS if we're stalling on GPU completion. Apply via
//      bundle_perf_max_latency.
//   3. Present flags exposed via bundle_perf_present_stats.
//
// IMPORTANT: this component does NOT install its own Present hook --
// download_overlay.cpp already hooks IDXGISwapChain::Present at vtable[8]
// for its imgui overlay. MinHook refuses a second hook on the same target.
// Instead, engine_perf::intercept_present() is called from inside
// download_overlay's existing stub.

namespace engine_perf {
namespace {

std::atomic<bool> g_present_log_active{false};
std::atomic<int>  g_present_log_remaining{0};
std::atomic<int32_t> g_sync_interval_override{-1};   // -1 = passthrough
std::atomic<uint64_t> g_present_count{0};
std::atomic<uint64_t> g_present_overrides_applied{0};
std::atomic<uint32_t> g_last_seen_sync_interval{UINT32_MAX};
std::atomic<uint32_t> g_last_seen_flags{0};

std::atomic<bool> g_max_latency_applied{false};
std::atomic<int>  g_target_max_latency{0};

// Render-thread wait-point investigation. Goal: see if the render thread
// (the one that calls Present) burns measurable wall time inside
// WaitForSingleObjectEx / WaitForMultipleObjectsEx -- if yes, those waits
// could be the FPS cap we keep hitting.
//
// To avoid the mutex-contention crash mode from the Sleep histogram, we
// keep counters TOTAL only (no per-caller breakdown), and we identify the
// render thread by capturing the TID inside intercept_present(). We only
// account waits from that one thread.
std::atomic<DWORD> g_render_tid{0};

// Draw-call counting. Hooks ID3D11DeviceContext::Draw* methods to count
// calls per frame. If BO3 does thousands of draw calls per frame, API
// overhead is the render-thread bottleneck.
std::atomic<uint64_t> g_draw_indexed_total{0};
std::atomic<uint64_t> g_draw_total{0};
std::atomic<uint64_t> g_draw_indexed_inst_total{0};
std::atomic<uint64_t> g_draw_inst_total{0};
std::atomic<uint64_t> g_draws_this_frame{0};
std::atomic<uint64_t> g_draws_sum_for_avg{0};
std::atomic<uint64_t> g_frames_for_avg{0};
std::atomic<uint32_t> g_last_frame_draws{0};
std::atomic<uint32_t> g_max_frame_draws{0};
std::atomic<bool>    g_draw_hooks_installed{false};

// Per-caller draw histogram. Lock-free fixed-size hash table with linear
// probe (atomic CAS for insertion). At hot rate (~444K draws/sec) a mutex
// would dominate -- this is wait-free for already-inserted callers and
// only contended on first sighting of a new caller.
constexpr size_t kCallerTableSize = 512;
struct CallerEntry {
  std::atomic<uintptr_t> addr{0};
  std::atomic<uint64_t> count{0};
};
CallerEntry g_caller_table[kCallerTableSize];     // L1: direct caller of Draw* (likely a wrapper)
CallerEntry g_caller_table_l2[kCallerTableSize];  // L2: grandparent (= the render loop)
std::atomic<bool> g_capture_l2{false};            // toggle since RtlCaptureStackBackTrace is ~100ns/call

// Skip list: up to 16 caller addresses. If a draw call's _ReturnAddress
// matches, the draw is silently dropped (counted as skipped). Use to
// experimentally remove specific draw sources after identifying them via
// bundle_perf_draw_callers.
constexpr size_t kSkipListSize = 16;
std::atomic<uintptr_t> g_draw_skip_list[kSkipListSize]{};
std::atomic<uint64_t> g_draws_skipped{0};

void record_into(CallerEntry *table, uintptr_t ret) {
  const uint32_t h = static_cast<uint32_t>(ret >> 4) & (kCallerTableSize - 1);
  for (size_t i = 0; i < kCallerTableSize; ++i) {
    const auto idx = (h + i) & (kCallerTableSize - 1);
    auto cur = table[idx].addr.load(std::memory_order_relaxed);
    if (cur == ret) {
      table[idx].count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (cur == 0) {
      uintptr_t expected = 0;
      if (table[idx].addr.compare_exchange_strong(
              expected, ret, std::memory_order_relaxed)) {
        table[idx].count.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (table[idx].addr.load(std::memory_order_relaxed) == ret) {
        table[idx].count.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
}

void record_draw_caller(uintptr_t ret) { record_into(g_caller_table, ret); }

void record_draw_grandparent() {
  // Skip count for grandparent: hook stub + record helper + wrapper's
  // internal RA = 3. (Previous skip=2 gave the wrapper RA itself, which
  // matches L1's _ReturnAddress() and isn't useful.)
  void *frames[2];
  const USHORT n = RtlCaptureStackBackTrace(3, 2, frames, nullptr);
  if (n >= 1) {
    record_into(g_caller_table_l2, reinterpret_cast<uintptr_t>(frames[0]));
  }
}

bool draw_is_skipped(uintptr_t ret) {
  for (size_t i = 0; i < kSkipListSize; ++i) {
    const auto v = g_draw_skip_list[i].load(std::memory_order_relaxed);
    if (v == 0) return false;
    if (v == ret) return true;
  }
  return false;
}

utils::hook::detour g_di_hook;
utils::hook::detour g_d_hook;
utils::hook::detour g_dii_hook;
utils::hook::detour g_di_inst_hook;

void __stdcall draw_indexed_stub(ID3D11DeviceContext *ctx, UINT idx_count,
                                 UINT start_idx, INT base_vertex) {
  g_draw_indexed_total.fetch_add(1, std::memory_order_relaxed);
  g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
  const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
  record_draw_caller(ret);
  if (g_capture_l2.load(std::memory_order_relaxed)) {
    record_draw_grandparent();
  }
  if (draw_is_skipped(ret)) {
    g_draws_skipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_di_hook.invoke<void>(ctx, idx_count, start_idx, base_vertex);
}

void __stdcall draw_stub(ID3D11DeviceContext *ctx, UINT vertex_count,
                         UINT start_vertex) {
  g_draw_total.fetch_add(1, std::memory_order_relaxed);
  g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
  const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
  record_draw_caller(ret);
  if (g_capture_l2.load(std::memory_order_relaxed)) {
    record_draw_grandparent();
  }
  if (draw_is_skipped(ret)) {
    g_draws_skipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_d_hook.invoke<void>(ctx, vertex_count, start_vertex);
}

void __stdcall draw_indexed_instanced_stub(ID3D11DeviceContext *ctx,
                                           UINT idx_count_per_inst,
                                           UINT inst_count, UINT start_idx,
                                           INT base_vertex, UINT start_inst) {
  g_draw_indexed_inst_total.fetch_add(1, std::memory_order_relaxed);
  g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
  const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
  record_draw_caller(ret);
  if (g_capture_l2.load(std::memory_order_relaxed)) {
    record_draw_grandparent();
  }
  if (draw_is_skipped(ret)) {
    g_draws_skipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_dii_hook.invoke<void>(ctx, idx_count_per_inst, inst_count, start_idx,
                          base_vertex, start_inst);
}

void __stdcall draw_instanced_stub(ID3D11DeviceContext *ctx,
                                   UINT vertex_count_per_inst,
                                   UINT inst_count, UINT start_vertex,
                                   UINT start_inst) {
  g_draw_inst_total.fetch_add(1, std::memory_order_relaxed);
  g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
  const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
  record_draw_caller(ret);
  if (g_capture_l2.load(std::memory_order_relaxed)) {
    record_draw_grandparent();
  }
  if (draw_is_skipped(ret)) {
    g_draws_skipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_di_inst_hook.invoke<void>(ctx, vertex_count_per_inst, inst_count,
                              start_vertex, start_inst);
}

void install_draw_hooks() {
  if (g_draw_hooks_installed.exchange(true)) return;

  // Spin up a dummy device just to steal the ID3D11DeviceContext vtable.
  // Same pattern as download_overlay's swap-chain theft.
  WNDCLASSEXA wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "DX11Dummy_DrawHook";
  RegisterClassExA(&wc);
  HWND dummy_hwnd = CreateWindowExA(0, wc.lpszClassName, nullptr, WS_OVERLAPPED,
                                    0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
  if (!dummy_hwnd) { UnregisterClassA(wc.lpszClassName, wc.hInstance); return; }

  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 1;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = dummy_hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  IDXGISwapChain *sc = nullptr;
  D3D_FEATURE_LEVEL fl{};
  const HRESULT hr = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
      D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);

  DestroyWindow(dummy_hwnd);
  UnregisterClassA(wc.lpszClassName, wc.hInstance);

  if (FAILED(hr) || !ctx) {
    printf("[ bo3-bundle engine ] draw-hook install: D3D11CreateDevice failed hr=0x%08lX\n", hr);
    g_draw_hooks_installed.store(false);
    return;
  }

  // ID3D11DeviceContext vtable indices:
  //   12 = DrawIndexed
  //   13 = Draw
  //   20 = DrawIndexedInstanced
  //   21 = DrawInstanced
  void **vtable = *reinterpret_cast<void ***>(ctx);
  try {
    g_di_hook.create(vtable[12], draw_indexed_stub);
    g_d_hook.create(vtable[13], draw_stub);
    g_dii_hook.create(vtable[20], draw_indexed_instanced_stub);
    g_di_inst_hook.create(vtable[21], draw_instanced_stub);
    printf("[ bo3-bundle engine ] draw hooks installed (DrawIndexed/Draw/DrawIndexedInstanced/DrawInstanced)\n");
  } catch (const std::exception &e) {
    printf("[ bo3-bundle engine ] draw hook install threw: %s\n", e.what());
  }

  if (sc) sc->Release();
  if (dev) dev->Release();
  if (ctx) ctx->Release();
}

constexpr int kPresentStackDepth = 8;
std::atomic<uintptr_t> g_present_stack[kPresentStackDepth]{};
std::atomic<bool> g_present_stack_captured{false};
std::atomic<bool>  g_wait_log_active{false};
std::atomic<uint64_t> g_wait_calls_total{0};
std::atomic<uint64_t> g_wait_calls_render{0};
std::atomic<uint64_t> g_wait_ns_render{0};
std::atomic<uint32_t> g_wait_max_timeout_seen_render{0};
std::atomic<uint64_t> g_wait_render_with_timeout{0}; // count with timeout!=INFINITE
std::atomic<uint64_t> g_wait_render_short{0};        // count with timeout < 100ms

utils::hook::detour g_wait_single_hook;
utils::hook::detour g_wait_multiple_hook;

DWORD WINAPI perf_wait_single_stub(HANDLE handle, DWORD ms_timeout, BOOL alertable) {
  g_wait_calls_total.fetch_add(1, std::memory_order_relaxed);
  const auto render_tid = g_render_tid.load(std::memory_order_relaxed);
  const auto this_tid = GetCurrentThreadId();
  const bool on_render = render_tid != 0 && this_tid == render_tid;
  if (on_render) {
    g_wait_calls_render.fetch_add(1, std::memory_order_relaxed);
    if (ms_timeout != INFINITE) {
      g_wait_render_with_timeout.fetch_add(1, std::memory_order_relaxed);
      if (ms_timeout < 100) {
        g_wait_render_short.fetch_add(1, std::memory_order_relaxed);
      }
      uint32_t cur_max = g_wait_max_timeout_seen_render.load(std::memory_order_relaxed);
      while (ms_timeout > cur_max &&
             !g_wait_max_timeout_seen_render.compare_exchange_weak(cur_max, ms_timeout)) {}
    }
  }
  if (!on_render) {
    return g_wait_single_hook.invoke<DWORD>(handle, ms_timeout, alertable);
  }
  LARGE_INTEGER t0, t1, freq;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  const DWORD r = g_wait_single_hook.invoke<DWORD>(handle, ms_timeout, alertable);
  QueryPerformanceCounter(&t1);
  const uint64_t ns = (uint64_t)((t1.QuadPart - t0.QuadPart) * 1000000000ULL / freq.QuadPart);
  g_wait_ns_render.fetch_add(ns, std::memory_order_relaxed);
  return r;
}

DWORD WINAPI perf_wait_multiple_stub(DWORD count, const HANDLE *handles,
                                     BOOL wait_all, DWORD ms_timeout, BOOL alertable) {
  g_wait_calls_total.fetch_add(1, std::memory_order_relaxed);
  const auto render_tid = g_render_tid.load(std::memory_order_relaxed);
  const bool on_render = render_tid != 0 && GetCurrentThreadId() == render_tid;
  if (on_render) {
    g_wait_calls_render.fetch_add(1, std::memory_order_relaxed);
    if (ms_timeout != INFINITE) {
      g_wait_render_with_timeout.fetch_add(1, std::memory_order_relaxed);
      if (ms_timeout < 100) g_wait_render_short.fetch_add(1, std::memory_order_relaxed);
    }
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    const DWORD r = g_wait_multiple_hook.invoke<DWORD>(count, handles, wait_all,
                                                       ms_timeout, alertable);
    QueryPerformanceCounter(&t1);
    g_wait_ns_render.fetch_add(
        (uint64_t)((t1.QuadPart - t0.QuadPart) * 1000000000ULL / freq.QuadPart),
        std::memory_order_relaxed);
    return r;
  }
  return g_wait_multiple_hook.invoke<DWORD>(count, handles, wait_all,
                                            ms_timeout, alertable);
}

void try_apply_max_latency(IDXGISwapChain *swap_chain) {
  const int target = g_target_max_latency.load(std::memory_order_relaxed);
  if (target <= 0) return;
  if (g_max_latency_applied.exchange(true, std::memory_order_relaxed)) return;

  ID3D11Device *device = nullptr;
  if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&device)))) return;
  IDXGIDevice1 *dxgi_dev = nullptr;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_dev)))) {
    const HRESULT hr = dxgi_dev->SetMaximumFrameLatency(target);
    printf("[ bo3-bundle engine ] SetMaximumFrameLatency(%d) hr=0x%08lX\n",
           target, hr);
    dxgi_dev->Release();
  }
  device->Release();
}

}  // namespace

void intercept_present(IDXGISwapChain *swap_chain, UINT &sync_interval,
                       UINT &flags) {
  g_present_count.fetch_add(1, std::memory_order_relaxed);
  g_last_seen_sync_interval.store(sync_interval, std::memory_order_relaxed);
  g_last_seen_flags.store(flags, std::memory_order_relaxed);
  // Capture render thread id once -- whichever thread calls Present is the
  // render thread.
  const DWORD me = GetCurrentThreadId();
  DWORD expected = 0;
  g_render_tid.compare_exchange_strong(expected, me, std::memory_order_relaxed);

  // Per-frame draw-call snapshot. Done before any other work so the
  // counter resets cleanly on each Present.
  const auto df = g_draws_this_frame.exchange(0, std::memory_order_relaxed);
  if (df > 0) {
    g_last_frame_draws.store(static_cast<uint32_t>(df), std::memory_order_relaxed);
    g_draws_sum_for_avg.fetch_add(df, std::memory_order_relaxed);
    g_frames_for_avg.fetch_add(1, std::memory_order_relaxed);
    uint32_t cur_max = g_max_frame_draws.load(std::memory_order_relaxed);
    while (df > cur_max && !g_max_frame_draws.compare_exchange_weak(
                              cur_max, static_cast<uint32_t>(df))) {}
  }

  // One-time stack-trace capture so we can disasm BO3's render path. Skip
  // the first frame (intercept_present + download_overlay::present_stub).
  if (!g_present_stack_captured.load(std::memory_order_relaxed)) {
    void *frames[kPresentStackDepth] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, kPresentStackDepth, frames, nullptr);
    for (USHORT i = 0; i < n && i < kPresentStackDepth; ++i) {
      g_present_stack[i].store(reinterpret_cast<uintptr_t>(frames[i]),
                               std::memory_order_relaxed);
    }
    g_present_stack_captured.store(true, std::memory_order_relaxed);
  }

  try_apply_max_latency(swap_chain);

  if (g_present_log_active.load(std::memory_order_relaxed)) {
    const int remaining = g_present_log_remaining.fetch_sub(1, std::memory_order_relaxed);
    if (remaining > 0) {
      printf("[ bo3-bundle engine ] Present sync_interval=%u flags=0x%X\n",
             sync_interval, flags);
    }
    if (remaining <= 1) g_present_log_active.store(false);
  }

  const int32_t override = g_sync_interval_override.load(std::memory_order_relaxed);
  if (override >= 0 && static_cast<UINT>(override) != sync_interval) {
    sync_interval = static_cast<UINT>(override);
    g_present_overrides_applied.fetch_add(1, std::memory_order_relaxed);
  }
}

struct component final : client_component {
  void post_unpack() override {
    command::add("bundle_perf_present_log", [](const command::params &params) {
      const int n = params.size() >= 2 ? std::atoi(params.get(1)) : 5;
      if (n <= 0) {
        g_present_log_active.store(false);
        printf("[ bo3-bundle engine ] Present log off\n");
        return;
      }
      g_present_log_remaining.store(n);
      g_present_log_active.store(true);
      printf("[ bo3-bundle engine ] Present log: next %d calls\n", n);
    });

    command::add("bundle_perf_present_interval", [](const command::params &params) {
      if (params.size() < 2) {
        printf("usage: bundle_perf_present_interval <auto|0|1|2|...>  (current=%d)\n",
               g_sync_interval_override.load());
        return;
      }
      const std::string arg = params.get(1);
      int32_t v = -1;
      if (arg == "auto") {
        v = -1;
      } else {
        try { v = std::stoi(arg); } catch (...) {
          printf("[ bo3-bundle engine ] bad arg: %s\n", arg.c_str()); return;
        }
        if (v < 0 || v > 4) {
          printf("[ bo3-bundle engine ] out of range 0..4\n"); return;
        }
      }
      g_sync_interval_override.store(v);
      printf("[ bo3-bundle engine ] sync_interval override = %d\n", v);
    });

    command::add("bundle_perf_max_latency", [](const command::params &params) {
      if (params.size() < 2) {
        printf("usage: bundle_perf_max_latency <0|1|2|3>  (0=don't touch, 1=lowest)\n");
        return;
      }
      const int v = std::atoi(params.get(1));
      if (v < 0 || v > 16) { printf("[ bo3-bundle engine ] out of range\n"); return; }
      g_target_max_latency.store(v);
      g_max_latency_applied.store(false);
      printf("[ bo3-bundle engine ] max latency target = %d (applies on next Present)\n", v);
    });

    command::add("bundle_perf_draw_hook", [](const command::params &) {
      install_draw_hooks();
    });

    command::add("bundle_perf_draw_stats", [](const command::params &) {
      const auto di = g_draw_indexed_total.load();
      const auto d  = g_draw_total.load();
      const auto dii= g_draw_indexed_inst_total.load();
      const auto dinst = g_draw_inst_total.load();
      const auto last = g_last_frame_draws.load();
      const auto mx = g_max_frame_draws.load();
      const auto sumavg = g_draws_sum_for_avg.load();
      const auto frames = g_frames_for_avg.load();
      const double avg = frames ? double(sumavg) / double(frames) : 0.0;
      printf("[ bo3-bundle engine ] draw totals: DrawIndexed=%llu Draw=%llu DrawIndexedInstanced=%llu DrawInstanced=%llu\n",
             (unsigned long long)di, (unsigned long long)d,
             (unsigned long long)dii, (unsigned long long)dinst);
      printf("[ bo3-bundle engine ] per-frame draws: last=%u  max=%u  avg=%.1f over %llu frames\n",
             last, mx, avg, (unsigned long long)frames);
    });

    command::add("bundle_perf_draw_callers", [](const command::params &params) {
      const int top_n = params.size() >= 2 ? std::atoi(params.get(1)) : 16;
      std::vector<std::pair<uintptr_t, uint64_t>> snap;
      snap.reserve(kCallerTableSize);
      for (size_t i = 0; i < kCallerTableSize; ++i) {
        const auto a = g_caller_table[i].addr.load();
        const auto c = g_caller_table[i].count.load();
        if (a && c) snap.emplace_back(a, c);
      }
      std::sort(snap.begin(), snap.end(),
                [](auto &a, auto &b) { return a.second > b.second; });
      // Figure out BO3 image base so we can show static offsets too
      uintptr_t bo3_base = 0, bo3_end = 0;
      if (auto *m = GetModuleHandleA("BlackOps3.exe")) {
        MODULEINFO mi{};
        if (GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi))) {
          bo3_base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
          bo3_end = bo3_base + mi.SizeOfImage;
        }
      }
      printf("[ bo3-bundle engine ] top %d draw call sites (caller_addr  count  static_off  module):\n",
             top_n);
      const int shown = std::min<int>(top_n, static_cast<int>(snap.size()));
      for (int i = 0; i < shown; ++i) {
        const auto [a, c] = snap[i];
        const bool in_bo3 = bo3_base && a >= bo3_base && a < bo3_end;
        if (in_bo3) {
          const uintptr_t off = a - bo3_base;
          printf("[ bo3-bundle engine ]   0x%016llX  cnt=%llu  +0x%llX  BO3\n",
                 (unsigned long long)a, (unsigned long long)c,
                 (unsigned long long)off);
        } else {
          printf("[ bo3-bundle engine ]   0x%016llX  cnt=%llu  -            (other)\n",
                 (unsigned long long)a, (unsigned long long)c);
        }
      }
    });

    command::add("bundle_perf_draw_l2", [](const command::params &params) {
      const bool on = params.size() < 2 || params.get(1) == std::string("1");
      g_capture_l2.store(on);
      printf("[ bo3-bundle engine ] grandparent draw-caller capture: %s\n",
             on ? "ON (~4%% perf cost)" : "off");
    });

    command::add("bundle_perf_draw_callers_l2", [](const command::params &params) {
      const int top_n = params.size() >= 2 ? std::atoi(params.get(1)) : 16;
      std::vector<std::pair<uintptr_t, uint64_t>> snap;
      for (size_t i = 0; i < kCallerTableSize; ++i) {
        const auto a = g_caller_table_l2[i].addr.load();
        const auto c = g_caller_table_l2[i].count.load();
        if (a && c) snap.emplace_back(a, c);
      }
      std::sort(snap.begin(), snap.end(),
                [](auto &a, auto &b) { return a.second > b.second; });
      uintptr_t bo3_base = 0, bo3_end = 0;
      if (auto *m = GetModuleHandleA("BlackOps3.exe")) {
        MODULEINFO mi{};
        if (GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi))) {
          bo3_base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
          bo3_end = bo3_base + mi.SizeOfImage;
        }
      }
      printf("[ bo3-bundle engine ] top %d grandparent (render-loop) sites:\n", top_n);
      const int shown = std::min<int>(top_n, static_cast<int>(snap.size()));
      for (int i = 0; i < shown; ++i) {
        const auto [a, c] = snap[i];
        const bool in_bo3 = bo3_base && a >= bo3_base && a < bo3_end;
        if (in_bo3) {
          printf("[ bo3-bundle engine ]   0x%016llX  cnt=%llu  +0x%llX  BO3\n",
                 (unsigned long long)a, (unsigned long long)c,
                 (unsigned long long)(a - bo3_base));
        } else {
          printf("[ bo3-bundle engine ]   0x%016llX  cnt=%llu  (other)\n",
                 (unsigned long long)a, (unsigned long long)c);
        }
      }
    });

    command::add("bundle_perf_draw_skip_add", [](const command::params &params) {
      if (params.size() < 2) {
        printf("usage: bundle_perf_draw_skip_add <hex_runtime_addr>\n");
        return;
      }
      uintptr_t addr = 0;
      try { addr = std::stoull(params.get(1), nullptr, 16); }
      catch (...) { printf("bad hex\n"); return; }
      for (size_t i = 0; i < kSkipListSize; ++i) {
        uintptr_t expected = 0;
        if (g_draw_skip_list[i].compare_exchange_strong(expected, addr)) {
          printf("[ bo3-bundle engine ] draw skip slot %zu = 0x%llX\n",
                 i, (unsigned long long)addr);
          return;
        }
        if (g_draw_skip_list[i].load() == addr) {
          printf("[ bo3-bundle engine ] already in skip list at slot %zu\n", i);
          return;
        }
      }
      printf("[ bo3-bundle engine ] skip list full (max %zu)\n", kSkipListSize);
    });

    command::add("bundle_perf_draw_skip_clear", [](const command::params &) {
      for (size_t i = 0; i < kSkipListSize; ++i) g_draw_skip_list[i].store(0);
      g_draws_skipped.store(0);
      printf("[ bo3-bundle engine ] draw skip list cleared\n");
    });

    command::add("bundle_perf_draw_skip_list", [](const command::params &) {
      printf("[ bo3-bundle engine ] draw skip list (drawn skipped total = %llu):\n",
             (unsigned long long)g_draws_skipped.load());
      for (size_t i = 0; i < kSkipListSize; ++i) {
        const auto v = g_draw_skip_list[i].load();
        if (v) printf("[ bo3-bundle engine ]   [%zu] 0x%llX\n", i, (unsigned long long)v);
      }
    });

    command::add("bundle_perf_draw_reset", [](const command::params &) {
      g_draw_indexed_total.store(0);
      g_draw_total.store(0);
      g_draw_indexed_inst_total.store(0);
      g_draw_inst_total.store(0);
      g_draws_this_frame.store(0);
      g_draws_sum_for_avg.store(0);
      g_frames_for_avg.store(0);
      g_last_frame_draws.store(0);
      g_max_frame_draws.store(0);
      for (size_t i = 0; i < kCallerTableSize; ++i) {
        g_caller_table[i].addr.store(0);
        g_caller_table[i].count.store(0);
        g_caller_table_l2[i].addr.store(0);
        g_caller_table_l2[i].count.store(0);
      }
      g_draws_skipped.store(0);
      printf("[ bo3-bundle engine ] draw stats reset\n");
    });

    command::add("bundle_perf_present_stack", [](const command::params &) {
      printf("[ bo3-bundle engine ] Present call stack (captured once, render tid=%lu):\n",
             g_render_tid.load());
      for (int i = 0; i < kPresentStackDepth; ++i) {
        const auto a = g_present_stack[i].load();
        if (!a) break;
        const char *origin = "?";
        auto *bo3 = GetModuleHandleA("BlackOps3.exe");
        auto *boiii = GetModuleHandleA("boiii.exe");
        MODULEINFO mi{};
        if (bo3 && GetModuleInformation(GetCurrentProcess(), bo3, &mi, sizeof(mi))) {
          const auto bo3_base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
          if (a >= bo3_base && a < bo3_base + mi.SizeOfImage) origin = "BO3";
        }
        if (boiii && GetModuleInformation(GetCurrentProcess(), boiii, &mi, sizeof(mi))) {
          const auto boiii_base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
          if (a >= boiii_base && a < boiii_base + mi.SizeOfImage) origin = "boiii";
        }
        printf("[ bo3-bundle engine ]   [%d] 0x%016llX  %s\n",
               i, static_cast<unsigned long long>(a), origin);
      }
    });

    command::add("bundle_perf_wait_hook", [](const command::params &) {
      static std::atomic<bool> installed{false};
      if (installed.exchange(true)) {
        printf("[ bo3-bundle engine ] wait hooks already installed\n");
        return;
      }
      try {
        g_wait_single_hook.create(WaitForSingleObjectEx, perf_wait_single_stub);
        g_wait_multiple_hook.create(WaitForMultipleObjectsEx, perf_wait_multiple_stub);
        printf("[ bo3-bundle engine ] WaitForSingleObjectEx + WaitForMultipleObjectsEx hooked\n");
      } catch (const std::exception &e) {
        printf("[ bo3-bundle engine ] wait hook install threw: %s\n", e.what());
      } catch (...) {
        printf("[ bo3-bundle engine ] wait hook install threw unknown\n");
      }
    });

    command::add("bundle_perf_wait_stats", [](const command::params &) {
      const auto rt = g_render_tid.load();
      const auto total = g_wait_calls_total.load();
      const auto rcalls = g_wait_calls_render.load();
      const auto rns = g_wait_ns_render.load();
      const auto rwt = g_wait_render_with_timeout.load();
      const auto rsh = g_wait_render_short.load();
      const auto mx = g_wait_max_timeout_seen_render.load();
      printf("[ bo3-bundle engine ] wait stats: render_tid=%lu  total_calls=%llu\n",
             rt, static_cast<unsigned long long>(total));
      printf("[ bo3-bundle engine ]   render-thread calls: %llu  with_timeout=%llu  short(<100ms)=%llu  max_timeout=%ums\n",
             static_cast<unsigned long long>(rcalls),
             static_cast<unsigned long long>(rwt),
             static_cast<unsigned long long>(rsh), mx);
      printf("[ bo3-bundle engine ]   render-thread total wait time: %.2fms  (avg per call: %.3fms)\n",
             rns / 1e6,
             rcalls ? (rns / 1e6) / rcalls : 0.0);
    });

    command::add("bundle_perf_wait_reset", [](const command::params &) {
      g_wait_calls_total.store(0);
      g_wait_calls_render.store(0);
      g_wait_ns_render.store(0);
      g_wait_max_timeout_seen_render.store(0);
      g_wait_render_with_timeout.store(0);
      g_wait_render_short.store(0);
      printf("[ bo3-bundle engine ] wait stats reset\n");
    });

    command::add("bundle_perf_present_stats", [](const command::params &) {
      const auto si = g_last_seen_sync_interval.load();
      printf("[ bo3-bundle engine ] Present count=%llu  overrides=%llu  override_setting=%d\n",
             static_cast<unsigned long long>(g_present_count.load()),
             static_cast<unsigned long long>(g_present_overrides_applied.load()),
             g_sync_interval_override.load());
      printf("[ bo3-bundle engine ] last seen: sync_interval=%s flags=0x%X\n",
             si == UINT32_MAX ? "(none yet)" : std::to_string(si).c_str(),
             g_last_seen_flags.load());
      printf("[ bo3-bundle engine ] max_latency target=%d applied=%s\n",
             g_target_max_latency.load(),
             g_max_latency_applied.load() ? "yes" : "no");
    });
  }
};

}  // namespace engine_perf

REGISTER_COMPONENT(engine_perf::component)
