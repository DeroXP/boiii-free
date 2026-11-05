#include "sha256.hpp"

#include <algorithm>
#include <cstring>

namespace xerces_core {
namespace {
constexpr std::array<std::uint32_t, 64> round_constants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotate_right(std::uint32_t value, int bits) {
  return (value >> bits) | (value << (32 - bits));
}
} // namespace

sha256::sha256()
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void sha256::block(const std::uint8_t *chunk) {
  std::array<std::uint32_t, 64> words{};
  for (int i = 0; i < 16; ++i) {
    words[i] = (std::uint32_t{chunk[i * 4]} << 24) |
               (std::uint32_t{chunk[i * 4 + 1]} << 16) |
               (std::uint32_t{chunk[i * 4 + 2]} << 8) |
               std::uint32_t{chunk[i * 4 + 3]};
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotate_right(words[i - 15], 7) ^
                             rotate_right(words[i - 15], 18) ^
                             (words[i - 15] >> 3);
    const std::uint32_t s1 = rotate_right(words[i - 2], 17) ^
                             rotate_right(words[i - 2], 19) ^
                             (words[i - 2] >> 10);
    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
  }

  std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t s1 =
        rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choice = (e & f) ^ (~e & g);
    const std::uint32_t t1 = h + s1 + choice + round_constants[i] + words[i];
    const std::uint32_t s0 =
        rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void sha256::update(const void *data, std::size_t size) {
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  length_ += size;
  // Whole blocks are hashed where they are; only the bytes that do not fill
  // a block are copied into buffer_ until more arrive.
  if (buffered_ > 0) {
    const std::size_t take = std::min(size, buffer_.size() - buffered_);
    std::memcpy(buffer_.data() + buffered_, bytes, take);
    buffered_ += take;
    bytes += take;
    size -= take;
    if (buffered_ < buffer_.size()) {
      return;
    }
    block(buffer_.data());
    buffered_ = 0;
  }
  for (; size >= buffer_.size(); size -= buffer_.size()) {
    block(bytes);
    bytes += buffer_.size();
  }
  if (size > 0) {
    std::memcpy(buffer_.data(), bytes, size);
    buffered_ = size;
  }
}

void sha256::update(std::string_view data) { update(data.data(), data.size()); }

sha256::digest sha256::finish() {
  const std::uint64_t bit_length = length_ * 8;
  const std::uint8_t pad_start = 0x80;
  update(&pad_start, 1);
  const std::uint8_t zero = 0;
  while (buffered_ != 56) {
    update(&zero, 1);
  }
  std::uint8_t length_bytes[8];
  for (int i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<std::uint8_t>(bit_length >> (56 - i * 8));
  }
  update(length_bytes, sizeof(length_bytes));

  digest out{};
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
    out[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
    out[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
    out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
  }
  return out;
}

sha256::digest sha256::of(std::string_view data) {
  sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

std::string sha256::to_hex(const digest &value) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const std::uint8_t byte : value) {
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 0x0f]);
  }
  return out;
}

std::string sha256::hex(std::string_view data) { return to_hex(of(data)); }

std::string sha256_tagged(std::string_view data) {
  return "sha256:" + sha256::hex(data);
}
} // namespace xerces_core
