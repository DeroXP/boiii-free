// Xerces' Zombies round settings (docs/DESIGN.md 7.9), the toggles in the XERCES SETTINGS menu. The host's game reads
// them from the host's saved settings (the xerces_zm_* dvars, src/client/xerces/round_settings.cpp) once, as the match
// loads, so everyone in the match gets the host's settings. Not on dedicated servers. Only scripts that the core Zombies
// script uses itself are used, so this links on every Zombies map, and each setting goes through the hook the game's
// own scripts read rather than replacing a map's or a mod's script.
//
// What a player sees is drawn through the game's generic script menus HudElementText and HudElementImage, which every
// map registers with their data keys and whose text the game sends as it is. A LUI event or HUD label with a new name
// is refused, and registering one breaks the game's check when a player joins (DESIGN.md 7.9). A hidden
// HudElementText holds the match's settings for Xerces' menus on each player's PC (data/ui_scripts/xerces_settings).
// Those menus send the wheel and coin flip requests back as menu responses under StartMenu_Main, which the game's own
// handler ignores.
#using scripts\shared\ai\zombie_utility;
#using scripts\shared\aat_shared;
#using scripts\shared\callbacks_shared;
#using scripts\shared\flag_shared;
#using scripts\shared\laststand_shared;
#using scripts\shared\system_shared;
#using scripts\shared\util_shared;
#using scripts\zm\_zm_laststand;
#using scripts\zm\_zm_magicbox;
#using scripts\zm\_zm_perks;
#using scripts\zm\_zm_score;
#using scripts\zm\_zm_spawner;
#using scripts\zm\_zm_utility;
#using scripts\zm\_zm_weapons;

#insert scripts\shared\shared.gsh;

#namespace xerces_rounds;

// The settings mask in the hidden menu text
#define MASK_MAX_AMMO 1
#define MASK_AUTO_PAP 2
#define MASK_NO_PERK_LIMIT 4
#define MASK_BOX_STAYS 8
#define MASK_POWER_ON 16
#define MASK_DOORS_OPEN 32
#define MASK_ROUND_HUD 64
#define MASK_WHEEL 128
#define MASK_COIN_FLIP 256
#define MASK_ROUND_SKIPS 512
#define MASK_RANDOM_LOADOUT 1024
#define MASK_SELF_REVIVES 2048
#define MASK_ROUND_999 4096
#define MASK_SMART_ZOMBIES 8192

#define NO_PERK_LIMIT 99
#define WHEEL_COST 2500
// What the mystery box costs during a Fire Sale (zm_magicbox::fire_sale_fix)
#define WHEEL_FIRE_SALE_COST 10
#define MIN_BET 100
// The game's own points cap: a score set higher reads back as this (measured 2026-11-07)
#define MAX_SCORE 4194303

// Round skips: what a skip pays each player, exactly (zm_score::add_to_player_score, which Double Points does not
// touch), and the kinds of round that can be skipped
#define SKIP_POINTS 400
#define SKIP_NONE 0
#define SKIP_PRIME 1
#define SKIP_SPECIAL 2
// The game's round number stops here (zm::set_round_number), and Xerces counts on to the last round's vote
#define GAME_ROUND_CAP 255
#define FINAL_ROUND 999
// Self-revives: back to one every this many rounds, and how long the use button is held
#define SELF_REVIVE_EVERY 10
#define SELF_REVIVE_HOLD_MS 1500

// Smart zombies: none before SMART_FIRST_ROUND, then a share (per 1,000) rising evenly from SMART_FIRST_SHARE to
// SMART_LAST_SHARE at round SMART_LAST_ROUND, and every zombie from the round after it on
#define SMART_FIRST_ROUND 20
#define SMART_FIRST_SHARE 100
#define SMART_LAST_ROUND 998
#define SMART_LAST_SHARE 990
#define SMART_NONE 0
#define SMART_PACK 1
#define SMART_BURST 2
#define SMART_AMBUSH 3
// How often a smart zombie looks again, in seconds
#define SMART_TICK 0.25
// Pack hunters hold this far from their player until this many are waiting, or for at most PACK_WAIT_MS; then the pack
// sprints at one player, and each hunter leaves the last stretch to the game's own attack
#define PACK_SIZE 3
#define PACK_HOLD_DIST 600
#define PACK_WAIT_MS 10000
#define PACK_CLOSE_DIST 160
// Burst sprinters walk, and within BURST_RANGE of their player sprint for a moment every few seconds
#define BURST_RANGE 900
// Ambushers go to a spot their player cannot see, AMBUSH_MIN_DIST to AMBUSH_MAX_DIST from them, and wait there until
// the player comes within AMBUSH_STRIKE_DIST or turns their back within AMBUSH_BACK_DIST, or for at most AMBUSH_WAIT_MS
#define AMBUSH_MIN_DIST 350
#define AMBUSH_MAX_DIST 700
#define AMBUSH_TRAVEL_MS 12000
#define AMBUSH_WAIT_MS 10000
#define AMBUSH_STRIKE_DIST 320
#define AMBUSH_BACK_DIST 1000
// How many of the spots near the player an ambusher checks for one out of sight
#define AMBUSH_SPOTS_CHECKED 12

// HudElementText's alignment in its box, from x to x + width (seen in the game on 2026-11-07)
#define ALIGN_LEFT 1
#define ALIGN_CENTER 2
#define ALIGN_RIGHT 4

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
	s.wheel = GetDvarInt("xerces_zm_wheel") != 0;
	s.coin_flip = GetDvarInt("xerces_zm_coinFlip") != 0;
	s.round_skips = GetDvarInt("xerces_zm_roundSkips") != 0;
	s.random_loadout = GetDvarInt("xerces_zm_randomLoadout") != 0;
	s.self_revives = GetDvarInt("xerces_zm_selfRevives") != 0;
	s.round_999 = GetDvarInt("xerces_zm_round999") != 0;
	s.smart_zombies = GetDvarInt("xerces_zm_smartZombies") != 0;
	if (s.start_points < 0)
		s.start_points = 0;
	// The round as of its start_of_round (the game raises level.round_number as the last one ends), counted on by
	// Xerces where the game's stops (255), and how many rounds have started
	s.round = 1;
	s.rounds_started = 0;
	// This round's skip (SKIP_*), whether the round 999 vote is open, and what it decided
	s.skip = SKIP_NONE;
	s.vote = 0;
	s.exfil = 0;
	s.endless = 0;
	s.notice_id = 0;
	// Smart zombies: whether players were told they have started, the pack hunters' current pack and its player
	s.smart_announced = 0;
	s.pack_wave = 0;
	s.pack_target = undefined;

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
	if (s.wheel)
		s.mask += MASK_WHEEL;
	if (s.coin_flip)
		s.mask += MASK_COIN_FLIP;
	if (s.round_skips)
		s.mask += MASK_ROUND_SKIPS;
	if (s.random_loadout)
		s.mask += MASK_RANDOM_LOADOUT;
	if (s.self_revives)
		s.mask += MASK_SELF_REVIVES;
	if (s.round_999)
		s.mask += MASK_ROUND_999;
	if (s.smart_zombies)
		s.mask += MASK_SMART_ZOMBIES;
	level.xerces_rounds = s;

	callback::on_connect(&on_player_connect);
	callback::on_spawned(&on_player_spawned);
	level thread main();
}

function main()
{
	s = level.xerces_rounds;
	level thread round_watch();

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
	if (s.self_revives)
		install_self_revive_checks();
	if (s.round_999)
		install_end_screen();
	// Rounds to 999 needs them too: keeping going past round 999 brings only smart zombies
	if (s.smart_zombies || s.round_999)
		install_smart_zombies();
	level thread close_on_end_game();

	if (s.power_on)
	{
		// The switch turns the power on in its own time, and electric doors open with it
		if (power_on() > 0 && s.doors_open)
			wait 2;
	}
	if (s.doors_open)
		open_doors();
}

function on_player_connect()
{
	self thread menu_responses();
}

function on_player_spawned()
{
	if (!IS_TRUE(self.xerces_spawned))
	{
		self.xerces_spawned = 1;
		self thread first_spawn();
	}
	if (level.xerces_rounds.auto_pap)
		self thread watch_auto_pap();
}

// Once per player, from the end of the black screen: the data for Xerces' menus, the HUD, the settings message and a
// self-revive
function first_spawn()
{
	self endon("disconnect");
	wait_for_flag("initial_blackscreen_passed");
	s = level.xerces_rounds;
	self.xerces_data = self lui_text(data_text(), ALIGN_LEFT, 0, 0, 10, 10, (1, 1, 1), 0);
	if (s.self_revives)
	{
		self.xerces_self_revives = 1;
		self thread self_revive_watch();
	}
	self create_hud();
	wait 4;
	self show_settings();
}

// "XERCES <mask> <starting points> <round> <skip> <vote>", which data/ui_scripts/xerces_settings reads: the round's
// skip (SKIP_*) and whether the round 999 vote is open
function data_text()
{
	s = level.xerces_rounds;
	return "XERCES " + s.mask + " " + s.start_points + " " + s.round + " " + s.skip + " " + s.vote;
}

function update_data_text()
{
	text = data_text();
	foreach (player in GetPlayers())
	{
		if (isdefined(player.xerces_data))
			player SetLUIMenuData(player.xerces_data, "text", text);
	}
}

// The host's settings, once per player, as a game message
function show_settings()
{
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
	if (s.round_hud)
		text = add_setting(text, "Round HUD");
	if (s.wheel)
		text = add_setting(text, "Pack-a-Punch wheel");
	if (s.coin_flip)
		text = add_setting(text, "Coin flip");
	if (s.round_skips)
		text = add_setting(text, "Round skips");
	if (s.random_loadout)
		text = add_setting(text, "Random loadout");
	if (s.self_revives)
		text = add_setting(text, "Self-revives");
	if (s.round_999)
		text = add_setting(text, "Rounds to 999");
	if (s.smart_zombies)
		text = add_setting(text, "Smart zombies");
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
	sign = "";
	if (GetSubStr(text, 0, 1) == "-")
	{
		sign = "-";
		text = GetSubStr(text, 1);
	}
	grouped = "";
	while (text.size > 3)
	{
		grouped = "," + GetSubStr(text, text.size - 3) + grouped;
		text = GetSubStr(text, 0, text.size - 3);
	}
	return sign + text + grouped;
}

// 83 -> "1:23"; 3723 -> "1:02:03"
function clock(seconds)
{
	hours = int(seconds / 3600);
	minutes = int(seconds / 60) % 60;
	text = "" + minutes;
	if (hours > 0)
		text = hours + ":" + two_digits(minutes);
	return text + ":" + two_digits(seconds % 60);
}

function two_digits(number)
{
	if (number < 10)
		return "0" + number;
	return "" + number;
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

// The game's generic script menus, in a 1280x720 screen. A text sits in its box by its alignment; an image's x and y
// are its top left corner.
function lui_text(text, alignment, x, y, width, height, color, alpha)
{
	menu = self OpenLUIMenu("HudElementText");
	self SetLUIMenuData(menu, "text", text);
	self SetLUIMenuData(menu, "alignment", alignment);
	self SetLUIMenuData(menu, "x", x);
	self SetLUIMenuData(menu, "y", y);
	self SetLUIMenuData(menu, "width", width);
	self SetLUIMenuData(menu, "height", height);
	self lui_color(menu, color, alpha);
	return menu;
}

function lui_image(material, x, y, width, height, color, alpha)
{
	menu = self OpenLUIMenu("HudElementImage");
	self SetLUIMenuData(menu, "material", material);
	self SetLUIMenuData(menu, "x", x);
	self SetLUIMenuData(menu, "y", y);
	self SetLUIMenuData(menu, "width", width);
	self SetLUIMenuData(menu, "height", height);
	self lui_color(menu, color, alpha);
	return menu;
}

function lui_color(menu, color, alpha)
{
	self SetLUIMenuData(menu, "red", color[0]);
	self SetLUIMenuData(menu, "green", color[1]);
	self SetLUIMenuData(menu, "blue", color[2]);
	self SetLUIMenuData(menu, "alpha", alpha);
}

function lui_close(menus)
{
	foreach (menu in menus)
		self CloseLUIMenu(menu);
}

function zombies_left()
{
	left = zombie_utility::get_current_zombie_count();
	if (isdefined(level.zombie_total) && level.zombie_total > 0)
		left += level.zombie_total;
	return left;
}

// A round runs from zm::round_think's start_of_round to its end_of_round. The game's round number stops at 255
// (zm::set_round_number), so from there Xerces' count goes one up each round.
function round_watch()
{
	level endon("end_game");
	s = level.xerces_rounds;
	for (;;)
	{
		level waittill("start_of_round");
		if (level.round_number < GAME_ROUND_CAP)
			s.round = level.round_number;
		else
			s.round++;
		s.rounds_started++;
		s.round_start = GetTime();
		s.round_seconds = 0;
		s.round_running = 1;
		round_started();
		// The round and its skip, for the coin flip's payout and the pause menu entries in Xerces' menus
		update_data_text();

		level waittill("end_of_round");
		s.round_seconds = int((GetTime() - s.round_start) / 1000);
		s.round_running = 0;
		round_ended();
	}
}

// What stage 3 does as a round starts: the self-revive refill, the round's skip, the random loadouts, the round past
// 255 on the HUD, and the round 999 vote
function round_started()
{
	s = level.xerces_rounds;
	if (s.self_revives && s.round % SELF_REVIVE_EVERY == 0)
		refill_self_revives();
	if (s.round_skips)
		open_skip();
	if (s.random_loadout)
	{
		foreach (player in GetPlayers())
			player thread random_loadout();
	}
	if (s.round_999)
	{
		if (s.round > GAME_ROUND_CAP)
		{
			// The game's own end screen would say 255 rounds
			level._supress_survived_screen = 1;
			foreach (player in GetPlayers())
				player update_round_row();
		}
		if (s.round == FINAL_ROUND)
			open_final_vote();
	}
	// Once, on the first round with smart zombies that has no other notice
	if (!s.smart_announced && s.skip == SKIP_NONE && !s.vote && smart_share() > 0)
	{
		s.smart_announced = 1;
		notice_all(smart_notice(), 12);
	}
}

function round_ended()
{
	s = level.xerces_rounds;
	if (s.skip != SKIP_NONE)
	{
		s.skip = SKIP_NONE;
		clear_votes();
		update_data_text();
	}
	// Round 999 ended with no decision: it keeps going
	if (s.vote)
		level thread keep_going();
}

// Top left: zombies left and the round's time (the round HUD setting; between rounds the last round's time), the
// self-revives left, and past round 255 the round itself, which the game's own HUD stops at 255. Under them, centred,
// a line for the round's notices.
function create_hud()
{
	s = level.xerces_rounds;
	if (!s.round_hud && !s.self_revives && !s.round_skips && !s.round_999)
		return;
	grey = (0.75, 0.75, 0.75);
	white = (1, 1, 1);
	hud = SpawnStruct();
	hud.menus = [];
	y = 20;
	if (s.round_hud)
	{
		hud.left_label = self hud_text(hud, "ZOMBIES LEFT", 24, y, grey);
		hud.left = self hud_text(hud, "", 180, y, white);
		y += 30;
		hud.time_label = self hud_text(hud, "", 24, y, grey);
		hud.time = self hud_text(hud, "", 180, y, white);
		y += 30;
		hud.shown_left = "";
		hud.shown_label = "";
		hud.shown_time = "";
	}
	if (s.self_revives)
	{
		hud.revives_label = self hud_text(hud, "SELF-REVIVES", 24, y, grey);
		hud.revives = self hud_text(hud, "", 180, y, white);
		y += 30;
	}
	if (s.round_999)
	{
		hud.round_label = self hud_text(hud, "", 24, y, grey);
		hud.round = self hud_text(hud, "", 180, y, white);
	}
	hud.notice = self lui_text("", ALIGN_CENTER, 140, 130, 1000, 24, (1, 0.85, 0.3), 1);
	hud.menus[hud.menus.size] = hud.notice;
	self.xerces_hud = hud;
	if (s.self_revives)
		self update_revives_row();
	if (s.round_999)
		self update_round_row();
}

function hud_text(hud, text, x, y, color)
{
	menu = self lui_text(text, ALIGN_LEFT, x, y, 150, 24, color, 1);
	hud.menus[hud.menus.size] = menu;
	return menu;
}

function update_revives_row()
{
	if (!isdefined(self.xerces_hud) || !isdefined(self.xerces_hud.revives))
		return;
	count = 0;
	if (isdefined(self.xerces_self_revives))
		count = self.xerces_self_revives;
	self SetLUIMenuData(self.xerces_hud.revives, "text", "" + count);
}

function update_round_row()
{
	if (!isdefined(self.xerces_hud) || !isdefined(self.xerces_hud.round))
		return;
	round = level.xerces_rounds.round;
	if (round <= GAME_ROUND_CAP)
		return;
	self SetLUIMenuData(self.xerces_hud.round_label, "text", "ROUND");
	self SetLUIMenuData(self.xerces_hud.round, "text", "" + round);
}

function round_hud()
{
	level endon("end_game");
	s = level.xerces_rounds;
	for (;;)
	{
		left = "" + zombies_left();
		label = "";
		time = "";
		if (isdefined(s.round_start))
		{
			seconds = s.round_seconds;
			label = "LAST ROUND";
			if (IS_TRUE(s.round_running))
			{
				seconds = int((GetTime() - s.round_start) / 1000);
				label = "ROUND TIME";
			}
			time = clock(seconds);
		}
		foreach (player in GetPlayers())
		{
			if (isdefined(player.xerces_hud) && isdefined(player.xerces_hud.left))
				player update_round_hud(left, label, time);
		}
		wait 0.25;
	}
}

// Sends only what changed
function update_round_hud(left, label, time)
{
	hud = self.xerces_hud;
	if (hud.shown_left != left)
	{
		self SetLUIMenuData(hud.left, "text", left);
		hud.shown_left = left;
	}
	if (hud.shown_label != label)
	{
		self SetLUIMenuData(hud.time_label, "text", label);
		hud.shown_label = label;
	}
	if (hud.shown_time != time)
	{
		self SetLUIMenuData(hud.time, "text", time);
		hud.shown_time = time;
	}
}

function close_on_end_game()
{
	level waittill("end_game");
	foreach (player in GetPlayers())
	{
		hud = player.xerces_hud;
		if (!isdefined(hud))
			continue;
		player.xerces_hud = undefined;
		player lui_close(hud.menus);
		if (isdefined(player.xerces_prompt))
		{
			player CloseLUIMenu(player.xerces_prompt);
			player.xerces_prompt = undefined;
		}
	}
}

// Requests from Xerces' menus on the player's PC (data/ui_scripts/xerces_settings): "xerces_wheel",
// "xerces_flip:<points|all>", "xerces_skip", "xerces_exfil" and "xerces_keep", sent as menu responses under
// StartMenu_Main. A response ends at its first space (seen in the game on 2026-11-07), so the parts are joined by ":".
function menu_responses()
{
	self endon("disconnect");
	for (;;)
	{
		self waittill("menuresponse", menu, response);
		if (menu != "StartMenu_Main" || !isdefined(level.xerces_rounds))
			continue;
		words = StrTok(response, ":");
		if (words.size == 0)
			continue;
		if (words[0] == "xerces_wheel")
			self thread wheel_request();
		else if (words[0] == "xerces_flip" && words.size > 1)
			self thread coin_flip_request(words[1]);
		else if (words[0] == "xerces_skip")
			self skip_request();
		else if (words[0] == "xerces_exfil")
			self final_vote_request("exfil");
		else if (words[0] == "xerces_keep")
			self final_vote_request("keep");
	}
}

// The wheel's eight slices: the five ammo mods, with a "nothing" between some so the three are spread out. An ammo mod
// the map does not have counts as nothing.
function wheel_slices()
{
	names = array("zm_aat_blast_furnace", "", "zm_aat_dead_wire", "zm_aat_fire_works", "", "zm_aat_thunder_wall", "zm_aat_turned", "");
	labels = array("BLAST FURNACE", "NOTHING", "DEAD WIRE", "FIREWORKS", "NOTHING", "THUNDER WALL", "TURNED", "NOTHING");
	colors = array((1, 0.55, 0.15), (0.7, 0.7, 0.7), (0.45, 0.75, 1), (1, 0.45, 0.85), (0.7, 0.7, 0.7), (0.6, 0.9, 1), (0.5, 1, 0.5), (0.7, 0.7, 0.7));
	slices = [];
	for (i = 0; i < names.size; i++)
	{
		slice = SpawnStruct();
		slice.name = names[i];
		slice.label = labels[i];
		slice.color = colors[i];
		if (slice.name != "" && (!isdefined(level.aat) || !isdefined(level.aat[slice.name])))
		{
			slice.name = "";
			slice.label = "NOTHING";
			slice.color = (0.7, 0.7, 0.7);
		}
		// Its HUD icon; one a mod registers without an icon shows its name
		if (slice.name != "")
			slice.icon = level.aat[slice.name].damage_feedback_icon;
		// Centre of the slice, clockwise from the top
		slice.x = 640 + int(125 * sin(i * 45));
		slice.y = 300 - int(125 * cos(i * 45));
		slices[slices.size] = slice;
	}
	return slices;
}

// During a Fire Sale the wheel costs what the mystery box does then
function wheel_cost()
{
	if (IS_TRUE(level.zombie_vars["zombie_powerup_fire_sale_on"]))
		return WHEEL_FIRE_SALE_COST;
	return WHEEL_COST;
}

// A player who is down or out, or whose last spin or flip is still on screen, is told why nothing happens (the pause
// menu has closed by then)
function cannot_play()
{
	if (IS_TRUE(self.xerces_busy))
	{
		self IPrintLnBold("Wait for the last spin or flip to finish");
		return true;
	}
	if (!zm_utility::is_player_valid(self))
	{
		self IPrintLnBold("Not while you are down");
		return true;
	}
	return false;
}

// The wheel's once-a-round lock goes by rounds started, which keeps counting past round 255; the moments before round 1
// count as round 1
function wheel_round()
{
	if (level.xerces_rounds.rounds_started < 1)
		return 1;
	return level.xerces_rounds.rounds_started;
}

// 2,500 points (10 in a Fire Sale), once a round, on a Pack-a-Punched gun the ammo mods can go on
function wheel_request()
{
	if (!level.xerces_rounds.wheel)
	{
		self IPrintLnBold("The host has not turned the Pack-a-Punch wheel on");
		return;
	}
	if (self cannot_play())
		return;
	if (isdefined(self.xerces_wheel_round) && self.xerces_wheel_round == wheel_round())
	{
		self IPrintLnBold("One spin a round: spin again next round");
		return;
	}
	weapon = self GetCurrentWeapon();
	if (!IS_TRUE(level.aat_in_use) || !zm_weapons::is_weapon_upgraded(weapon) || aat::is_exempt_weapon(weapon))
	{
		self IPrintLnBold("Hold a Pack-a-Punched gun to spin the wheel");
		return;
	}
	cost = wheel_cost();
	if (!self zm_score::can_player_purchase(cost))
	{
		self IPrintLnBold("The wheel costs " + with_commas(cost) + " points");
		return;
	}
	before = self.score;
	self zm_score::minus_to_player_score(cost);
	// What it took: nothing while a gum makes purchases free
	paid = before - self.score;
	self.xerces_wheel_round = wheel_round();
	self thread wheel_spin(weapon, cost, paid);
}

function wheel_spin(weapon, cost, paid)
{
	self endon("disconnect");
	self.xerces_busy = 1;
	slices = wheel_slices();
	result = RandomInt(slices.size);

	menus = [];
	menus[menus.size] = self lui_image("black", 460, 120, 360, 360, (1, 1, 1), 0.6);
	highlight = self lui_image("white", slices[0].x - 38, slices[0].y - 38, 76, 76, (1, 0.42, 0.26), 0.3);
	menus[menus.size] = highlight;
	foreach (slice in slices)
	{
		if (isdefined(slice.icon))
			menus[menus.size] = self lui_image(slice.icon, slice.x - 28, slice.y - 28, 56, 56, (1, 1, 1), 1);
		else
			menus[menus.size] = self lui_text(slice.label, ALIGN_CENTER, slice.x - 50, slice.y - 10, 100, 20, slice.color, 1);
	}
	menus[menus.size] = self lui_text("PACK-A-PUNCH WHEEL", ALIGN_CENTER, 440, 84, 400, 28, (1, 0.42, 0.26), 1);
	price = with_commas(cost);
	if (cost == WHEEL_FIRE_SALE_COST)
		price = "FIRE SALE: " + price;
	centre = self lui_text(price, ALIGN_CENTER, 540, 290, 200, 22, (1, 1, 1), 1);
	menus[menus.size] = centre;

	// Three turns, then on to the result, slowing down
	steps = slices.size * 3 + result;
	for (i = 1; i <= steps; i++)
	{
		slice = slices[i % slices.size];
		self SetLUIMenuData(highlight, "x", slice.x - 38);
		self SetLUIMenuData(highlight, "y", slice.y - 38);
		t = i / (steps * 1.0);
		wait(0.05 + 0.3 * t * t * t);
	}

	slice = slices[result];
	self SetLUIMenuData(centre, "text", slice.label);
	self lui_color(centre, slice.color, 1);
	for (i = 0; i < 3; i++)
	{
		self SetLUIMenuData(highlight, "alpha", 0.75);
		wait 0.15;
		self SetLUIMenuData(highlight, "alpha", 0.25);
		wait 0.15;
	}

	if (slice.name == "")
		self teddy();
	else if (self HasWeapon(weapon))
	{
		self aat::acquire(weapon, slice.name);
		if (isdefined(level.aat[slice.name].damage_feedback_sound))
			self PlayLocalSound(level.aat[slice.name].damage_feedback_sound);
	}
	else
	{
		// The gun went while the wheel turned: what was paid comes back, and so does the round's spin
		self.score += paid;
		self.pers["score"] = self.score;
		self.xerces_wheel_round = undefined;
		if (paid > 0)
			self IPrintLnBold("The gun is gone: your " + with_commas(paid) + " points are back");
		else
			self IPrintLnBold("The gun is gone: spin again");
	}
	wait 1.5;
	self lui_close(menus);
	self.xerces_busy = 0;
}

// What the mystery box shows when it moves: its teddy bear rises and laughs, here in front of the player
function teddy()
{
	if (isdefined(level.zmb_laugh_alias))
		self PlayLocalSound(level.zmb_laugh_alias);
	if (!isdefined(level.chest_joker_model))
		return;
	angles = self GetPlayerAngles();
	origin = self GetEye() + AnglesToForward((0, angles[1], 0)) * 60 - (0, 0, 20);
	bear = Spawn("script_model", origin);
	bear SetModel(level.chest_joker_model);
	bear.angles = (0, angles[1] + 180, 0);
	bear thread teddy_rise();
}

// The bear's own thread, so it goes away even if the player leaves
function teddy_rise()
{
	self MoveZ(50, 2.5, 0.5, 0.5);
	self RotateYaw(720, 2.5);
	wait 2.5;
	self Delete();
}

// "xerces_flip:2500" or "xerces_flip:all"
function coin_flip_request(amount)
{
	if (!level.xerces_rounds.coin_flip)
	{
		self IPrintLnBold("The host has not turned the coin flip on");
		return;
	}
	if (self cannot_play())
		return;
	if (self.score >= MAX_SCORE)
	{
		self IPrintLnBold("You have the most points the game holds: a win could not pay");
		return;
	}
	bet = self.score;
	if (amount != "all")
		bet = int(amount);
	if (bet < MIN_BET)
	{
		self IPrintLnBold("The smallest bet is 100 points");
		return;
	}
	if (bet > self.score)
	{
		self IPrintLnBold("You don't have " + with_commas(bet) + " points");
		return;
	}
	self thread coin_flip(bet);
}

// A win pays the bet times 2 at round 1, 0.1 more each round, at most 5 (in tenths). The round is the one Xerces' menus
// show: between rounds, the last one.
function flip_payout_tenths()
{
	tenths = 20 + level.xerces_rounds.round - 1;
	if (tenths > 50)
		tenths = 50;
	return tenths;
}

// What heads pays, the bet included: the bet times tenths / 10, rounded down, but at most room, what the score can
// still take. GSC multiplies and divides in single-precision floats, which turn 2.1 into 2.0999999 (100 x 2.1 paid
// 209), so this works in whole numbers: the bet's tens times tenths, plus the last digit's share.
function flip_payout(bet, tenths, room)
{
	// A score already at the cap gets its bet back
	if (room <= bet)
		return bet;
	ones = bet % 10;
	tens = int((bet - ones) / 10);
	// That division is in floats too, so put tens right
	while (tens * 10 > bet - ones)
		tens--;
	while ((tens + 1) * 10 <= bet - ones)
		tens++;
	// Past this, tens * tenths is over room anyway
	if (tens > int(room / tenths) + 1)
		return room;
	payout = tens * tenths + int(ones * tenths / 10);
	if (payout > room)
		payout = room;
	return payout;
}

function coin_flip(bet)
{
	self endon("disconnect");
	self.xerces_busy = 1;
	tenths = flip_payout_tenths();
	win = RandomInt(2) == 0;
	// The bet is not a purchase, so a gum that makes purchases free does not cover it
	self.score -= bet;
	self.pers["score"] = self.score;

	menus = [];
	menus[menus.size] = self lui_image("black", 490, 200, 300, 150, (1, 1, 1), 0.6);
	menus[menus.size] = self lui_text("COIN FLIP: " + with_commas(bet), ALIGN_CENTER, 490, 212, 300, 24, (1, 0.42, 0.26), 1);
	coin = self lui_text("HEADS", ALIGN_CENTER, 490, 250, 300, 36, (1, 0.85, 0.3), 1);
	menus[menus.size] = coin;
	result = self lui_text("", ALIGN_CENTER, 490, 300, 300, 24, (1, 1, 1), 1);
	menus[menus.size] = result;

	// Heads wins
	flips = 13;
	if (win)
		flips = 14;
	for (i = 1; i <= flips; i++)
	{
		side = "TAILS";
		if (i % 2 == 0)
			side = "HEADS";
		self SetLUIMenuData(coin, "text", side);
		wait(0.05 + 0.01 * i);
	}

	if (win)
	{
		payout = flip_payout(bet, tenths, MAX_SCORE - self.score);
		self.score += payout;
		self.pers["score"] = self.score;
		times = "x" + int(tenths / 10) + "." + tenths % 10;
		if (self.score >= MAX_SCORE)
			times = "MAX POINTS";
		self SetLUIMenuData(result, "text", "YOU WIN " + with_commas(payout) + " (" + times + ")");
		self lui_color(result, (0.5, 1, 0.5), 1);
		self zm_utility::play_sound_on_ent("purchase");
	}
	else
	{
		self SetLUIMenuData(result, "text", "YOU LOSE " + with_commas(bet));
		self lui_color(result, (1, 0.4, 0.4), 1);
		zm_utility::play_sound_at_pos("no_purchase", self.origin);
	}
	wait 2;
	self lui_close(menus);
	self.xerces_busy = 0;
}

// Stage 3 (DESIGN.md 7.9): round skips, random loadouts, self-revives and round 999

// A line on every player's screen under the HUD, cleared after the given seconds unless a newer one replaced it
function notice_all(text, seconds)
{
	s = level.xerces_rounds;
	s.notice_id++;
	foreach (player in GetPlayers())
	{
		if (isdefined(player.xerces_hud))
			player SetLUIMenuData(player.xerces_hud.notice, "text", text);
	}
	level thread clear_notice(s.notice_id, seconds);
}

function clear_notice(id, seconds)
{
	level endon("end_game");
	wait seconds;
	if (level.xerces_rounds.notice_id != id)
		return;
	foreach (player in GetPlayers())
	{
		if (isdefined(player.xerces_hud))
			player SetLUIMenuData(player.xerces_hud.notice, "text", "");
	}
}

// Each player's vote: "skip", or "exfil" or "keep" in the round 999 vote. More than half the players decides.
function clear_votes()
{
	foreach (player in GetPlayers())
		player.xerces_vote = undefined;
}

function count_votes(choice)
{
	votes = 0;
	foreach (player in GetPlayers())
	{
		if (isdefined(player.xerces_vote) && player.xerces_vote == choice)
			votes++;
	}
	return votes;
}

function is_majority(votes)
{
	return votes * 2 > GetPlayers().size;
}

function is_prime(number)
{
	if (number < 2)
		return false;
	for (divisor = 2; divisor * divisor <= number; divisor++)
	{
		if (number % divisor == 0)
			return false;
	}
	return true;
}

// The maps' special rounds (dogs, parasites, spiders and the like) all set the game's special_round flag
function skip_kind()
{
	if (level flag::exists("special_round") && level flag::get("special_round"))
		return SKIP_SPECIAL;
	if (is_prime(level.xerces_rounds.round))
		return SKIP_PRIME;
	return SKIP_NONE;
}

function open_skip()
{
	s = level.xerces_rounds;
	clear_votes();
	s.skip = SKIP_NONE;
	// Round 999's vote decides that round
	if (s.round_999 && s.round == FINAL_ROUND)
		return;
	s.skip = skip_kind();
	if (s.skip == SKIP_SPECIAL)
		notice_all("SPECIAL ROUND: SKIP IT FROM THE PAUSE MENU FOR 400 POINTS AND A MAX AMMO", 8);
	else if (s.skip == SKIP_PRIME)
		notice_all("PRIME ROUND " + s.round + ": SKIP IT FROM THE PAUSE MENU FOR 400 POINTS", 8);
}

// A vote to skip this round; in solo it is the only vote, so the round goes at once
function skip_request()
{
	s = level.xerces_rounds;
	if (!s.round_skips)
	{
		self IPrintLnBold("The host has not turned round skips on");
		return;
	}
	if (IS_TRUE(s.skipping))
		return;
	if (s.skip == SKIP_NONE)
	{
		self IPrintLnBold("Only special rounds and prime rounds can be skipped");
		return;
	}
	if (isdefined(self.xerces_vote))
	{
		self IPrintLnBold("You voted: waiting for the others");
		return;
	}
	self.xerces_vote = "skip";
	votes = count_votes("skip");
	if (is_majority(votes))
		level thread skip_round();
	else
		notice_all("SKIP THIS ROUND: " + votes + " OF " + GetPlayers().size + " PLAYERS VOTED", 8);
}

// Ends the round the way the Round Robbin gum does (zm_bgb_round_robbin): no more spawns this round, last_ai_down with
// the last zombie, then every zombie dies. On a special round last_ai_down is what drops the round's own Max Ammo, as
// when its last enemy dies; nothing listens for it on other rounds. Each player who is up gets SKIP_POINTS.
function skip_round()
{
	level endon("end_game");
	s = level.xerces_rounds;
	if (IS_TRUE(s.skipping))
		return;
	s.skipping = 1;
	kind = s.skip;
	round = s.rounds_started;
	// A special round's first enemies come a few seconds in
	for (i = 0; i < 300 && s.rounds_started == round && s.round_running && !round_has_zombies(); i++)
		wait 0.1;
	if (s.rounds_started != round || !s.round_running || !round_has_zombies())
	{
		s.skipping = 0;
		// Still the same round: the votes go, so the players can vote again
		if (s.rounds_started == round && s.round_running)
		{
			clear_votes();
			notice_all("THE ROUND COULD NOT BE SKIPPED YET: VOTE AGAIN", 6);
		}
		return;
	}
	s.skip = SKIP_NONE;
	clear_votes();
	update_data_text();

	level.zombie_total = 0;
	level notify("kill_round");
	PlaySoundAtPosition("zmb_bgb_round_robbin", (0, 0, 0));
	wait 0.1;
	// last_ai_down goes with an enemy that can die, then they die; one still in its spawn effect is shielded (a dog
	// arriving in its lightning) and dies once it is out. Without a single such enemy, the round's own last death sends
	// last_ai_down.
	announced = 0;
	for (i = 0; i < 100; i++)
	{
		zombies = GetAITeamArray(level.zombie_team);
		if (!announced)
		{
			last = undefined;
			foreach (zombie in zombies)
			{
				if (dies_in_skip(zombie))
					last = zombie;
			}
			if (isdefined(last))
			{
				announced = 1;
				level.last_ai_origin = last.origin;
				level notify("last_ai_down", last);
				util::wait_network_frame();
			}
		}
		remaining = 0;
		foreach (zombie in zombies)
		{
			if (dies_in_skip(zombie))
				zombie DoDamage(zombie.health + 666, zombie.origin);
			else if (isdefined(zombie) && IsAlive(zombie) && !IS_TRUE(zombie.ignore_round_robbin_death))
				remaining++;
		}
		if (remaining == 0)
			break;
		wait 0.1;
	}
	foreach (player in GetPlayers())
	{
		if (zm_utility::is_player_valid(player))
			player zm_score::add_to_player_score(SKIP_POINTS);
	}
	if (kind == SKIP_SPECIAL)
		notice_all("ROUND SKIPPED: 400 POINTS, AND THE ROUND'S MAX AMMO", 6);
	else
		notice_all("ROUND SKIPPED: 400 POINTS", 6);
	s.skipping = 0;
}

// At least one enemy the skip can kill: the round's last_ai_down needs one (a special round ends on it), and an enemy
// still in its spawn effect is shielded
function round_has_zombies()
{
	if (!level flag::get("spawn_zombies"))
		return false;
	foreach (zombie in GetAITeamArray(level.zombie_team))
	{
		if (dies_in_skip(zombie))
			return true;
	}
	return false;
}

// Round Robbin's own exceptions
function dies_in_skip(zombie)
{
	if (!isdefined(zombie) || !IsAlive(zombie))
		return false;
	if (IS_TRUE(zombie.ignore_round_robbin_death) || IS_TRUE(zombie.marked_for_death))
		return false;
	return !zm_utility::is_magic_bullet_shield_enabled(zombie);
}

// Each round the player's guns go for two new ones, drawn evenly from the guns the mystery box would give this player
// (zm_magicbox::treasure_chest_canplayerreceiveweapon: in the box, under its wonder weapon limits, a mod's own box rules,
// never a gun the player has), handed over the way the box does (zm_weapons::weapon_give), so Auto Pack-a-Punch
// upgrades them. The box's own pick is not used: a mod can order it so that its first gun is always the same (seen
// with DS4C, which gave one gun a round). Hero weapons, equipment and knives stay.
function random_loadout()
{
	self endon("disconnect");
	level endon("end_game");
	// After the round's own start and, for round 1, after the black screen, which ends with the map handing out its
	// starting pistol; and not while the player is down, out, drinking a perk or holding a power-up's gun
	wait_for_flag("initial_blackscreen_passed");
	wait 1;
	for (i = 0; i < 200 && !self can_swap_guns(); i++)
		wait 0.1;
	if (!self can_swap_guns())
		return;
	candidates = [];
	foreach (weapon in GetArrayKeys(level.zombie_weapons))
	{
		if (zm_magicbox::treasure_chest_canplayerreceiveweapon(self, weapon, undefined))
			candidates[candidates.size] = weapon;
	}
	picks = [];
	while (picks.size < 2 && candidates.size > 0)
	{
		index = RandomInt(candidates.size);
		picks[picks.size] = candidates[index];
		rest = [];
		for (i = 0; i < candidates.size; i++)
		{
			if (i != index)
				rest[rest.size] = candidates[i];
		}
		candidates = rest;
	}
	if (picks.size == 0)
		return;
	foreach (weapon in self GetWeaponsListPrimaries())
	{
		if (!zm_utility::is_hero_weapon(weapon))
			self TakeWeapon(weapon);
	}
	foreach (weapon in picks)
		self zm_weapons::weapon_give(weapon, 0, 1);
}

function can_swap_guns()
{
	if (!zm_utility::is_player_valid(self) || IS_TRUE(self.has_powerup_weapon))
		return false;
	return !isdefined(self.is_drinking) || self.is_drinking <= 0;
}

// Self-revives: one each, back to one every SELF_REVIVE_EVERY rounds
function refill_self_revives()
{
	refilled = 0;
	foreach (player in GetPlayers())
	{
		if (!isdefined(player.xerces_self_revives) || player.xerces_self_revives >= 1)
			continue;
		player.xerces_self_revives = 1;
		player update_revives_row();
		refilled = 1;
	}
	if (refilled)
		notice_all("SELF-REVIVES REFILLED", 6);
}

function self_revive_watch()
{
	self endon("disconnect");
	for (;;)
	{
		self waittill("player_downed");
		self thread self_revive_offer();
	}
}

// A player who goes down with a self-revive holds the use button to get up, through the game's own revive
// (zm_laststand::auto_revive, as solo Quick Revive does). Solo Quick Revive goes first.
function self_revive_offer()
{
	self endon("disconnect");
	level endon("end_game");
	if (!isdefined(self.xerces_self_revives) || self.xerces_self_revives < 1)
		return;
	if (level flag::get("solo_game") && isdefined(self.lives) && self.lives > 0 && self HasPerk("specialty_quickrevive"))
		return;
	self.xerces_prompt = self lui_text("HOLD USE TO REVIVE YOURSELF", ALIGN_CENTER, 340, 470, 600, 26, (1, 1, 1), 1);
	self thread end_game_if_bled_out();
	held = 0;
	revived = 0;
	while (self laststand::player_is_in_laststand())
	{
		if (self UseButtonPressed())
			held += 50;
		else
			held = 0;
		if (held >= SELF_REVIVE_HOLD_MS)
		{
			revived = 1;
			break;
		}
		wait 0.05;
	}
	if (isdefined(self.xerces_prompt))
	{
		self CloseLUIMenu(self.xerces_prompt);
		self.xerces_prompt = undefined;
	}
	if (!revived)
		return;
	self.xerces_self_revives--;
	self update_revives_row();
	self zm_laststand::auto_revive(self);
}

// The game was kept going for this player's self-revive (install_self_revive_checks). If they bleed out instead and
// nobody is up, the game ends here, as it would have when they went down.
function end_game_if_bled_out()
{
	self endon("disconnect");
	self endon("player_revived");
	level endon("end_game");
	self waittill("bled_out");
	wait 0.5;
	foreach (player in GetPlayers())
	{
		if (zm_utility::is_player_valid(player))
			return;
		if (player laststand::player_is_in_laststand() && isdefined(player.xerces_self_revives) && player.xerces_self_revives > 0)
			return;
	}
	level notify("pre_end_game");
	util::wait_network_frame();
	level notify("end_game");
}

// zm::player_damage_override asks these when the last player up goes down: a player who can still self-revive keeps
// the game going (solo: level.check_end_solo_game_override true; co-op: level._game_module_game_end_check false). A
// check the map or mode had is asked first.
function install_self_revive_checks()
{
	s = level.xerces_rounds;
	s.old_solo_end_check = level.check_end_solo_game_override;
	level.check_end_solo_game_override = &solo_game_continues;
	s.old_game_end_check = level._game_module_game_end_check;
	level._game_module_game_end_check = &game_ends;
}

function solo_game_continues()
{
	s = level.xerces_rounds;
	if (isdefined(s.old_solo_end_check) && [[s.old_solo_end_check]]())
		return true;
	return someone_can_self_revive();
}

function game_ends()
{
	s = level.xerces_rounds;
	if (isdefined(s.old_game_end_check) && ![[s.old_game_end_check]]())
		return false;
	return !someone_can_self_revive();
}

// Everyone is down or out when these are asked, so a player who is not out and has a self-revive left can still get up
function someone_can_self_revive()
{
	foreach (player in GetPlayers())
	{
		if (player.sessionstate == "spectator")
			continue;
		if (isdefined(player.xerces_self_revives) && player.xerces_self_revives > 0)
			return true;
	}
	return false;
}

// Round 999: exfil (the game ends as a win) or keep going, by more than half the players; no decision by the round's
// end keeps going
function open_final_vote()
{
	s = level.xerces_rounds;
	s.vote = 1;
	clear_votes();
	notice_all("ROUND 999: EXFIL OR KEEP GOING? VOTE IN THE PAUSE MENU", 15);
}

function final_vote_request(choice)
{
	s = level.xerces_rounds;
	if (!s.round_999 || !s.vote)
	{
		self IPrintLnBold("There is no vote now");
		return;
	}
	if (isdefined(self.xerces_vote))
	{
		self IPrintLnBold("You voted: waiting for the others");
		return;
	}
	self.xerces_vote = choice;
	exfil = count_votes("exfil");
	keep = count_votes("keep");
	if (is_majority(exfil))
		level thread exfil();
	else if (is_majority(keep))
		level thread keep_going();
	else
		notice_all("EXFIL " + exfil + ", KEEP GOING " + keep + ", OF " + GetPlayers().size + " PLAYERS", 10);
}

// The game ends as a win, with Xerces' end screen in place of the game's (install_end_screen)
function exfil()
{
	s = level.xerces_rounds;
	if (!s.vote)
		return;
	s.vote = 0;
	s.exfil = 1;
	clear_votes();
	update_data_text();
	level._supress_survived_screen = 1;
	notice_all("EXFIL", 5);
	wait 2;
	level notify("pre_end_game");
	util::wait_network_frame();
	level notify("end_game");
}

// Every player gets every perk, infinite ammo and infinite points from here on, and every new zombie is a smart one
// (smart_share)
function keep_going()
{
	s = level.xerces_rounds;
	if (!s.vote)
		return;
	s.vote = 0;
	s.endless = 1;
	s.smart_announced = 1;
	clear_votes();
	update_data_text();
	notice_all("KEEP GOING: EVERY PERK, INFINITE AMMO AND POINTS, AND ONLY SMART ZOMBIES", 8);
	level thread endless();
}

function endless()
{
	level endon("end_game");
	tick = 0;
	for (;;)
	{
		foreach (player in GetPlayers())
		{
			if (!zm_utility::is_player_valid(player))
				continue;
			player.score = MAX_SCORE;
			player.pers["score"] = player.score;
			foreach (weapon in player GetWeaponsList(1))
			{
				if (!refills(weapon) || !player HasWeapon(weapon))
					continue;
				if (weapon.clipsize > 0)
					player SetWeaponAmmoClip(weapon, weapon.clipsize);
				player GiveMaxAmmo(weapon);
			}
			if (tick % 4 == 0)
				player give_every_perk();
		}
		tick++;
		wait 0.25;
	}
}

function give_every_perk()
{
	if (!isdefined(level._custom_perks))
		return;
	foreach (perk in GetArrayKeys(level._custom_perks))
	{
		if (!self HasPerk(perk) && !self zm_perks::has_perk_paused(perk))
			self zm_perks::give_perk(perk, 0);
	}
}

// After an exfil, and for a game over past round 255, the end screen is Xerces': the game's own would say 255 rounds.
// zm::end_game leaves its own out when level._supress_survived_screen is set and runs level.custom_end_screen.
function install_end_screen()
{
	s = level.xerces_rounds;
	s.old_end_screen = level.custom_end_screen;
	level.custom_end_screen = &end_screen;
}

function end_screen()
{
	s = level.xerces_rounds;
	if (isdefined(s.old_end_screen))
		level [[s.old_end_screen]]();
	if (!IS_TRUE(level._supress_survived_screen) || (!s.exfil && s.round <= GAME_ROUND_CAP))
		return;
	// Above the game's end-of-game scoreboard, which starts about y 258
	title = "GAME OVER";
	if (s.exfil)
		title = "EXFIL";
	foreach (player in GetPlayers())
	{
		if (s.exfil)
		{
			player lui_text(title, ALIGN_CENTER, 240, 150, 800, 44, (1, 1, 1), 1);
			player lui_text("YOU WIN", ALIGN_CENTER, 240, 196, 800, 26, (0.5, 1, 0.5), 1);
		}
		else
			player lui_text(title, ALIGN_CENTER, 240, 170, 800, 44, (1, 1, 1), 1);
		player lui_text("YOU SURVIVED " + with_commas(s.round) + " ROUNDS", ALIGN_CENTER, 240, 224, 800, 26, (1, 1, 1), 1);
	}
}

// Smart zombies (stage 4): pack hunters, burst sprinters and ambushers, each with its own glow on the head from the
// power-up effects every map loads (zm_powerups: yellow, green and blue; its red one is not on every map). A zombie
// becomes one as it comes into the play area, by the round's share (smart_share). They steer through what the game's
// own chase reads (zm_behavior::zombiefindflesh): v_zombie_custom_goal_pos and n_zombie_custom_goal_radius send a
// zombie to a spot, and the run-cycle override (zombie_utility) sets its speed. Anything else that draws a zombie goes
// first: a Monkey Bomb or other point of interest, something it attacks, a goal or speed override a map or effect set,
// being turned.
function install_smart_zombies()
{
	// zm_spawner::zombie_spawn_init runs these on each new zombie; a map may have set one function rather than a list
	if (isdefined(level._zombie_custom_spawn_logic) && !IsArray(level._zombie_custom_spawn_logic))
	{
		single = level._zombie_custom_spawn_logic;
		level._zombie_custom_spawn_logic = [];
		level._zombie_custom_spawn_logic[0] = single;
	}
	zm_spawner::add_custom_zombie_spawn_logic(&smart_spawn);
}

// "SMART ZOMBIES: YELLOW HUNT IN PACKS, GREEN SPRINT IN BURSTS, BLUE LIE IN WAIT". The colours are split: shared.gsh
// defines YELLOW, GREEN and BLUE as vectors, and the compiler expands them inside strings too (seen in the game on
// 2026-11-07 as "(1, 1, 0) HUNT IN PACKS").
function smart_notice()
{
	return "SMART ZOMBIES: YEL" + "LOW HUNT IN PACKS, GRE" + "EN SPRINT IN BURSTS, BL" + "UE LIE IN WAIT";
}

// How many of each 1,000 new zombies are smart this round: none before round 20, 100 there rising evenly to 990 at
// round 998, and all of them after it or once round 999 has kept going
function smart_share()
{
	s = level.xerces_rounds;
	if (s.endless)
		return 1000;
	if (!s.smart_zombies || s.round < SMART_FIRST_ROUND)
		return 0;
	if (s.round > SMART_LAST_ROUND)
		return 1000;
	// In whole numbers: GSC divides in single-precision floats
	rise = (s.round - SMART_FIRST_ROUND) * (SMART_LAST_SHARE - SMART_FIRST_SHARE);
	return SMART_FIRST_SHARE + int(rise / (SMART_LAST_ROUND - SMART_FIRST_ROUND));
}

// On each new zombie (zm_spawner::zombie_spawn_init); only the plain zombies, not dogs, bosses or other kinds
function smart_spawn()
{
	self endon("death");
	if (!isdefined(self.archetype) || self.archetype != "zombie")
		return;
	if (!IS_TRUE(self.completed_emerging_into_playable_area))
		self waittill("completed_emerging_into_playable_area");
	if (RandomInt(1000) >= smart_share())
		return;
	type = 1 + RandomInt(3);
	self.xerces_smart = type;
	self smart_glow(type);
	if (type == SMART_PACK)
		self pack_hunter();
	else if (type == SMART_BURST)
		self burst_sprinter();
	else
		self ambusher();
}

// A tag_origin model on the zombie's head with its kind's glow, which also lights the zombie: yellow for pack hunters
// (the caution glow), green for burst sprinters, blue for ambushers
function smart_glow(type)
{
	fx = level._effect["powerup_on_caution"];
	if (type == SMART_BURST)
		fx = level._effect["powerup_on"];
	else if (type == SMART_AMBUSH)
		fx = level._effect["powerup_on_solo"];
	if (!isdefined(fx))
		return;
	glow = Spawn("script_model", self GetTagOrigin("j_head"));
	glow SetModel("tag_origin");
	glow LinkTo(self, "j_head");
	glow thread glow_follow(self, fx);
}

// On the glow model: the effect once the model has reached the players, and gone with the zombie (dead, removed or
// turned)
function glow_follow(zombie, fx)
{
	util::wait_network_frame();
	if (isdefined(zombie) && IsAlive(zombie))
		PlayFXOnTag(fx, self, "tag_origin");
	while (isdefined(zombie) && IsAlive(zombie) && zombie.team == level.zombie_team)
		wait 0.5;
	self Delete();
}

// Whether a smart zombie may steer now: in the play area, still the zombies', and nothing else drawing it
function smart_can_steer()
{
	if (!IsAlive(self) || self.team != level.zombie_team || IS_TRUE(level.intermission))
		return false;
	if (!IS_TRUE(self.completed_emerging_into_playable_area) || IS_TRUE(self.ignore_find_flesh))
		return false;
	if (isdefined(self.zombie_poi) || isdefined(self.attackable))
		return false;
	// A goal the map set
	if (isdefined(self.v_zombie_custom_goal_pos) && !self own_goal())
		return false;
	return true;
}

function own_goal()
{
	return isdefined(self.xerces_goal) && isdefined(self.v_zombie_custom_goal_pos) && self.v_zombie_custom_goal_pos == self.xerces_goal;
}

function smart_goal(position, radius)
{
	self.v_zombie_custom_goal_pos = position;
	self.n_zombie_custom_goal_radius = radius;
	self.xerces_goal = position;
}

// Back to the game's own chase; a goal the map set stays
function smart_goal_clear()
{
	if (self own_goal())
	{
		self.v_zombie_custom_goal_pos = undefined;
		self.n_zombie_custom_goal_radius = undefined;
	}
	self.xerces_goal = undefined;
}

// Its speed through the game's run-cycle override. An override something else set (a slowing effect) goes first, and
// a zombie without legs crawls as it does.
function smart_speed(speed)
{
	if (IS_TRUE(self.missinglegs))
		return;
	if (isdefined(self.zombie_move_speed_override))
	{
		if (!isdefined(self.xerces_speed) || self.zombie_move_speed_override != self.xerces_speed)
			return;
		if (self.xerces_speed == speed)
			return;
		// zombie_utility::set_zombie_run_cycle keeps the current speed while an override is set
		self.zombie_move_speed_override = undefined;
	}
	self zombie_utility::set_zombie_run_cycle_override_value(speed);
	self.xerces_speed = speed;
}

// Where a player stands, as the game's own chase uses it
function ground_position(player)
{
	if (isdefined(player.last_valid_position))
		return player.last_valid_position;
	return player.origin;
}

// Pack hunters come to within PACK_HOLD_DIST of the pack's player and hold there until PACK_SIZE of them are waiting
// (or PACK_WAIT_MS passes). Then that pack sprints at the player together; each hunter leaves the last stretch to the
// game's own attack. One the player walks up to stops waiting.
function pack_hunter()
{
	self endon("death");
	s = level.xerces_rounds;
	wave = s.pack_wave;
	self.xerces_pack_waiting = 1;
	since = GetTime();
	hold = undefined;
	while (s.pack_wave == wave)
	{
		if (pack_waiting() >= PACK_SIZE || GetTime() - since > PACK_WAIT_MS)
		{
			// The next pack picks its own player
			s.pack_wave++;
			s.pack_target = undefined;
			break;
		}
		target = pack_target();
		if (isdefined(target) && DistanceSquared(self.origin, target.origin) < PACK_CLOSE_DIST * PACK_CLOSE_DIST)
			break;
		if (isdefined(target) && self smart_can_steer() && DistanceSquared(self.origin, target.origin) < PACK_HOLD_DIST * PACK_HOLD_DIST)
		{
			if (!isdefined(hold))
				hold = self.origin;
			self smart_goal(hold, 64);
		}
		else
		{
			hold = undefined;
			self smart_goal_clear();
		}
		wait SMART_TICK;
	}
	self.xerces_pack_waiting = 0;
	self smart_goal_clear();
	self smart_speed("sprint");
	target = pack_target();
	for (;;)
	{
		if (!isdefined(target) || !zm_utility::is_player_valid(target))
			target = pack_target();
		if (!isdefined(target) || !self smart_can_steer() || DistanceSquared(self.origin, target.origin) < PACK_CLOSE_DIST * PACK_CLOSE_DIST)
			self smart_goal_clear();
		else
			self smart_goal(ground_position(target), 30);
		wait SMART_TICK;
	}
}

function pack_waiting()
{
	count = 0;
	foreach (zombie in GetAITeamArray(level.zombie_team))
	{
		if (IsAlive(zombie) && IS_TRUE(zombie.xerces_pack_waiting))
			count++;
	}
	return count;
}

// The pack's player: kept while it can be attacked, else the one furthest from the other players (in co-op the pack
// picks off whoever is alone)
function pack_target()
{
	s = level.xerces_rounds;
	if (isdefined(s.pack_target) && zm_utility::is_player_valid(s.pack_target))
		return s.pack_target;
	players = [];
	foreach (player in GetPlayers())
	{
		if (zm_utility::is_player_valid(player))
			players[players.size] = player;
	}
	best = undefined;
	best_gap = -1;
	foreach (player in players)
	{
		// The squared distance to the nearest other player; 0 alone
		gap = 0;
		first = 1;
		foreach (other in players)
		{
			if (other == player)
				continue;
			distance = DistanceSquared(player.origin, other.origin);
			if (first || distance < gap)
				gap = distance;
			first = 0;
		}
		if (gap > best_gap)
		{
			best = player;
			best_gap = gap;
		}
	}
	s.pack_target = best;
	return best;
}

// Burst sprinters walk, and within BURST_RANGE of their player sprint for a moment every few seconds
function burst_sprinter()
{
	self endon("death");
	self smart_speed("walk");
	for (;;)
	{
		wait RandomFloatRange(2.5, 4.5);
		target = self.favoriteenemy;
		if (!isdefined(target) || !IsPlayer(target) || DistanceSquared(self.origin, target.origin) > BURST_RANGE * BURST_RANGE)
			continue;
		self smart_speed("sprint");
		wait RandomFloatRange(1.5, 2.5);
		self smart_speed("walk");
	}
}

// Ambushers run to a spot near their player that the player cannot see and wait there; they strike, sprinting, when
// the player comes close or turns their back, or when the wait runs out
function ambusher()
{
	self endon("death");
	target = self.favoriteenemy;
	if (!isdefined(target) || !IsPlayer(target))
		target = zm_utility::get_closest_valid_player(self.origin);
	spot = undefined;
	if (isdefined(target))
		spot = self ambush_spot(target);
	if (isdefined(spot))
	{
		self smart_speed("run");
		since = GetTime();
		while (GetTime() - since < AMBUSH_TRAVEL_MS && zm_utility::is_player_valid(target))
		{
			if (DistanceSquared(self.origin, spot) < 48 * 48 || self ambush_strikes(target, 0))
				break;
			if (self smart_can_steer())
				self smart_goal(spot, 32);
			else
				self smart_goal_clear();
			wait SMART_TICK;
		}
		since = GetTime();
		hold = self.origin;
		while (GetTime() - since < AMBUSH_WAIT_MS && zm_utility::is_player_valid(target))
		{
			if (self ambush_strikes(target, 1))
				break;
			if (self smart_can_steer())
				self smart_goal(hold, 32);
			else
				self smart_goal_clear();
			wait SMART_TICK;
		}
	}
	self smart_goal_clear();
	self smart_speed("sprint");
}

// Whether an ambusher strikes now: its player is close, or (when it is waiting) has turned their back within
// AMBUSH_BACK_DIST
function ambush_strikes(target, waiting)
{
	distance = DistanceSquared(self.origin, target.origin);
	if (distance < AMBUSH_STRIKE_DIST * AMBUSH_STRIKE_DIST)
		return true;
	if (!waiting || distance > AMBUSH_BACK_DIST * AMBUSH_BACK_DIST)
		return false;
	angles = target GetPlayerAngles();
	facing = AnglesToForward((0, angles[1], 0));
	toward = VectorNormalize((self.origin[0] - target.origin[0], self.origin[1] - target.origin[1], 0));
	return VectorDot(facing, toward) < 0;
}

// A spot on the navigation mesh AMBUSH_MIN_DIST to AMBUSH_MAX_DIST from the player that the player cannot see: of the
// nearest AMBUSH_SPOTS_CHECKED to the zombie, the first hidden one
function ambush_spot(target)
{
	query = PositionQuery_Source_Navigation(target.origin, AMBUSH_MIN_DIST, AMBUSH_MAX_DIST, 128, 96);
	if (!isdefined(query) || !isdefined(query.data) || query.data.size == 0)
		return undefined;
	eye = target GetEye();
	checked = [];
	for (i = 0; i < AMBUSH_SPOTS_CHECKED && i < query.data.size; i++)
	{
		// The nearest spot to the zombie not checked yet
		best = undefined;
		best_distance = 0;
		for (j = 0; j < query.data.size; j++)
		{
			if (IS_TRUE(checked[j]))
				continue;
			distance = DistanceSquared(self.origin, query.data[j].origin);
			if (!isdefined(best) || distance < best_distance)
			{
				best = j;
				best_distance = distance;
			}
		}
		if (!isdefined(best))
			break;
		checked[best] = 1;
		spot = query.data[best].origin;
		if (!SightTracePassed(eye, spot + (0, 0, 50), 0, target))
			return spot;
	}
	return undefined;
}
