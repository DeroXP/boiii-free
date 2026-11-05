// Catalog scans (docs/DESIGN.md 5.3, 6.8): hashing installed mods without
// holding their files in memory.

#include "harness.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <xerces_core/catalog.hpp>
#include <xerces_core/diagnostics.hpp>
#include <xerces_core/sha256.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
fs::path fresh_dir(const std::string &name) {
  const fs::path dir = fs::temp_directory_path() / ("xerces_catalog_" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void write_bytes(const fs::path &path, const std::string &bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}
} // namespace

TEST_CASE("catalog: a file hashed in pieces matches its whole-file hash") {
  const fs::path dir = fresh_dir("pieces");
  // Two full 1 MiB pieces and a partial one, with bytes that differ per
  // piece, so a piece hashed twice or skipped changes the result.
  std::string big((std::size_t{1} << 21) + 13, '\0');
  for (std::size_t i = 0; i < big.size(); ++i) {
    big[i] = static_cast<char>((i * 131 + i / 4099) & 0xFF);
  }
  write_bytes(dir / "big.ff", big);
  write_bytes(dir / "exact.bin", std::string(std::size_t{1} << 20, 'x'));
  write_bytes(dir / "abc.txt", "abc");
  write_bytes(dir / "empty.txt", "");

  CHECK_EQ(file_sha256_hex(dir / "abc.txt"),
           std::string("ba7816bf8f01cfea414140de5dae2223"
                       "b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(file_sha256_hex(dir / "big.ff"), sha256::hex(big));
  CHECK_EQ(file_sha256_hex(dir / "exact.bin"),
           sha256::hex(std::string(std::size_t{1} << 20, 'x')));
  CHECK_EQ(file_sha256_hex(dir / "empty.txt"), sha256::hex(""));
  // Unreadable files hash as empty, the way read_files reads them.
  CHECK_EQ(file_sha256_hex(dir / "missing.bin"), sha256::hex(""));
  fs::remove_all(dir);
}

TEST_CASE("catalog: a scan's content hash does not change with streaming") {
  const fs::path dir = fresh_dir("content_hash");
  std::string big((std::size_t{1} << 20) + 7, '\0');
  for (std::size_t i = 0; i < big.size(); ++i) {
    big[i] = static_cast<char>(i % 251);
  }
  write_bytes(dir / "zone" / "core_mod.ff", big);
  write_bytes(dir / "zone" / "workshop.json",
              R"({"Title":"Fixture","PublisherID":"123","Type":"mod"})");
  write_bytes(dir / "scripts" / "zm" / "a.gsc", "init() {}\n");

  const auto files = read_files(dir);
  const auto digests = hash_files(dir);
  REQUIRE_EQ(digests.size(), files.size());
  std::vector<std::string> names;
  for (std::size_t i = 0; i < files.size(); ++i) {
    CHECK_EQ(digests[i].first, files[i].first);
    CHECK_EQ(digests[i].second, sha256::hex(files[i].second));
    names.push_back(files[i].first);
  }
  CHECK_EQ(content_hash_of_digests(digests), content_hash_of(files));
  CHECK(list_files(dir) == names);

  // load_mod_folder hashes by streaming and still reads workshop.json.
  diagnostics diags;
  const installed_content content =
      load_mod_folder(dir, std::set<std::string>{}, diags);
  CHECK_EQ(content.content_hash, content_hash_of(files));
  CHECK(content.files == names);
  CHECK(!content.info.id.empty());
  fs::remove_all(dir);
}
