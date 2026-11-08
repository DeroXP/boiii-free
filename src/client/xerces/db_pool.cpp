#include <std_include.hpp>

// xerces_db_pool (docs/DESIGN.md 5.4 step 4): an opt-in larger zone-memory
// pool, on only when the game starts with -xerces-db-pool (the launcher's
// "Larger zone memory (experimental)"), until it has been tested in the game.
//
// The engine's DB (fastfile) stack has a fixed 2 GiB "main" pool. Two custom
// maps with DS4C overflow it while their main zone loads, and the engine then
// ends the game. Its streamed-texture pool ("gpu_prt", 18 GiB) is the other
// limit: MW4 Shipment's streamed images overflow it with DS4C. The sizes come
// from a start-up function Arxan keeps encrypted, which runs right after the
// components' post_unpack; it passes each pool's size to a plain helper and
// the shared page budget to another (xerces_core/db_pool.hpp). This detours
// both helpers, found through the signature registry and checked byte for
// byte first: the first pool (the DB main pool) gets 4 GiB instead of 2 GiB,
// the first "gpu_prt" pool 30 GiB instead of 18 GiB, the page budget 8 GiB
// instead of 5 GiB, and every other call passes through unchanged.
//
// The detours run inside the engine's PMem set-up, before Com_Init. The game
// log is open by then (post_unpack logs), but they still only copy what they
// see into fixed storage: no allocation, no lock, no file. The first main-loop
// frame reads the result back and the async pipeline logs it
// (xerces.db_pool), so a test run shows whether the hooks were hit.

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <loader/component_loader.hpp>

#include <utils/hook.hpp>

#include <xerces_core/db_pool.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/sigruntime.hpp>

#include "db_pool.hpp"
#include "runtime.hpp"
#include "signatures.hpp"

namespace xerces_db_pool {
namespace {
namespace core = xerces_core::db_pool;
namespace ids = xerces_core::db_pool_signatures;

utils::hook::detour create_pool_hook;
utils::hook::detour init_page_budget_hook;

// ------------------------------------------------- what the detours record
// The engine creates 23 pools on build 0x6531394; room for more, and a count
// of every call either way.
constexpr std::uint32_t max_recorded = 64;

struct recorded_call {
  std::atomic<bool> done{false};
  core::pool_call call;
  std::uintptr_t region = 0; // r9: the pool's descriptor + 8
};

recorded_call pool_calls[max_recorded];
std::atomic<std::uint32_t> pool_call_count{0};
// A "gpu_prt" pool was created: only the first (the DB stack's) may grow.
std::atomic<bool> prt_seen{false};

std::atomic<bool> budget_done{false};
core::budget_call first_budget;
std::atomic<std::uint32_t> budget_call_count{0};

// The helper at registry id pmem.create_pool (0x1420C3520 on 0x6531394):
// rcx the pool's name, edx its flags (32 bits, stored at [r9+0x1C]), r8 its
// size (64 bits, stored at [r9+0x10]; 0 means 64 KiB), r9 its region. It
// returns a bool zero-extended into eax (movzx eax, bl: whether the
// reservation succeeded); the whole of rax is passed back unchanged.
std::uint64_t create_pool_stub(const char *name, const std::uint32_t flags,
                               const std::uint64_t size, void *region) {
  const std::uint32_t index =
      pool_call_count.fetch_add(1, std::memory_order_relaxed);
  core::pool_call call;
  call.name = core::copy_pool_name(name);
  call.flags = flags;
  call.size_in = size;
  const bool first_prt = call.name.view() == core::prt_pool_name &&
                         !prt_seen.exchange(true, std::memory_order_relaxed);
  call.size_out =
      core::pool_size(index == 0, first_prt, call.name.view(), flags, size);
  const auto result = create_pool_hook.invoke<std::uint64_t>(
      name, flags, call.size_out, region);
  call.result = static_cast<std::uint8_t>(result);
  if (index < max_recorded) {
    pool_calls[index].call = call;
    pool_calls[index].region = reinterpret_cast<std::uintptr_t>(region);
    pool_calls[index].done.store(true, std::memory_order_release);
  }
  return result;
}

// The helper at registry id pmem.init_page_budget (0x1420C3F90): rcx the
// shared page budget in bytes, rdx a second byte count it passes on (shifted)
// to the page adder, which never reads it. It returns nothing: rax holds
// whatever its last instructions left there.
void init_page_budget_stub(const std::uint64_t bytes, const std::uint64_t rdx) {
  const std::uint64_t given = core::page_budget(bytes);
  if (budget_call_count.fetch_add(1, std::memory_order_relaxed) == 0) {
    first_budget.bytes_in = bytes;
    first_budget.bytes_out = given;
    first_budget.rdx = rdx;
    budget_done.store(true, std::memory_order_release);
  }
  init_page_budget_hook.invoke<void>(given, rdx);
}

// ------------------------------------------------------------- read back
bool readable(const void *start, const std::size_t size) {
  const auto *at = static_cast<const std::uint8_t *>(start);
  const auto *end = at + size;
  while (at < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(at, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 ||
        (info.Protect &
         (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
          PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
      return false;
    }
    at = static_cast<const std::uint8_t *>(info.BaseAddress) + info.RegionSize;
  }
  return true;
}

// The DB main pool's region, once its first call was recorded and the bytes
// the engine keeps there are readable: its size at +0x10 and its committed
// 64 KiB pages at +0x20 (docs/DESIGN.md 5.4 step 4).
std::atomic<std::uintptr_t> main_region{0};
std::atomic<std::uint64_t> main_size{0};
std::atomic<std::uint32_t> peak_pages{0};
constexpr std::size_t size_offset = 0x10;
constexpr std::size_t pages_offset = 0x20;

std::uint32_t main_pages() {
  const std::uintptr_t region = main_region.load(std::memory_order_acquire);
  if (region == 0) {
    return 0;
  }
  return reinterpret_cast<const volatile std::uint32_t *>(region +
                                                          pages_offset)[0];
}

core::report snapshot() {
  core::report r;
  r.pool_calls = pool_call_count.load(std::memory_order_relaxed);
  r.budget_calls = budget_call_count.load(std::memory_order_relaxed);
  for (std::uint32_t i = 0; i < std::min(r.pool_calls, max_recorded); ++i) {
    if (!pool_calls[i].done.load(std::memory_order_acquire)) {
      break;
    }
    r.calls.push_back(pool_calls[i].call);
  }
  if (budget_done.load(std::memory_order_acquire)) {
    r.budget = first_budget;
  }
  if (core::pool_raised(r)) {
    const std::uintptr_t region = pool_calls[0].region;
    if (region != 0 &&
        readable(reinterpret_cast<const void *>(region), pages_offset + 4)) {
      r.main_size =
          *reinterpret_cast<const std::uint64_t *>(region + size_offset);
      main_size.store(*r.main_size, std::memory_order_relaxed);
      main_region.store(region, std::memory_order_release);
      r.main_pages = main_pages();
    }
  }
  return r;
}

void log_report(const core::report &r) {
  xerces_runtime::log(core::applied(r) ? xerces_core::log_level::info
                                       : xerces_core::log_level::warn,
                      "xerces.db_pool", core::message(r), core::data_json(r));
}

// Once a second on the main loop: the DB main pool's committed size, logged
// (from the async pipeline) the first time it passes the stock 2 GiB and each
// time the journal enters the front end, a level's load or a level. Whether
// the pool empties between maps: on 2026-11-07 MW4 Shipment ran out of zone
// memory one second into its load, right after Shadows of Evil, with the
// larger pool and a stack that should have left it more than 1 GiB.
void watch_usage() {
  static bool logged_above = false;
  static std::optional<xerces_core::launch_phase> last_phase;
  const std::uint32_t pages = main_pages();
  const std::uint32_t peak =
      std::max(peak_pages.load(std::memory_order_relaxed), pages);
  peak_pages.store(peak, std::memory_order_relaxed);
  const auto log_usage = [&](const char *when) {
    const std::string data =
        xerces_core::db_pool::usage_json(pages, peak, when);
    const std::string text =
        xerces_core::db_pool::usage_message(pages, peak, when);
    scheduler::once(
        [data, text] {
          xerces_runtime::log_info("xerces.db_pool.usage", text, data);
        },
        scheduler::async);
  };
  if (!logged_above && core::above_stock(pages)) {
    logged_above = true;
    log_usage("above_stock");
  }
  if (!xerces_runtime::journal_active()) {
    return;
  }
  const xerces_core::launch_phase phase = xerces_runtime::journal_phase();
  if (last_phase == phase) {
    return;
  }
  last_phase = phase;
  if (phase == xerces_core::launch_phase::frontend ||
      phase == xerces_core::launch_phase::loading_map ||
      phase == xerces_core::launch_phase::in_game) {
    log_usage(xerces_core::to_string(phase));
    // Every pool in use at that moment (the out-of-memory check is the same
    // for all of them).
    const std::string data =
        "{\"when\":" + xerces_core::json::quote(xerces_core::to_string(phase)) +
        ",\"pools\":" + pools_json() + "}";
    scheduler::once(
        [data] {
          xerces_runtime::log_info("xerces.db_pool.pools",
                                   "The engine's memory pools in use", data);
        },
        scheduler::async);
  }
}
} // namespace

std::optional<main_pool_usage> main_usage() {
  if (main_region.load(std::memory_order_acquire) == 0) {
    return std::nullopt;
  }
  main_pool_usage usage;
  usage.pages = main_pages();
  usage.peak_pages =
      std::max(peak_pages.load(std::memory_order_relaxed), usage.pages);
  usage.size = main_size.load(std::memory_order_relaxed);
  return usage;
}

std::vector<pool_state> pools() {
  std::vector<pool_state> out;
  const std::uint32_t count =
      std::min(pool_call_count.load(std::memory_order_relaxed), max_recorded);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!pool_calls[i].done.load(std::memory_order_acquire)) {
      break;
    }
    const std::uintptr_t region = pool_calls[i].region;
    if (region == 0 ||
        !readable(reinterpret_cast<const void *>(region), pages_offset + 4)) {
      continue;
    }
    pool_state state;
    state.index = i;
    state.name = std::string(pool_calls[i].call.name.view());
    state.flags = pool_calls[i].call.flags;
    state.size =
        *reinterpret_cast<const volatile std::uint64_t *>(region + size_offset);
    state.pages = *reinterpret_cast<const volatile std::uint32_t *>(
        region + pages_offset);
    out.push_back(std::move(state));
  }
  return out;
}

std::optional<std::uint32_t> pool_of_descriptor(const std::uint64_t address) {
  const std::uint32_t count =
      std::min(pool_call_count.load(std::memory_order_relaxed), max_recorded);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (pool_calls[i].done.load(std::memory_order_acquire) &&
        pool_calls[i].region != 0 && pool_calls[i].region - 8 == address) {
      return i;
    }
  }
  return std::nullopt;
}

std::string pool_json(const pool_state &state) {
  namespace json = xerces_core::json;
  char flags[16];
  std::snprintf(flags, sizeof(flags), "0x%X", state.flags);
  return "{\"i\":" + std::to_string(state.index) +
         ",\"name\":" + json::quote(json::valid_utf8(state.name, 32)) +
         ",\"flags\":" + json::quote(flags) + ",\"mib\":" +
         std::to_string(std::uint64_t{state.pages} * core::page_size >> 20) +
         ",\"size_mib\":" + std::to_string(state.size >> 20) + "}";
}

std::string pools_json() {
  std::string out = "[";
  for (const pool_state &state : pools()) {
    if (state.pages != 0) {
      out += (out.size() > 1 ? "," : "") + pool_json(state);
    }
  }
  return out + "]";
}

namespace {

// ---------------------------------------------------------------- install
std::string rva(const std::uintptr_t address) {
  char text[24];
  std::snprintf(
      text, sizeof(text), "0x%llX",
      static_cast<unsigned long long>(address - game::get_engine_base()));
  return text;
}

constexpr std::string_view still_works =
    "The game starts with the stock 2 GiB zone memory.";

void log_off(const std::string &message, const std::string &why) {
  xerces_runtime::log_warn("xerces.db_pool.off", message,
                           "{\"feature\":\"db_pool\",\"reason\":" +
                               xerces_core::json::quote(why) + "}");
}

void off(const std::string &why) {
  log_off("Larger zone memory is off: " + why + ". " + std::string(still_works),
          why);
}

bool install() {
  if (!xerces_signatures::feature_enabled(core::feature)) {
    log_off(xerces_signatures::feature_message(
                core::feature, "Larger zone memory", still_works),
            "a signature did not resolve");
    return false;
  }
  const auto pool_at = xerces_signatures::address(ids::create_pool);
  const auto budget_at = xerces_signatures::address(ids::init_page_budget);
  if (!pool_at || !budget_at) {
    off("the signature registry has no address for " +
        std::string(!pool_at ? ids::create_pool : ids::init_page_budget));
    return false;
  }
  // Never write code bytes before the whole expected sequence is there
  // (DESIGN.md 4.3): the five or six bytes each jmp overwrites, which the
  // registry leaves as wildcards, and every byte of the registry's pattern.
  std::string why;
  if (!xerces_signatures::code_matches(ids::create_pool,
                                       core::create_pool_entry, &why) ||
      !xerces_signatures::code_matches(ids::init_page_budget,
                                       core::init_page_budget_entry, &why)) {
    off(why);
    return false;
  }
  // Both or neither: a larger pool without a larger page budget would leave
  // the other pools short of pages.
  try {
    init_page_budget_hook.create(
        reinterpret_cast<void *>(*budget_at),
        reinterpret_cast<void *>(&init_page_budget_stub));
    create_pool_hook.create(reinterpret_cast<void *>(*pool_at),
                            reinterpret_cast<void *>(&create_pool_stub));
  } catch (const std::exception &e) {
    init_page_budget_hook.clear();
    create_pool_hook.clear();
    off(std::string("a hook could not be installed (") + e.what() + ")");
    return false;
  }
  xerces_runtime::log_info(
      "xerces.db_pool.installed",
      "Larger zone memory: hooks installed; the engine sets up its pools next",
      "{\"create_pool\":" + xerces_core::json::quote(rva(*pool_at)) +
          ",\"init_page_budget\":" + xerces_core::json::quote(rva(*budget_at)) +
          "}");
  return true;
}
} // namespace

class component final : public generic_component {
  DEFINE_COMPONENT_NAME("xerces_db_pool");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client()) {
      return;
    }
    if (!core::requested(xerces_runtime::command_line_args())) {
      return;
    }
    if (!install()) {
      return;
    }
    // The first main-loop frame: the engine set its pools up long before.
    scheduler::once(
        [] {
          const core::report r = snapshot();
          scheduler::once([r] { log_report(r); }, scheduler::async);
          if (main_region.load(std::memory_order_acquire) != 0) {
            scheduler::loop(watch_usage, scheduler::main, 1s);
          }
        },
        scheduler::main);
  }
};
} // namespace xerces_db_pool

REGISTER_COMPONENT(xerces_db_pool::component)
