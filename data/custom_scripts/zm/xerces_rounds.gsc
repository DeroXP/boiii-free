// Xerces' Zombies round settings (docs/DESIGN.md 7.9), the toggles in the XERCES SETTINGS menu. The host's game reads
// them from the host's saved settings (the xerces_zm_* dvars, src/client/xerces/round_settings.cpp) once, as the match
// loads, so everyone in the match gets the host's settings. Not on dedicated servers. Only scripts that the core Zombies
// script uses itself are used, so this links on every Zombies map, and each setting goes through the hook the game's
// own scripts read rather than replacing a map's or a mod's script.
//
// Nothing here registers new strings with the game: a LUI event name or a HUD label it did not load from the map's
// zones is refused, and registering one breaks the check the game makes when a player joins (DESIGN.md 7.9). So each
// player gets the host's settings as a game message (plain text), and the round HUD and the pause menu's read-only
// XERCES SETTINGS are drawn by the host's own menus from two dvars, which a listen server shares with the host's client.
#using scripts\shared\ai\zombie_utility;
#using scripts\shared\callbacks_shared;
#using scripts\shared\flag_shared;
#using scripts\shared\laststand_shared;
#using scripts\shared\system_shared;
#using scripts\zm\_zm_utility;
#using scripts\zm\_zm_weapons;

#insert scripts\shared\shared.gsh;

#namespace xerces_rounds;

// The settings mask in xerces_zm_match, which data/ui_scripts/xerces_settings reads
#define MASK_MAX_AMMO 1
#define MASK_AUTO_PAP 2
#define MASK_NO_PERK_LIMIT 4
#define MASK_BOX_STAYS 8
#define MASK_POWER_ON 16
#define MASK_DOORS_OPEN 32
#define MASK_ROUND_HUD 64

#define NO_PERK_LIMIT 99

REGISTER_SYSTEM("xerces_rounds", &__init__, undefined)

function __init__()
{
	if (IsDedicated())
		return;

	s = SpawnStruct();
	s.max_ammo = GetDvarInt("xerces_zm_maxAmmo") != 0;
	s.auto_pap = GetDvarInt("xerces_zm_autoPap") != 0;
	s.start_points = GetDvarInt("xerces_zm_startPoints");
	s.no_perk_limit = GetDvarInt("xerces_zm_noPerkLimit") != 0;
	s.box_stays = GetDvarInt("xerces_zm_boxStays") != 0;
	s.power_on = GetDvarInt("xerces_zm_powerOn") != 0;
	s.doors_open = GetDvarInt("xerces_zm_doorsOpen") != 0;
	s.round_hud = GetDvarInt("xerces_zm_roundHud") != 0;
	if (s.start_points < 0)
		s.start_points = 0;

	s.mask = 0;
	if (s.max_ammo)
		s.mask += MASK_MAX_AMMO;
	if (s.auto_pap)
		s.mask += MASK_AUTO_PAP;
	if (s.no_perk_limit)
		s.mask += MASK_NO_PERK_LIMIT;
	if (s.box_stays)
		s.mask += MASK_BOX_STAYS;
	if (s.power_on)
		s.mask += MASK_POWER_ON;
	if (s.doors_open)
		s.mask += MASK_DOORS_OPEN;
	if (s.round_hud)
		s.mask += MASK_ROUND_HUD;
	level.xerces_rounds = s;

	// For the host's menus: what this match uses, and no round HUD until it starts
	SetDvar("xerces_zm_match", "" + s.mask + " " + s.start_points);
	SetDvar("xerces_zm_hud", "");

	callback::on_spawned(&on_player_spawned);
	level thread main();
}

function main()
{
	s = level.xerces_rounds;
	if (s.round_hud)
		level thread round_hud_rounds();

	wait_for_flag("initial_blackscreen_passed");

	// After the map's own setup, which sets the stock values
	if (s.start_points > 0)
		starting_points(s.start_points);
	if (s.no_perk_limit)
	{
		level.perk_purchase_limit = NO_PERK_LIMIT;
		level.get_player_perk_purchase_limit = &no_perk_limit;
	}
	if (s.box_stays)
		level._zombiemode_chest_joker_chance_override_func = &box_never_moves;
	if (s.max_ammo)
		level thread max_ammo_every_round();
	if (s.round_hud)
		level thread round_hud();

	if (s.power_on)
	{
		// The switch turns the power on in its own time, and electric doors open with it
		if (power_on() > 0 && s.doors_open)
			wait 2;
	}
	if (s.doors_open)
		open_doors();
}

function on_player_spawned()
{
	if (!IS_TRUE(self.xerces_settings_shown))
	{
		self.xerces_settings_shown = 1;
		self thread show_settings();
	}
	if (level.xerces_rounds.auto_pap)
		self thread watch_auto_pap();
}

// The host's settings, once per player, as a game message
function show_settings()
{
	self endon("disconnect");
	wait_for_flag("initial_blackscreen_passed");
	wait 4;
	s = level.xerces_rounds;
	text = "";
	if (s.max_ammo)
		text = add_setting(text, "Max Ammo every round");
	if (s.auto_pap)
		text = add_setting(text, "Auto Pack-a-Punch");
	if (s.start_points > 0)
		text = add_setting(text, with_commas(s.start_points) + " starting points");
	if (s.no_perk_limit)
		text = add_setting(text, "No perk limit");
	if (s.box_stays)
		text = add_setting(text, "Box stays");
	if (s.power_on)
		text = add_setting(text, "Power on");
	if (s.doors_open)
		text = add_setting(text, "Doors open");
	// Only the host's menus draw it
	if (s.round_hud && self IsHost())
		text = add_setting(text, "Round HUD");
	if (text != "")
		self IPrintLn("Xerces settings: " + text);
}

function add_setting(text, setting)
{
	if (text == "")
		return setting;
	return text + ", " + setting;
}

// 50000 -> "50,000"
function with_commas(number)
{
	text = "" + number;
	grouped = "";
	while (text.size > 3)
	{
		grouped = "," + GetSubStr(text, text.size - 3) + grouped;
		text = GetSubStr(text, 0, text.size - 3);
	}
	return text + grouped;
}

function wait_for_flag(name)
{
	while (!level flag::exists(name))
		wait 0.05;
	level flag::wait_till(name);
}

function first_valid_player()
{
	foreach (player in GetPlayers())
	{
		if (zm_utility::is_player_valid(player))
			return player;
	}
	return undefined;
}

// Players who join later get them through the map's own starting points
function starting_points(points)
{
	level.player_starting_points = points;
	foreach (player in GetPlayers())
	{
		player.score = points;
		player.pers["score"] = points;
	}
}

function no_perk_limit()
{
	return NO_PERK_LIMIT;
}

// The chance the mystery box moves, from treasure_chest_should_move
function box_never_moves(chance_of_joker)
{
	return -1;
}

function max_ammo_every_round()
{
	level endon("end_game");
	for (;;)
	{
		level waittill("start_of_round");
		give_max_ammo();
	}
}

// What the Max Ammo power-up does (zm_powerup_full_ammo::full_ammo_powerup), for every player. Its HUD text and
// event are the game's own, which every Zombies map loads.
function give_max_ammo()
{
	level notify("zmb_max_ammo_level");
	foreach (player in GetPlayers())
	{
		if (player.sessionstate == "spectator" || player laststand::player_is_in_laststand())
			continue;
		if (isdefined(level.check_player_is_ready_for_ammo) && [[level.check_player_is_ready_for_ammo]](player) == 0)
			continue;

		player notify("zmb_max_ammo");
		player notify("zmb_lost_knife");
		foreach (weapon in player GetWeaponsList(1))
		{
			if (refills(weapon) && player HasWeapon(weapon))
				player GiveMaxAmmo(weapon);
		}
		player PlayLocalSound("zmb_full_ammo");
	}
	LUINotifyEvent(&"zombie_notification", 1, &"ZOMBIE_POWERUP_MAX_AMMO");
}

function refills(weapon)
{
	if (IS_TRUE(level.headshots_only) && zm_utility::is_lethal_grenade(weapon))
		return false;
	if (isdefined(level.zombie_include_equipment) && isdefined(level.zombie_include_equipment[weapon]))
	{
		if (!isdefined(level.zombie_equipment) || !isdefined(level.zombie_equipment[weapon]) || !IS_TRUE(level.zombie_equipment[weapon].refill_max_ammo))
			return false;
	}
	if (isdefined(level.zombie_weapons_no_max_ammo) && isdefined(level.zombie_weapons_no_max_ammo[weapon.name]))
		return false;
	return !zm_utility::is_hero_weapon(weapon);
}

// Wall buys and the mystery box give guns through zm_weapons::weapon_give, which sends weapon_give. So does the
// Pack-a-Punch machine, with a gun that is already upgraded.
function watch_auto_pap()
{
	self notify("xerces_auto_pap");
	self endon("xerces_auto_pap");
	self endon("disconnect");
	for (;;)
	{
		self waittill("weapon_give", weapon);
		self thread auto_pap(weapon);
	}
}

// Swaps the gun as the Pack-a-Punch machine hands one over (wait_for_player_to_take). A first upgrade has no ammo mod.
function auto_pap(weapon)
{
	self endon("disconnect");
	// weapon_give still gives the gun its ammo and switches to it after the notify
	waittillframeend;
	if (!isdefined(weapon) || !self HasWeapon(weapon) || !can_auto_pap(weapon))
		return;
	upgrade = zm_weapons::get_upgrade_weapon(weapon, 0);
	if (!isdefined(upgrade) || upgrade == level.weaponnone)
		return;

	self TakeWeapon(weapon);
	upgrade = self zm_weapons::give_build_kit_weapon(upgrade);
	self GiveStartAmmo(upgrade);
	self SwitchToWeapon(upgrade);
}

function can_auto_pap(weapon)
{
	if (weapon == level.weaponnone || zm_weapons::is_weapon_upgraded(weapon) || !zm_weapons::can_upgrade_weapon(weapon))
		return false;
	if (zm_utility::is_hero_weapon(weapon) || zm_utility::is_offhand_weapon(weapon) || zm_utility::is_melee_weapon(weapon))
		return false;
	// Guns that weapon_give sets up in their own way
	if (IS_TRUE(weapon.isballisticknife) || IS_TRUE(weapon.isriotshield))
		return false;
	return !isdefined(level.zombie_weapons_callbacks) || !isdefined(level.zombie_weapons_callbacks[weapon]);
}

// Flips every standard power switch, which runs the map's own power-on (zm_power::electric_switch, or the map's
// level.temporary_power_switch_logic). Maps that power up some other way are left as they are.
function power_on()
{
	player = first_valid_player();
	if (!isdefined(player))
		return 0;
	switches = GetEntArray("use_elec_switch", "targetname");
	foreach (trig in switches)
	{
		if (isdefined(trig))
			trig notify("trigger", player, 1);
	}
	return switches.size;
}

// A forced trigger opens a door for free (zm_blockers::door_buy). Debris still charges its cost when forced, unless
// zombie_unlock_all is set, so it is set while they open. A map's own door check (level.custom_door_buy_check) still
// applies, and electric doors only open with the power.
function open_doors()
{
	player = first_valid_player();
	if (!isdefined(player))
		return;
	unlock_all = GetDvarInt("zombie_unlock_all");
	SetDvar("zombie_unlock_all", 1);
	foreach (door in GetEntArray("zombie_door", "targetname"))
	{
		// Doors that share their parts open together, so the others are skipped
		if (!isdefined(door) || IS_TRUE(door._door_open) || IS_TRUE(door.door_is_moving))
			continue;
		door notify("trigger", player, 1);
		wait 0.05;
	}
	foreach (debris in GetEntArray("zombie_debris", "targetname"))
	{
		if (!isdefined(debris))
			continue;
		debris notify("trigger", player, 1);
		wait 0.05;
	}
	SetDvar("zombie_unlock_all", unlock_all);
}

function zombies_left()
{
	left = zombie_utility::get_current_zombie_count();
	if (isdefined(level.zombie_total) && level.zombie_total > 0)
		left += level.zombie_total;
	return left;
}

// "<zombies left> <round seconds> <1 while a round is on>" in xerces_zm_hud, four times a second; between rounds the
// seconds are the last round's time
function round_hud()
{
	level endon("end_game");
	level thread clear_round_hud();
	s = level.xerces_rounds;
	for (;;)
	{
		seconds = 0;
		running = 0;
		if (isdefined(s.round_start))
		{
			running = IS_TRUE(s.round_running);
			seconds = s.round_seconds;
			if (running)
				seconds = int((GetTime() - s.round_start) / 1000);
		}
		SetDvar("xerces_zm_hud", "" + zombies_left() + " " + seconds + " " + running);
		wait 0.25;
	}
}

function clear_round_hud()
{
	level waittill("end_game");
	SetDvar("xerces_zm_hud", "");
}

// A round runs from zm::round_think's start_of_round to its end_of_round
function round_hud_rounds()
{
	level endon("end_game");
	s = level.xerces_rounds;
	for (;;)
	{
		level waittill("start_of_round");
		s.round_start = GetTime();
		s.round_seconds = 0;
		s.round_running = 1;

		level waittill("end_of_round");
		s.round_seconds = int((GetTime() - s.round_start) / 1000);
		s.round_running = 0;
	}
}
