#pragma once

// The opt-in larger zone-memory pool (docs/DESIGN.md 5.4 step 4): the rules
// the game client's db_pool component (src/client/xerces/db_pool.cpp) applies,
// kept here so they can be tested without the game.
//
// The engine reserves its PMem pools at start-up, before Com_Init, from a
// function Arxan keeps encrypted (never read, scanned or patched). That
// function passes each pool's size to a plain helper (registry id
// pmem.create_pool: rcx name, edx flags, r8 size, r9 the pool's region) and
// the shared page budget to another (pmem.init_page_budget: rcx bytes). With
// the launch flag -xerces-db-pool the client detours both: the first pool, the
// DB (fastfile) stack's "main" pool, gets 4 GiB instead of 2 GiB, the DB
// stack's streamed-texture pool (the first "gpu_prt") 30 GiB instead of 18 GiB,
// and the page budget 8 GiB instead of 5 GiB. Every other call passes through
// unchanged.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xerces_core::db_pool {
// The launch flag, and its name as the launcher's settings handle flags
// (settings_options' has_flag and with_flag take names without the dash).
inline constexpr std::string_view flag = "-xerces-db-pool";
inline constexpr std::string_view flag_name = "xerces-db-pool";
// The registry's feature tag for the two signatures.
inline constexpr std::string_view feature = "db_pool";

// The flag before the first "+" command, spelled "-name" or "--name" (both
// count in the launcher's settings too).
bool requested(const std::vector<std::string> &args);

// The DB stack's main pool as the engine creates it (build 0x6531394), and
// what it becomes.
inline constexpr std::string_view main_pool_name = "main";
inline constexpr std::uint32_t main_pool_flags = 0x1E;
inline constexpr std::uint64_t stock_pool_size = 0x80000000;   // 2 GiB
inline constexpr std::uint64_t raised_pool_size = 0x100000000; // 4 GiB
// The shared page budget, in bytes: bookkeeping, nothing is committed for it.
inline constexpr std::uint64_t stock_page_budget = 0x140000000;  // 5 GiB
inline constexpr std::uint64_t raised_page_budget = 0x200000000; // 8 GiB
// The budget helper clears a page table of 0x40000 pages and adds the budget's
// pages without checking that bound: never more than 16 GiB.
inline constexpr std::uint64_t page_size = 0x10000; // 64 KiB
inline constexpr std::uint64_t page_budget_ceiling = 0x40000 * page_size;
static_assert(raised_page_budget <= page_budget_ceiling);
static_assert(raised_pool_size > stock_pool_size);

// The DB stack's streamed-texture pool, the first "gpu_prt" the engine
// creates: every loaded zone's block 8 (its streamed images) takes address
// space there. 18 GiB on build 0x6531394; MW4 Shipment (10.4 GiB of block 8)
// ran out of it with DS4C, short by 1322 MiB, and with six mods by 5950 MiB
// (2026-11-07, docs/TESTING.md "MW4 Shipment"). It reserves address space
// only. A pool's 64 KiB page index is 19 bits (0x7FFFF means none), so no pool
// may reach 32 GiB.
inline constexpr std::string_view prt_pool_name = "gpu_prt";
inline constexpr std::uint32_t prt_pool_flags = 0x7E;
inline constexpr std::uint64_t stock_prt_size = 0x480000000;  // 18 GiB
inline constexpr std::uint64_t raised_prt_size = 0x780000000; // 30 GiB
inline constexpr std::uint64_t pool_size_ceiling = 0x7FFFF * page_size;
static_assert(raised_prt_size > stock_prt_size);
static_assert(raised_prt_size < pool_size_ceiling);

// The bytes each detour's jmp overwrites. The registry's patterns leave them
// as wildcards (a dump of a client with the detours has a jmp there), so the
// client checks them itself: nothing is written unless they are exactly these.
// mov rax, rsp; push r12
inline constexpr std::array<std::uint8_t, 5> create_pool_entry = {
    0x48, 0x8B, 0xC4, 0x41, 0x54};
// push rdi; push r14; push r15 (the jmp ends inside push r15)
inline constexpr std::array<std::uint8_t, 6> init_page_budget_entry = {
    0x40, 0x57, 0x41, 0x56, 0x41, 0x57};

// The size the pool helper is given. The DB main pool is raised only on the
// helper's first call (the DB stack's main pool is created first) and only
// when that call is exactly the stock DB main pool; the engine creates five
// more "main" pools of 2 GiB with the same flags for other stacks, which keep
// their size. The streamed-texture pool is raised only on the first "gpu_prt"
// call (first_prt_call) and only when it is exactly the stock one; three more
// "gpu_prt" pools keep their size.
std::uint64_t pool_size(bool first_call, bool first_prt_call,
                        std::string_view name, std::uint32_t flags,
                        std::uint64_t size);
// The page budget the budget helper is given: raised only from exactly the
// stock budget.
std::uint64_t page_budget(std::uint64_t bytes);

// A pool name as the detour keeps it: at most 15 characters, up to the first
// NUL; empty for a null pointer. No allocation, so it is safe that early.
struct pool_name {
  char text[16] = {};
  std::string_view view() const { return text; }
};
pool_name copy_pool_name(const char *name);

// What the detours saw, kept in fixed storage while the engine starts and
// logged once it runs.
struct pool_call {
  pool_name name;
  std::uint32_t flags = 0;
  std::uint64_t size_in = 0;
  std::uint64_t size_out = 0;
  std::uint64_t result = 0; // what the helper returned: 1 when it reserved
};
struct budget_call {
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
  std::uint64_t rdx = 0; // the second argument, passed on unchanged
};

struct report {
  std::vector<pool_call> calls;      // the recorded ones, in call order
  std::uint32_t pool_calls = 0;      // every call, recorded or not
  std::optional<budget_call> budget; // the first call
  std::uint32_t budget_calls = 0;
  // Read back from the first pool's region once the engine runs: its size
  // field (+0x10) and its committed 64 KiB pages (+0x20).
  std::optional<std::uint64_t> main_size;
  std::optional<std::uint32_t> main_pages;
};
bool pool_raised(const report &r);
// The first recorded "gpu_prt" call was the stock streamed-texture pool and
// was given the raised size.
bool prt_raised(const report &r);
bool budget_raised(const report &r);
// All three raised, and the main pool's size read back (when it could be) is
// the raised one.
bool applied(const report &r);
// The xerces.db_pool event: a sentence and its data (every call included).
std::string message(const report &r);
std::string data_json(const report &r);
std::string pool_call_json(const pool_call &call);

// The first pool holds more than the stock 2 GiB: what only the larger pool
// allows, logged once as the proof a map used it.
bool above_stock(std::uint32_t pages);
// The xerces.db_pool.usage event: the DB main pool's committed 64 KiB pages
// now and the most seen (sampled once a second), and why it is logged:
// "above_stock" (the first time), or the journal phase it entered
// ("frontend", "loading_map", "in_game"; each time).
std::string usage_message(std::uint32_t pages, std::uint32_t peak_pages,
                          std::string_view when);
std::string usage_json(std::uint32_t pages, std::uint32_t peak_pages,
                       std::string_view when);
} // namespace xerces_core::db_pool
