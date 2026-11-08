# Xerces beta: read this first

Xerces is a launcher for Call of Duty: Black Ops III that loads several Steam
Workshop mods at once, in the order you choose, and helps when a mix of mods
breaks the game. It is for people who own Black Ops III on Steam. It plays
offline and on games players host themselves; it never touches official
matchmaking.

This is a beta. Things will go wrong, and telling us about them is the point.
KNOWN_ISSUES.txt lists what we already know about.

## What you need

- Windows 10 or 11, 64-bit.
- Black Ops III on Steam, installed, at a build Xerces supports. Xerces checks
  this when it starts: Diagnostics shows your build and whether it is supported.
- Steam running and signed in with the account that owns the game. Mods come
  from your Workshop subscriptions, through Steam.
- Nothing else: no installer, no runtimes, no admin rights.

## Install

1. Check the zip before you unzip it. The private link comes with the zip's
   SHA-256. In PowerShell, in the folder with the zip, run
   `Get-FileHash .\xerces-<version>.zip` (with your zip's name) and compare the
   hash it prints with the one sent with the link. If they differ, do not use
   the zip, and tell us.
2. Unzip the whole zip into a folder of its own, for example `C:\Games\Xerces`.
   Not into the Black Ops III folder, and not into a folder OneDrive syncs
   (Documents and Desktop often are: their path then has `OneDrive` in it),
   where the game can fail to start. Keep the files together: Xerces.exe needs
   boiii.exe, data and compat next to it. If you move the folder later, close
   Xerces first: only one Xerces runs at a time.
3. Start **Xerces.exe**, and only Xerces.exe. It starts the game (boiii.exe)
   itself, with the mods you picked. Do not start boiii.exe or BlackOps3.exe
   yourself while you test: boiii.exe started on its own does not find Xerces'
   data and behaves like BOIII (KNOWN_ISSUES.txt).
4. Windows SmartScreen may say "Windows protected your PC", because the beta is
   not signed yet. Choose "More info", then "Run anyway".
5. The first start reads your whole Workshop folder to learn your mods. From an
   SSD that takes seconds; from a hard disk with a big Workshop folder it can
   take several minutes. Later starts reuse what it learned and are quick.

SHA256SUMS.txt in the folder lists the SHA-256 of every file in it, to check
later that no file has changed. It came in the same zip, so only the zip's own
hash, sent with the link, shows that the zip is the one we made.

## Playing

- **Home** launches the game, with your mods or without them, for the mode you
  pick (Zombies, Multiplayer or Campaign).
- **Mods** lists your Workshop mods. Turn them on, put them in order (the top
  one is the main mod), and read what Xerces says about mods that do not go
  together or maps that will not fit.
- **Settings** has the game's options (name, field of view, FPS limit, display
  mode) and Xerces' own.
- **In the game**, a camo or character that a mod added names that mod in a
  small tag on its icon: in the camo picker (Zombies weapon kits, Gunsmith,
  Create-a-Class) and in DS4C's camo and character screens. Only one mod's camo
  list and character list load at a time, so the tag names the mod whose list
  you are seeing.
- **A mod's own menu in a match.** Some mods (PEGASUS, the Avali character pack)
  open their menu from a button in their own pause menu, which another mod's
  pause menu (DS4C's) leaves out. Xerces then adds an entry named after the mod,
  such as "PEGASUS MENU", to the pause menu you get.
- **XERCES SETTINGS (Zombies).** In the Zombies lobby, next to BOIII SETTINGS:
  Max Ammo every round, guns from wall buys and the box already Pack-a-Punched,
  starting points (500, 5,000 or 50,000), no perk limit, a box that never moves,
  the power on and every door open at the start, and a zombies-left counter with
  a round timer, a random loadout (two box guns each round) and a self-revive
  (back every 10 rounds; down, hold Use). Its GAMBLING tab has the Pack-a-Punch
  wheel and the coin flip; its ROUNDS tab has round skips (special and prime
  rounds, 400 points, a vote in co-op), rounds to 999 (the count goes past 255,
  and at 999 everyone votes to exfil or keep going) and smart zombies (from
  round 20 some zombies hunt in packs, sprint in bursts or wait out of sight,
  each kind with its own glow). All are off until you turn them on. When you
  host, everyone in the match gets your settings, and each player sees them
  listed in a message when they spawn. In the match, the pause menu has XERCES
  SETTINGS (what this match uses) and, when they are on, PACK-A-PUNCH WHEEL
  (2,500 points, 10 in a Fire Sale, once a round, with a Pack-a-Punched gun in
  hand: an ammo mod or the box's teddy) and COIN FLIP (pick a bet; heads wins,
  and the payout grows each round), SKIP ROUND on a round that can be skipped,
  and EXFIL and KEEP GOING at round 999. Not on dedicated servers.
- **Diagnostics** shows what Xerces found: the game, its build, the mods, the
  logs and crash bundles. "Copy report" puts all of it on the clipboard for a
  bug report.

## When the game crashes

- The game's crash dialog offers "Restart game" and "Close game". Choose **Close
  game**, then go back to Xerces (start it again if it is closed). "Restart
  game" starts the game again with the same mods, past Xerces, so Xerces cannot
  help with the next start.
- After a crash with mods on, Home offers to launch with mods off (safe mode),
  and "Find the culprit" tries your mods in smaller groups, one test launch at a
  time, to find the one that crashes.
- **Crash reports are on by default.** When the game crashes, Xerces makes a
  crash report for the Xerces server so we can fix it, as soon as the game has
  closed (or the next time you open Xerces). A report holds the crash's code and
  where it happened (the module and offset, the start-up, menu or match phase),
  the mode, map and mods, the names of the launch flags, the Xerces version and
  commit and the game build, this PC's Windows version, CPU, graphics card and
  driver, RAM and display, the last lines of the game's and Xerces' logs
  (without chat), and a small dump of the threads' stacks.
- What is taken out: in the logs, your Windows user name, player names and
  server addresses, and your user folder is written as `%USERPROFILE%`. Other
  folder paths stay, such as the game's, the Workshop's and the Xerces folder's.
  In the stack dump only your user name and the player names are masked: the
  paths of the loaded programs stay, and the rest of its stack memory is raw and
  can hold bits of what the game was doing (a path, a name, an address). Never
  sent: usage data, your Steam id or game identity, full crash dumps, your
  settings, packs or files. A report goes with a random id kept in your Xerces
  folder, never your name, Steam account or hardware. The server is to store no
  IP address and to delete reports after 90 days.
- Turn crash reports off in Settings > Privacy: no report is sent then, and
  reports still waiting are removed. "Delete my crash reports" there deletes
  what this PC sent. With them off, report crashes on GitHub (Reporting bugs,
  below; Settings > Privacy has the link).
- **A report goes out when the game has closed:** right away while Xerces is
  open, or at its next start. When the server cannot be reached, it waits in
  `crash\outbox` in your Xerces folder and goes later; Xerces keeps at most 20
  and removes those older than 7 days without sending them.
- Xerces also keeps crash bundles on your PC (Diagnostics, Crash bundles; "Make
  a bundle" makes one any time). Attach one to a bug report if we ask.

## Reporting bugs

Open an issue at <https://github.com/DeroXP/boiii-free/issues>, or tell us in
the tester channel. Please include:

- the Xerces version (About, or the top of the game's main menu), for example
  `Xerces 0.1.0-beta.9 (3f09512d)`;
- what you did, what you expected and what happened;
- the mods you had on, in their order, the mode and the map;
- Diagnostics > Copy report, pasted in;
- a crash bundle if the game crashed and we ask for one.

Never attach game files or files from the Workshop folder.

## Playing with other testers

Joining another tester's game has been tried once, on two PCs on 2026-11-07: a
modded Zombies game, with the host's port forwarded by hand (KNOWN_ISSUES.txt).
If you try it, tell us how it went.

- **Hosting.** Launch the game from Home. In the Zombies or Multiplayer online
  lobby (not Play Local), press the button under the map button that reads
  DIRECT CONNECT: CLOSED, so it reads DIRECT CONNECT: OPEN. Your lobby now
  answers joins by address. While it is open, the game asks your router to
  forward its UDP port (UPnP), and a toast says what the router answered.
  "Router port open" shows the internet address and port to share, for example
  `203.0.113.7:27017`; if the router did not say its address, give your public
  IP address with the port instead. On the same home network, share your PC's
  local address (`ipconfig`, the IPv4 Address) with port 27017. Press the button
  again to close the lobby; the router stops forwarding the port then, or when
  you quit the game. If the game crashes while the lobby is open, the router
  keeps forwarding the port: open and close the lobby again, or remove the
  "Xerces lobby (Black Ops III)" entry in the router's settings.
- **Joining.** In Xerces, Servers, type the host's address and port, for example
  `192.168.1.20:27017`, then Join vanilla or Join modded. Xerces starts the
  game; press a key at its title screen, and the game joins a few seconds after
  its main menu is up (with big mods the menu takes a minute or so). The host
  decides which mods load: your game loads the host's mods, in the host's order,
  if you have them. A mod or map you lack is named in a dialog that opens its
  Workshop page in Steam, so you can subscribe and join again; nothing is
  downloaded from the host. If the game already runs, close it first, or type
  `connect <address>` in the game's console at the main menu.
- **Turn off a VPN to host.** With a VPN on (NordVPN and the like), the address
  you share is the VPN's, not your router's, and your game's answers leave
  through the VPN, so nobody can join. Turn it off while you host.
- **Changing maps.** Pick the next map from the lobby with everyone in it. When
  the host's game restarts or crashes, the others are dropped: the host opens
  the lobby again (DIRECT CONNECT: OPEN) and they join again.
- **Anyone who can reach an open lobby can join it.** Without a password, that
  is everyone who has your address. To ask for one, type
  `set net_password <password>` in the game's console after each start of the
  game (it is not saved), and give it only to the players you invite. Servers
  has no password field yet: when a joiner's game says the game needs a
  password, they type `set net_password <password>`, then `connect <address>`,
  in their game's console. The host's own game checks the password, and the
  password itself never travels over the network. It is checked on the joins
  Xerces handles, but not on the game's own lobby join, so close the lobby when
  you are done. Pick a long random password, and clear yours
  (`set net_password ""`) after you have joined: a host you join while it is set
  can test guesses against your answer.
- **No UPnP?** When the toast says "No UPnP router found" or "Router refused the
  port", turn UPnP on in the router, or forward UDP port 27017 to the host's PC
  by hand (the game takes the next free port, up to 27026, when another program
  already has 27017; the toast names the port), then close and open the lobby
  again. "Address shared" or "another router in front" means the internet cannot
  reach the host: let someone else host, or everyone joins one VPN that puts
  them on the same network. Players on the host's own network can always join
  with the local address.
- **Friends and invites.** On the Friends tab, make a Xerces account (a
  username, and a password of at least 10 characters) or sign in. Add friends by
  their Xerces username; they accept on their Friends tab. While Xerces is open
  you see whether each friend is online, in a game and which mode, and whether
  their lobby is open. To invite: host as above with the lobby open, then press
  Invite next to a friend. The invite gives them your lobby's address (the
  router's, from UPnP, or the internet address Xerces' server sees with your
  game's port), your mods, and a ticket only they can use: with "Who can join"
  on Invited players (the default) nobody else gets into your lobby. They get a
  toast in the game and an entry on Friends. Join there checks they have your
  mods: any they lack open on the Workshop in Steam, they subscribe, and the
  join starts by itself once Steam has downloaded them. Then their game joins
  yours from its main menu, restarts with your mods first (it asks), or starts
  with the join. Xerces asks your router to let them in (hole punching); most
  routers do, and if yours does not, forward the game's port as above. Invites
  last 10 minutes. **Invite links**: Make an invite link on Friends and share it
  anywhere; whoever opens it asks to join, and you Let them in (with your lobby
  open) or Ignore them. "Open invite links in Xerces" lets a click on a link
  open Xerces. **Follow hosts you join** (on by default): when a friend you
  played with invites you again, Xerces joins at once. After a crash in a
  friend's lobby, Friends offers Rejoin. **Show on Discord** (off until you turn
  it on, on both PCs): your Discord profile shows your game, and while your
  lobby is open your Discord friends get Ask to Join; you answer "Let them
  join?" in Xerces, and they join as with an invite. Keep Xerces open while you
  play, or friends see you offline and invites wait (Settings, "Close Xerces
  when the game starts" off). Remove and Block are next to each friend; the
  privacy buttons on Friends say who may send you requests, see you online and
  invite you.
- The first time you host or join, Windows asks whether "BOIII" may use your
  networks. That is Xerces' game client (boiii.exe, it keeps that name for now).
  Allow it, on private networks at least; offline play does not need it.
- **Voice chat** reaches the other players in the game. A new game profile
  starts on push-to-talk (talk key B, unless you rebind it). A profile you
  already have, from BOIII or an earlier start, keeps its own setting; change it
  in the game's voice options. This is not yet checked in the game either.

## What goes over the network

- **Crash reports**, while they are on (When the game crashes, above), and the
  question whether the server takes them yet.
- **The server list.** The game client, like BOIII, asks Ezz's public master
  servers (`master.ezz.lol`, `m.ezz.lol`) for the list of public servers
  whenever its main menu loads or its server browser opens, and then contacts
  each server on the list for its players and ping. Crash reports off or on,
  this happens. Xerces builds leave out BOIII's friend presence, invites and
  Discord status, so your friend code is not sent there.
- **Steam.** Your mods come through your Steam client. The game client also asks
  Steam's web API (`api.steampowered.com`) for details and preview pictures of
  Workshop items it shows that ship none (their ids only).
- **Playing together.** Servers you join, and players who join your lobby, see
  your IP address. While your lobby is open the game asks your router to forward
  its port (UPnP). Voice chat reaches the other players.
- **Friends**, once you sign in: your username and password go to Xerces'
  server, which keeps a hash of the password, never the password itself. While
  Xerces is open it keeps a connection to that server and tells your friends
  whether you are online or in a game, its mode, and whether its lobby is open
  (or nothing, with "Who sees you online" set to Nobody). An invite you send
  gives that friend your lobby's address and port, the list of your mods (as a
  pack code), and a ticket for your lobby; to find the address without UPnP,
  Xerces asks the server which internet address it sees (the server neither
  stores nor logs it). When a friend is about to join, the server passes their
  internet address and port to your Xerces so your game can open your router to
  them, as the join would show it anyway. An invite link names nobody; whoever
  asks to join through it shows you their username. The game's own traffic is
  not encrypted (it never was); tickets keep others out of your lobby. On your
  PC, Xerces keeps your sign-in in `%LOCALAPPDATA%\Xerces\config\account.dat`,
  encrypted for your Windows account; Sign out deletes it.
- **Discord**, only with Show on Discord on: Xerces tells the Discord app on
  your PC what you play (the mode, whether you are in a lobby or a match, since
  when), and Discord shows it to your Discord friends; while your lobby is open
  it also gets your invite link's code, which Discord passes only to a friend
  you let join. Discord gets nothing from Xerces with it off.
- Links you click (GitHub issues, Black Ops III's store page, the licence) open
  in your browser or in Steam.

## What Xerces leaves on your PC

- `%LOCALAPPDATA%\Xerces`: Xerces' settings, logs, caches, crash bundles and the
  crash reports waiting to be sent.
- `%LOCALAPPDATA%\boiii` and `boiii_players` in the Black Ops III folder: the
  game client's own files, shared with BOIII (settings, your player identity
  key, crash dumps). Xerces keeps them where BOIII does, so your identity stays
  the same.
- `%TEMP%\boiii-tlsdll.dll`: a small helper the game client unpacks each time it
  starts.
- A Windows firewall rule for boiii.exe, if you allowed it.
- A DirectX graphics preference for boiii.exe (Windows Settings > System >
  Display > Graphics), so laptops run the game on the faster graphics card.
- After a crash while you hosted a lobby: the router's forwarding of the game's
  UDP port ("Xerces lobby (Black Ops III)"), until the lobby is opened and
  closed again or you remove it in the router.
- If you started boiii.exe yourself and it showed BOIII's old launcher:
  `%LOCALAPPDATA%\EZZ BOIII`, that launcher's browser data.

To remove Xerces, delete its folder and `%LOCALAPPDATA%\Xerces`. Leave
`%LOCALAPPDATA%\boiii` and `boiii_players` if you also play BOIII.

## Updating

Xerces does not update itself in this beta. When a new zip comes out, close
Xerces and the game, delete the files in the old Xerces folder, and unzip the
new zip there (or into a new folder). Your settings stay in
`%LOCALAPPDATA%\Xerces`.

## When Steam updates Black Ops III

Xerces only starts game builds it knows. After a Steam update it says so on Home
and does not launch. Play through Steam until a Xerces zip supports the new
build; the tester channel will say when. Do not try to keep or restore an older
game build for Xerces.

## Licence and source

Xerces is free software under the GNU General Public License, version 3, with no
warranty: see LICENSE. It is a modified version of Ezz BOIII, itself based on
BOIII by momo5502. SOURCE.txt says where the exact source of this build is, and
THIRD_PARTY_NOTICES.txt lists the libraries in it and their licences. Xerces is
not made, endorsed or supported by Activision or Treyarch.
