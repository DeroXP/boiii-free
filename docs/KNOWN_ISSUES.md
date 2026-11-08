# Known issues in this Xerces beta

What we already know about, from the tests in docs/TESTING.md. You do not need
to report these again, but tell us if you see them somewhere else or worse.

## Mods and maps

- **Shadows of Evil with DS4C drops you before the level.** When the 3.5 minute
  intro plays to the end, DS4C's scripts error on every frame and the game drops
  you before you are in the level. Skip the intro and it plays. This is DS4C's
  own and has not been checked on plain BOIII.
- **BACKROOMZ, Super Mario 64 and MW4 Shipment with DS4C run out of memory.**
  DS4C's files take about 450 MiB of the memory the game keeps for loaded
  content and 2.7 GiB of the room it keeps for streamed textures. BACKROOMZ and
  Super Mario 64 need more of the first than is left, MW4 Shipment (10.4 GiB of
  streamed textures) more of the second. The game shows "Black Ops III ran out
  of memory", names the map and the mods, and closes when you press OK. The Mods
  screen warns about maps that will not fit before you launch. Turning on
  Settings > Launch > "Larger zone memory (experimental)" makes them load:
  BACKROOMZ and Super Mario 64 played that way in our tests, twice each, plus
  one smaller map, and MW4 Shipment with six mods twice, once right after
  Shadows of Evil. Turn it off again if the game crashes with it on.
- **Two camo mods: a camo can look like another.** DS4C and Ascendance both
  replace the camo list and use the same camo numbers for different camos. The
  higher mod's list wins, but each gun's camo looks come from whichever mod has
  that gun, so a camo picked from DS4C's list can show one of Ascendance's on
  some guns (seen on the Vesper). Use one camo mod at a time if that matters.
- **PEGASUS only runs as the top mod.** PEGASUS checks as the game starts that
  it is the top mod and closes the game otherwise, which ends in a crash. That
  check is the mod's own and Xerces leaves it alone: the Mods screen warns and
  offers "Move to the top". With PEGASUS on top and DS4C below it, PEGASUS's
  menus replace DS4C's, so DS4C's PROGRESSION entry in the Zombies menu is gone
  and the main menu's Multiplayer button reads "Legacy camos" (a DS4C text the
  two mods share). The Zombies camos in the weapon kits are tagged PEGASUS,
  whose copies of those icons win. In a match you get DS4C's pause menu, and
  PEGASUS's own menu is under "PEGASUS MENU" there.
- **PEGASUS asks for unsafe Lua.** As the main menu loads and in a match, the
  game asks whether to allow unsafe Lua functions (file access): PEGASUS tries
  to open a file. The menu waits until you answer. No keeps it blocked; the
  menus and the match played normally in our tests that way.
- **A mod's menu file in the lobby.** When a mod's copy of a Lua file the menus
  and the lobby share fails in the lobby (Ascendance's), Xerces uses the game's
  own copy there and the mod's copy in the menus, and the game log says so
  (`lua.lobby.game_copy`). Tell us if a mod's lobby behaves differently.
- **DS4C prints script errors on custom maps.** Lines such as "undefined is not
  a field object" or "has unmatching types" fill the console while you play. The
  game keeps running. They come from DS4C's scripts.
- **"Playlist error" lines with any mod.** With any mod loaded, the console and
  the log show 123 lines of
  `Playlist error: line N: 'rule' found outside of any gametype or playlist block`.
  We have not seen them do anything. There are none without mods.
- **The conflict report is not yet checked in the game for older mods.** For two
  mods that replace the same script and say nothing about each other (almost
  every Workshop mod today), the report before launch says which one wins. We
  have not yet checked in the game that the one it names is the one that runs.
- **Mods with native code run it.** A few mods ship a DLL. Xerces keeps
  upstream's block list of known mod DLLs (T7Overcharged and others stay
  blocked), but any other DLL a mod loads runs on your PC with your rights.
  Xerces does not ask first yet. Only turn on mods you trust.

## Zombies round settings (XERCES SETTINGS)

- **Other players need Xerces for the pause menu entries.** The counter, the
  timer, the notices, the wheel and the coin flip show on every player's screen,
  but the pause menu's XERCES SETTINGS, PACK-A-PUNCH WHEEL, COIN FLIP, SKIP
  ROUND, EXFIL and KEEP GOING come from Xerces on each player's PC. A player
  without Xerces cannot spin, bet or vote, and a vote needs more than half of
  all the players in the match.
- **Not tested with two PCs yet.** Everything here ran on one PC, as the host.
  Not seen yet: a player who joins during a match, one who bleeds out and comes
  back the next round (whether their counter and timer are still there), votes
  from more than one player, and a self-revive while others are still up.
- **A map's own reasons not to end a round early are not checked.** A skip ends
  the round the way the Round Robbin gum does, but a map can tell Round Robbin
  not to run (during a boss fight, for example) through a check whose name only
  the map's compiled script has, and Xerces cannot ask it. On such a map a prime
  or special round may be skippable when Round Robbin would refuse.
- **Rounds to 999 keeps the game's zombie health.** Zombie health stops growing
  at round 163 (about 2 billion), so past there a zombie needs an insta-kill, a
  trap or a very strong weapon.
- **Power On at Start flips the standard power switch.** A map that turns its
  power on another way (a quest, several switches of its own) is left as it is,
  and so are doors that only a quest or a map's own check opens.
- **Smart zombies glow brightly up close.** Each smart zombie carries a power-up
  glow (yellow, green or blue) that also lights it, so a crowd of them next to
  you tints the screen. Tell us if it gets in the way.
- **Smart zombies were tested against a player who stood still.** In our tests
  the player could not be hurt and did not move or shoot. How the packs,
  sprinters and ambushers play against someone moving and shooting, and in co-op
  (a pack goes for the player furthest from the others), is not seen yet. An
  ambusher comes out after about 22 s at most.
- **Tested on Kino der Toten only, so far.** Without mods and with a seven-mod
  stack (PEGASUS on top, DS4C below it). Other maps and mods with their own box,
  perk or Pack-a-Punch rules may do things differently.

## The game and Xerces

- **Tested on one PC only.** This zip ran in the game on 2026-11-06 on one PC
  (Windows 11, RTX 5070 Ti): without mods and with three Workshop mods, on a
  custom map, with the recovery after a crash, and with a crash report sent. It
  has not met other hardware, other mods or a person playing for long yet.
- **Keep Xerces out of OneDrive.** Unzipped into a folder OneDrive syncs
  (`...\OneDrive\Documents\...`), the first beta zip crashed at every start,
  with mods or without. This build no longer crashes there that way and warns on
  Home, but it has not been tried in a OneDrive folder yet: unzip it to a folder
  such as `C:\Games\Xerces`.
- **Some crashes are not explained yet.** In our tests the game sometimes closed
  while a map was starting, with no crash report, and ran fine the next time.
  Once in 12 quits with mods the game crashed while closing; the next start of
  Xerces then offers safe mode, and launching normally is fine. If either
  happens to you, report it (README.txt, Reporting bugs) and say what you did
  just before.
- **The two experimental display settings do not help yet.** In our tests on one
  PC, "Flip-model presentation (experimental)" changed nothing measurable in
  borderless, and "Precise FPS limit (experimental)" held about 235 FPS at a 240
  limit with slightly less even frames. Leave both off unless we ask you to test
  them.
- **Joining another tester's game has been tried once.** On 2026-11-07 a second
  PC joined a modded Zombies lobby (four mods) by typing `connect <address>` at
  its main menu and played a custom map. The host's router had no UPnP ("No UPnP
  router found"), so its UDP port was forwarded by hand, and the host had to
  turn its VPN off. Found there and fixed in this build: Join in Servers started
  the game with the join already in it, and the joining game crashed about a
  minute and a half later, as it sent its stats before it had loaded them; the
  game now joins a few seconds after its main menu is up (past the title
  screen), which was checked on one PC against an address nobody answered. Not
  tried yet: the router's port through UPnP, Join in Servers since that fix, the
  dialogs that name a mod or map you lack, and the network password. Two limits
  are known: while a lobby is open, anyone who can reach it can join, and a
  password is checked on the joins Xerces handles but not on the game's own
  lobby join, so close the lobby when you are done. Whether the router's port
  closes by itself when you leave the lobby for the main menu is not seen yet;
  closing the lobby or quitting the game closes it. After a crash with the lobby
  open the router keeps forwarding the port until the lobby is opened and closed
  again (README.txt, Playing with other testers).
- **Friends and invites are new and tested on one PC only.** Accounts, friend
  requests, presence, privacy, block, sign-out, the open lobby showing as
  joinable, the invite toast in the game and Join into a running game worked
  with two launchers on one PC against a test server. An invite has not yet
  reached a friend on another PC and brought them into a real lobby. An invite
  still needs your port forwarded when the router has no UPnP, as direct connect
  does. Friends only see you while Xerces is open.
- **Multiplayer with Zombies mods can close the game.** On a tester's PC with
  four Zombies mods loaded, switching the main menu to Multiplayer ended with
  "Could not allocate asset - increase XASSET_ENTRY_POOL_SIZE" and the game
  closed. Launch Multiplayer from Home instead (with mods that support it, or
  without mods).
- **Crash reports are new.** Two real crashes went from the game to the Xerces
  server in our tests, but leaving chat and names out of a report was checked
  with tests only, not with other players in a match. If the crash dialog or a
  report looks wrong, tell us.
- **Push-to-talk on a new game profile is not checked in the game yet.** If a
  new profile starts on open mic, turn on Push to talk in the game's voice
  options, and tell us.
- **Some names still say BOIII.** The game program is still called boiii.exe, so
  the Windows firewall prompt says "BOIII", and so does the game's own settings
  menu ("BOIII SETTINGS"). Allow the firewall prompt if you want to host games
  or join other people's; offline play does not need it.
- **Starting boiii.exe yourself does not go through Xerces.** On its own it does
  not find Xerces' data and behaves like BOIII: it may ask for the game folder,
  stop with BOIII's "Missing required data" message, or, if BOIII was installed
  on this PC, show BOIII's old launcher with BOIII's options and links (which
  also leaves `%LOCALAPPDATA%\EZZ BOIII` behind). Close it and start Xerces.exe.
- **Xerces does not update itself.** A new beta comes as a new zip.
