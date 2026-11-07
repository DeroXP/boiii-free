# Known issues in this Xerces beta

What we already know about, from the tests in docs/TESTING.md. You do not need
to report these again, but tell us if you see them somewhere else or worse.

## Mods and maps

- **Shadows of Evil with DS4C drops you before the level.** When the 3.5 minute
  intro plays to the end, DS4C's scripts error on every frame and the game drops
  you before you are in the level. Skip the intro and it plays. This is DS4C's
  own and has not been checked on plain BOIII.
- **BACKROOMZ and Super Mario 64 with DS4C run out of memory.** DS4C's files
  take about 450 MiB of the memory the game keeps for loaded content, and these
  two maps need more than is left. With BACKROOMZ the game shows "Black Ops III
  ran out of memory", names the map and the mods, and closes when you press OK.
  Super Mario 64 should do the same, but we have not checked that yet. The Mods
  screen warns about maps that will not fit before you launch. Turning on
  Settings > Launch > "Larger zone memory (experimental)" makes them load: both
  played that way in our tests, twice each, plus one smaller map. Those five
  runs are all the testing that setting has had. Turn it off again if the game
  crashes with it on.
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
- **PEGASUS asks for unsafe Lua.** In a match the game asks whether to allow
  unsafe Lua functions (file access): PEGASUS tries to open a file. No keeps it
  blocked; the match played normally in our tests that way.
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
- **Joining another tester's game is not tested yet.** Opening a lobby to direct
  connect, the router's port (UPnP) and the address it shows, joining by address
  (Servers), the dialogs that name a mod or map you lack, the network password
  and playing a custom map together all need a second PC, and none has been
  tried with Xerces (README.txt, Playing with other testers). Two limits are
  known: while a lobby is open, anyone who can reach it can join, and a password
  is checked on the joins Xerces handles but not on the game's own lobby join,
  so close the lobby when you are done. Whether the router's port closes by
  itself when you leave the lobby for the main menu is not seen yet; closing the
  lobby or quitting the game closes it. After a crash with the lobby open the
  router keeps forwarding the port until the lobby is opened and closed again
  (README.txt, Playing with other testers).
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
