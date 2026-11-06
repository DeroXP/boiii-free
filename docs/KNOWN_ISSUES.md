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

- **Some crashes are not explained yet.** In our tests the game sometimes closed
  while a map was starting, with no crash report, and ran fine the next time. If
  it happens to you, report it (README.txt, Reporting bugs) and say what you did
  just before.
- **Joining another tester's game is not tested yet.** Joining by address
  (Servers), the message that names a mod you lack, and playing a custom map
  together all need a second PC, and none has been tried with Xerces. Xerces
  does not open a port on your router either (README.txt, Playing with other
  testers).
- **Some names still say BOIII.** The game program is still called boiii.exe, so
  the Windows firewall prompt says "BOIII", and so does the game's own settings
  menu ("BOIII SETTINGS"). Allow the firewall prompt if you want to host games
  or join other people's; offline play does not need it.
- **Xerces does not update itself.** A new beta comes as a new zip.
