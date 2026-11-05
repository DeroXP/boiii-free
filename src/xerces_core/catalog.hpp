#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
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
  // The folder itself could be listed. False with a catalog.read error: then
  // nothing in it was seen, so nothing in it may be taken as gone.
  bool listed = false;
};

// Remembers each file's SHA-256 by its path, size and last write time, so a
// rescan reads only files that changed. The Workshop folder on the test PC
// holds 59-63 GB, which takes minutes to hash from scratch. Stored as text
// under <home>\cache (DESIGN.md 3.5): a header line, then one line per file,
// "<size>\t<write time>\t<sha256>\t<path>". Safe to share between threads:
// a scan hashes several files at once through one cache.
//
// Paths are compared as Windows compares them, ASCII letters in any case
// ("C:/Program Files" and "c:/program files" are one file), and written back
// as they were first seen, so older builds still read the file.
//
// A line that does not hold a size, a time, 64 lower-case hex digits and a
// path of valid UTF-8 without control characters is skipped: a file cut
// short (a crash after a save, two programs saving at once) can end inside a
// character or in zeros, and no such line names a real file.
//
// Version 1 was written by builds that stored the hash of whatever they could
// read, so a file that could not be opened kept SHA-256 of nothing until it
// changed. Loading it migrates it: every entry is read as version 2 reads
// its own, which drops any entry giving the empty-input hash for a file with
// bytes in it, and the next save writes version 2. What stays possible is an
// entry for a file whose read failed partway (a disk error mid-file), which
// no check can tell from a good one; on the test PC every entry of its one
// version 1 cache matched a full re-read (2026-11-05).
inline constexpr std::string_view hash_cache_header = "xerces.hashcache/2";
inline constexpr std::string_view hash_cache_header_v1 = "xerces.hashcache/1";

class hash_cache {
public:
  // A missing or unreadable file leaves the cache empty; it is only a cache.
  // True when a version 1 or 2 file was read.
  bool load(const std::filesystem::path &file);
  // Another cache to fall back on, read only: a file the own cache does not
  // know (with the same size and write time) takes its hash from here, and
  // only the own cache is ever saved. A new home seeds itself this way from
  // XERCES_HASH_CACHE or another home's cache (launcher_state.hpp).
  bool load_seed(const std::filesystem::path &file);

  // Writes every entry to file through a temporary file.
  bool save(const std::filesystem::path &file) const;
  // The same, leaving out entries that were neither looked up nor added
  // since load and whose file lies under one of pruned_roots: the folders a
  // scan read in full, so a file there that it did not see is gone. Entries
  // anywhere else (a folder this scan did not read, BlackOps3.exe) stay.
  bool save(const std::filesystem::path &file,
            const std::vector<std::filesystem::path> &pruned_roots) const;

  struct stats {
    std::size_t hits = 0, misses = 0;
    std::size_t seed_hits = 0; // hits found in the seed
    // Bytes of the files read in full and hashed: a read that failed or was
    // cancelled counts as a miss, never here.
    std::uint64_t bytes_hashed = 0;
    // Time spent hashing, summed over files (several at once count each).
    double hash_seconds = 0;
    // The file that took longest to hash.
    std::string slowest_file;
    std::uint64_t slowest_bytes = 0;
    double slowest_seconds = 0;
  };

  // The file's hash: cached if its size and write time are unchanged,
  // otherwise read and remembered. Nothing when the file could not be read in
  // full (or cancel was set while reading it); then nothing is remembered for
  // it either, so the next scan tries again instead of reusing a wrong hash.
  // The lookup counts in the cache's stats and, when given, in sink as well
  // (under the cache's lock, so one sink can serve several threads hashing
  // through this cache; read it once they are done). A caller that shares
  // the cache with other work (the launcher's scan and its pre-flight) gets
  // its own numbers this way.
  std::optional<std::string>
  file_sha256(const std::filesystem::path &path,
              const std::atomic<bool> *cancel = nullptr, stats *sink = nullptr);

  std::size_t hits() const;
  std::size_t misses() const;
  std::size_t size() const;
  std::uint64_t bytes_hashed() const;
  std::size_t seed_size() const;
  // 0: nothing loaded, 1: a version 1 file was migrated, 2: current.
  int loaded_version() const;

  // Every lookup since this cache was made, by whoever made it.
  stats get_stats() const;

private:
  struct entry {
    std::uint64_t size = 0;
    std::int64_t write_time = 0;
    std::string sha256;
    std::string path; // as written in the file (the key is case-folded)
    bool used = false;
  };
  using entry_map = std::map<std::string, entry>;
  static int read_file(const std::filesystem::path &file, entry_map &into);
  bool write_file(const std::filesystem::path &file,
                  const std::vector<std::string> &pruned_prefixes) const;

  mutable std::mutex mutex_;
  entry_map entries_;
  entry_map seed_;
  int loaded_version_ = 0;
  stats stats_;
};

// The cache's key for a path: its generic form ('/' separators), ASCII
// letters lower-cased.
std::string hash_cache_key(const std::filesystem::path &path);

struct scan_options {
  hash_cache *cache = nullptr; // optional
  // With a cache: this scan's lookups are counted here too
  // (hash_cache::file_sha256's sink).
  hash_cache::stats *stats = nullptr;
  // Called each time every file of a mod folder has been hashed, with the
  // number of folders finished before it, the folder count and its name.
  // Folders finish in no fixed order (the largest files are read first), and
  // the call comes from a hashing thread, never two at once.
  std::function<void(std::size_t, std::size_t, const std::string &)> progress;
  // Checked before each file and between 1 MiB reads; a cancelled scan
  // returns the folders it finished, in order up to the first one it did
  // not, and reports scan.cancelled.
  const std::atomic<bool> *cancel = nullptr;
  // Where the folders come from, which decides where the client looks for a
  // mod's or map's asset pool file (asset_limits.hpp).
  content_layout layout = content_layout::game_folder;
  // Files hashed at once; 0 is default_hash_threads(directory) (file_hash.hpp).
  unsigned hash_threads = 0;
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

// Every file under a folder as (relative '/' path, SHA-256 hex), sorted, one
// file at a time. A file that could not be read has an empty hash and, if
// unreadable is given, its relative path is added there.
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
