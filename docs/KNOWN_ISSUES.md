<!-- beta -->
# Known issues in this Xerces beta
<!-- stable -->
# Known issues
<!-- end -->

What we already know about, from our tests (docs/TESTING.md in the source). You
do not need to report these again, but tell us if you see them somewhere else or
worse.

## Mods and maps

- **About nine or ten mods fit in one game.** The game has a table of 63 zones
  (packs of loaded content). In Zombies it takes 23 of them itself as a level
  loads, and most mods add 4 (2 for the menus, 2 for Zombies, each with its
  English text); some add 2. Eight mods like DS4C, Ascendance, im_oxo QOL,
  Classic Viewmodel FOV, DBZ Villains, Girls' Frontline 2.0, Avali and Neptunia
  take 30: 53 of 63, read from the running game. A stack over 63 hangs while the
  level loads. The Mods screen counts the zones and says before
  you launch when a stack is over, and by how many; take off mods until it
  fits. The limit is the game's own (a table of 64 slots, and the engine stops
  at 64); Xerces does not raise it.
- **Two more limits of the game's own, met with big stacks** (2026-11-15, the
  eight mods above plus each of the others installed on the test PC):
  - All-around Enhancement fills the game's table of loaded assets (about
    156,700 entries; the eight mods use about 85,000 in Shadows of Evil): "Could
    not allocate asset - increase XASSET_ENTRY_POOL_SIZE", or a crash as the
    level loads. It runs with fewer mods.
  - The game holds 11,264 models. The Black Ops 3 Wonder Weapon Pack with DS4C
    and four character mods (DBZ, Girls' Frontline 2.0, Avali, Neptunia) needs
    more: the game stops as the menus load with "Exceeded limit of 11264
    'xmodel' assets" and says to turn off some mods (up to beta.27 the texture
    streamer crashed there instead). The eight mods above take up to about
    10,900 of them as a level loads. The Wonder Weapon Pack runs alone, with
    DS4C, and with DS4C, Ascendance, im_oxo QOL and Classic Viewmodel FOV.
  - Solo Easter Eggs and Zombie Counter run with the eight.
  - The eight mods come close to two more tables of the game's: in the menus
    they use 10,957 of its 11,264 models and 48,518 of its 49,152 images (read
    from the running game on every map, 2026-11-15). A mod that brings many
    models or textures on top can stop the game as the menus load with
    "Exceeded limit of ... 'xmodel' assets" (or 'image'), and Xerces says to
    turn off some mods. The levels themselves use fewer.
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
- **PEGASUS and MW4 Shipment ask for unsafe Lua.** As the main menu loads and
  in a match, PEGASUS's menus try to open a file; in MW4 Shipment's mode screen
  the map's own menu asks for package.loadlib, which loads a DLL into the game.
  The game asks whether to allow unsafe Lua functions and waits until you
  answer; the question names the function and what it does. No
  keeps them blocked; PEGASUS's menus and match played normally that way.
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

<!-- beta -->
- **Up to beta.27: with many mods stacked, a map's lights or sounds that its
  scripts drive could stay off.** The game has room for eight "client systems"; a
  map and each mod take some (Kino der Toten four, DS4C three). The ones that found
  no room used to end the match with "ClientSysSetState - state index (-1) out of
  bounds"; from beta.22 Xerces left such a system out, so on Kino with eight mods
  the lights that show where the mystery box is stayed dark. Now none is left out:
  the ones past seven share one of the game's slots (Zombies). A friend who joins
  with an older Xerces (or without it) misses the shared ones, the seventh
  included: on Kino with eight mods, the map's lights as well as the box's.
<!-- stable -->
- **With many mods stacked, a friend without Xerces misses some map effects.**
  The game has room for eight "client systems", the channels a map's and a mod's
  scripts use to tell each player's game something (Kino der Toten takes four,
  DS4C three). With more than that, Xerces lets the ones past seven share one of
  the game's slots (Zombies), and every player with Xerces gets them all. A
  player who joins without Xerces, or with a beta before 1.0.0-rc.1, misses the
  shared ones: on Kino der Toten with eight mods, the map's lights and the ones
  that show where the mystery box is.
<!-- end -->
- **A long frame (about 140 ms) every third of a second on a large custom map.**
  Seen on TOWN REIMAGINED with eight mods, from about a minute into a match. The
  game re-sorts its list of loaded textures with a sort that is slow when the list
  is far out of order, and the frame waits for it. Xerces sorts that
  list another way (about a hundred times quicker in that case, into the same
  order). What puts the list that far out of order is not known, but it happens
  in real matches: in eight minutes of MW2 Rust with a mod stack the
  list of about 28,600 textures was that far out of order 14 times, each a sort
  of some 290 ms for the game's own way and about 2 ms for Xerces'. If you still
  see long frames, send the game's log: its `xerces.stream_sort.far` lines say
  how often the list was that far out of order and what the sorts cost. To
  compare with the game's own sort, type `-xerces-stream-sort-engine` into
  Settings > Launch, Extra launch flags.
<!-- beta -->
- **After beta.27: a `map` typed in the console from inside a level no longer
  crashes into Moon.** Kino der Toten to Moon by `map zm_moon` closed the game's
  server every time (it froze, then said the server stopped sending updates): the
  game looked up Moon's character table before Moon had loaded and kept the empty
  answer. Xerces now looks it up again. Moon from the menus was never affected.
<!-- end -->

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
- **Its feel was tuned by play once.** After a controller playtest,
  Modern was cut to about half of what it was at 1 to 3 m (50% rotation, 40%
  slowdown, all of it only from 3 m), dying and hidden zombies stopped being
  targets, and the view settles around the neck. Stronger is what Modern was
  plus more (95% and 75%). Played again, it felt right. Say what feels
  off: strength and ranges are single numbers.
- **It needs an XInput controller.** A controller in another mode (DualShock or
  Switch mode without a wrapper) is not seen; `xerces_aim_debug` in a match says
  "controller NOT found". It is tied to this build of the game: after a game update
  it turns itself off and writes `xerces.aim_assist.off` to the log.
- **The game's own aim assist still runs under it.** Game's Own leaves only that.
- **It is not exactly Modern Warfare III's numbers.** Modern is a little under
  them and keeps some of the assist at point blank, where theirs fades to nothing;
  Stronger is full from 1.5 m and nearly full from the hip. Unlike those games it
  moves the view up or down toward a zombie's torso-to-head band when you aim
  under or over it (never sideways).
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

<!-- beta -->
- **Before beta.27: the game could close as the main menu came up.** Rarely, at
  the start, the game closed with a crash report naming `hksi_lua_getinfo`; and
  any error in something Xerces asked of the game's menu scripts, or in one of
  Xerces' own console commands or background tasks, closed the game. Fixed in
  beta.27: such an error is written to the console or the log and the game goes on.

- **Before beta.26: a custom map failed after joining someone.** After joining a
  host whose map scripts first was off, or a host without Xerces, the next custom
  map you started yourself from the lobby with a mod that ships the same scripts
  (DS4C) ended in "GSC LINK ERROR" until the game was restarted. Fixed in beta.26.

<!-- end -->
- **Crash files are never deleted.** Each crash leaves about 22 MB in
  `%LOCALAPPDATA%\boiii\minidumps` (a dump, a text report and a zip), each
  freeze about 0.6 MB. Delete old ones by hand when the folder grows.
- **The launcher closes when the graphics driver resets.** After a driver update
  or a GPU fault it says so in a message box and closes. Open it again.

- **A Workshop mod installed twice** (through Steam and as an old manual copy in
  the game's `mods` or `usermaps` folder): which copy loads is not settled. Mods
  says so on both rows; delete the copy you do not want and scan
  again.

- **Tested mostly on one PC.** Each build runs in the game on one PC (Windows
  11, RTX 5070 Ti) before it goes out: the launch checks without mods, with mods,
  on a custom map, the recovery after a crash and a crash report sent, and an
  eight-mod stack on every installed Zombies map (the website's Tested page).
  Other hardware, other mods and long play have met it only on testers' PCs.
- **Keep Xerces out of OneDrive.** Unzipped into a folder OneDrive syncs
  (`...\OneDrive\Documents\...`), the first beta zip crashed at every start,
  with mods or without. This build no longer crashes there that way and warns on
  Home, but it has not been tried in a OneDrive folder yet: unzip it to a folder
  such as `C:\Games\Xerces`.
- **Shi No Numa froze once going back to the menu.** With the eight mods, one
  game in six (2026-11-15) stopped as it left the match for the menu: the game
  was freeing its script strings and looped on one that was missing from its
  table. Xerces wrote a freeze report and closed the game after 60 s; the next
  start is normal. The cause is not found yet. If it happens to you, send the
  `freeze-<time>.txt` (README.txt, Reporting bugs).
- **Some crashes are not explained yet.** In our tests the game sometimes closed
  while a map was starting, with no crash report, and ran fine the next time.
  If it happens to you, report it (README.txt, Reporting bugs) and say what you
  did just before.
<!-- beta -->
- **A crash while the game closes (up to beta.23).** Once in 12 quits with mods
  in the first betas, and once in the tests of beta.23 with Xerces closed and
  opened again while the game ran, the game crashed as it quit; the next start
  of Xerces then offers safe mode, and launching normally is fine. The cause is
  known now: a game thread opened a file after the game had closed its file
  table, while Xerces was still finishing its own shutdown. Builds after
  beta.23 finish that shutdown without the wait that made room for it, and end
  such a quit as the quit it was.
<!-- end -->
- **The two experimental display settings do not help yet.** In our tests on one
  PC, "Flip-model presentation (experimental)" changed nothing measurable in
  borderless, and "Precise FPS limit (experimental)" held about 235 FPS at a 240
  limit with slightly less even frames. Leave both off unless we ask you to test
  them.
- **Joining another player's game has been tried once.** On 2026-11-07 a second
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
  again (README.txt,
<!-- beta -->
  Playing with other testers).
<!-- stable -->
  Playing with friends).
<!-- end -->
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
<!-- beta -->
- **Xerces does not update itself.** A new beta comes as a new zip.
<!-- stable -->
- **Xerces does not update itself.** A new version comes as a new zip; the
  website's download page has the latest.
<!-- end -->
