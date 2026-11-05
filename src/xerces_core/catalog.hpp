#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
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
};

catalog_scan scan_mods_directory(const std::filesystem::path &directory);

// One mod folder: its xerces.mod.json, or a synthesized legacy manifest.
installed_content load_mod_folder(const std::filesystem::path &folder,
                                  const std::set<std::string> &taken_ids,
                                  diagnostics &diags);

// Every file under a folder as (relative '/' path, contents), sorted. This
// holds every byte in memory: scans use hash_files, and only the conflict
// index, which needs the contents, reads loose files this way.
std::vector<std::pair<std::string, std::string>>
read_files(const std::filesystem::path &folder);

// Every regular file under a folder as a relative '/' path, sorted.
std::vector<std::string> list_files(const std::filesystem::path &folder);

// Lower-case hex SHA-256 of one file, read in 1 MiB pieces so multi-gigabyte
// fastfiles never sit in memory. A file that cannot be opened hashes as
// empty, the way read_files reads it.
std::string file_sha256_hex(const std::filesystem::path &path);

// Every file under a folder as (relative '/' path, SHA-256 hex), sorted.
std::vector<std::pair<std::string, std::string>>
hash_files(const std::filesystem::path &folder);

// SHA-256 over the sorted (path, file hash) list, leaving out workshop.json
// as upstream's content hash does.
std::string content_hash_of_digests(
    const std::vector<std::pair<std::string, std::string>> &digests);

// The same hash from (path, contents) pairs.
std::string
content_hash_of(const std::vector<std::pair<std::string, std::string>> &files);
} // namespace xerces_core
