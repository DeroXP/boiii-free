// Fixture Alpha's hook on xerces_test_probe::record (docs/DESIGN.md 6.9).
#namespace xerces_alpha;

function on_record(tag)
{
	current = GetDvarString("xerces_test_hooks");
	SetDvar("xerces_test_hooks", current + "fixture.alpha;");
	return xerces_next(tag);
}
