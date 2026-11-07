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
#using scripts\zm\_zm_score;
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

#define NO_PERK_LIMIT 99
#define WHEEL_COST 2500
// What the mystery box costs during a Fire Sale (zm_magicbox::fire_sale_fix)
#define WHEEL_FIRE_SALE_COST 10
#define MIN_BET 100
// The game's own points cap: a score set higher reads back as this (measured 2026-11-07)
#define MAX_SCORE 4194303

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
	if (s.start_points < 0)
		s.start_points = 0;
	// The round as of its start_of_round (the game raises level.round_number as the last one ends), and how many
	// rounds have started, which keeps counting where the round number stops (255)
	s.round = 1;
	s.rounds_started = 0;

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

// Once per player, from the end of the black screen: the data for Xerces' menus, the round HUD and the settings message
function first_spawn()
{
	self endon("disconnect");
	wait_for_flag("initial_blackscreen_passed");
	self.xerces_data = self lui_text(data_text(), ALIGN_LEFT, 0, 0, 10, 10, (1, 1, 1), 0);
	if (level.xerces_rounds.round_hud)
		self create_round_hud();
	wait 4;
	self show_settings();
}

// "XERCES <mask> <starting points> <round>", which data/ui_scripts/xerces_settings reads
function data_text()
{
	s = level.xerces_rounds;
	return "XERCES " + s.mask + " " + s.start_points + " " + s.round;
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

// A round runs from zm::round_think's start_of_round to its end_of_round
function round_watch()
{
	level endon("end_game");
	s = level.xerces_rounds;
	for (;;)
	{
		level waittill("start_of_round");
		s.round = level.round_number;
		s.rounds_started++;
		s.round_start = GetTime();
		s.round_seconds = 0;
		s.round_running = 1;
		// The round, for the coin flip's payout in Xerces' menus
		foreach (player in GetPlayers())
		{
			if (isdefined(player.xerces_data))
				player SetLUIMenuData(player.xerces_data, "text", data_text());
		}

		level waittill("end_of_round");
		s.round_seconds = int((GetTime() - s.round_start) / 1000);
		s.round_running = 0;
	}
}

// Zombies left and the round time, top left. Between rounds it shows the last round's time.
function create_round_hud()
{
	grey = (0.75, 0.75, 0.75);
	white = (1, 1, 1);
	hud = SpawnStruct();
	hud.left_label = self lui_text("ZOMBIES LEFT", ALIGN_LEFT, 24, 20, 150, 24, grey, 1);
	hud.left = self lui_text("", ALIGN_LEFT, 180, 20, 160, 24, white, 1);
	hud.time_label = self lui_text("", ALIGN_LEFT, 24, 50, 150, 24, grey, 1);
	hud.time = self lui_text("", ALIGN_LEFT, 180, 50, 160, 24, white, 1);
	hud.shown_left = "";
	hud.shown_label = "";
	hud.shown_time = "";
	self.xerces_hud = hud;
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
			if (isdefined(player.xerces_hud))
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
		player lui_close(array(hud.left_label, hud.left, hud.time_label, hud.time));
	}
}

// Requests from Xerces' menus on the player's PC (data/ui_scripts/xerces_settings): "xerces_wheel" and
// "xerces_flip:<points|all>", sent as menu responses under StartMenu_Main. A response ends at its first space
// (seen in the game on 2026-11-07), so the parts are joined by ":".
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
