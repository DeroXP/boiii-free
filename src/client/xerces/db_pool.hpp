#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The larger zone memory's view of the DB main pool (db_pool.cpp), for the
// out-of-memory report.
namespace xerces_db_pool {
struct main_pool_usage {
  std::uint32_t pages = 0;      // committed 64 KiB pages now
  std::uint32_t peak_pages = 0; // the most seen
  std::uint64_t size = 0;       // the pool's size field
};

// Only with the larger zone memory on and the pool's region read back;
// nothing otherwise. Reads two values the engine keeps: safe on any thread.
std::optional<main_pool_usage> main_usage();

// Every pool the engine created (23 on build 0x6531394), as its descriptor
// says now: the same size (+0x10) and committed 64 KiB pages (+0x20) fields
// as the main pool's. With the larger zone memory on only (the detour records
// the pools); a descriptor that cannot be read is left out. The engine's
// out-of-memory check is the same for every pool, so a report needs them all
// to say which one filled up (2026-11-07: MW4 Shipment with the main pool at
// 2753 of 4096 MiB).
struct pool_state {
  std::uint32_t index = 0; // creation order
  std::string name;
  std::uint32_t flags = 0;
  std::uint64_t size = 0;
  std::uint32_t pages = 0;
};
std::vector<pool_state> pools();
// One pool as a JSON object (index, name, flags, MiB in use, size), and the
// pools in use as an array of them.
std::string pool_json(const pool_state &state);
std::string pools_json();
// The index of the pool whose descriptor starts at that address (the
// detour's r9 minus 8), if any.
std::optional<std::uint32_t> pool_of_descriptor(std::uint64_t address);
} // namespace xerces_db_pool
