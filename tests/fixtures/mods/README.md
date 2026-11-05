# Fixture mods

Loose-file mods for the both-apply proof (docs/DESIGN.md 6.9). Everything here
is written for Xerces; nothing comes from the game.

- `../stock/` stands in for the game's own copy of the test table.
- `fixture.probe` provides `xerces_test_probe.gsc::record`, which appends a tag
  to the `xerces_test_hooks` dvar.
- `fixture.alpha` and `fixture.beta` both depend on the probe. Each adds a row
  to, and changes a different row of, `gamedata/xerces_test/both_apply.csv`,
  and wraps `record` with a hook that appends its own id and calls the next
  link.

With the stack `fixture.alpha, fixture.beta` the merged table has both new rows
and both edits, and a call to `record("probe")` sets the dvar to
`fixture.alpha;fixture.beta;probe;` (the highest mod is outermost). The unit
tests check the planner, merge and chain order; `xerces_cli test run
both-apply` checks the same in the game once the loose loader exists (M2).
