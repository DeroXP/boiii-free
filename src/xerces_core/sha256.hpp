#pragma once

#include <array>
#include <cstdint>
#include <span>
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

// HMAC-SHA256 (RFC 2104, FIPS 198-1).
sha256::digest hmac_sha256(std::string_view key, std::string_view message);
// PBKDF2 with HMAC-SHA256 (RFC 8018), the first 32-byte block only.
// iterations 0 counts as 1.
sha256::digest pbkdf2_sha256(std::string_view password, std::string_view salt,
                             std::uint32_t iterations);
// Whether a and b hold the same bytes, in a time that depends only on their
// lengths (a secret compared this way does not leak through timing).
bool equal_constant_time(std::span<const std::uint8_t> a,
                         std::span<const std::uint8_t> b);
} // namespace xerces_core
