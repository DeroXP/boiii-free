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
   Not into the Black Ops III folder, and keep the files together: Xerces.exe
   needs boiii.exe, data and compat next to it.
3. Start **Xerces.exe**, and only Xerces.exe. It starts the game (boiii.exe)
   itself, with the mods you picked. Do not start boiii.exe or BlackOps3.exe
   yourself while you test.
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
- **Crash reports are on by default.** When the game crashes, Xerces sends a
  crash report to the Xerces server so we can fix it, as soon as the game has
  closed (or the next time you open Xerces). A report holds the crash's code and
  where it happened (the module and offset, the start-up, menu or match phase),
  the mode, map and mods, the names of the launch flags, the Xerces version and
  commit and the game build, this PC's Windows version, CPU, graphics card and
  driver, RAM and display, the last lines of the game's and Xerces' logs
  (without chat), and a small dump of the threads' stacks. Your user name,
  player names, folders and server addresses are removed; the rest of the stack
  memory is raw and can hold bits of what the game was doing. Never sent: usage
  data, your Steam id or game identity, full crash dumps, your settings, packs
  or files. A report goes with a random id kept in your Xerces folder, never
  your name, Steam account or hardware; the server stores no IP address and
  deletes reports after 90 days.
- Turn crash reports off in Settings > Privacy: nothing is sent then, and
  reports still waiting are removed. "Delete my crash reports" there deletes
  what this PC sent. With them off, report crashes on GitHub (Reporting bugs,
  below; Settings > Privacy has the link).
- In this beta the Xerces server does not take crash reports yet. Until it does,
  they wait in `crash\outbox` in your Xerces folder: Xerces tries again every 10
  minutes while it is open and at every start, keeps at most 20, and removes
  those older than 7 days without sending them. Leaving crash reports on does no
  harm meanwhile, and "Delete my crash reports" has nothing on the server to
  delete yet.
- Xerces also keeps crash bundles on your PC (Diagnostics, Crash bundles; "Make
  a bundle" makes one any time). Attach one to a bug report if we ask.

## Reporting bugs

Open an issue at <https://github.com/DeroXP/boiii-free/issues>, or tell us in
the tester channel. Please include:

- the Xerces version (About, or the top of the game's main menu), for example
  `Xerces 0.1.0-beta.1 (3f09512d)`;
- what you did, what you expected and what happened;
- the mods you had on, in their order, the mode and the map;
- Diagnostics > Copy report, pasted in;
- a crash bundle if the game crashed and we ask for one.

Never attach game files or files from the Workshop folder.

## Playing with other testers

Joining another tester's game has not been tested with Xerces yet: it needs two
PCs, and that test has not been run (KNOWN_ISSUES.txt). If you try it, tell us
how it went.

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
  you quit the game.
- **Joining.** In Xerces, Servers, type the host's address and port, for example
  `192.168.1.20:27017`, then Join vanilla. The host decides which mods load:
  your game loads the host's mods, in the host's order, if you have them. A mod
  or map you lack is named in a dialog that opens its Workshop page in Steam, so
  you can subscribe and join again; nothing is downloaded from the host. If the
  game already runs, close it first, or type `connect <address>` in the game's
  console.
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
- Friend invites are off in this build (the Friends screen says why).
- The first time you host or join, Windows asks whether "BOIII" may use your
  networks. That is Xerces' game client (boiii.exe, it keeps that name for now).
  Allow it, on private networks at least; offline play does not need it.
- **Voice chat** reaches the other players in the game. A new game profile
  starts on push-to-talk (talk key B, unless you rebind it). A profile you
  already have, from BOIII or an earlier start, keeps its own setting; change it
  in the game's voice options. This is not yet checked in the game either.

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
