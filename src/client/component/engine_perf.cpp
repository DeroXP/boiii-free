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
