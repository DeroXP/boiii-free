#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "command.hpp"

#include <utils/hook.hpp>

#include <unordered_map>
#include <mutex>
#include <vector>
#include <algorithm>

namespace perf {
namespace {

// NtSetTimerResolution / NtQueryTimerResolution are undocumented but stable.
// Resolution values are in 100-ns units. Most modern Windows builds expose
// ~5000 (0.5ms) as the system minimum; older / busy systems may only allow
// 10000 (1ms). Either is a huge upgrade over the default ~156000 (15.6ms).
using NtSetTimerResolution_t = NTSTATUS(NTAPI *)(ULONG desired,
                                                 BOOLEAN set,
                                                 PULONG current);
using NtQueryTimerResolution_t = NTSTATUS(NTAPI *)(PULONG minimum,
                                                   PULONG maximum,
                                                   PULONG current);

NtSetTimerResolution_t nt_set_timer_resolution{nullptr};
NtQueryTimerResolution_t nt_query_timer_resolution{nullptr};

ULONG g_applied_timer_res{0};

void resolve_nt() {
  auto *ntdll = GetModuleHandleA("ntdll.dll");
  if (!ntdll) return;
  nt_set_timer_resolution = reinterpret_cast<NtSetTimerResolution_t>(
      GetProcAddress(ntdll, "NtSetTimerResolution"));
  nt_query_timer_resolution = reinterpret_cast<NtQueryTimerResolution_t>(
      GetProcAddress(ntdll, "NtQueryTimerResolution"));
}

bool apply_min_timer_resolution() {
  if (!nt_set_timer_resolution || !nt_query_timer_resolution) return false;
  ULONG min_res = 0, max_res = 0, cur_res = 0;
  if (nt_query_timer_resolution(&min_res, &max_res, &cur_res) != 0) return false;
  ULONG actual = 0;
  if (nt_set_timer_resolution(max_res, TRUE, &actual) == 0) {
    g_applied_timer_res = max_res;
    return true;
  }
  return false;
}

void revert_timer_resolution() {
  if (g_applied_timer_res && nt_set_timer_resolution) {
    ULONG actual = 0;
    nt_set_timer_resolution(g_applied_timer_res, FALSE, &actual);
    g_applied_timer_res = 0;
  }
}

bool ensure_all_cores() {
  DWORD_PTR proc_mask = 0, sys_mask = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask)) {
    return false;
  }
  if (proc_mask == sys_mask) return true;
  return SetProcessAffinityMask(GetCurrentProcess(), sys_mask) != 0;
}

const char *priority_name(DWORD pc) {
  switch (pc) {
    case IDLE_PRIORITY_CLASS:         return "IDLE";
    case BELOW_NORMAL_PRIORITY_CLASS: return "BELOW_NORMAL";
    case NORMAL_PRIORITY_CLASS:       return "NORMAL";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "ABOVE_NORMAL";
    case HIGH_PRIORITY_CLASS:         return "HIGH";
    case REALTIME_PRIORITY_CLASS:     return "REALTIME";
    default:                          return "?";
  }
}

// === Sleep / SleepEx / NtDelayExecution hunt =================================
// Goal: find the engine's main-loop frame throttle (typically Sleep(1)) so we
// can suppress it for higher FPS on fast hardware. Strategy: hook every short
// sleep path, log a per-caller histogram, then let the user enable selective
// skip after looking at the dump.

std::atomic<bool> g_sleep_log_enabled{false};
std::atomic<bool> g_sleep_skip_short_enabled{false};   // skip Sleep/SleepEx with ms <= threshold
std::atomic<uint32_t> g_sleep_skip_threshold_ms{1};
std::atomic<uint64_t> g_sleep_calls{0};
std::atomic<uint64_t> g_sleep_ex_calls{0};
std::atomic<uint64_t> g_sleep_skipped{0};

struct caller_stats {
  uint64_t count = 0;
  uint64_t total_ms = 0;
  uint32_t last_ms = 0;
  uint32_t min_ms = UINT32_MAX;
  uint32_t max_ms = 0;
};
std::mutex g_sleep_caller_mutex;
std::unordered_map<uintptr_t, caller_stats> g_sleep_callers;

void record_sleep_caller(uintptr_t ret_addr, uint32_t ms) {
  std::lock_guard<std::mutex> lk(g_sleep_caller_mutex);
  auto &s = g_sleep_callers[ret_addr];
  s.count++;
  s.total_ms += ms;
  s.last_ms = ms;
  if (ms < s.min_ms) s.min_ms = ms;
  if (ms > s.max_ms) s.max_ms = ms;
}

// Only skip when the caller lives in BlackOps3.exe -- never suppress Sleeps
// from BOIII, Steam overlay, NVIDIA reflex, etc.
bool caller_in_bo3(uintptr_t ret_addr) {
  static uintptr_t bo3_base = 0;
  static uintptr_t bo3_end = 0;
  if (!bo3_base) {
    auto *mod = GetModuleHandleA("BlackOps3.exe");
    if (!mod) return false;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi))) {
      return false;
    }
    bo3_base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    bo3_end = bo3_base + mi.SizeOfImage;
  }
  return ret_addr >= bo3_base && ret_addr < bo3_end;
}

utils::hook::detour g_sleep_hook;
utils::hook::detour g_sleep_ex_hook;

void WINAPI perf_sleep_stub(DWORD ms) {
  g_sleep_calls.fetch_add(1, std::memory_order_relaxed);
  const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
  if (g_sleep_log_enabled.load(std::memory_order_relaxed)) {
    record_sleep_caller(ret, ms);
  }
  if (g_sleep_skip_short_enabled.load(std::memory_order_relaxed) &&
      ms <= g_sleep_skip_threshold_ms.load(std::memory_order_relaxed) &&
      caller_in_bo3(ret)) {
    g_sleep_skipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_sleep_hook.invoke<void>(ms);
}

DWORD WINAPI perf_sleep_ex_stub(DWORD ms, BOOL alertable) {
  g_sleep_ex_calls.fetch_add(1, std::memory_order_relaxed);
  const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
  if (g_sleep_log_enabled.load(std::memory_order_relaxed)) {
    record_sleep_caller(ret, ms);
  }
  if (g_sleep_skip_short_enabled.load(std::memory_order_relaxed) &&
      ms <= g_sleep_skip_threshold_ms.load(std::memory_order_relaxed) &&
      !alertable && caller_in_bo3(ret)) {
    g_sleep_skipped.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  return g_sleep_ex_hook.invoke<DWORD>(ms, alertable);
}

void dump_sleep_histogram(int top_n) {
  std::vector<std::pair<uintptr_t, caller_stats>> snapshot;
  {
    std::lock_guard<std::mutex> lk(g_sleep_caller_mutex);
    snapshot.reserve(g_sleep_callers.size());
    for (const auto &kv : g_sleep_callers) snapshot.emplace_back(kv);
  }
  std::sort(snapshot.begin(), snapshot.end(),
            [](const auto &a, const auto &b) { return a.second.count > b.second.count; });
  printf("[ bo3-bundle perf ] sleep totals: Sleep=%llu  SleepEx=%llu  skipped=%llu\n",
         static_cast<unsigned long long>(g_sleep_calls.load()),
         static_cast<unsigned long long>(g_sleep_ex_calls.load()),
         static_cast<unsigned long long>(g_sleep_skipped.load()));
  printf("[ bo3-bundle perf ] %d busiest sleep callers (return-addr count avg_ms last min max in_bo3):\n", top_n);
  int shown = 0;
  for (const auto &[ret, s] : snapshot) {
    if (shown++ >= top_n) break;
    const auto avg = s.count ? static_cast<double>(s.total_ms) / s.count : 0.0;
    printf("[ bo3-bundle perf ]   0x%016llX  cnt=%llu  avg=%.2fms  last=%ums  min=%ums  max=%ums  %s\n",
           static_cast<unsigned long long>(ret),
           static_cast<unsigned long long>(s.count), avg, s.last_ms,
           s.min_ms == UINT32_MAX ? 0 : s.min_ms, s.max_ms,
           caller_in_bo3(ret) ? "[BO3]" : "");
  }
}

void reset_sleep_histogram() {
  std::lock_guard<std::mutex> lk(g_sleep_caller_mutex);
  g_sleep_callers.clear();
  g_sleep_calls.store(0);
  g_sleep_ex_calls.store(0);
  g_sleep_skipped.store(0);
}

}  // namespace

struct component final : client_component {
  void post_load() override {
    resolve_nt();
    // INTENTIONALLY MINIMAL: ship-blocking crash at "Loading fastfile" was
    // reproduced with full step-1 here. Defaulting everything off; user
    // turns each on at runtime via the bundle_perf_* commands so we can
    // identify which one BO3 doesn't tolerate.
  }

  void post_unpack() override {
    // Sleep hook installation gated behind a command so we can isolate
    // whether the hook itself or one of the step-1 boosts is the crash
    // trigger. User runs bundle_perf_sleep_hook 1 to arm hooks.

    command::add("bundle_perf_status", [](const command::params &) {
      ULONG min_r = 0, max_r = 0, cur_r = 0;
      if (nt_query_timer_resolution) {
        nt_query_timer_resolution(&min_r, &max_r, &cur_r);
      }
      DWORD_PTR proc_m = 0, sys_m = 0;
      GetProcessAffinityMask(GetCurrentProcess(), &proc_m, &sys_m);
      const auto pc = GetPriorityClass(GetCurrentProcess());
      printf("[ bo3-bundle perf ] priority   : %s (0x%lX)\n",
             priority_name(pc), pc);
      printf("[ bo3-bundle perf ] affinity   : 0x%llX / system 0x%llX\n",
             static_cast<unsigned long long>(proc_m),
             static_cast<unsigned long long>(sys_m));
      printf("[ bo3-bundle perf ] timer res  : cur=%lu (%.3fms)  min=%lu  max=%lu\n",
             cur_r, cur_r / 10000.0, min_r, max_r);
      printf("[ bo3-bundle perf ] applied    : %lu (%.3fms)\n",
             g_applied_timer_res, g_applied_timer_res / 10000.0);
      printf("[ bo3-bundle perf ] sleep log  : %s  skip_short: %s (<=%ums)\n",
             g_sleep_log_enabled.load() ? "ON" : "off",
             g_sleep_skip_short_enabled.load() ? "ON" : "off",
             g_sleep_skip_threshold_ms.load());
    });

    command::add("bundle_perf_priority", [](const command::params &params) {
      if (params.size() < 2) {
        printf("usage: bundle_perf_priority <normal|above|high>\n");
        return;
      }
      const std::string arg = params.get(1);
      DWORD pc = 0;
      if (arg == "normal")        pc = NORMAL_PRIORITY_CLASS;
      else if (arg == "above")    pc = ABOVE_NORMAL_PRIORITY_CLASS;
      else if (arg == "high")     pc = HIGH_PRIORITY_CLASS;
      else { printf("unknown: %s (use normal|above|high)\n", arg.c_str()); return; }
      const auto ok = SetPriorityClass(GetCurrentProcess(), pc);
      printf("[ bo3-bundle perf ] set priority %s: %s\n",
             priority_name(pc), ok ? "ok" : "FAIL");
    });

    command::add("bundle_perf_timer", [](const command::params &params) {
      if (params.size() < 2) {
        printf("usage: bundle_perf_timer <0|1>\n");
        return;
      }
      const std::string arg = params.get(1);
      if (arg == "1") {
        const auto ok = apply_min_timer_resolution();
        printf("[ bo3-bundle perf ] timer boost ON: %s (applied %lu)\n",
               ok ? "ok" : "FAIL", g_applied_timer_res);
      } else {
        revert_timer_resolution();
        printf("[ bo3-bundle perf ] timer boost OFF\n");
      }
    });

    command::add("bundle_perf_sleep_log", [](const command::params &params) {
      if (params.size() < 2) {
        printf("usage: bundle_perf_sleep_log <0|1>\n");
        return;
      }
      const auto on = params.get(1) == std::string("1");
      g_sleep_log_enabled.store(on);
      printf("[ bo3-bundle perf ] sleep logging %s\n", on ? "ON" : "off");
    });

    command::add("bundle_perf_sleep_dump", [](const command::params &params) {
      const int n = params.size() >= 2 ? std::atoi(params.get(1)) : 16;
      dump_sleep_histogram(n > 0 ? n : 16);
    });

    command::add("bundle_perf_sleep_reset", [](const command::params &) {
      reset_sleep_histogram();
      printf("[ bo3-bundle perf ] sleep histogram reset\n");
    });

    command::add("bundle_perf_sleep_skip", [](const command::params &params) {
      if (params.size() < 2) {
        printf("usage: bundle_perf_sleep_skip <0|1> [threshold_ms]\n");
        return;
      }
      const auto on = params.get(1) == std::string("1");
      if (params.size() >= 3) {
        const int t = std::atoi(params.get(2));
        if (t >= 0 && t <= 1000) {
          g_sleep_skip_threshold_ms.store(static_cast<uint32_t>(t));
        }
      }
      g_sleep_skip_short_enabled.store(on);
      printf("[ bo3-bundle perf ] sleep skip-short %s (threshold <=%ums, BO3 callers only)\n",
             on ? "ON" : "off", g_sleep_skip_threshold_ms.load());
    });

    static std::atomic<bool> sleep_hooks_armed{false};
    command::add("bundle_perf_sleep_hook", [](const command::params &params) {
      if (params.size() < 2) {
        printf("usage: bundle_perf_sleep_hook <0|1>  (1 installs Sleep/SleepEx detours)\n");
        return;
      }
      if (params.get(1) == std::string("1")) {
        if (sleep_hooks_armed.load()) {
          printf("[ bo3-bundle perf ] sleep hooks already armed\n");
          return;
        }
        g_sleep_hook.create(Sleep, perf_sleep_stub);
        g_sleep_ex_hook.create(SleepEx, perf_sleep_ex_stub);
        sleep_hooks_armed.store(true);
        printf("[ bo3-bundle perf ] sleep hooks armed\n");
      } else {
        printf("[ bo3-bundle perf ] (detour disarm not supported -- restart BOIII to remove)\n");
      }
    });

    command::add("bundle_perf_affinity_all", [](const command::params &) {
      const auto ok = ensure_all_cores();
      DWORD_PTR proc_m = 0, sys_m = 0;
      GetProcessAffinityMask(GetCurrentProcess(), &proc_m, &sys_m);
      printf("[ bo3-bundle perf ] affinity now 0x%llX / system 0x%llX  %s\n",
             static_cast<unsigned long long>(proc_m),
             static_cast<unsigned long long>(sys_m), ok ? "ok" : "FAIL");
    });
  }

  void pre_destroy() override { revert_timer_resolution(); }
};

}  // namespace perf

REGISTER_COMPONENT(perf::component)
