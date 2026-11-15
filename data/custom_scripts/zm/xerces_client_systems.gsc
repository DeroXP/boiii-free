// Test of the shared client system (src/client/xerces/client_systems.cpp; the client side is
// xerces_client_systems.csc). Off unless the dvar xerces_cs_test is 1 when the level starts: then ten extra systems
// "xerces_cs_test0" to "xerces_cs_test9" are registered after the level's own (so the game's eight are full and these
// share slot 7), and every two seconds each is set to "xerces_cs_test<i>:<count>", which the client script puts in the
// dvar xerces_cs_test<i>.
#using scripts\shared\system_shared;
#using scripts\shared\util_shared;

#insert scripts\shared\shared.gsh;

#namespace xerces_client_systems;

REGISTER_SYSTEM("xerces_client_systems", &__init__, undefined)

#define TEST_SYSTEMS 10

function __init__()
{
	if (GetDvarInt("xerces_cs_test") != 1)
		return;
	level thread test();
}

function test()
{
	// after the level's own systems
	wait 2;
	for (i = 0; i < TEST_SYSTEMS; i++)
		util::registerClientSys("xerces_cs_test" + i);
	for (count = 1; ; count++)
	{
		for (i = 0; i < TEST_SYSTEMS; i++)
			util::setClientSysState("xerces_cs_test" + i, "xerces_cs_test" + i + ":" + count);
		wait 2;
	}
}
