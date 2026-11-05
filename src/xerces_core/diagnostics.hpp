#pragma once

#include <string>
#include <vector>

namespace xerces_core {
enum class severity { info, warning, error };

const char *to_string(severity value);

// One finding about an input. `code` is a stable dotted name (for scripts and
// tests), `path` a JSON pointer into the input when there is one.
struct diagnostic {
  severity level = severity::error;
  std::string code;
  std::string path;
  std::string message;
};

class diagnostics {
public:
  void add(severity level, std::string code, std::string path,
           std::string message);
  void error(std::string code, std::string path, std::string message);
  void warning(std::string code, std::string path, std::string message);
  void info(std::string code, std::string path, std::string message);
  void append(const diagnostics &other);

  bool has_errors() const;
  bool has(const std::string &code) const;
  std::size_t count(severity level) const;
  const std::vector<diagnostic> &items() const { return items_; }
  bool empty() const { return items_.empty(); }

private:
  std::vector<diagnostic> items_;
};
} // namespace xerces_core
