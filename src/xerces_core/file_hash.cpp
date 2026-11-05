#include "file_hash.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>
#include <winioctl.h>

#include <algorithm>
#include <memory>
#include <string_view>
#include <thread>

namespace xerces_core {
namespace {
// bcrypt.dll is loaded when first needed rather than linked, so programs that
// link xerces_core but never hash a file (the game client) import nothing new.
using open_provider_fn = NTSTATUS(WINAPI *)(BCRYPT_ALG_HANDLE *, LPCWSTR,
                                            LPCWSTR, ULONG);
using create_hash_fn = NTSTATUS(WINAPI *)(BCRYPT_ALG_HANDLE,
                                          BCRYPT_HASH_HANDLE *, PUCHAR, ULONG,
                                          PUCHAR, ULONG, ULONG);
using hash_data_fn = NTSTATUS(WINAPI *)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG,
                                        ULONG);
using finish_hash_fn = NTSTATUS(WINAPI *)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG,
                                          ULONG);
using destroy_hash_fn = NTSTATUS(WINAPI *)(BCRYPT_HASH_HANDLE);

struct cng_functions {
  BCRYPT_ALG_HANDLE algorithm = nullptr; // shared: CNG allows it across threads
  create_hash_fn create = nullptr;
  hash_data_fn hash = nullptr;
  finish_hash_fn finish = nullptr;
  destroy_hash_fn destroy = nullptr;
};

template <typename F> F function(HMODULE module, const char *name) {
  return reinterpret_cast<F>(
      reinterpret_cast<void *>(GetProcAddress(module, name)));
}

const cng_functions *open_cng() {
  const HMODULE module =
      LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (module == nullptr) {
    return nullptr;
  }
  static cng_functions api;
  const auto open =
      function<open_provider_fn>(module, "BCryptOpenAlgorithmProvider");
  api.create = function<create_hash_fn>(module, "BCryptCreateHash");
  api.hash = function<hash_data_fn>(module, "BCryptHashData");
  api.finish = function<finish_hash_fn>(module, "BCryptFinishHash");
  api.destroy = function<destroy_hash_fn>(module, "BCryptDestroyHash");
  if (open == nullptr || api.create == nullptr || api.hash == nullptr ||
      api.finish == nullptr || api.destroy == nullptr ||
      !BCRYPT_SUCCESS(
          open(&api.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    return nullptr;
  }
  // Trust it only once it gives the portable code's digest for a known
  // input; anything else and every hash stays portable.
  BCRYPT_HASH_HANDLE probe = nullptr;
  sha256::digest digest{};
  static constexpr std::string_view input = "abc";
  const bool same =
      BCRYPT_SUCCESS(
          api.create(api.algorithm, &probe, nullptr, 0, nullptr, 0, 0)) &&
      BCRYPT_SUCCESS(api.hash(
          probe, reinterpret_cast<PUCHAR>(const_cast<char *>(input.data())),
          ULONG(input.size()), 0)) &&
      BCRYPT_SUCCESS(
          api.finish(probe, digest.data(), ULONG(digest.size()), 0)) &&
      digest == sha256::of(input);
  if (probe != nullptr) {
    api.destroy(probe);
  }
  return same ? &api : nullptr;
}

const cng_functions *cng() {
  static const cng_functions *const api = open_cng();
  return api;
}

// CreateFileW's failure value is not null, so the closer skips it itself.
struct handle_closer {
  void operator()(void *handle) const {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};
using unique_handle = std::unique_ptr<void, handle_closer>;
} // namespace

bool cng_sha256_available() { return cng() != nullptr; }

sha256_stream::sha256_stream(sha256_engine engine) {
  if (engine != sha256_engine::automatic) {
    return;
  }
  if (const cng_functions *api = cng()) {
    BCRYPT_HASH_HANDLE handle = nullptr;
    if (BCRYPT_SUCCESS(
            api->create(api->algorithm, &handle, nullptr, 0, nullptr, 0, 0))) {
      handle_ = handle;
    }
  }
}

sha256_stream::~sha256_stream() {
  if (handle_ != nullptr) {
    cng()->destroy(handle_);
  }
}

void sha256_stream::update(const void *data, std::size_t size) {
  if (handle_ == nullptr) {
    portable_.update(data, size);
    return;
  }
  auto *bytes = static_cast<PUCHAR>(const_cast<void *>(data));
  while (size > 0 && !failed_) {
    const ULONG take = ULONG(std::min<std::size_t>(size, std::size_t{1} << 30));
    failed_ = !BCRYPT_SUCCESS(cng()->hash(handle_, bytes, take, 0));
    bytes += take;
    size -= take;
  }
}

std::optional<sha256::digest> sha256_stream::finish() {
  if (handle_ == nullptr) {
    return portable_.finish();
  }
  sha256::digest digest{};
  if (failed_ || !BCRYPT_SUCCESS(cng()->finish(handle_, digest.data(),
                                               ULONG(digest.size()), 0))) {
    return std::nullopt;
  }
  return digest;
}

std::optional<std::string>
sha256_file_of_size(const std::filesystem::path &path, std::uint64_t size,
                    const std::atomic<bool> *cancel, sha256_engine engine) {
  const unique_handle file(
      CreateFileW(path.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) {
    return std::nullopt; // gone, or held open by a program that shares no reads
  }
  sha256_stream hasher(engine);
  constexpr DWORD piece = DWORD{1} << 20;
  // Not zeroed: every byte hashed was read into it first.
  const std::unique_ptr<std::uint8_t[]> buffer(new std::uint8_t[piece]);
  std::uint64_t total = 0;
  for (;;) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      return std::nullopt;
    }
    DWORD got = 0;
    if (!ReadFile(file.get(), buffer.get(), piece, &got, nullptr)) {
      return std::nullopt;
    }
    if (got == 0) {
      break;
    }
    total += got;
    if (total > size) {
      return std::nullopt; // it grew while being read
    }
    hasher.update(buffer.get(), got);
  }
  if (total != size) {
    return std::nullopt;
  }
  const std::optional<sha256::digest> digest = hasher.finish();
  if (!digest) {
    return std::nullopt;
  }
  return sha256::to_hex(*digest);
}

std::optional<bool> drive_has_seek_penalty(const std::filesystem::path &path) {
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  wchar_t mount[MAX_PATH + 1]{};
  if (ec || !GetVolumePathNameW(absolute.c_str(), mount, MAX_PATH)) {
    return std::nullopt;
  }
  // \\?\Volume{guid}\ without its last separator opens the volume itself;
  // no access rights are needed to ask for its properties.
  wchar_t volume[64]{};
  if (!GetVolumeNameForVolumeMountPointW(mount, volume, 64)) {
    return std::nullopt;
  }
  std::wstring device = volume;
  if (!device.empty() && device.back() == L'\\') {
    device.pop_back();
  }
  const unique_handle drive(CreateFileW(device.c_str(), 0,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, 0, nullptr));
  if (drive.get() == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceSeekPenaltyProperty;
  query.QueryType = PropertyStandardQuery;
  DEVICE_SEEK_PENALTY_DESCRIPTOR answer{};
  DWORD bytes = 0;
  if (!DeviceIoControl(drive.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query,
                       sizeof(query), &answer, sizeof(answer), &bytes,
                       nullptr) ||
      bytes < sizeof(answer)) {
    return std::nullopt;
  }
  return answer.IncursSeekPenalty != FALSE;
}

unsigned default_hash_threads(const std::filesystem::path &folder) {
  if (drive_has_seek_penalty(folder).value_or(false)) {
    return 1;
  }
  return std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
}
} // namespace xerces_core
