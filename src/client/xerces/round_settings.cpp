#include <std_include.hpp>

// xerces_round_settings: the Zombies round settings in the XERCES SETTINGS
// menu (docs/DESIGN.md 7.9). They are saved with the player's other settings
// and are all off by default. The host's game reads them when a Zombies match
// starts (data/custom_scripts/zm/xerces_rounds.gsc), so everyone in the match
// gets the host's settings; data/ui_scripts/xerces_settings is the menu.

#include <game/utils.hpp>
#include <loader/component_loader.hpp>

namespace xerces_round_settings {
namespace {
const game::DvarFlags saved{.archive = 1};

void on_off(const char *name, const char *description) {
  game::register_dvar_bool(name, false, saved, description);
}
} // namespace

class component final : public client_component {
  DEFINE_COMPONENT_NAME("xerces_round_settings");

public:
  void post_unpack() override {
    on_off("xerces_zm_maxAmmo",
           "Zombies, when you host: everyone gets a Max Ammo at the start of "
           "each round.");
    on_off("xerces_zm_autoPap",
           "Zombies, when you host: guns from wall buys and the mystery box "
           "come already Pack-a-Punched.");
    game::register_dvar_int(
        "xerces_zm_startPoints", 0, 0, 1000000, saved,
        "Zombies, when you host: the points each player starts the match "
        "with. 0 keeps the map's own.");
    on_off("xerces_zm_noPerkLimit",
           "Zombies, when you host: players can buy every perk instead of "
           "stopping at four.");
    on_off("xerces_zm_boxStays",
           "Zombies, when you host: the mystery box never moves.");
    on_off("xerces_zm_powerOn",
           "Zombies, when you host: the power is on when the match starts.");
    on_off("xerces_zm_doorsOpen",
           "Zombies, when you host: every door and debris pile opens when the "
           "match starts.");
    on_off("xerces_zm_roundHud",
           "Zombies, when you host: shows the zombies left in the round and "
           "the round's time.");
    on_off("xerces_zm_wheel",
           "Zombies, when you host: a Pack-a-Punch wheel in the pause menu: "
           "2,500 points (10 during a Fire Sale), once a round, on a "
           "Pack-a-Punched gun, for one of the five ammo mods or nothing.");
    on_off("xerces_zm_coinFlip",
           "Zombies, when you host: a coin flip in the pause menu: bet points "
           "on a 50/50; a win pays the bet times 2 at round 1, 0.1 more each "
           "round, up to 5.");
  }
};
} // namespace xerces_round_settings

REGISTER_COMPONENT(xerces_round_settings::component)
