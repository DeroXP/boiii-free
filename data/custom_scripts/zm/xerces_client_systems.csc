// Xerces' client side of the shared client system (src/client/xerces/client_systems.cpp, docs/research/engine-re.md
// "Client systems"). The game has eight client systems; when a level's scripts and mods register more, the server
// sends the states of the one in slot 7 and every later one through the system "xerces_mux" as "<name>|<state>".
// This hands each to the client script registered under <name>, as the game does for its own eight.
//
// Test (off unless the dvar xerces_cs_test is 1 when the level starts; the server side is in xerces_client_systems.gsc):
// ten extra systems "xerces_cs_test0" to "xerces_cs_test9"; each state that reaches this side is put in the dvar of the
// same name, which the console then shows.
#using scripts\shared\system_shared;
#using scripts\shared\util_shared;

#insert scripts\shared\shared.gsh;

#namespace xerces_client_systems;

REGISTER_SYSTEM("xerces_client_systems", &__init__, undefined)

#define TEST_SYSTEMS 10

function __init__()
{
	util::registerSystem("xerces_mux", &on_shared_state);
	if (GetDvarInt("xerces_cs_test") == 1)
	{
		for (i = 0; i < TEST_SYSTEMS; i++)
			util::registerSystem("xerces_cs_test" + i, &on_test_state);
	}
}

// "<name>|<state>" for the system registered under <name>
function on_shared_state(localClientNum, state)
{
	if (!isdefined(state))
		return;
	parts = StrTok(state, "|");
	if (!isdefined(parts) || parts.size < 1)
		return;
	name = parts[0];
	value = "";
	if (state.size > name.size + 1)
		value = GetSubStr(state, name.size + 1);
	if (!isdefined(level._systemStates) || !isdefined(level._systemStates[name]) ||
		!isdefined(level._systemStates[name].callback))
		return;
	[[level._systemStates[name].callback]](localClientNum, value);
}

function on_test_state(localClientNum, state)
{
	parts = StrTok(state, ":");
	if (isdefined(parts) && parts.size == 2)
		SetDvar(parts[0], parts[1]);
}
