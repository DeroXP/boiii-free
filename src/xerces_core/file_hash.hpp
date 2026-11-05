#pragma once

// Hashing files fast for catalog scans (DESIGN.md 12): SHA-256 through
// Windows CNG (bcrypt.dll, inbox; it uses the CPU's SHA extensions where
// there are any), read straight into one buffer per file. The portable
// sha256 class stays the reference: the digests are the same bytes, and the
// tests compare the two on random buffers and edge sizes.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "sha256.hpp"

namespace xerces_core {
enum class sha256_engine {
  automatic, // CNG when bcrypt.dll offers SHA-256, else portable
  portable,  // always the portable code (tests, benchmarks)
};

// Whether CNG's SHA-256 could be opened in this process (once, then
// remembered).
bool cng_sha256_available();

// One SHA-256 computation through the chosen engine.
class sha256_stream {
public:
  explicit sha256_stream(sha256_engine engine = sha256_engine::automatic);
  ~sha256_stream();
  sha256_stream(const sha256_stream &) = delete;
  sha256_stream &operator=(const sha256_stream &) = delete;

  void update(const void *data, std::size_t size);
  // Nothing when CNG reported an error on the way (never seen in practice;
  // a caller treats it as a failed read).
  std::optional<sha256::digest> finish();
  bool native() const { return handle_ != nullptr; }

private:
  void *handle_ = nullptr; // BCRYPT_HASH_HANDLE
  sha256 portable_;
  bool failed_ = false;
};

// Lower-case hex SHA-256 of exactly `size` bytes of the file, or nothing: the
// file could not be opened (gone, or held by a program that shares no
// reads), a read failed, it held more or fewer bytes than `size` (it changed
// while being read), or cancel was set (checked between 1 MiB reads). Opened
// for reading only, sharing reads, writes and deletes, so Steam can still
// update or remove it meanwhile.
std::optional<std::string>
sha256_file_of_size(const std::filesystem::path &path, std::uint64_t size,
                    const std::atomic<bool> *cancel = nullptr,
                    sha256_engine engine = sha256_engine::automatic);

// Whether the drive holding path is a spinning disk (Windows' seek-penalty
// property), where reading several files at once makes the head jump
// between them. Nothing when Windows does not say.
std::optional<bool> drive_has_seek_penalty(const std::filesystem::path &path);

// How many files a scan hashes at once under folder: 1 on a spinning disk,
// else half the logical processors, from 1 to 6.
unsigned default_hash_threads(const std::filesystem::path &folder);
} // namespace xerces_core
