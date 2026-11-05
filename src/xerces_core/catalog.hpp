#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diagnostics.hpp"
#include "planner.hpp"

namespace xerces_core {
// A folder of mods on disk (a game's mods/ or usermaps/ folder, a Workshop
// content folder, or test fixtures), read in sorted order (6.8).
struct catalog_scan {
  std::vector<installed_content> catalog;
  // id -> folder. With two versions of one id this is the folder scanned last;
  // installed_content::path is the folder of each version.
  std::map<std::string, std::filesystem::path> roots;
  diagnostics diags;
  bool cancelled = false; // scan_options::cancel stopped it early
};

// Remembers each file's SHA-256 by its path, size and last write time, so a
// rescan reads only files that changed. The Workshop folder on the test PC
// holds 59 GB, which takes minutes to hash from scratch. Stored as text under
// <home>\cache (DESIGN.md 3.5): a header line, then one line per file,
// "<size>\t<write time>\t<sha256>\t<path>". Not thread-safe: one scan at a
// time owns it.
//
// Version 1 was written by builds that stored the hash of whatever they could
// read, so a file held open by another program kept SHA-256 of nothing (or
// of its first part) until it changed. Those caches are not trusted: loading
// one leaves the cache empty, and the next scan hashes everything once.
inline constexpr std::string_view hash_cache_header = "xerces.hashcache/2";

class hash_cache {
public:
  // A missing or unreadable file leaves the cache empty; it is only a cache.
  bool load(const std::filesystem::path &file);
  // Writes the entries, all of them or only those looked up or added since
  // load (prune), to file through a temporary file.
  bool save(const std::filesystem::path &file, bool prune) const;

  // The file's hash: cached if its size and write time are unchanged,
  // otherwise read and remembered. Nothing when the file could not be read in
  // full; then nothing is remembered for it either, so the next scan tries
  // again instead of reusing a wrong hash.
  std::optional<std::string> file_sha256(const std::filesystem::path &path);

  std::size_t hits() const { return hits_; }
  std::size_t misses() const { return misses_; }
  std::size_t size() const { return entries_.size(); }
  std::uint64_t bytes_hashed() const { return bytes_hashed_; }

private:
  struct entry {
    std::uint64_t size = 0;
    std::int64_t write_time = 0;
    std::string sha256;
    bool used = false;
  };
  std::map<std::string, entry> entries_;
  std::size_t hits_ = 0;
  std::size_t misses_ = 0;
  std::uint64_t bytes_hashed_ = 0;
};

struct scan_options {
  hash_cache *cache = nullptr; // optional
  // Called before each mod folder with its index, the folder count and its
  // name; from the scanning thread.
  std::function<void(std::size_t, std::size_t, const std::string &)> progress;
  // Checked between folders; a cancelled scan returns what it has so far and
  // reports scan.cancelled.
  const std::atomic<bool> *cancel = nullptr;
  // Where the folders come from, which decides where the client looks for a
  // mod's or map's asset pool file (asset_limits.hpp).
  content_layout layout = content_layout::game_folder;
};

catalog_scan scan_mods_directory(const std::filesystem::path &directory,
                                 const scan_options &options = {});

// The layout of a folder of mods: Steam's Workshop content folder for the
// game (...\workshop\content\311210) holds Workshop items, anything else is
// taken as a game's mods\ or usermaps\ folder.
content_layout layout_of_directory(const std::filesystem::path &directory);

// One mod folder: its xerces.mod.json, or a synthesized legacy manifest. For
// a mod or map, also the asset pool file the client would read (pool_file,
// pool_limits); one larger than max_pool_file_bytes, or one that cannot be
// read, is marked pool_file_unread, with a warning.
installed_content
load_mod_folder(const std::filesystem::path &folder,
                const std::set<std::string> &taken_ids, diagnostics &diags,
                hash_cache *cache = nullptr,
                content_layout layout = content_layout::game_folder);

// Every file under a folder as (relative '/' path, contents), sorted. This
// holds every byte in memory: scans use hash_files, and only the conflict
// index, which needs the contents, reads loose files this way.
std::vector<std::pair<std::string, std::string>>
read_files(const std::filesystem::path &folder);

// Every regular file under a folder as a relative '/' path, sorted.
std::vector<std::string> list_files(const std::filesystem::path &folder);

// Lower-case hex SHA-256 of one file, read in 1 MiB pieces so multi-gigabyte
// fastfiles never sit in memory. Nothing when the file cannot be opened, a
// read fails, or the bytes read are not the file's size (it changed while
// being read): a hash of what happened to be read would look valid.
std::optional<std::string>
try_file_sha256_hex(const std::filesystem::path &path);

// The same, with an empty string (never a valid hash) when it fails, for
// callers that only compare the result with a known hash.
std::string file_sha256_hex(const std::filesystem::path &path);

// Every file under a folder as (relative '/' path, SHA-256 hex), sorted. A
// file that could not be read has an empty hash and, if unreadable is given,
// its relative path is added there.
std::vector<std::pair<std::string, std::string>>
hash_files(const std::filesystem::path &folder, hash_cache *cache = nullptr,
           std::vector<std::string> *unreadable = nullptr);

// SHA-256 over the sorted (path, file hash) list, leaving out workshop.json
// as upstream's content hash does.
std::string content_hash_of_digests(
    const std::vector<std::pair<std::string, std::string>> &digests);

// The same hash from (path, contents) pairs.
std::string
content_hash_of(const std::vector<std::pair<std::string, std::string>> &files);
} // namespace xerces_core
