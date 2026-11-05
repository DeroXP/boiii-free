#include "fastfile_header.hpp"

#include <fstream>
#include <limits>
#include <string>

namespace xerces_core {
namespace {
// Little-endian, as the game writes them.
template <typename T> T read_le(std::string_view bytes, std::size_t at) {
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(static_cast<unsigned char>(bytes[at + i]))
             << (8 * i);
  }
  return value;
}
} // namespace

std::uint64_t fastfile_header::db_main_pool_bytes() const {
  const std::uint64_t a = blocks[5];
  const std::uint64_t b = blocks[6];
  return a > std::numeric_limits<std::uint64_t>::max() - b
             ? std::numeric_limits<std::uint64_t>::max()
             : a + b;
}

std::optional<fastfile_header> parse_fastfile_header(std::string_view bytes) {
  if (bytes.size() < fastfile_header_bytes ||
      !bytes.starts_with(fastfile_magic)) {
    return std::nullopt;
  }
  fastfile_header header;
  header.version = read_le<std::uint32_t>(bytes, fastfile_magic.size());
  if (header.version != fastfile_version) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < fastfile_block_count; ++i) {
    header.blocks[i] =
        read_le<std::uint64_t>(bytes, fastfile_blocks_offset + 8 * i);
  }
  return header;
}

std::optional<fastfile_header>
read_fastfile_header(const std::filesystem::path &file) {
  std::ifstream in(file, std::ios::binary);
  if (!in.is_open()) {
    return std::nullopt;
  }
  std::string bytes(fastfile_header_bytes, '\0');
  in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(in.gcount()));
  return parse_fastfile_header(bytes);
}
} // namespace xerces_core
