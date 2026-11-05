#include "png.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace xerces_launcher {
namespace {
std::uint32_t crc32(const unsigned char *data, std::size_t size,
                    std::uint32_t crc = 0) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> out{};
    for (std::uint32_t n = 0; n < 256; ++n) {
      std::uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      out[n] = c;
    }
    return out;
  }();
  crc = ~crc;
  for (std::size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

void put32(std::vector<unsigned char> &out, std::uint32_t value) {
  out.push_back(static_cast<unsigned char>(value >> 24));
  out.push_back(static_cast<unsigned char>(value >> 16));
  out.push_back(static_cast<unsigned char>(value >> 8));
  out.push_back(static_cast<unsigned char>(value));
}

void chunk(std::ofstream &file, const char *type,
           const std::vector<unsigned char> &body) {
  std::vector<unsigned char> head;
  put32(head, static_cast<std::uint32_t>(body.size()));
  file.write(reinterpret_cast<const char *>(head.data()), 4);
  std::vector<unsigned char> typed(type, type + 4);
  typed.insert(typed.end(), body.begin(), body.end());
  file.write(reinterpret_cast<const char *>(typed.data()),
             static_cast<std::streamsize>(typed.size()));
  std::vector<unsigned char> tail;
  put32(tail, crc32(typed.data(), typed.size()));
  file.write(reinterpret_cast<const char *>(tail.data()), 4);
}
} // namespace

bool write_png_rgb(const std::filesystem::path &path, const unsigned char *rgb,
                   int width, int height) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  static const unsigned char signature[8] = {0x89, 'P', 'N', 'G',
                                             '\r', '\n', 0x1A, '\n'};
  file.write(reinterpret_cast<const char *>(signature), 8);

  std::vector<unsigned char> header;
  put32(header, static_cast<std::uint32_t>(width));
  put32(header, static_cast<std::uint32_t>(height));
  header.insert(header.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
  chunk(file, "IHDR", header);

  // Raw scanlines, each with filter byte 0.
  const std::size_t stride = std::size_t(width) * 3;
  std::vector<unsigned char> raw;
  raw.reserve((stride + 1) * std::size_t(height));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb + std::size_t(y) * stride,
               rgb + std::size_t(y + 1) * stride);
  }
  // zlib stream of stored blocks (at most 65535 bytes each) and Adler-32.
  std::vector<unsigned char> z = {0x78, 0x01};
  std::uint32_t a = 1, b = 0;
  for (const unsigned char byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  std::size_t offset = 0;
  do {
    const std::size_t size = std::min<std::size_t>(65535, raw.size() - offset);
    const bool last = offset + size == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<unsigned char>(size & 0xFF));
    z.push_back(static_cast<unsigned char>(size >> 8));
    z.push_back(static_cast<unsigned char>(~size & 0xFF));
    z.push_back(static_cast<unsigned char>((~size >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + std::ptrdiff_t(offset),
             raw.begin() + std::ptrdiff_t(offset + size));
    offset += size;
  } while (offset < raw.size());
  put32(z, (b << 16) | a);
  chunk(file, "IDAT", z);
  chunk(file, "IEND", {});
  return static_cast<bool>(file);
}
} // namespace xerces_launcher
