#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace xerces_core {
// SHA-256 (FIPS 180-4). Small and dependency-free so xerces_core stays
// headless.
class sha256 {
public:
  using digest = std::array<std::uint8_t, 32>;

  sha256();
  void update(std::string_view data);
  void update(const void *data, std::size_t size);
  digest finish();

  static digest of(std::string_view data);
  // Lower-case hex, 64 characters.
  static std::string hex(std::string_view data);
  static std::string to_hex(const digest &value);

private:
  void block(const std::uint8_t *chunk);

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t length_ = 0;
};

// "sha256:" + hex, the form manifests and packs use.
std::string sha256_tagged(std::string_view data);
} // namespace xerces_core
