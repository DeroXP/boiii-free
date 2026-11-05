// xerces_tests.exe --bench-hash [--threads 1,2,4,6] [--mem-mib 1024] <file>...
//
// The hashing micro-benchmark (docs/TESTING.md, Launcher speed): how fast
// the catalog scan's hashing is, kept apart from how fast the disk reads.
// Files are only ever opened for reading. Not a test: nothing here runs
// unless --bench-hash is the first argument.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <xerces_core/catalog.hpp>
#include <xerces_core/file_hash.hpp>
#include <xerces_core/sha256.hpp>

namespace xerces_tests {
namespace {
namespace fs = std::filesystem;
using clock_type = std::chrono::steady_clock;

double seconds_since(clock_type::time_point start) {
  return std::chrono::duration<double>(clock_type::now() - start).count();
}

void report(const char *name, std::uint64_t bytes, double seconds,
            const std::string &note = {}) {
  const double gib = static_cast<double>(bytes) / (1024.0 * 1024 * 1024);
  std::printf("%-34s %8.2f GiB %8.2f s %9.0f MiB/s%s%s\n", name, gib, seconds,
              seconds > 0 ? gib * 1024 / seconds : 0.0,
              note.empty() ? "" : "  ", note.c_str());
  std::fflush(stdout);
}

// Reads the whole file, bypassing the file cache when unbuffered (the disk's
// own speed whatever is cached), else through it. Returns the bytes read.
std::uint64_t read_file(const fs::path &path, bool unbuffered) {
  const DWORD flags = unbuffered
                          ? FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN
                          : FILE_FLAG_SEQUENTIAL_SCAN;
  const HANDLE file =
      CreateFileW(path.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, flags, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return 0;
  }
  constexpr DWORD chunk = 4u << 20;
  void *buffer =
      VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  std::uint64_t total = 0;
  DWORD got = 0;
  while (ReadFile(file, buffer, chunk, &got, nullptr) && got > 0) {
    total += got;
  }
  VirtualFree(buffer, 0, MEM_RELEASE);
  CloseHandle(file);
  return total;
}

// Runs work(file) for every file on `threads` threads, largest first.
void run_parallel(const std::vector<fs::path> &files, unsigned threads,
                  const std::function<void(const fs::path &)> &work) {
  std::vector<fs::path> order = files;
  std::ranges::sort(order, [](const fs::path &a, const fs::path &b) {
    return fs::file_size(a) > fs::file_size(b);
  });
  std::atomic<std::size_t> next{0};
  std::vector<std::thread> pool;
  for (unsigned i = 0; i < threads; ++i) {
    pool.emplace_back([&] {
      for (std::size_t at = next++; at < order.size(); at = next++) {
        work(order[at]);
      }
    });
  }
  for (auto &thread : pool) {
    thread.join();
  }
}

std::vector<unsigned> parse_list(std::string_view text) {
  std::vector<unsigned> out;
  while (!text.empty()) {
    const std::size_t comma = text.find(',');
    out.push_back(static_cast<unsigned>(
        std::strtoul(std::string(text.substr(0, comma)).c_str(), nullptr, 10)));
    text = comma == std::string_view::npos ? std::string_view()
                                           : text.substr(comma + 1);
  }
  return out;
}
} // namespace

int run_hash_bench(int argc, char **argv) {
  std::vector<unsigned> threads = {1, 2, 4, 6};
  std::size_t mem_mib = 1024;
  std::vector<fs::path> files;
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--threads" && i + 1 < argc) {
      threads = parse_list(argv[++i]);
    } else if (arg == "--mem-mib" && i + 1 < argc) {
      mem_mib = std::strtoull(argv[++i], nullptr, 10);
    } else {
      files.emplace_back(std::string(arg));
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: xerces_tests --bench-hash [--threads 1,2,4,6] "
                         "[--mem-mib 1024] <file>...\n");
    return 1;
  }
  std::uint64_t total = 0;
  for (const auto &file : files) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) {
      std::fprintf(stderr, "cannot read %s\n", file.string().c_str());
      return 1;
    }
    total += size;
  }
  std::printf("%zu files, %.2f GiB; %u logical processors\n", files.size(),
              static_cast<double>(total) / (1024.0 * 1024 * 1024),
              std::thread::hardware_concurrency());

  // CPU only: one buffer in memory, hashed once.
  {
    std::string buffer(mem_mib << 20, '\0');
    for (std::size_t i = 0; i < buffer.size(); ++i) {
      buffer[i] = static_cast<char>((i * 2654435761u) >> 13);
    }
    auto start = clock_type::now();
    const std::string portable = xerces_core::sha256::hex(buffer);
    report("memory: portable sha256", buffer.size(), seconds_since(start));
    start = clock_type::now();
    xerces_core::sha256_stream cng;
    cng.update(buffer.data(), buffer.size());
    const auto digest = cng.finish();
    report(cng.native() ? "memory: CNG sha256" : "memory: CNG unavailable",
           buffer.size(), seconds_since(start),
           digest && xerces_core::sha256::to_hex(*digest) == portable
               ? "same digest"
               : "DIGESTS DIFFER");
  }

  // The disk: unbuffered, one file at a time, then several at once.
  {
    for (const unsigned n : threads) {
      std::atomic<std::uint64_t> bytes{0};
      const auto start = clock_type::now();
      run_parallel(files, n, [&](const fs::path &file) {
        bytes += read_file(file, true);
      });
      report(("disk read, unbuffered x" + std::to_string(n)).c_str(), bytes,
             seconds_since(start));
    }
  }

  // Through the file cache: the first pass fills it (as far as memory
  // allows), the second shows a cached read.
  for (const char *pass : {"buffered read (first)", "buffered read (again)"}) {
    std::uint64_t bytes = 0;
    const auto start = clock_type::now();
    for (const auto &file : files) {
      bytes += read_file(file, false);
    }
    report(pass, bytes, seconds_since(start));
  }

  // The scan's file hash as it is built, one file after another.
  {
    const auto start = clock_type::now();
    std::size_t failed = 0;
    for (const auto &file : files) {
      failed += xerces_core::try_file_sha256_hex(file) ? 0 : 1;
    }
    report("try_file_sha256_hex, serial", total, seconds_since(start),
           failed ? std::to_string(failed) + " failed" : "");
  }
  {
    const auto start = clock_type::now();
    for (const auto &file : files) {
      (void)xerces_core::sha256_file_of_size(
          file, fs::file_size(file), nullptr,
          xerces_core::sha256_engine::portable);
    }
    report("file, portable, serial", total, seconds_since(start));
  }
  for (const unsigned n : threads) {
    std::atomic<std::size_t> failed{0};
    const auto start = clock_type::now();
    run_parallel(files, n, [&](const fs::path &file) {
      failed += xerces_core::try_file_sha256_hex(file) ? 0 : 1;
    });
    report(("file, CNG x" + std::to_string(n)).c_str(), total,
           seconds_since(start),
           failed ? std::to_string(failed.load()) + " failed" : "");
  }
  std::printf(
      "default_hash_threads here: %u (seek penalty: %s)\n",
      xerces_core::default_hash_threads(files.front().parent_path()), [&] {
        const auto penalty = xerces_core::drive_has_seek_penalty(files.front());
        return !penalty ? "unknown" : *penalty ? "yes" : "no";
      }());
  return 0;
}
} // namespace xerces_tests
