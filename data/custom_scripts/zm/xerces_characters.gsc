// Xerces' character slots for Zombies (docs/DESIGN.md 7.10, 7.12, 7.13): the player picks any character the game's character
// table has (DS4C's list, the base characters, every other character mod's and the game's own as extra outfits) in the pause
// menu's CHARACTERS screen (data/ui_scripts/xerces_characters). The menu sends "xerces_char:<index>:<style>" as a menu
// response under StartMenu_Main, or "xerces_char:default" to go back to the character the match gave the player.
//
// The character is set with SetCharacterBodyType and SetCharacterBodyStyle, which is how the game itself (and DS4C) does
// it. A mod's own character script would put its own pick back, so the player is locked: src/client/xerces/character_slots.cpp
// skips those calls from other scripts while the player's bit is set in xerces_char_lock, and lets these calls through
// while xerces_char_apply is 1 (set here around them, with no wait in between).
//
// The host's last pick is kept in the saved dvar xerces_char_saved (index * 16 + style + 1) by the menu and applied when the
// host spawns, so it is the same next match. Other players pick again each match.
//
// Preview (DESIGN.md 7.13): the screen shows the player's own character in the world, from the front, and puts each
// character on as it is moved over. The screen is opened from here ("xerces_char:open", sent as the pause menu closes), so
// the match runs under it; for a player alone in the match the zombies stand still meanwhile and the player cannot be hurt,
// as under the pause menu. The screen says it is still open every two seconds; when it stops saying so the preview ends
// by itself.
#using scripts\shared\callbacks_shared;
#using scripts\shared\flag_shared;
#using scripts\shared\system_shared;
#using scripts\shared\util_shared;

#insert scripts\shared\shared.gsh;

#namespace xerces_characters;

REGISTER_SYSTEM("xerces_characters", &__init__, undefined)

// How long after a spawn the pick is put on again: after the game's and the mods' own spawn scripts
#define APPLY_DELAY 1.5
// How long the third-person view stays after a pick
#define SHOW_OFF_TIME 4
#define MAX_INDEX 15
#define MAX_STYLE 15
// How long the screen may stay silent before the preview ends by itself
#define PREVIEW_TIMEOUT 6
// The preview's camera: in front of the player, this far at most and at least (it comes closer in a tight spot), at this
// height, looking at this height; the share of the distance it stands to the side, so the character is beside the list
// and not behind it; and how long it stays on a pick after the screen closes
#define CAMERA_DISTANCE 105
#define CAMERA_MIN 45
#define CAMERA_HEIGHT 50
#define CAMERA_LOOK_HEIGHT 40
#define CAMERA_SIDE 0.27
#define LINGER_TIME 1.5

function __init__()
{
	callback::on_connect(&on_player_connect);
	callback::on_spawned(&on_player_spawned);
	level thread drop_failed_client_systems();
}

// The game has room for eight client systems (the channels a level's scripts use to tell its client scripts something).
// One registered after that gets the id -1 and stays in the list, and the first script that sets its state ends the
// match: "ClientSysSetState - state index (-1) out of bounds (0 - 8)". Seen on 2026-11-12 on Kino der Toten with eight
// mods stacked: the map needs four (musicCmd, levelNotify, lsm, box_indicator), DS4C takes three, two more came from
// other mods, so the map's box lights (box_indicator) had none, and the match ended when the power came on. The game's
// own util::setClientSysState leaves out a system that is not in the list, so the ones without an id are taken out of
// it: that map's lights then do not show where the box is, and the match goes on. This script is here for every Zombies
// level, which is why it lives in this file.
function drop_failed_client_systems()
{
	// after every script registered its own, before anything is turned on; and once more for a late one
	for (pass = 0; pass < 2; pass++)
	{
		wait 3 + pass * 9;
		if (!isdefined(level._clientSys))
			continue;
		foreach (name in GetArrayKeys(level._clientSys))
		{
			system = level._clientSys[name];
			if (isdefined(system) && isdefined(system.sysID) && system.sysID < 0)
				level._clientSys[name] = undefined;
		}
	}
}

function on_player_connect()
{
	// The dvar outlives the match: a player who joins a new match starts unlocked
	self set_lock(false);
	self thread menu_responses();
}

function on_player_spawned()
{
	// a spawn while the screen was open (a revive, a new round's respawn) starts from the plain view
	self preview_end(false);
	self linger_end();
	self thread apply_after_spawn();
}

function apply_after_spawn()
{
	self notify("xerces_char_spawn");
	self endon("xerces_char_spawn");
	self endon("disconnect");
	level flag::wait_till("initial_blackscreen_passed");
	wait APPLY_DELAY;
	if (!isdefined(self.xerces_char_index))
		self load_saved();
	if (isdefined(self.xerces_char_index))
		self apply(self.xerces_char_index, self.xerces_char_style);
}

// Whether the table in use has this character and outfit. Xerces' game side says how many characters it has
// (xerces_char_bodies; 0 when it has not counted them: the base game's nine then); the game says how many outfits.
function valid(index, style)
{
	if (index < 0 || index > MAX_INDEX || style < 0 || style > MAX_STYLE)
		return false;
	bodies = GetDvarInt("xerces_char_bodies", 0);
	if (bodies < 1)
		bodies = 9;
	if (index >= bodies)
		return false;
	count = GetCharacterBodyModelCount(index, CurrentSessionMode());
	return isdefined(count) && style < count;
}

// The host's saved pick (the server's dvars are the host's); only for the first player
function load_saved()
{
	if (self GetEntityNumber() != 0)
		return;
	saved = GetDvarInt("xerces_char_saved", 0);
	if (saved < 1)
		return;
	index = Int((saved - 1) / 16);
	style = (saved - 1) - (index * 16);
	// saved with other mods loaded: that character or outfit may not be there now
	if (!valid(index, style))
		return;
	self.xerces_char_index = index;
	self.xerces_char_style = style;
}

function set_lock(on)
{
	bit = 1 << self GetEntityNumber();
	lock = GetDvarInt("xerces_char_lock", 0);
	if (on)
		lock = lock | bit;
	else if ((lock & bit) != 0)
		lock = lock - bit;
	SetDvar("xerces_char_lock", lock);
}

// What the player wears now, to put it on again later: the character and its body's model. The game has no call that
// says which outfit is on, so the outfit is found again by its model.
function note_look()
{
	look = SpawnStruct();
	look.index = self GetCharacterBodyType();
	look.model = self GetCharacterBodyModel();
	return look;
}

// The player stays locked afterwards; the caller unlocks when the mods' own scripts may act again
function wear(index, style)
{
	self set_lock(true);
	SetDvar("xerces_char_apply", 1);
	self SetCharacterBodyType(index);
	self SetCharacterBodyStyle(style);
	SetDvar("xerces_char_apply", 0);
}

// Puts a noted look on again: the character, then the first of its outfits whose body is that model (its own outfits come
// before the ones Xerces added, so a look the match or a mod gave is found before another mod's with the same model name)
function wear_look(look)
{
	if (!isdefined(look) || !isdefined(look.index) || !valid(look.index, 0))
		return;
	self set_lock(true);
	SetDvar("xerces_char_apply", 1);
	self SetCharacterBodyType(look.index);
	found = false;
	if (isdefined(look.model))
	{
		count = GetCharacterBodyModelCount(look.index, CurrentSessionMode());
		for (style = 0; style < count && style <= MAX_STYLE && !found; style++)
		{
			self SetCharacterBodyStyle(style);
			model = self GetCharacterBodyModel();
			if (isdefined(model) && model == look.model)
				found = true;
		}
	}
	if (!found)
		self SetCharacterBodyStyle(0);
	SetDvar("xerces_char_apply", 0);
}

function apply(index, style)
{
	if (!valid(index, style))
		return;
	if (!isdefined(self.xerces_char_default))
		self.xerces_char_default = self note_look();
	self.xerces_char_index = index;
	self.xerces_char_style = style;
	self wear(index, style);
}

// Back to the character the match gave the player. The mods' own scripts may act on the player again from here.
function reset()
{
	self.xerces_char_index = undefined;
	self.xerces_char_style = undefined;
	if (isdefined(self.xerces_char_default))
		self wear_look(self.xerces_char_default);
	self.xerces_char_default = undefined;
	self set_lock(false);
}

// In last stand (the game's own test, laststand_shared.gsc)
function is_down()
{
	return isdefined(self.revivetrigger) || IS_TRUE(self.laststand);
}

function preview_begin()
{
	if (isdefined(self.xerces_preview))
		return;
	self linger_end();
	self.xerces_preview = SpawnStruct();
	self.xerces_preview.look = self note_look();
	self.xerces_preview.heard = GetTime();
	self.xerces_preview.safe = false;
	self preview_safety();
	self notify("xerces_char_show_off");
	self SetClientThirdPerson(0);
	self camera_on(self.xerces_preview);
	self thread preview_watch();
	self thread preview_left(self.xerces_preview);
}

// The camera goes in front of the player, who is turned to where there is most room: the game's own third-person view
// only looks from behind (SetClientThirdPersonAngle changes nothing), so this is a scripted camera aimed at a point
// beside the player's chest
function camera_on(preview)
{
	preview.angles = self GetPlayerAngles();
	eye = self.origin + (0, 0, CAMERA_HEIGHT);
	best_yaw = preview.angles[1];
	best_room = 0;
	for (i = 0; i < 8; i++)
	{
		yaw = preview.angles[1] + i * 45;
		trace = BulletTrace(eye, eye + AnglesToForward((0, yaw, 0)) * (CAMERA_DISTANCE + 24), false, self);
		room = Distance(eye, trace["position"]);
		// the way the player looks wins unless another has clearly more room
		if (room > best_room + 8)
		{
			best_room = room;
			best_yaw = yaw;
		}
	}
	distance = best_room - 16;
	if (distance > CAMERA_DISTANCE)
		distance = CAMERA_DISTANCE;
	if (distance < CAMERA_MIN)
		distance = CAMERA_MIN;
	self SetPlayerAngles((0, best_yaw, 0));
	forward = AnglesToForward((0, best_yaw, 0));
	right = AnglesToRight((0, best_yaw, 0));
	side = distance * CAMERA_SIDE;
	look_at = self.origin + (0, 0, CAMERA_LOOK_HEIGHT) + right * side;
	from = self.origin + (0, 0, CAMERA_HEIGHT) + forward * distance + right * side;
	// Aimed at an entity of its own. The angles given to CameraSetPosition were followed with DS4C on top and were not
	// with Girls' Frontline, where the camera looked the way the player did and showed the street (2026-11-12). The
	// target is a model (tag_origin), which the player's game is told about; it is not told about a script_origin, and
	// the camera turned away from one.
	preview.target = Spawn("script_model", look_at);
	preview.target SetModel("tag_origin");
	self SetClientUIVisibilityFlag("hud_visible", 0);
	self thread camera_start(preview, from, look_at);
}

// The camera goes on a moment after its target was made, when the player's game has it. On its own thread: a wait in
// the thread that hears the screen would miss what the screen says meanwhile.
function camera_start(preview, from, look_at)
{
	self endon("disconnect");
	wait 0.1;
	if (IS_TRUE(preview.off) || !isdefined(preview.target))
		return;
	self CameraSetPosition(from, VectorToAngles(look_at - from));
	self CameraSetLookAt(preview.target);
	self CameraActivate(true);
	// The player's own body is drawn for them in the third-person state
	self SetClientThirdPerson(1);
	// Once more half a second later, the same: once (2026-11-13) the camera did not come on while the third-person state
	// did, and the game's own view showed the player from behind. Most likely the player's game did not know the target
	// yet. Set again it is the same camera, so a camera that came on does not move.
	wait 0.5;
	if (IS_TRUE(preview.off) || !isdefined(preview.target))
		return;
	self CameraSetPosition(from, VectorToAngles(look_at - from));
	self CameraSetLookAt(preview.target);
	self CameraActivate(true);
}

function camera_off(preview)
{
	preview.off = true;
	self CameraActivate(false);
	self SetClientThirdPerson(0);
	if (isdefined(preview.target))
	{
		preview.target Delete();
		preview.target = undefined;
	}
	if (isdefined(preview.angles))
		self SetPlayerAngles(preview.angles);
	self SetClientUIVisibilityFlag("hud_visible", 1);
}

// A player alone in the match is safe while the screen is open, as under the pause menu, which a solo match stands still
// under: the player cannot be hurt and is not chased, the zombies stand where they are and no new ones come. With company
// the match goes on as under any menu.
function preview_safety()
{
	alone = GetPlayers().size == 1;
	if (alone && !self.xerces_preview.safe)
	{
		self.xerces_preview.safe = true;
		self.xerces_preview.ignoreme = self.ignoreme;
		self EnableInvulnerability();
		self.ignoreme = true;
		self.xerces_preview.spawning = level flag::exists("spawn_zombies") && level flag::get("spawn_zombies");
		if (self.xerces_preview.spawning)
			level flag::clear("spawn_zombies");
		self.xerces_preview.frozen = [];
	}
	else if (!alone && self.xerces_preview.safe)
		self preview_unsafe(self.xerces_preview);
	if (self.xerces_preview.safe)
		freeze_zombies(self.xerces_preview);
}

// Every zombie not yet held is held where it stands (one that spawned as the screen opened is caught a second later)
function freeze_zombies(preview)
{
	team = "axis";
	if (isdefined(level.zombie_team))
		team = level.zombie_team;
	zombies = GetAITeamArray(team);
	foreach (zombie in zombies)
	{
		if (!isdefined(zombie) || !IsAlive(zombie) || isdefined(zombie.xerces_frozen))
			continue;
		zombie.xerces_frozen = true;
		zombie SetEntityPaused(true);
		preview.frozen[preview.frozen.size] = zombie;
	}
}

function preview_unsafe(preview)
{
	if (!preview.safe)
		return;
	preview.safe = false;
	foreach (zombie in preview.frozen)
	{
		if (!isdefined(zombie))
			continue;
		zombie.xerces_frozen = undefined;
		zombie SetEntityPaused(false);
	}
	preview.frozen = [];
	if (preview.spawning)
		level flag::set("spawn_zombies");
	self DisableInvulnerability();
	self.ignoreme = preview.ignoreme;
}

// Ends the preview. Without a pick the player gets back what they wore when it began, and the view at once; with one
// the camera stays on the new character for a moment.
function preview_end(picked)
{
	if (!isdefined(self.xerces_preview))
		return;
	before = self.xerces_preview;
	self.xerces_preview = undefined;
	self notify("xerces_char_preview_end");
	// The look DEFAULT gives back: noted here, from before the preview. apply() notes what the player wears when it
	// runs, and after a preview that is the character under the selection, not the match's.
	if (!isdefined(self.xerces_char_default) && !isdefined(self.xerces_char_index))
		self.xerces_char_default = before.look;
	if (!picked)
	{
		if (isdefined(self.xerces_char_index))
			self wear(self.xerces_char_index, self.xerces_char_style);
		else
		{
			self wear_look(before.look);
			self set_lock(false);
		}
		self preview_finish(before);
		return;
	}
	self.xerces_linger = before;
	self thread linger(before);
}

function preview_finish(preview)
{
	self notify("xerces_char_preview_done");
	self preview_unsafe(preview);
	self camera_off(preview);
}

// The player left with the preview on. Nothing here is called on the player, who is gone: the camera's target is
// deleted, and the zombies and the spawning that were held for a player alone go on (a server that lives on without
// them would otherwise keep them held).
function preview_left(preview)
{
	self endon("xerces_char_preview_done");
	self waittill("disconnect");
	if (isdefined(preview.target))
		preview.target Delete();
	if (!IS_TRUE(preview.safe))
		return;
	if (isdefined(preview.frozen))
	{
		foreach (zombie in preview.frozen)
		{
			if (!isdefined(zombie))
				continue;
			zombie.xerces_frozen = undefined;
			zombie SetEntityPaused(false);
		}
	}
	if (IS_TRUE(preview.spawning))
		level flag::set("spawn_zombies");
}

function linger(preview)
{
	self endon("disconnect");
	self endon("xerces_char_linger_end");
	wait LINGER_TIME;
	self.xerces_linger = undefined;
	self preview_finish(preview);
}

function linger_end()
{
	if (!isdefined(self.xerces_linger))
		return;
	preview = self.xerces_linger;
	self.xerces_linger = undefined;
	self notify("xerces_char_linger_end");
	self preview_finish(preview);
}

function preview_watch()
{
	self endon("disconnect");
	self endon("xerces_char_preview_end");
	for (;;)
	{
		wait 1;
		if (!isdefined(self.xerces_preview))
			return;
		if (GetTime() - self.xerces_preview.heard > PREVIEW_TIMEOUT * 1000 || !IsAlive(self) || self is_down()
			|| IS_TRUE(level.intermission))
		{
			// On a thread of its own: preview_end notifies the very event this thread ends on, and a thread that
			// notifies its own endon stops there, before the look, the camera and the zombies were put back (found
			// by a second reading, 2026-11-12; with PREVIEW_TIMEOUT 1 the camera stayed on for good)
			self thread preview_end(false);
			return;
		}
		self preview_safety();
	}
}

// A pick made in the screen: the third-person view for a few seconds more, so the player sees the character on them
function show_off()
{
	self notify("xerces_char_show_off");
	self endon("xerces_char_show_off");
	self endon("disconnect");
	wait 0.3;
	self SetClientThirdPerson(1);
	wait SHOW_OFF_TIME;
	self SetClientThirdPerson(0);
}

function menu_responses()
{
	self endon("disconnect");
	for (;;)
	{
		self waittill("menuresponse", menu, response);
		if (menu != "StartMenu_Main")
			continue;
		words = StrTok(response, ":");
		if (words.size < 2 || words[0] != "xerces_char")
			continue;
		if (words[1] == "open")
		{
			// a player who is down lies on the ground, out of the camera's sight (seen 2026-11-12): the list works
			// for them, without the view
			if (!IsAlive(self) || self is_down() || IS_TRUE(level.intermission))
				continue;
			self preview_begin();
			continue;
		}
		if (words[1] == "alive")
		{
			if (isdefined(self.xerces_preview))
				self.xerces_preview.heard = GetTime();
			continue;
		}
		if (words[1] == "close")
		{
			self preview_end(false);
			continue;
		}
		if (words[1] == "preview")
		{
			if (words.size < 4 || !isdefined(self.xerces_preview))
				continue;
			index = Int(words[2]);
			style = Int(words[3]);
			if (!valid(index, style))
				continue;
			self.xerces_preview.heard = GetTime();
			self wear(index, style);
			continue;
		}
		if (words[1] == "default")
		{
			previewed = isdefined(self.xerces_preview);
			self preview_end(true);
			self reset();
			if (!previewed)
				self thread show_off();
			continue;
		}
		// A pick is two numbers. A word that is not one (a response this script does not know, from a newer screen)
		// would read as 0 and put the first character on.
		if (words.size < 3 || words[1] != "" + Int(words[1]) || words[2] != "" + Int(words[2]))
			continue;
		index = Int(words[1]);
		style = Int(words[2]);
		if (!valid(index, style))
		{
			// the screen has closed and says no more: without this the camera stayed until the watch gave up
			self preview_end(false);
			continue;
		}
		// with the preview the camera stays on the pick a moment; without it (the screen under the pause menu) the
		// third-person view shows it
		previewed = isdefined(self.xerces_preview);
		self preview_end(true);
		self apply(index, style);
		if (!previewed)
			self thread show_off();
	}
}
