#pragma once

// A small doctest-style harness: TEST_CASE registers a test, CHECK records a
// failure and continues, REQUIRE stops the test. Swap for doctest when it is
// vendored; the macro names match.

#include <concepts>
#include <exception>
#include <filesystem>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace xerces_tests {
using test_function = void (*)();

struct registrar {
  registrar(const char *name, const char *file, int line, test_function fn);
};

struct abort_test {};

void check_failed(const char *expression, const char *file, int line,
                  const std::string &detail);

template <typename T> std::string describe(const T &value) {
  if constexpr (requires(std::ostream &out, const T &v) { out << v; }) {
    std::ostringstream out;
    out << value;
    return out.str();
  } else {
    return "<value>";
  }
}

template <typename A, typename B>
void check_equal(const A &left, const B &right, const char *expression,
                 const char *file, int line, bool required) {
  if (left == right) {
    return;
  }
  check_failed(expression, file, line,
               "left:  " + describe(left) + "\n    right: " + describe(right));
  if (required) {
    throw abort_test{};
  }
}

// tests/fixtures, found from XERCES_FIXTURES, the working directory or the
// executable's location (build/bin/x64/<config>/).
std::filesystem::path fixtures_dir();
std::string read_file(const std::filesystem::path &path);
} // namespace xerces_tests

#define XT_CONCAT_INNER(a, b) a##b
#define XT_CONCAT(a, b) XT_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                        \
  static void XT_CONCAT(xt_test_, __LINE__)();                                 \
  static const ::xerces_tests::registrar XT_CONCAT(xt_registrar_, __LINE__)(   \
      name, __FILE__, __LINE__, &XT_CONCAT(xt_test_, __LINE__));               \
  static void XT_CONCAT(xt_test_, __LINE__)()

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      ::xerces_tests::check_failed(#expr, __FILE__, __LINE__, {});             \
    }                                                                          \
  } while (false)

#define REQUIRE(expr)                                                          \
  do {                                                                         \
    if (!(expr)) {                                                             \
      ::xerces_tests::check_failed(#expr, __FILE__, __LINE__, {});             \
      throw ::xerces_tests::abort_test{};                                      \
    }                                                                          \
  } while (false)

#define CHECK_EQ(left, right)                                                  \
  ::xerces_tests::check_equal((left), (right), #left " == " #right, __FILE__,  \
                              __LINE__, false)

#define REQUIRE_EQ(left, right)                                                \
  ::xerces_tests::check_equal((left), (right), #left " == " #right, __FILE__,  \
                              __LINE__, true)
