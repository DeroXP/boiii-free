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
           "Zombies, when you host: a coin flip in the pause menu, three times "
           "a round: bet points on a 50/50; a win pays the bet times 2 at "
           "round 1, 0.1 more each round, up to 5.");
    game::register_dvar_int(
        "xerces_zm_startRound", 1, 1, 255, saved,
        "Zombies, when you host: the round the match starts on, with that "
        "round's zombies. 1 is the game's own start.");
    on_off("xerces_zm_roundSkips",
           "Zombies, when you host: special rounds and prime-numbered rounds "
           "can be skipped from the pause menu for 400 points (a special round "
           "also drops its Max Ammo); in co-op more than half must vote.");
    on_off("xerces_zm_randomLoadout",
           "Zombies, when you host: at each round's start everyone's guns are "
           "swapped for two the mystery box picks.");
    on_off("xerces_zm_selfRevives",
           "Zombies, when you host: everyone has one self-revive, back 10 "
           "rounds after it was used; down with one, hold Jump or Use to get "
           "up.");
    on_off("xerces_zm_round999",
           "Zombies, when you host: the round count goes past 255; at round "
           "999 everyone votes to exfil and win, or keep going with every "
           "perk, infinite ammo, infinite points and only smart zombies.");
    on_off("xerces_zm_smartZombies",
           "Zombies, when you host: from round 20 some zombies are smart "
           "(pack hunters, burst sprinters and ambushers, each with its own "
           "glow): 10% at round 20, rising to 99% at round 998, all of them "
           "from round 999.");
  }
};
} // namespace xerces_round_settings

REGISTER_COMPONENT(xerces_round_settings::component)
