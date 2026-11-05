// Faster catalog hashing and a hash cache that is kept (docs/DESIGN.md 12):
// CNG gives the portable code's digests, files hashed several at once give
// the same catalog, version 1 caches are migrated, keys ignore letter case,
// a home can start from another home's cache, and only folders a scan read
// in full lose entries.

#include "harness.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <xerces_core/catalog.hpp>
#include <xerces_core/compat.hpp>
#include <xerces_core/file_hash.hpp>
#include <xerces_core/launcher_home.hpp>
#include <xerces_core/launcher_state.hpp>
#include <xerces_core/sha256.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
fs::path fresh_dir(const std::string &name) {
  const fs::path dir = fs::temp_directory_path() / ("xerces_hashspeed_" + name);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

void write_bytes(const fs::path &path, const std::string &bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}

std::string random_bytes(std::size_t size, std::uint32_t seed) {
  std::mt19937 random(seed);
  std::string out(size, '\0');
  for (char &c : out) {
    c = static_cast<char>(random() & 0xFF);
  }
  return out;
}

std::string utf8(const fs::path &path) {
  const auto text = path.generic_u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

// A cache line as the cache writes it for a file as it is now.
std::string line_for(const fs::path &file, const std::string &hash,
                     const std::string &shown = {}) {
  return std::to_string(fs::file_size(file)) + "\t" +
         std::to_string(fs::last_write_time(file).time_since_epoch().count()) +
         "\t" + hash + "\t" + (shown.empty() ? utf8(file) : shown) + "\n";
}

std::string to_upper_ascii(std::string text) {
  for (char &c : text) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return text;
}

// Sets an environment variable for this process, and puts the old value
// back on scope exit.
struct scoped_env {
  std::wstring name;
  std::optional<std::wstring> old;
  scoped_env(const wchar_t *variable, const std::wstring &value)
      : name(variable) {
    wchar_t buffer[32768];
    const DWORD size = GetEnvironmentVariableW(variable, buffer, 32768);
    if (size > 0 && size < 32768) {
      old = std::wstring(buffer, size);
    }
    SetEnvironmentVariableW(variable, value.empty() ? nullptr : value.c_str());
  }
  ~scoped_env() {
    SetEnvironmentVariableW(name.c_str(), old ? old->c_str() : nullptr);
  }
};
} // namespace

// ------------------------------------------------------------------ digests

TEST_CASE("hash speed: the portable code gives the published digests") {
  CHECK_EQ(sha256::hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb924"
                                        "27ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(sha256::hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223"
                                           "b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(
      sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      std::string("248d6a61d20638b8e5c026930c3e6039"
                  "a33ce45964ff2167f6ecedd419db06c1"));
  CHECK_EQ(sha256::hex(std::string(1000000, 'a')),
           std::string("cdc76e5c9914fb9281a1c7e284d73e67"
                       "f1809a48a497200e046d39ccc7112cd0"));
}

TEST_CASE("hash speed: blocks hashed in place match byte-by-byte hashing") {
  // The portable update() hashes whole blocks where they lie; feeding the
  // same bytes one at a time takes the buffered path for every byte.
  for (const std::size_t size :
       {0u, 1u, 55u, 56u, 63u, 64u, 65u, 127u, 128u, 129u, 1000u, 4097u}) {
    const std::string bytes = random_bytes(size, std::uint32_t(size) + 7);
    sha256 one_by_one;
    for (const char c : bytes) {
      one_by_one.update(&c, 1);
    }
    CHECK_EQ(sha256::to_hex(one_by_one.finish()), sha256::hex(bytes));
  }
}

TEST_CASE("hash speed: CNG gives the portable code's digests") {
  // Every Windows Xerces supports has CNG's SHA-256.
  CHECK(cng_sha256_available());
  std::vector<std::size_t> sizes = {
      0,  1,   2,   3,   31,  32,  33,  55,  56,   57,   63,   64,
      65, 119, 120, 127, 128, 129, 191, 192, 4095, 4096, 4097, 65535};
  sizes.push_back((std::size_t{1} << 20) - 1);
  sizes.push_back(std::size_t{1} << 20);
  sizes.push_back((std::size_t{1} << 20) + 1);
  sizes.push_back((std::size_t{3} << 20) + 37);
  std::mt19937 splits(1234);
  for (const std::size_t size : sizes) {
    const std::string bytes = random_bytes(size, std::uint32_t(size) * 31 + 1);
    const std::string expected = sha256::hex(bytes);

    sha256_stream whole(sha256_engine::automatic);
    CHECK(whole.native());
    whole.update(bytes.data(), bytes.size());
    const auto digest = whole.finish();
    REQUIRE(digest.has_value());
    CHECK_EQ(sha256::to_hex(*digest), expected);

    // The same bytes in pieces of random sizes, through both engines.
    for (const sha256_engine engine :
         {sha256_engine::automatic, sha256_engine::portable}) {
      sha256_stream pieces(engine);
      CHECK_EQ(pieces.native(), engine == sha256_engine::automatic);
      std::size_t at = 0;
      while (at < bytes.size()) {
        const std::size_t take =
            std::min<std::size_t>(bytes.size() - at, splits() % 200 + 1);
        pieces.update(bytes.data() + at, take);
        at += take;
      }
      const auto split = pieces.finish();
      REQUIRE(split.has_value());
      CHECK_EQ(sha256::to_hex(*split), expected);
    }
  }
}

TEST_CASE("hash speed: files hash the same through CNG and portable code") {
  const fs::path dir = fresh_dir("files");
  for (const std::size_t size :
       {std::size_t{0}, std::size_t{1}, std::size_t{64},
        (std::size_t{1} << 20) - 1, std::size_t{1} << 20,
        (std::size_t{1} << 20) + 1, (std::size_t{2} << 20) + 13}) {
    const std::string bytes = random_bytes(size, std::uint32_t(size) + 99);
    const fs::path file = dir / ("f" + std::to_string(size) + ".bin");
    write_bytes(file, bytes);
    const std::string expected = sha256::hex(bytes);
    CHECK_EQ(sha256_file_of_size(file, size).value_or("none"), expected);
    CHECK_EQ(sha256_file_of_size(file, size, nullptr, sha256_engine::portable)
                 .value_or("none"),
             expected);
    // The wrong size (the file changed after it was measured) is no hash.
    CHECK(!sha256_file_of_size(file, size + 1).has_value());
    if (size > 0) {
      CHECK(!sha256_file_of_size(file, size - 1).has_value());
    }
  }
  // Cancelled before the first read: nothing, and nothing cached.
  const fs::path big = dir / "big.bin";
  write_bytes(big, random_bytes(std::size_t{1} << 21, 5));
  std::atomic<bool> cancel = true;
  CHECK(!sha256_file_of_size(big, fs::file_size(big), &cancel).has_value());
  hash_cache cache;
  CHECK(!cache.file_sha256(big, &cancel).has_value());
  CHECK_EQ(cache.size(), std::size_t{0});
  cancel = false;
  CHECK(cache.file_sha256(big, &cancel).has_value());
  fs::remove_all(dir);
}

TEST_CASE("hash speed: hashing several files at once gives the same catalog") {
  const fs::path dir = fresh_dir("parallel");
  const fs::path mods = dir / "mods";
  // Folders with files of very different sizes, so the largest-first order
  // finishes folders out of their sorted order.
  for (int i = 0; i < 12; ++i) {
    const std::string id = std::to_string(100 + i);
    write_bytes(mods / id / "zone" / "workshop.json",
                R"({"Title":"M)" + id + R"(","PublisherID":")" + id +
                    R"(","Type":"mod"})");
    write_bytes(mods / id / "zone" / "core_mod.ff",
                random_bytes(std::size_t(1 + (i * 7919) % 13) * 50000, i));
    write_bytes(mods / id / "scripts" / "a.gsc", "init() {}\n" + id);
  }
  const catalog_scan one = [&] {
    scan_options options;
    options.hash_threads = 1;
    return scan_mods_directory(mods, options);
  }();
  REQUIRE_EQ(one.catalog.size(), std::size_t{12});
  for (const unsigned threads : {2u, 4u, 6u, 16u}) {
    hash_cache cache;
    scan_options options;
    options.cache = &cache;
    options.hash_threads = threads;
    std::multiset<std::string> seen;
    std::size_t calls = 0;
    options.progress = [&](std::size_t index, std::size_t total,
                           const std::string &name) {
      CHECK_EQ(index, calls); // never two at once, counted in order
      CHECK_EQ(total, std::size_t{12});
      ++calls;
      seen.insert(name);
    };
    const catalog_scan many = scan_mods_directory(mods, options);
    CHECK(!many.cancelled);
    CHECK_EQ(seen.size(), std::size_t{12});
    CHECK_EQ(std::set<std::string>(seen.begin(), seen.end()).size(),
             std::size_t{12});
    REQUIRE_EQ(many.catalog.size(), one.catalog.size());
    for (std::size_t i = 0; i < one.catalog.size(); ++i) {
      CHECK_EQ(many.catalog[i].folder, one.catalog[i].folder);
      CHECK_EQ(many.catalog[i].content_hash, one.catalog[i].content_hash);
      CHECK(many.catalog[i].files == one.catalog[i].files);
      CHECK_EQ(many.catalog[i].info.id, one.catalog[i].info.id);
    }
    CHECK_EQ(cache.misses(), std::size_t{36});
  }
  fs::remove_all(dir);
}

// ------------------------------------------------------------- the cache

TEST_CASE("hash speed: a version 1 cache is migrated, not thrown away") {
  const fs::path dir = fresh_dir("migrate");
  const fs::path good = dir / "Workshop" / "core_mod.ff";
  const fs::path failed = dir / "Workshop" / "held.ff";
  const fs::path empty = dir / "Workshop" / "empty.ff";
  write_bytes(good, std::string(4096, 'g'));
  write_bytes(failed, std::string(4096, 'h'));
  write_bytes(empty, "");
  const fs::path file = dir / "cache" / "hashes.txt";
  // As version 1 builds wrote it: the real hash of one file, the hash of no
  // bytes for one they could not open, and an empty file's real hash.
  const std::string body = line_for(good, sha256::hex(std::string(4096, 'g'))) +
                           line_for(failed, sha256::hex("")) +
                           line_for(empty, sha256::hex(""));
  write_bytes(file, std::string(hash_cache_header_v1) + "\n" + body);

  hash_cache cache;
  REQUIRE(cache.load(file));
  CHECK_EQ(cache.loaded_version(), 1);
  CHECK_EQ(cache.size(), std::size_t{2}); // the failed read is dropped
  CHECK_EQ(cache.file_sha256(good).value_or(""),
           sha256::hex(std::string(4096, 'g')));
  CHECK_EQ(cache.file_sha256(empty).value_or("none"), sha256::hex(""));
  CHECK_EQ(cache.hits(), std::size_t{2});
  CHECK_EQ(cache.misses(), std::size_t{0});
  CHECK_EQ(cache.file_sha256(failed).value_or(""),
           sha256::hex(std::string(4096, 'h')));
  CHECK_EQ(cache.misses(), std::size_t{1});

  // Saved, it is version 2 with every entry.
  REQUIRE(cache.save(file));
  const std::string saved = xerces_tests::read_file(file);
  CHECK(saved.starts_with(std::string(hash_cache_header) + "\n"));
  hash_cache again;
  REQUIRE(again.load(file));
  CHECK_EQ(again.loaded_version(), 2);
  CHECK_EQ(again.size(), std::size_t{3});
  fs::remove_all(dir);
}

TEST_CASE("hash speed: a real version 1 cache migrates entry for entry") {
  // XERCES_TEST_V1_CACHE names a version 1 cache written by an older build
  // (on the test PC C:\Users\Ginld\XercesHome\cache\hashes.txt). It is copied
  // first and never changed; without it this test checks nothing.
  wchar_t named[32768];
  const DWORD length =
      GetEnvironmentVariableW(L"XERCES_TEST_V1_CACHE", named, 32768);
  if (length == 0 || length >= 32768) {
    return;
  }
  const fs::path original(std::wstring(named, length));
  REQUIRE(fs::is_regular_file(original));
  const auto original_time = fs::last_write_time(original);
  const fs::path dir = fresh_dir("real_v1");
  const fs::path copy = dir / "hashes.txt";
  fs::copy_file(original, copy);
  const std::string before = xerces_tests::read_file(copy);
  REQUIRE(before.starts_with(std::string(hash_cache_header_v1) + "\n"));
  std::size_t lines = 0;
  for (const char c : before) {
    lines += c == '\n' ? 1 : 0;
  }
  hash_cache cache;
  REQUIRE(cache.load(copy));
  CHECK_EQ(cache.loaded_version(), 1);
  CHECK_EQ(cache.size(), lines - 1); // nothing in it was a failed read
  REQUIRE(cache.save(copy));
  // The same lines under the version 2 header (sorted by the case-folded
  // path now, so compared as sets).
  const std::string after = xerces_tests::read_file(copy);
  CHECK(after.starts_with(std::string(hash_cache_header) + "\n"));
  const auto body_lines = [](const std::string &text) {
    std::multiset<std::string> out;
    std::size_t at = text.find('\n') + 1;
    while (at < text.size()) {
      const std::size_t end = text.find('\n', at);
      out.insert(text.substr(at, end - at));
      at = end == std::string::npos ? text.size() : end + 1;
    }
    return out;
  };
  CHECK(body_lines(after) == body_lines(before));
  CHECK(fs::last_write_time(original) == original_time);
  fs::remove_all(dir);
}

TEST_CASE("hash speed: cache keys ignore letter case, files keep theirs") {
  const fs::path dir = fresh_dir("case");
  const fs::path ff = dir / "Mods" / "Core_Mod.ff";
  write_bytes(ff, "case");
  hash_cache cache;
  REQUIRE(cache.file_sha256(ff).has_value());
  CHECK_EQ(cache.misses(), std::size_t{1});
  // The same file, spelled in capitals (a saved game folder typed by hand
  // against one found in the registry): a hit, not a second entry.
  const std::string upper = to_upper_ascii(utf8(ff));
  const std::u8string upper8(reinterpret_cast<const char8_t *>(upper.data()),
                             upper.size());
  CHECK_EQ(cache.file_sha256(fs::path(upper8)).value_or(""),
           sha256::hex("case"));
  CHECK_EQ(cache.hits(), std::size_t{1});
  CHECK_EQ(cache.size(), std::size_t{1});
  CHECK_EQ(hash_cache_key(fs::path(upper8)), hash_cache_key(ff));

  // Written as first seen, so builds that compare exactly still hit it.
  const fs::path file = dir / "cache" / "hashes.txt";
  REQUIRE(cache.save(file));
  CHECK(xerces_tests::read_file(file).find(utf8(ff)) != std::string::npos);
  fs::remove_all(dir);
}

TEST_CASE("hash speed: a seed cache is read, never written") {
  const fs::path dir = fresh_dir("seed");
  const fs::path ff = dir / "Workshop" / "111" / "core_mod.ff";
  const fs::path other = dir / "Workshop" / "222" / "core_mod.ff";
  write_bytes(ff, std::string(10000, 's'));
  write_bytes(other, "other");
  // The seed's hash for ff is a marker no read would give: a hit returns
  // it, a read would not.
  const std::string marker(64, 'b');
  const fs::path seed_home = dir / "other_home";
  const fs::path seed = seed_home / "cache" / "hashes.txt";
  write_bytes(seed,
              std::string(hash_cache_header) + "\n" + line_for(ff, marker) +
                  // the wrong size: stale, so not used
                  "5\t1\t" + std::string(64, 'c') + "\t" + utf8(other) + "\n");
  const std::string seed_before = xerces_tests::read_file(seed);
  const auto seed_time = fs::last_write_time(seed);

  hash_cache cache;
  CHECK(!cache.load(dir / "home" / "cache" / "hashes.txt"));
  REQUIRE(cache.load_seed(seed));
  CHECK_EQ(cache.seed_size(), std::size_t{2});
  CHECK_EQ(cache.size(), std::size_t{0});
  CHECK_EQ(cache.file_sha256(ff).value_or(""), marker);
  CHECK_EQ(cache.file_sha256(other).value_or(""), sha256::hex("other"));
  const hash_cache::stats stats = cache.get_stats();
  CHECK_EQ(stats.seed_hits, std::size_t{1});
  CHECK_EQ(stats.hits, std::size_t{1});
  CHECK_EQ(stats.misses, std::size_t{1});
  CHECK_EQ(stats.bytes_hashed, std::uint64_t{5});

  // The home's own cache takes both; the seed is as it was.
  const fs::path own = dir / "home" / "cache" / "hashes.txt";
  REQUIRE(cache.save(own));
  hash_cache reloaded;
  REQUIRE(reloaded.load(own));
  CHECK_EQ(reloaded.size(), std::size_t{2});
  CHECK_EQ(reloaded.file_sha256(ff).value_or(""), marker);
  CHECK(xerces_tests::read_file(seed) == seed_before);
  CHECK(fs::last_write_time(seed) == seed_time);

  // XERCES_HASH_CACHE names a home, its cache folder or a file; the home's
  // own file is never its own seed.
  {
    const scoped_env unset(L"XERCES_HASH_CACHE", L"");
    CHECK(!hash_cache_seed_file(own).has_value());
  }
  for (const fs::path &named :
       {seed_home, seed_home / "cache", fs::path(seed)}) {
    const scoped_env set(L"XERCES_HASH_CACHE", named.wstring());
    const auto found = hash_cache_seed_file(own);
    REQUIRE(found.has_value());
    CHECK(fs::equivalent(*found, seed));
  }
  {
    const scoped_env set(L"XERCES_HASH_CACHE", own.wstring());
    CHECK(!hash_cache_seed_file(own).has_value());
  }
  {
    const scoped_env set(L"XERCES_HASH_CACHE", (dir / "nothing").wstring());
    CHECK(!hash_cache_seed_file(own).has_value());
  }
  // open_hash_cache loads both.
  {
    const scoped_env set(L"XERCES_HASH_CACHE", seed_home.wstring());
    const fs::path fresh_own = dir / "new_home" / "cache" / "hashes.txt";
    hash_cache opened_cache;
    const hash_cache_opened opened = open_hash_cache(opened_cache, fresh_own);
    CHECK_EQ(opened.version, 0);
    REQUIRE(opened.seed.has_value());
    CHECK_EQ(opened.seed_entries, std::size_t{2});
    CHECK_EQ(opened_cache.file_sha256(ff).value_or(""), marker);
  }
  fs::remove_all(dir);
}

TEST_CASE("hash speed: only folders a scan read in full are pruned") {
  const fs::path dir = fresh_dir("prune");
  const fs::path mods = dir / "game" / "mods";
  const fs::path mods2 = dir / "game" / "mods2"; // shares the prefix "mods"
  const fs::path workshop = dir / "workshop" / "311210";
  const fs::path exe = dir / "game" / "BlackOps3.exe";
  write_bytes(mods / "a" / "kept.ff", "kept");
  write_bytes(mods / "b" / "gone.ff", "gone");
  write_bytes(mods2 / "c" / "x.ff", "x");
  write_bytes(workshop / "1" / "y.ff", "y");
  write_bytes(exe, "MZ");
  hash_cache cache;
  for (const fs::path &file :
       {mods / "a" / "kept.ff", mods / "b" / "gone.ff", mods2 / "c" / "x.ff",
        workshop / "1" / "y.ff", exe}) {
    REQUIRE(cache.file_sha256(file).has_value());
  }
  const fs::path file = dir / "cache" / "hashes.txt";
  REQUIRE(cache.save(file));
  fs::remove_all(mods / "b");

  // A scan that read mods\ in full and nothing else: only mods\b's entry,
  // which it did not see, goes. mods2\, the Workshop folder (not read this
  // time: its drive may be unplugged) and the exe stay.
  hash_cache rescan;
  REQUIRE(rescan.load(file));
  scan_options options;
  options.cache = &rescan;
  scan_mods_directory(mods, options);
  REQUIRE(rescan.save(file, {mods}));
  hash_cache after;
  REQUIRE(after.load(file));
  CHECK_EQ(after.size(), std::size_t{4});
  const std::string text = xerces_tests::read_file(file);
  CHECK(text.find("/b/gone.ff") == std::string::npos);
  CHECK(text.find("/a/kept.ff") != std::string::npos);
  CHECK(text.find("/c/x.ff") != std::string::npos);
  CHECK(text.find("/1/y.ff") != std::string::npos);
  CHECK(text.find("BlackOps3.exe") != std::string::npos);

  // No folders read in full (a cancelled scan): nothing pruned at all.
  hash_cache untouched;
  REQUIRE(untouched.load(file));
  REQUIRE(untouched.save(file, {}));
  hash_cache same;
  REQUIRE(same.load(file));
  CHECK_EQ(same.size(), std::size_t{4});
  fs::remove_all(dir);
}

TEST_CASE("hash speed: no game found leaves the cache as it was") {
  const fs::path dir = fresh_dir("no_game");
  const fs::path ff = dir / "elsewhere" / "core_mod.ff";
  write_bytes(ff, "elsewhere");
  const fs::path file = dir / "cache" / "hashes.txt";
  {
    hash_cache cache;
    REQUIRE(cache.file_sha256(ff).has_value());
    REQUIRE(cache.save(file));
  }
  const game_state nothing; // detection found no game
  const catalog_state scanned = scan_catalog(nothing, file);
  CHECK(scanned.items.empty());
  CHECK(scanned.scanned_roots.empty());
  hash_cache after;
  REQUIRE(after.load(file));
  CHECK_EQ(after.size(), std::size_t{1});
  fs::remove_all(dir);
}

TEST_CASE("hash speed: BlackOps3.exe is hashed through the cache") {
  const fs::path dir = fresh_dir("exe");
  std::string pe(0x40 + 24 + 96 + 5000, '\0');
  pe[0] = 'M';
  pe[1] = 'Z';
  const std::uint32_t nt = 0x40, signature = 0x00004550;
  std::memcpy(pe.data() + 0x3C, &nt, 4);
  std::memcpy(pe.data() + 0x40, &signature, 4);
  const fs::path exe = dir / "BlackOps3.exe";
  write_bytes(exe, pe);
  hash_cache cache;
  std::string error;
  const auto first = read_exe_facts(exe, &error, &cache);
  REQUIRE(first.has_value());
  CHECK_EQ(first->sha256, sha256::hex(pe));
  CHECK_EQ(cache.misses(), std::size_t{1});
  const auto second = read_exe_facts(exe, &error, &cache);
  REQUIRE(second.has_value());
  CHECK_EQ(second->sha256, first->sha256);
  CHECK_EQ(cache.hits(), std::size_t{1});
  CHECK_EQ(cache.misses(), std::size_t{1});
  fs::remove_all(dir);
}
