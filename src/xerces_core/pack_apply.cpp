#include "pack_apply.hpp"

#include <ctime>

#include "backup.hpp"
#include "diagnostics.hpp"

namespace xerces_core {
namespace {
// Backups are named by UTC time, as the other backups are (8.5).
std::string backup_stamp() {
  const std::time_t t = std::time(nullptr);
  std::tm utc{};
  gmtime_s(&utc, &t);
  char text[32];
  std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &utc);
  return text;
}
} // namespace

void apply_pack_to_profile(mode_profile &profile, const pack &value) {
  // The profile becomes the pack: its order, its exact versions (8.3).
  profile.mods = value.order;
  profile.pins.clear();
  for (const auto &mod : value.mods) {
    profile.pins[mod.id] = mod.ver.to_string();
  }
  profile.map =
      value.map ? std::optional<std::string>(value.map->id) : std::nullopt;
  profile.pack_name = value.name;
  profile.pack_id = value.id;
  // The pack's decisions go with it: conflicts it allows and per-mod
  // settings (pack_to_request does the same for a pack that is only checked).
  profile.allowed_conflicts = value.decisions.allow_conflicts;
  profile.settings.clear();
  for (const auto &mod : value.mods) {
    if (!mod.settings.empty()) {
      profile.settings[mod.id] = mod.settings;
    }
  }
}

std::optional<std::filesystem::path> backup_config(const launcher_paths &paths,
                                                   const std::string &why,
                                                   const event_log &log) {
  diagnostics diags;
  const auto made = create_backup(paths.home / "backups",
                                  {{"config", paths.config}}, backup_stamp(),
                                  {}, diags);
  if (made) {
    emit(log, log_level::info, "packs.backup", why);
  } else {
    emit(log, log_level::warn, "packs.backup_failed",
         diags.items().empty() ? why : diags.items().front().message);
  }
  return made;
}

std::optional<std::filesystem::path>
apply_pack(const launcher_paths &paths, launcher_config &config,
           const pack &value, const event_log &log, const std::string &mode) {
  const auto backup = backup_config(paths, "apply pack " + value.id, log);
  apply_pack_to_profile(profile_for(config, mode.empty() ? value.mode : mode),
                        value);
  return backup;
}
} // namespace xerces_core
