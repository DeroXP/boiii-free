// Catalog scans (docs/DESIGN.md 5.3, 6.8): hashing installed mods without
// holding their files in memory.

#include "harness.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
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
  CHECK_EQ(try_file_sha256_hex(dir / "empty.txt").value_or("none"),
           sha256::hex(""));
  // A file that cannot be read has no hash: the hash of no bytes would pass
  // for a real one.
  CHECK(!try_file_sha256_hex(dir / "missing.bin").has_value());
  CHECK_EQ(file_sha256_hex(dir / "missing.bin"), std::string());
  fs::remove_all(dir);
}

TEST_CASE("catalog: a file held without shared reads is never cached") {
  const fs::path dir = fresh_dir("held");
  const fs::path mods = dir / "mods";
  const fs::path ff = mods / "111" / "zone" / "core_mod.ff";
  const std::string bytes(4096, 'z');
  write_bytes(ff, bytes);
  write_bytes(mods / "111" / "zone" / "workshop.json",
              R"({"Title":"One","PublisherID":"111","Type":"mod"})");
  const catalog_scan real = scan_mods_directory(mods);
  REQUIRE_EQ(real.catalog.size(), std::size_t{1});
  CHECK(!real.diags.has("catalog.file.unreadable"));

  // Held the way Steam can hold a Workshop file it is updating.
  const auto hold = [](const fs::path &path) {
    return CreateFileW(path.wstring().c_str(), GENERIC_READ, 0, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  };
  const fs::path cache_file = dir / "cache" / "hashes.txt";
  {
    HANDLE held = hold(ff);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    CHECK(!try_file_sha256_hex(ff).has_value());
    hash_cache cache;
    CHECK(!cache.file_sha256(ff).has_value());
    CHECK_EQ(cache.size(), std::size_t{0});
    scan_options options;
    options.cache = &cache;
    const catalog_scan scan = scan_mods_directory(mods, options);
    CHECK(scan.diags.has("catalog.file.unreadable"));
    REQUIRE_EQ(scan.catalog.size(), std::size_t{1});
    CHECK(scan.catalog[0].content_hash != real.catalog[0].content_hash);
    CHECK(cache.save(cache_file));
    CloseHandle(held);
  }

  // Once the file is free, the next run (with the saved cache) reads it.
  hash_cache reloaded;
  REQUIRE(reloaded.load(cache_file));
  CHECK_EQ(reloaded.size(), std::size_t{1}); // only workshop.json
  CHECK_EQ(reloaded.file_sha256(ff).value_or(""), sha256::hex(bytes));
  scan_options options;
  options.cache = &reloaded;
  const catalog_scan again = scan_mods_directory(mods, options);
  CHECK(!again.diags.has("catalog.file.unreadable"));
  REQUIRE_EQ(again.catalog.size(), std::size_t{1});
  CHECK_EQ(again.catalog[0].content_hash, real.catalog[0].content_hash);

  // A cached file that changes and is then held loses its entry rather than
  // keeping the old hash or getting a wrong one.
  const std::size_t before = reloaded.size();
  write_bytes(ff, bytes + "more");
  {
    HANDLE held = hold(ff);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    CHECK(!reloaded.file_sha256(ff).has_value());
    CloseHandle(held);
  }
  CHECK_EQ(reloaded.size(), before - 1);
  CHECK_EQ(reloaded.file_sha256(ff).value_or(""), sha256::hex(bytes + "more"));
  fs::remove_all(dir);
}

TEST_CASE("catalog: a failed read written down by an older build is dropped") {
  const fs::path dir = fresh_dir("poisoned");
  const fs::path ff = dir / "core_mod.ff";
  const fs::path empty = dir / "empty.ff";
  const std::string bytes(4096, 'p');
  write_bytes(ff, bytes);
  write_bytes(empty, "");
  // A cache line exactly as the old builds wrote it for a file they could
  // not open: the real size and time, and the hash of no bytes.
  const auto line = [](const fs::path &path, std::uint64_t size) {
    const auto key = path.generic_u8string();
    return std::to_string(size) + "\t" +
           std::to_string(fs::last_write_time(path).time_since_epoch().count()) +
           "\t" + sha256::hex("") + "\t" +
           std::string(reinterpret_cast<const char *>(key.data()), key.size()) +
           "\n";
  };
  const fs::path file = dir / "cache" / "hashes.txt";

  // A version 1 cache is migrated, with the same rule: the failed read is
  // dropped and the file is read again.
  write_bytes(file, "xerces.hashcache/1\n" + line(ff, bytes.size()));
  hash_cache old;
  CHECK(old.load(file));
  CHECK_EQ(old.loaded_version(), 1);
  CHECK_EQ(old.size(), std::size_t{0});
  CHECK_EQ(old.file_sha256(ff).value_or(""), sha256::hex(bytes));
  CHECK_EQ(old.misses(), std::size_t{1});

  // In a current cache, the empty-input hash for a file with bytes in it is
  // skipped; for an empty file it is that file's real hash and stays.
  write_bytes(file, std::string(hash_cache_header) + "\n" +
                        line(ff, bytes.size()) + line(empty, 0));
  hash_cache current;
  REQUIRE(current.load(file));
  CHECK_EQ(current.size(), std::size_t{1});
  CHECK_EQ(current.file_sha256(ff).value_or(""), sha256::hex(bytes));
  CHECK_EQ(current.misses(), std::size_t{1});
  CHECK_EQ(current.file_sha256(empty).value_or("none"), sha256::hex(""));
  CHECK_EQ(current.hits(), std::size_t{1});
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
