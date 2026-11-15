# Changelog

Every Xerces release and what changed for players, newest first, in the
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) way. The website's
changelog page is made from this file (tools/site/build_site.py). Betas went to
testers by private link; the first stable release is in preparation.

## [Unreleased]

### Added

- **No mod is left without its map effects.** The game has room for eight client
  systems, the channels a map's and a mod's scripts use to tell your game
  something (Kino der Toten's mystery box lights, DS4C's on-screen text). A big
  stack needs more; the ones past eight now share one of the game's slots
  instead of being left out.
- **Mods' own client scripts run.** Xerces' custom client scripts were linked at
  a time the game never ran them; they now load with the client's own scripts.
- A website, built from this repository's own documents.

### Changed

- **Faster loads are on by default**: the loading screen draws every frame, a
  match you start alone skips the lobby's waits, a Zombies level's first
  seconds run 4x while the game holds you at the start, and the game starts
  about 5 s sooner. Settings > Launch, "Faster loads", turns them off.
- **Frames and mouse input skip the game's millisecond waits by default.**
  Settings > Display turns it off.
- Safe mode turns both off, so a crash they caused is not met again with mods
  off.

### Fixed

- A `map` typed in the console from inside a level no longer crashes the game's
  server going into Moon (from Kino der Toten): the game looked up Moon's
  character table before Moon had loaded and kept the empty answer.

## [0.1.0-beta.27] - 2026-11-14

### Added

- Faster loads, opt-in (`-xerces-fast-loads`): Shadows of Evil from the menus in
  about 9 s instead of 14.6, Town Reimagined with three mods in about 9 s instead
  of 34.
- Reports for a load that never ends, a lost graphics device, and what a frozen
  frame was waiting for; reports name the game's threads; the launcher has a
  crash handler of its own.
- Opt-in `-xerces-present-wake` and `-xerces-input-wake`.

### Fixed

- A Lua error in something Xerces asks of the menu scripts, or in one of its own
  console commands or background tasks, no longer closes the game.
- The game could close as the main menu came up.
- A joined host's map-first rule no longer outlives the join.

## [0.1.0-beta.26] - 2026-11-13

### Fixed

- After joining a host without Xerces (or with its map scripts first off), a
  custom map started from the lobby menu with DS4C no longer ends in a script
  link error.

## [0.1.0-beta.25] - 2026-11-13

### Changed

- Aim assist after a controller playtest: weaker "Modern" setting, only live
  zombies you can see.

### Fixed

- 19 fixes from a review of the whole client; the launcher survives a lost
  graphics device; the CHARACTERS portrait and its preview camera.

## [0.1.0-beta.24] - 2026-11-13

### Fixed

- Quitting is no longer reported as a crash when a game thread faults on the way
  out.
- 21 fixes from a review of every part of the client: the freeze watchdog, the
  launch journal, join passwords, self-revive, character picks, crash-report
  scrubbing, Friends joins and invites, privacy texts and more.

## [0.1.0-beta.23] - 2026-11-12

### Fixed

- Long frames (about 140 ms, several a second) on large custom maps with many
  mods: the game's slow sort of its texture list is replaced with one about a
  hundred times quicker, into the same order.

## [0.1.0-beta.22] - 2026-11-12

### Added

- CHARACTERS as one screen with a tab per character mod and a live view of the
  character on the player.
- Crash, freeze and fatal-error reports as text you can read.

### Fixed

- A pass over the whole client: mods' precache lists, the zone plan's names, the
  aim assist's controller polling, the round HUD, and 14 launcher fixes.

## [0.1.0-beta.21] - 2026-11-12

### Added

- Every character mod's characters as character slots; stronger aim assist.

## [0.1.0-beta.20] - 2026-11-11

### Added

- Zombies aim assist for a controller, like the newer Call of Duty games, and No
  Hip-Fire Spread (Zombies only).

## [0.1.0-beta.19] - 2026-11-08

### Added

- CHARACTERS shows the character in third person for four seconds after a pick.

## [0.1.0-beta.18] - 2026-11-08

### Added

- The Zombies CHARACTERS screen: DS4C's roster, and a pick other mods cannot
  undo.

## [0.1.0-beta.14] - 2026-11-08

### Added

- Record frame times (Settings > Display), with a stall sampler for long frames
  (betas 14 to 17).

## [0.1.0-beta.13] - 2026-11-08

### Added

- NVIDIA DLSS and DLAA in the client, for players with NVIDIA's own file (not in
  the zip).

## [0.1.0-beta.12] - 2026-11-08

### Added

- AMD FSR 1 upscaler.

## [0.1.0-beta.11] - 2026-11-08

### Fixed

- Session fixes for hosting and joining.

## [0.1.0-beta.10] - 2026-11-07

### Added

- Join through Discord.

## [0.1.0-beta.8] - 2026-11-07

### Added

- Friends and invites (betas 8 and 9).

## [0.1.0-beta.7] - 2026-11-07

### Added

- XERCES SETTINGS: Zombies round settings.

### Fixed

- Running out of memory on MW4 Shipment.

## [0.1.0-beta.6] - 2026-11-07

### Fixed

- Joining a friend's game.

## [0.1.0-beta.5] - 2026-11-07

### Added

- A Mods entry in the game's own menu.

## [0.1.0-beta.4] - 2026-11-06

### Fixed

- Ascendance with other mods stacked.

## [0.1.0-beta.3] - 2026-11-06

### Added

- A mod's camos and characters name the mod that added them.

### Fixed

- The game no longer crashes at every start from a folder OneDrive syncs.
