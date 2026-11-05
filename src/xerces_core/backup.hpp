#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "diagnostics.hpp"

namespace xerces_core {
// Backup and rollback (DESIGN.md 8.5): before a change to packs, config or the
// plan, the folders that change are copied to <backups>/<timestamp>/<label>/.
struct backup_source {
  std::string label;            // "config", "players", ...
  std::filesystem::path folder; // copied recursively
};

struct backup_options {
  std::size_t keep = 10;
  std::uintmax_t size_cap = 64ull * 1024 * 1024; // per backup
};

// Files that look like credentials never go into a backup (10.4).
bool is_credential_file(const std::filesystem::path &relative);

std::optional<std::filesystem::path>
create_backup(const std::filesystem::path &backups_dir,
              const std::vector<backup_source> &sources,
              const std::string &timestamp, const backup_options &options,
              diagnostics &diags);

// Newest first.
std::vector<std::filesystem::path>
list_backups(const std::filesystem::path &backups_dir);

// Restoring backs up the current state first, then replaces each source
// folder with the backup's copy.
bool restore_backup(const std::filesystem::path &backup,
                    const std::filesystem::path &backups_dir,
                    const std::vector<backup_source> &sources,
                    const std::string &timestamp, const backup_options &options,
                    diagnostics &diags);
} // namespace xerces_core
