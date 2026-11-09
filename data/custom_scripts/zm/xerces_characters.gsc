// Xerces' character slots for Zombies (docs/DESIGN.md 7.10): the player picks any character the game's character table has
// (DS4C's list, the base characters, whatever a character mod such as Hyperdimension Neptunia gives them) in the pause menu's
// CHARACTERS screen (data/ui_scripts/xerces_characters). The menu sends "xerces_char:<index>:<style>" as a menu response
// under StartMenu_Main, or "xerces_char:default" to go back to the character the match gave the player.
//
// The character is set with SetCharacterBodyType and SetCharacterBodyStyle, which is how the game itself (and DS4C) does
// it. A mod's own character script would put its own pick back, so the player is locked: src/client/xerces/character_slots.cpp
// skips those calls from other scripts while the player's bit is set in xerces_char_lock, and lets these calls through
// while xerces_char_apply is 1 (set here around them, with no wait in between).
//
// The host's last pick is kept in the saved dvar xerces_char_saved (index * 16 + style + 1) by the menu and applied when the
// host spawns, so it is the same next match. Other players pick again each match.
#using scripts\shared\callbacks_shared;
#using scripts\shared\flag_shared;
#using scripts\shared\system_shared;
#using scripts\shared\util_shared;

#insert scripts\shared\shared.gsh;

#namespace xerces_characters;

REGISTER_SYSTEM("xerces_characters", &__init__, undefined)

// How long after a spawn the pick is put on again: after the game's and the mods' own spawn scripts
#define APPLY_DELAY 1.5
#define MAX_INDEX 63
#define MAX_STYLE 15

function __init__()
{
	callback::on_connect(&on_player_connect);
	callback::on_spawned(&on_player_spawned);
}

function on_player_connect()
{
	// The dvar outlives the match: a player who joins a new match starts unlocked
	self set_lock(false);
	self thread menu_responses();
}

function on_player_spawned()
{
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
	if (index < 0 || index > MAX_INDEX || style < 0 || style > MAX_STYLE)
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

function apply(index, style)
{
	if (!isdefined(self.xerces_char_default))
		self.xerces_char_default = self GetCharacterBodyType();
	self.xerces_char_index = index;
	self.xerces_char_style = style;
	self set_lock(true);
	SetDvar("xerces_char_apply", 1);
	self SetCharacterBodyType(index);
	self SetCharacterBodyStyle(style);
	SetDvar("xerces_char_apply", 0);
}

// Back to the character the match gave the player. The mods' own scripts may act on the player again from here.
function reset()
{
	self.xerces_char_index = undefined;
	self.xerces_char_style = undefined;
	self set_lock(false);
	if (isdefined(self.xerces_char_default))
	{
		self set_lock(true);
		SetDvar("xerces_char_apply", 1);
		self SetCharacterBodyType(self.xerces_char_default);
		self SetCharacterBodyStyle(0);
		SetDvar("xerces_char_apply", 0);
		self set_lock(false);
	}
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
		if (words[1] == "default")
		{
			self reset();
			continue;
		}
		if (words.size < 3)
			continue;
		index = Int(words[1]);
		style = Int(words[2]);
		if (index < 0 || index > MAX_INDEX || style < 0 || style > MAX_STYLE)
			continue;
		self apply(index, style);
	}
}
