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

// Every file under a folder as (relative '/' path, contents), sorted.
std::vector<std::pair<std::string, std::string>>
read_files(const std::filesystem::path &folder);

// SHA-256 over the sorted (path, file hash) list, leaving out workshop.json
// as upstream's content hash does.
std::string
content_hash_of(const std::vector<std::pair<std::string, std::string>> &files);
} // namespace xerces_core
