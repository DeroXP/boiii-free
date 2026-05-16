#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "command.hpp"

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

}  // namespace

// Final shape after the Sleep-hook exploration was abandoned:
//
// What this component DOES (manual, opt-in via console commands):
//   - bundle_perf_priority <normal|above|high>  -- SetPriorityClass
//   - bundle_perf_timer <0|1>                   -- toggle 0.5ms timer res
//   - bundle_perf_affinity_all                  -- ensure all cores enabled
//   - bundle_perf_status                        -- print current state
//
// What this component does NOT do (intentionally):
//   - Auto-apply at startup: previously crashed in post_load during fastfile
//     loading. Post_unpack might be safe but we never confirmed it for this
//     combination. Manual is the known-safe ship state.
//   - Sleep / SleepEx hooks: every Sleep-suppression variant tried (blanket
//     return, Sleep(0), SwitchToThread, rate-limited 1-in-N, state-gated)
//     crashed BO3 during gameplay transitions or fastfile loading. The
//     busy-wait loop at the engine's frame-limit is structurally
//     load-bearing -- yielding it always breaks cross-thread sync somewhere.
//     Pure logging (no skip) also crashed reliably in later tests. Removed.
//   - Auto-disable VSync: r_vsync is a game cvar; just run "r_vsync 0" in
//     the console after launch (or via bundle_commands.txt).
//
// Recommended workflow per session:
//   1. Launch BOIII, wait for main menu.
//   2. Console / bundle_commands.txt:
//        r_vsync 0
//        bundle_perf_priority high
//        bundle_perf_timer 1
//   3. Play normally. Real-gameplay FPS goes from VSync-locked ~60 (default)
//      to ~200 on a modern CPU/GPU. Boost is roughly priority class +
//      timer-res frame pacing + uncapped present.
struct component final : client_component {
  void post_load() override { resolve_nt(); }

  void post_unpack() override {
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
