#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <vector>

#include <Windows.h>
#include <winioctl.h>

#include "harness.hpp"

namespace xerces_tests {
namespace {
struct test_case {
  const char *name;
  const char *file;
  int line;
  test_function fn;
};

std::vector<test_case> &registry() {
  static std::vector<test_case> tests;
  return tests;
}

int current_failures = 0;
} // namespace

registrar::registrar(const char *name, const char *file, int line,
                     test_function fn) {
  registry().push_back({name, file, line, fn});
}

void check_failed(const char *expression, const char *file, int line,
                  const std::string &detail) {
  ++current_failures;
  std::cerr << "  " << file << "(" << line << "): CHECK " << expression
            << " failed\n";
  if (!detail.empty()) {
    std::cerr << "    " << detail << "\n";
  }
}

std::filesystem::path fixtures_dir() {
  if (const char *env = std::getenv("XERCES_FIXTURES"); env != nullptr) {
    return env;
  }
  if (std::filesystem::exists("tests/fixtures")) {
    return std::filesystem::absolute("tests/fixtures");
  }
  wchar_t module_path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  // build/bin/x64/<config>/xerces_tests.exe -> repository root
  return std::filesystem::path(module_path)
             .parent_path()
             .parent_path()
             .parent_path()
             .parent_path()
             .parent_path() /
         "tests" / "fixtures";
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool make_junction(const std::filesystem::path &link,
                   const std::filesystem::path &target,
                   const std::filesystem::path &shown) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(link, ec);
  const std::wstring substitute = L"\\??\\" + fs::absolute(target).native();
  const std::wstring print =
      (shown.empty() ? fs::absolute(target) : shown).native();
  const std::size_t names = (substitute.size() + 1 + print.size() + 1) * 2;
  std::vector<unsigned char> data(16 + names, 0);
  const auto put16 = [&](std::size_t at, std::size_t value) {
    const auto word = std::uint16_t(value);
    std::memcpy(data.data() + at, &word, sizeof(word));
  };
  const std::uint32_t tag = IO_REPARSE_TAG_MOUNT_POINT;
  std::memcpy(data.data(), &tag, sizeof(tag));
  put16(4, 8 + names);                    // data length after the header
  put16(8, 0);                            // substitute name offset
  put16(10, substitute.size() * 2);       // and length
  put16(12, (substitute.size() + 1) * 2); // print name offset
  put16(14, print.size() * 2);            // and length
  std::memcpy(data.data() + 16, substitute.data(), substitute.size() * 2);
  std::memcpy(data.data() + 16 + (substitute.size() + 1) * 2, print.data(),
              print.size() * 2);
  const HANDLE dir = CreateFileW(
      link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (dir == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD bytes = 0;
  const BOOL ok =
      DeviceIoControl(dir, FSCTL_SET_REPARSE_POINT, data.data(),
                      DWORD(data.size()), nullptr, 0, &bytes, nullptr);
  CloseHandle(dir);
  return ok != FALSE;
}

// bench_hash.cpp: the hashing micro-benchmark, only with --bench-hash.
int run_hash_bench(int argc, char **argv);
} // namespace xerces_tests

int main(int argc, char **argv) {
  using namespace xerces_tests;
  if (argc > 1 && std::string_view(argv[1]) == "--bench-hash") {
    return run_hash_bench(argc, argv);
  }
  const std::string_view filter = argc > 1 ? argv[1] : "";
  int failed_tests = 0;
  int run = 0;
  for (const auto &test : registry()) {
    if (!filter.empty() &&
        std::string_view(test.name).find(filter) == std::string_view::npos) {
      continue;
    }
    ++run;
    current_failures = 0;
    try {
      test.fn();
    } catch (const abort_test &) {
    } catch (const std::exception &e) {
      ++current_failures;
      std::cerr << "  unexpected exception: " << e.what() << "\n";
    }
    if (current_failures > 0) {
      ++failed_tests;
      std::cerr << "FAILED: " << test.name << " (" << test.file << ":"
                << test.line << ")\n";
    }
  }
  std::cout << run << " tests, " << (run - failed_tests) << " passed, "
            << failed_tests << " failed\n";
  return failed_tests == 0 ? 0 : 1;
}
