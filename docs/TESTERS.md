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
- **Crash reports are on by default.** After a crash, Xerces sends a crash
  report to the Xerces server so we can fix it: the Xerces version and commit,
  the game build, where the crash happened (the module and offset, the start-up,
  menu or match phase), the mode, map and mods, the names of the launch flags,
  this PC's Windows version, CPU, graphics card and driver, RAM and display, the
  last lines of the game's and Xerces' logs with your user name, folders and
  server addresses removed, and a small crash dump with the threads' registers
  and stacks and no other memory. It goes with a random id kept in your Xerces
  folder, never your name, Steam account or hardware. The server stores no IP
  address and deletes reports after 90 days. Turn this off in Settings >
  Privacy; "Delete my crash reports" there deletes what this PC sent. With it
  off, report crashes on GitHub (Reporting bugs, below).
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

Joining another tester's game has not been tested with Xerces yet
(KNOWN_ISSUES.txt). If you try it, tell us how it went.

- The host launches the game from Home and starts a private match. The others
  type the host's address and port in Servers, for example `192.168.1.20:27017`,
  and choose Join. Everyone needs the same mods, in the same order. Nothing is
  downloaded from the host: a player who lacks a mod should see it named, with
  its Workshop page to subscribe to in Steam.
- Xerces does not open a port on your router, and friend invites are off in this
  build (the Friends screen says why). On the same home network, join with the
  host's local address. Over the internet, the host forwards UDP port 27017 on
  the router to their PC (the game takes the next free port, up to 27026, when
  another program already has 27017), or everyone joins one VPN that puts them
  on the same network.
- The first time you host or join, Windows asks whether "BOIII" may use your
  networks. That is Xerces' game client (boiii.exe, it keeps that name for now).
  Allow private networks; offline play does not need it.

## What Xerces leaves on your PC

- `%LOCALAPPDATA%\Xerces`: Xerces' settings, logs, caches and crash bundles.
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
