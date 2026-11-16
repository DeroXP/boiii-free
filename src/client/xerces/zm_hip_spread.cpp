#include <std_include.hpp>

// xerces_zm_hip_spread: the Zombies setting "No Hip-Fire Spread"
// (xerces_zm_noHipSpread, docs/DESIGN.md 7.11). The game's own spread override
// (SetSpreadOverride) only holds spread at one value for one player, so
// instead every loaded weapon's hip spread (the smallest and largest when
// standing, crouched, prone and sliding) is set to 0 while this PC hosts a
// Zombies match with the setting on. The game's spread is the smallest plus a
// share of the gap to the largest, then only multiplied (perks, dual wield),
// so it is 0 for every shot and the crosshair stays closed. The host's game
// runs the match's server in the same process, so the shots of everyone in
// the match go where the crosshair is.
//
// Where a weapon keeps those eight numbers was measured in the game's own code
// on 2026-11-15 (xerces_core::hip_spread_signatures::offsets). Until then this
// wrote to the WeaponDef fields of src/client/game/structs, which are 0x1D4
// bytes off there: it never changed any spread (the user: "it's almost like it
// doesn't work at all"), and while it was on it set 12 other numbers of each
// weapon to 0. Now the registry's pattern for that function (the whole of it,
// with every offset it reads) must match this game build, or the setting does
// nothing and says so once in the log.
//
// A player who joined draws their own crosshair, tracers and bullet holes from
// their own game's weapons, which kept their spread: the shots landed in the
// middle, but on their screen they still spread (the user, 2026-11-15: "it
// doesn't exactly work yet"). So in a Zombies match this PC joined, the
// host's setting counts here too: the host's game sends its settings to each
// player (the hidden text of data/custom_scripts/zm/xerces_rounds.gsc), and
// data/ui_scripts/xerces_settings puts this one in xerces_zm_matchNoHipSpread
// once a second. That dvar is 0 again outside a level, so it never carries
// into the next match.
//
// Never in multiplayer or campaign: in any other level (and in a Zombies match
// with the setting off) every changed weapon that is still loaded gets its
// spread back, before a shot can be fired there. Weapons are only touched
// inside a level, in the game's own frame: at other times the game may be
// loading or unloading them.

#include <component/scheduler.hpp>
#include <game/game.hpp>
#include <game/utils.hpp>
#include <loader/component_loader.hpp>

#include <xerces_core/sigruntime.hpp>

#include "runtime.hpp"
#include "signatures.hpp"

namespace xerces_zm_hip_spread {
namespace {
using game::weapon::WeaponDef;
namespace xasset = game::db::xasset;

namespace ids = xerces_core::hip_spread_signatures;

constexpr std::size_t FIELDS = ids::offsets.size();
using values = std::array<float, FIELDS>;

// Whether this game build keeps the spread where the offsets say (the
// registry's pattern matched); decided once, in post_unpack
bool layout_ok = false;

float *field(WeaponDef *def, const std::size_t i) {
  return reinterpret_cast<float *>(reinterpret_cast<std::uint8_t *>(def) +
                                   ids::offsets[i]);
}

values read(WeaponDef *def) {
  values out{};
  for (std::size_t i = 0; i < FIELDS; ++i) {
    out[i] = *field(def, i);
  }
  return out;
}

void write(WeaponDef *def, const values &in) {
  for (std::size_t i = 0; i < FIELDS; ++i) {
    *field(def, i) = in[i];
  }
}

bool all_zero(const values &v) {
  return std::ranges::all_of(v, [](float f) { return f == 0.0f; });
}

// What each changed weapon had. Kept by the weapon itself and the name of the
// variant it was found under: a mod that replaces a weapon by name leaves two
// weapons of that name loaded (both are enumerated, with their overrides), and
// keyed by the name alone one's spread went back into the other (review,
// 2026-11-13). The name guards against another weapon loaded at the same
// address since. Kept until it is put back, across matches: a weapon's zone
// may stay loaded from one match into the next level.
struct changed_weapon {
  std::string name;
  values had{};
};
std::unordered_map<WeaponDef *, changed_weapon> saved;
// One scan's own: a weapon is shared by several variants
std::unordered_set<WeaponDef *> scanned;
int scan_count = 0;
uint64_t next_scan_ms = 0;

bool in_level() {
  return game::com::Com_IsInGame() && !game::com::Com_IsRunningUILevel() &&
         game::cg::clientUIActives->actives[0].connectionState ==
             game::connstate_t::ACTIVE;
}

constexpr const char *MATCH_SETTING = "xerces_zm_matchNoHipSpread";

// sv_running and the settings are bool dvars: read as bools, not as the raw
// first four bytes of the value, of which only one is defined (review,
// 2026-11-13). A match this PC hosts takes its own setting, as it is now; one
// it joined takes the host's.
bool wanted() {
  if (!game::com::Com_SessionMode_IsMode(game::eModes::ZOMBIES)) {
    return false;
  }
  const char *setting =
      game::server_running() ? "xerces_zm_noHipSpread" : MATCH_SETTING;
  return game::get_dvar_bool(setting).value_or(false);
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
        // The weapon's layout is this build's: numbers that are no spread
        // (another game build, a mod's own weapon format) are left alone
        if (!std::ranges::all_of(now, [](float f) {
              return std::isfinite(f) && f >= 0.0f && f < 360.0f;
            })) {
          return;
        }
        saved[variant->weapDef] = {variant->szInternalName, now};
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
        const auto found = saved.find(variant->weapDef);
        if (found == saved.end() ||
            found->second.name != variant->szInternalName ||
            !scanned.insert(variant->weapDef).second ||
            !all_zero(read(variant->weapDef))) {
          return;
        }
        write(variant->weapDef, found->second.had);
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
  if (!layout_ok) {
    return;
  }
  if (!in_level()) {
    next_scan_ms = 0; // the first frame of the next level scans at once
    if (game::get_dvar_bool(MATCH_SETTING).value_or(false)) {
      game::set_dvar_bool(MATCH_SETTING, false, false);
    }
    return;
  }
  const uint64_t now = GetTickCount64();
  if (now < next_scan_ms) {
    return;
  }
  if (wanted()) {
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
    game::register_dvar_bool(
        MATCH_SETTING, false, game::DvarFlags{},
        "Zombies: whether the host of the match this PC is in has No Hip-Fire "
        "Spread on. Set by Xerces' in-match script; 0 outside a match.");
    layout_ok = xerces_signatures::feature_enabled("zm_hip_spread");
    if (!layout_ok) {
      xerces_runtime::log_warn(
          "xerces.hip_spread.unavailable",
          xerces_signatures::feature_message(
              "zm_hip_spread", "No Hip-Fire Spread",
              "The setting does nothing; every weapon keeps its spread."));
    }
    scheduler::loop(frame, scheduler::main);
  }
};
} // namespace xerces_zm_hip_spread

REGISTER_COMPONENT(xerces_zm_hip_spread::component)
