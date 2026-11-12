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
  Settings > Launch > "Larger zone memory (experimental)" (Off, 4, 6 or 8 GB; 4
  GB is what those tests used) makes them load: BACKROOMZ and Super Mario 64
  played that way in our tests, twice each, plus one smaller map, and MW4
  Shipment with six mods twice, once right after Shadows of Evil. Turn it off
  again if the game crashes with it on.
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

## Maps

- **With many mods stacked, a map's lights or sounds that its scripts drive may
  not work.** The game has room for eight "client systems"; a map and each mod
  take some (Kino der Toten four, DS4C three). The ones that find no room used to
  end the match with "ClientSysSetState - state index (-1) out of bounds": Kino
  with the eight-mod test stack, when the power came on. Since beta.22 Xerces
  leaves such a system out instead, so the match goes on; on Kino the lights
  that show where the mystery box is then stay dark. Fewer mods bring them back.

## Zombies characters (CHARACTERS)

- **A character from a mod that only replaces models keeps the base character's
  name and voice.** DBZ Villain Characters and Hyperdimension Neptunia give no names
  or portraits: Cooler in Dempsey's place is still called Dempsey by the game and
  speaks as him, and its row has no portrait. Its name in the list is taken from
  its model's name.
- **A mod's character sits on one of the characters of the list in use.** The game
  sends a character as one of 16 characters and one of 16 outfits, so another mod's
  character is added as an outfit of the character with the same number (the name
  on the scoreboard and the voice are that character's). With more than 16 on one
  character the rest go to the character with the most room; past 256 in all, the
  mods lowest in the stack are left out (the log says how many).
- **Everyone in a match needs the same mods in the same order** for these slots, as
  for the match itself; then each player sees the others' picks.
- **Some rows look alike.** A mod may put the same character on several of the
  base characters (DBZ's Broly is on Nikolai and on Floyd): each is a row, because
  the name and voice differ.
- **Not tried:** two players, a joined match, and custom maps that bring their own
  character list (a map's own characters stay as they are; the mods' are added to
  them).
- **DS4C's own extra characters are not in the CHARACTERS list.** Disciple, Misty
  and Shimo (indexes 500 and up) are made by DS4C's own scripts; use DS4C's
  character screen for them (it cannot change a pick made in CHARACTERS until
  you pick DEFAULT).
- **Only the host's last pick is remembered.** Other players pick again each match.
- **The live view is safe in a solo match only.** There the zombies stand still
  and cannot hurt you while CHARACTERS is open. With other players in the match
  nothing is held, so open it somewhere safe: if you go down, the view closes by
  itself. Tried with a bot as the second player, not yet with a second person.
- **A long mod title is cut on its tab** ("DBZ VILLAIN.."); the line under the
  list has the full title.
- **The other players see each character you look at**, since the live view puts
  it on you; Back puts you back as you were.

## Zombies aim assist and hip-fire spread

- **The modern aim assist is off until you turn it on.** XERCES SETTINGS >
  CONTROLLER > Aim Assist (Zombies) starts on Game's Own, which is the game's own
  assist and nothing of Xerces'.
- **Its feel is still tuned by numbers, not by play.** Its rules were checked in a
  match with a simulated stick (beta.21: the view followed 75% of a zombie's motion,
  95% on Stronger). Say what feels off: strength and ranges are single numbers.
- **It needs an XInput controller.** A controller in another mode (DualShock or
  Switch mode without a wrapper) is not seen; `xerces_aim_debug` in a match says
  "controller NOT found". It is tied to this build of the game: after a game update
  it turns itself off and writes `xerces.aim_assist.off` to the log.
- **The game's own aim assist still runs under it.** Game's Own leaves only that.
- **It is no longer Modern Warfare III's numbers.** Those faded to nothing at
  point blank and were lower from the hip; since beta.21 it is full from 1.5 m and
  nearly full from the hip. Inside arm's length it still eases off, so a zombie on
  top of you does not swing the view.
- **No Hip-Fire Spread is the host's setting.** It does nothing in a match you
  joined, and the crosshair of players who joined still shows their own spread.

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
  joinable, the invite toast in the game, Join into a running game, invite links
  and asking to join, getting a host's mods and following a host worked with two
  launchers on one PC against a test server. An invite has not yet reached a
  friend on another PC and brought them into a real lobby, so tickets and hole
  punching are not proven between two routers. A router that changes the port
  for each destination (some mobile and carrier networks) still needs the port
  forwarded, as direct connect does. Friends only see you while Xerces is open.
  Discord's Ask to Join has not been tried between two Discord accounts yet;
  connecting to the Discord app has.
- **Joining a friend on SHOOT HAUS dropped out of sync twice**
  (EXE_ERR_RELIABLE_CYCLED_OUT, "The server sent updates faster than your game
  could confirm them") on 2026-11-08, while the map loaded; other maps joined.
  Xerces' round settings now open their menus later and a row at a time, which
  takes some of that load away; whether that is enough on this map is not seen
  yet, as it needs two PCs.
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
