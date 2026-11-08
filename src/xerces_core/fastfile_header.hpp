#pragma once

// A fastfile's (.ff) header, headless (docs/DESIGN.md 5.3 step 5): how much
// memory each of the zone's blocks takes when the engine loads it. Only the
// first 0x100 bytes of a file are read, never the zone itself.
//
// Black Ops III's fastfiles (Steam build of September 2026, measured on
// 2026-11-05) start with the 8 bytes "TAff0000", then the version, a u32,
// 0x251, at +8; the ten block sizes are u64s at +0xA8 + 8 * i. When a custom
// map loads, the engine takes blocks 5 and 6 of every loaded zone from one
// fixed pool, the DB stack's main pool, and block 8 from its streamed-texture
// pool (zone_memory.hpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace xerces_core {
inline constexpr std::string_view fastfile_magic = "TAff0000";
inline constexpr std::uint32_t fastfile_version = 0x251;
inline constexpr std::size_t fastfile_header_bytes = 0x100;
inline constexpr std::size_t fastfile_block_count = 10;
inline constexpr std::size_t fastfile_blocks_offset = 0xA8;

struct fastfile_header {
  std::uint32_t version = 0;
  std::array<std::uint64_t, fastfile_block_count> blocks{};

  // Blocks 5 and 6: the zone's share of the DB stack's main pool. Saturates
  // instead of wrapping on a header that claims absurd sizes.
  std::uint64_t db_main_pool_bytes() const;
  // Block 8: the zone's streamed images, which take address space in the DB
  // stack's streamed-texture pool ("gpu_prt", db_pool.hpp) while it is loaded
  // (2026-11-07: MW4 Shipment's 10.4 GiB overflowed its 18 GiB with DS4C).
  std::uint64_t db_prt_pool_bytes() const;
};

// The header from a file's first bytes. nullopt for another magic or version,
// or fewer than fastfile_header_bytes bytes.
std::optional<fastfile_header> parse_fastfile_header(std::string_view bytes);

// Reads the first fastfile_header_bytes bytes of a file and parses them;
// nullopt also when the file cannot be opened.
std::optional<fastfile_header>
read_fastfile_header(const std::filesystem::path &file);
} // namespace xerces_core
