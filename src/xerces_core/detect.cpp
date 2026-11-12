#include "detect.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iterator>

#include <Windows.h>

#include "launcher_home.hpp"

namespace xerces_core {
namespace {
bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

class vdf_reader {
public:
  explicit vdf_reader(std::string_view text) : text_(text) {}

  // A token: a quoted or bare string, '{' or '}'. Empty at the end.
  std::optional<std::string> next(bool &is_string) {
    skip();
    if (pos_ >= text_.size()) {
      return std::nullopt;
    }
    const char c = text_[pos_];
    if (c == '{' || c == '}') {
      ++pos_;
      is_string = false;
      return std::string(1, c);
    }
    is_string = true;
    std::string out;
    if (c == '"') {
      ++pos_;
      while (pos_ < text_.size() && text_[pos_] != '"') {
        if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
          const char escaped = text_[pos_ + 1];
          out.push_back(escaped == 'n'   ? '\n'
                        : escaped == 't' ? '\t'
                                         : escaped);
          pos_ += 2;
          continue;
        }
        out.push_back(text_[pos_++]);
      }
      if (pos_ >= text_.size()) {
        error_ = "a quoted string is not closed";
        return std::nullopt;
      }
      ++pos_;
      return out;
    }
    while (pos_ < text_.size() &&
           !std::isspace(static_cast<unsigned char>(text_[pos_])) &&
           text_[pos_] != '{' && text_[pos_] != '}' && text_[pos_] != '"') {
      out.push_back(text_[pos_++]);
    }
    return out;
  }

  const std::string &error() const { return error_; }

private:
  void skip() {
    while (pos_ < text_.size()) {
      if (std::isspace(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      } else if (text_.substr(pos_, 2) == "//") {
        while (pos_ < text_.size() && text_[pos_] != '\n') {
          ++pos_;
        }
      } else {
        break;
      }
    }
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  std::string error_;
};

bool parse_children(vdf_reader &reader, kv_node &parent, bool nested,
                    int depth) {
  if (depth > 32) {
    return false;
  }
  while (true) {
    bool is_string = false;
    auto key = reader.next(is_string);
    if (!key) {
      return !nested && reader.error().empty();
    }
    if (!is_string) {
      return *key == "}" && nested;
    }
    kv_node node;
    node.key = *key;
    auto value = reader.next(is_string);
    if (!value) {
      return false;
    }
    if (!is_string) {
      if (*value != "{") {
        return false;
      }
      node.section = true;
      if (!parse_children(reader, node, true, depth + 1)) {
        return false;
      }
    } else {
      node.value = *value;
    }
    parent.children.push_back(std::move(node));
  }
}

std::filesystem::path from_utf8(const std::string &text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::optional<std::filesystem::path>
registry_path(HKEY root, const wchar_t *key, const wchar_t *value) {
  wchar_t buffer[MAX_PATH * 2]{};
  DWORD size = sizeof(buffer);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) !=
      ERROR_SUCCESS) {
    return std::nullopt;
  }
  return std::filesystem::path(buffer);
}
} // namespace

const kv_node *kv_node::child(std::string_view name) const {
  const auto it = std::ranges::find_if(
      children, [&](const kv_node &node) { return iequals(node.key, name); });
  return it == children.end() ? nullptr : &*it;
}

std::optional<kv_node> parse_vdf(std::string_view text, std::string *error) {
  if (text.starts_with("\xEF\xBB\xBF")) {
    text.remove_prefix(3);
  }
  vdf_reader reader(text);
  kv_node root;
  root.section = true;
  if (!parse_children(reader, root, false, 0)) {
    if (error != nullptr) {
      *error = reader.error().empty() ? "unbalanced braces or a missing value"
                                      : reader.error();
    }
    return std::nullopt;
  }
  return root;
}

std::vector<std::filesystem::path> steam_libraries(std::string_view vdf) {
  std::vector<std::filesystem::path> out;
  const auto root = parse_vdf(vdf);
  if (!root) {
    return out;
  }
  const kv_node *folders = root->child("libraryfolders");
  if (folders == nullptr || !folders->section) {
    return out;
  }
  for (const auto &entry : folders->children) {
    const bool numbered =
        !entry.key.empty() && std::ranges::all_of(entry.key, [](char c) {
          return c >= '0' && c <= '9';
        });
    if (!numbered) {
      continue;
    }
    if (entry.section) {
      if (const kv_node *path = entry.child("path"); path != nullptr) {
        out.push_back(from_utf8(path->value));
      }
    } else if (!entry.value.empty()) {
      out.push_back(from_utf8(entry.value)); // the old layout: "1" "D:\\Lib"
    }
  }
  return out;
}

std::optional<std::string> appmanifest_installdir(std::string_view acf) {
  const auto root = parse_vdf(acf);
  if (!root) {
    return std::nullopt;
  }
  const kv_node *state = root->child("AppState");
  const kv_node *dir = state != nullptr ? state->child("installdir") : nullptr;
  if (dir == nullptr || dir->value.empty() ||
      dir->value.find_first_of("/\\:") != std::string::npos ||
      dir->value == "..") {
    return std::nullopt; // a folder name, nothing else
  }
  return dir->value;
}

std::optional<std::uint64_t> appmanifest_buildid(std::string_view acf) {
  const auto root = parse_vdf(acf);
  if (!root) {
    return std::nullopt;
  }
  const kv_node *state = root->child("AppState");
  const kv_node *build = state != nullptr ? state->child("buildid") : nullptr;
  if (build == nullptr || build->value.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const char *end = build->value.data() + build->value.size();
  const auto parsed = std::from_chars(build->value.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  return value;
}

detection detect_game(const std::optional<std::filesystem::path> &saved,
                      const std::vector<std::filesystem::path> &steam_roots,
                      const detection_io &io) {
  detection out;
  const auto check = [&](const std::filesystem::path &dir) {
    // UTF-8, as the screens and the CLI's JSON show it. string() converts to
    // the ANSI code page and throws for a folder outside it (a Steam library
    // with a Cyrillic name on a Western Windows).
    out.tried.push_back(path_utf8(dir));
    return io.exists(dir / std::string(bo3_exe));
  };
  if (saved && check(*saved)) {
    out.game_dir = *saved;
    return out;
  }
  for (const auto &root : steam_roots) {
    std::vector<std::filesystem::path> libraries{root};
    if (const auto vdf = io.read(root / "steamapps" / "libraryfolders.vdf")) {
      for (auto &library : steam_libraries(*vdf)) {
        if (std::ranges::find(libraries, library) == libraries.end()) {
          libraries.push_back(std::move(library));
        }
      }
    }
    for (const auto &library : libraries) {
      const auto manifest =
          io.read(library / "steamapps" /
                  ("appmanifest_" + std::string(bo3_app_id) + ".acf"));
      if (!manifest) {
        continue;
      }
      const auto installdir = appmanifest_installdir(*manifest);
      if (!installdir) {
        continue;
      }
      const auto dir =
          library / "steamapps" / "common" / from_utf8(*installdir);
      if (check(dir)) {
        out.game_dir = dir;
        return out;
      }
    }
  }
  return out;
}

void add_steam_root(std::vector<std::filesystem::path> &roots,
                    const std::filesystem::path &found) {
  const auto normal = found.lexically_normal();
  // same_path compares the wide text, so a root outside the ANSI code page
  // is compared too (string() throws for it).
  if (std::ranges::none_of(
          roots, [&](const auto &p) { return same_path(p, normal); })) {
    roots.push_back(normal);
  }
}

std::vector<std::filesystem::path> steam_roots_from_registry() {
  std::vector<std::filesystem::path> out;
  for (const auto &found :
       {registry_path(HKEY_CURRENT_USER, L"Software\\Valve\\Steam",
                      L"SteamPath"),
        registry_path(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
                      L"InstallPath")}) {
    if (found) {
      add_steam_root(out, *found);
    }
  }
  return out;
}

detection_io real_filesystem() {
  return {[](const std::filesystem::path &path) -> std::optional<std::string> {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
              return std::nullopt;
            }
            return std::string(std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>());
          },
          [](const std::filesystem::path &path) {
            std::error_code ec;
            return std::filesystem::is_regular_file(path, ec);
          }};
}
} // namespace xerces_core
