#include <std_include.hpp>

// xerces_zm_hip_spread: the Zombies setting "No Hip-Fire Spread"
// (xerces_zm_noHipSpread, docs/DESIGN.md 7.11). The game's own spread override
// (SetSpreadOverride) only holds spread at a weapon's smallest hip spread, so
// instead every loaded weapon's hip spread (the smallest and largest when
// standing, crouched, prone and sliding, and what firing, turning, moving and
// sprinting add) is set to 0 while this PC hosts a Zombies match with the
// setting on. The host's game runs the match's server in the same process, so
// the shots of everyone in the match go where the crosshair is.
//
// Never in multiplayer or campaign, and never when this PC only joined: in any
// other level (and in a hosted Zombies match with the setting off) every
// changed weapon that is still loaded gets its spread back, before a shot can
// be fired there. Weapons are only touched inside a level, in the game's own
// frame: at other times the game may be loading or unloading them.

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include "runtime.hpp"

namespace xerces_zm_hip_spread {
namespace {
using game::weapon::WeaponDef;
namespace xasset = game::db::xasset;

constexpr int FIELDS = 12;
using values = std::array<float, FIELDS>;

std::array<float *, FIELDS> fields_of(WeaponDef *def) {
  return {&def->fHipSpreadStandMin, &def->fHipSpreadDuckedMin,
          &def->fHipSpreadProneMin, &def->fHipSpreadSlideMin,
          &def->hipSpreadStandMax,  &def->hipSpreadDuckedMax,
          &def->hipSpreadProneMax,  &def->hipSpreadSlideMax,
          &def->fHipSpreadFireAdd,  &def->fHipSpreadTurnAdd,
          &def->fHipSpreadMoveAdd,  &def->fHipSpreadSprintAdd};
}

values read(WeaponDef *def) {
  const auto fields = fields_of(def);
  values out{};
  for (int i = 0; i < FIELDS; ++i) {
    out[i] = *fields[i];
  }
  return out;
}

void write(WeaponDef *def, const values &in) {
  const auto fields = fields_of(def);
  for (int i = 0; i < FIELDS; ++i) {
    *fields[i] = in[i];
  }
}

bool all_zero(const values &v) {
  return std::ranges::all_of(v, [](float f) { return f == 0.0f; });
}

// What each changed weapon had, by the name of the variant it was found
// under. Kept until it is put back, across matches: a weapon's zone may stay
// loaded from one match into the next level.
std::unordered_map<std::string, values> saved;
// One scan's own: a weapon is shared by several variants
std::unordered_set<WeaponDef *> scanned;
int scan_count = 0;
uint64_t next_scan_ms = 0;

bool in_level() {
  return game::com::Com_IsInGame() && !game::com::Com_IsRunningUILevel() &&
         game::cg::clientUIActives->actives[0].connectionState ==
             game::connstate_t::ACTIVE;
}

bool hosting_zombies() {
  const auto running = game::get_dvar_int("sv_running");
  return game::com::Com_SessionMode_IsMode(game::eModes::ZOMBIES) && running &&
         *running != 0;
}

// Every weapon now loaded that still has its spread: noted and set to 0
void apply() {
  scanned.clear();
  scan_count = 0;
  xasset::DB_EnumXAssets(
      xasset::XAssetType::WEAPON,
      [](xasset::XAssetHeader header, void *) {
        const auto *variant = header.weapon;
        if (!variant || !variant->weapDef || !variant->szInternalName ||
            !scanned.insert(variant->weapDef).second) {
          return;
        }
        const values now = read(variant->weapDef);
        if (all_zero(now)) {
          return; // done already, or a weapon with none
        }
        saved[variant->szInternalName] = now;
        write(variant->weapDef, values{});
        ++scan_count;
      },
      nullptr, true);
  if (scan_count > 0) {
    xerces_runtime::log_info("xerces.hip_spread.on",
                             "No hip-fire spread on " +
                                 std::to_string(scan_count) + " weapons");
  }
}

// Every changed weapon that is still loaded, and still at 0, gets its spread
// back; one whose zone was loaded again has it already
void restore() {
  scanned.clear();
  scan_count = 0;
  xasset::DB_EnumXAssets(
      xasset::XAssetType::WEAPON,
      [](xasset::XAssetHeader header, void *) {
        const auto *variant = header.weapon;
        if (!variant || !variant->weapDef || !variant->szInternalName) {
          return;
        }
        const auto found = saved.find(variant->szInternalName);
        if (found == saved.end() ||
            !scanned.insert(variant->weapDef).second ||
            !all_zero(read(variant->weapDef))) {
          return;
        }
        write(variant->weapDef, found->second);
        ++scan_count;
      },
      nullptr, true);
  xerces_runtime::log_info("xerces.hip_spread.off",
                           "Hip-fire spread back on " +
                               std::to_string(scan_count) + " weapons (" +
                               std::to_string(saved.size()) + " were changed)");
  saved.clear();
}

void frame() {
  if (!in_level()) {
    next_scan_ms = 0; // the first frame of the next level scans at once
    return;
  }
  const uint64_t now = GetTickCount64();
  if (now < next_scan_ms) {
    return;
  }
  const auto setting = game::get_dvar_int("xerces_zm_noHipSpread");
  if (setting && *setting != 0 && hosting_zombies()) {
    // a weapon loaded later in the match is caught on a later scan
    next_scan_ms = now + 5000;
    apply();
  } else {
    next_scan_ms = now + 1000;
    if (!saved.empty()) {
      restore();
    }
  }
}
} // namespace

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_zm_hip_spread");

public:
  void post_unpack() override {
    if (!game::is_client() || !game::is_new_client()) {
      return;
    }
    scheduler::loop(frame, scheduler::main);
  }
};
} // namespace xerces_zm_hip_spread

REGISTER_COMPONENT(xerces_zm_hip_spread::component)
