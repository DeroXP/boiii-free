#pragma once

// Applying a pack to a mode's profile (DESIGN.md 8.3, 8.5), shared by the
// Packs screen and `xerces_cli packs apply`: <home>\config is backed up first,
// then the profile becomes the pack (its order, exact versions, name and id,
// the conflicts it allows and its per-mod settings). Writing launcher.json is
// the caller's: the launcher keeps the config in memory. Headless: no UI.

#include <filesystem>
#include <optional>
#include <string>

#include "event_log.hpp"
#include "launcher_config.hpp"
#include "launcher_home.hpp"
#include "pack.hpp"

namespace xerces_core {
// The profile becomes the pack. Its map, too: none when the pack has none.
void apply_pack_to_profile(mode_profile &profile, const pack &value);

// A backup of <home>\config in <home>\backups\<UTC time>\config (8.5). A
// failed backup is logged (packs.backup_failed), not fatal: the change is
// still the one the player asked for. Logs packs.backup with why.
std::optional<std::filesystem::path> backup_config(const launcher_paths &paths,
                                                   const std::string &why,
                                                   const event_log &log);

// backup_config ("apply pack <id>"), then apply_pack_to_profile on the
// profile of mode (the pack's own mode when empty). Returns the backup.
std::optional<std::filesystem::path> apply_pack(const launcher_paths &paths,
                                                launcher_config &config,
                                                const pack &value,
                                                const event_log &log,
                                                const std::string &mode = {});
} // namespace xerces_core
