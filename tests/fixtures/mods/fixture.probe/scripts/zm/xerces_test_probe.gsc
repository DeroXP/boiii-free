// Xerces both-apply probe (docs/DESIGN.md 6.9). Hooks from the fixture mods
// wrap record(); each appends its id before calling the next link.
#namespace xerces_test_probe;

function record(tag)
{
	current = GetDvarString("xerces_test_hooks");
	SetDvar("xerces_test_hooks", current + tag + ";");
}
