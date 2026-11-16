# Changelog

Every Xerces release and what changed for players, newest first, in the
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) way. The website's
changelog page is made from this file (tools/site/build_site.py). Betas went to
testers by private link; the first stable release is in preparation, and
1.0.0-rc.1 is its first candidate.

## [Unreleased]

### Added

- **Join a friend's open lobby from Friends.** When a friend's lobby is open,
  Join is next to their name. If their "Who can join" is Anyone you get in at
  once, with their mods; if it is Invited players, they are asked first. Needs
  the update to Xerces' server.

### Changed

- **The Modern aim assist is less sticky** (Zombies, controller): less rotation
  and slowdown, a smaller area around the crosshair, none of it full until 3 to
  4 m away, and a slower drift to a zombie's neck. Modern, Stronger is gone; if
  you had it, you get Modern.

### Fixed

- **After a match you stay with the host.** Players who joined a friend's lobby
  landed in their own menus when the match ended; the host's game now calls
  everyone who was still in the match back to its lobby.
- **Asking to join through a link or Discord joins by itself** once the host
  lets you in, also with "Follow hosts you join" off.
- **No Hip-Fire Spread works.** It never took the spread off: it changed other
  numbers of each weapon instead. It now changes the ones the game reads for hip
  spread, so shots from the hip go where the crosshair is and the crosshair stays
  closed. Players who joined get it on their own screen too, a few seconds after
  they spawn.

## [1.0.0-rc.1] - 2026-11-15

The first candidate for the first stable release: beta.27 and a night of
fixes, checked with a real eight-mod stack on every installed Zombies map.

### Added

- **No mod is left without its map effects.** The game has room for eight client
  systems, the channels a map's and a mod's scripts use to tell your game
  something (Kino der Toten's mystery box lights, DS4C's on-screen text). A big
  stack needs more; the ones past eight now share one of the game's slots
  instead of being left out.
- **Mods' own client scripts run.** Xerces' custom client scripts were linked at
  a time the game never ran them; they now load with the client's own scripts.
- **The Mods screen says when a stack is too big** for the game's 63 zone slots,
  and by how many zones, before a launch would hang (the game's own count, read
  from a running game: 23 for Zombies, most mods 4).
- **The out-of-memory message says what helps**: turn on Larger zone memory, or
  raise it to 8 GB, before removing a mod.
- **The unsafe-Lua question names the function** a map or mod asked for and
  what it does ("package.loadlib (loading a DLL into the game)").
- A website, built from this repository's own documents.
- NOTICE and CREDITS in the zip (what Xerces is made from, and everyone who
  wrote it), and a source archive with the libraries for each release.

### Changed

- **Faster loads are on by default**: the loading screen draws every frame, a
  match you start alone skips the lobby's waits, the sound banks load more than
  a step a frame, and the game starts about 6 s sooner. Settings > Launch,
  "Faster loads", turns them off. Running a Zombies level's first seconds at 4x
  is opt-in (`-xerces-fast-level-start`): it made Die Rise with eight mods
  disconnect ("Clientfield Mismatch").
- **Frames and mouse input skip the game's millisecond waits by default.**
  Settings > Display turns it off.
- Safe mode turns both off, so a crash they caused is not met again with mods
  off.

### Fixed

- A `map` typed in the console from inside a level no longer crashes the game's
  server going into Moon (from Kino der Toten): the game looked up Moon's
  character table before Moon had loaded and kept the empty answer.
- Die Rise with eight mods disconnected with "Clientfield Mismatch" with
  beta.27's `-xerces-fast-loads`: its 4x level start is a flag of its own now.
- A stack with more models than the game holds (11,264; DS4C, four character
  mods and the Wonder Weapon Pack) crashed the texture streamer as the menus
  loaded. It now stops with the game's own "Exceeded limit of 11264 'xmodel'
  assets" and what to do: the model pool no longer grows past the game's size,
  as the image and mesh pools already did not, because the game finds models
  by their place in its own pool.
- A rare crash at the main menu: a server that did not answer the server list
  within a second was reported to the game's menu code from a background
  thread; it is now reported on the game's own thread, as answers are.

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
