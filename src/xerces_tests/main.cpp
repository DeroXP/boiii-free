#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>

#include <Windows.h>

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
} // namespace xerces_tests

int main(int argc, char **argv) {
  using namespace xerces_tests;
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
