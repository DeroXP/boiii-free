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

1. Unzip the whole zip into a folder of its own, for example `C:\Games\Xerces`.
   Not into the Black Ops III folder, and keep the files together: Xerces.exe
   needs boiii.exe, data and compat next to it.
2. Start **Xerces.exe**, and only Xerces.exe. It starts the game (boiii.exe)
   itself, with the mods you picked. Do not start boiii.exe or BlackOps3.exe
   yourself while you test.
3. Windows SmartScreen may say "Windows protected your PC", because the beta is
   not signed yet. Choose "More info", then "Run anyway". SHA256SUMS.txt lists
   the SHA-256 of every file in the folder if you want to check them first (in
   PowerShell: `Get-FileHash .\Xerces.exe`).
4. The first start reads your whole Workshop folder to learn your mods. From an
   SSD that takes seconds; from a hard disk with a big Workshop folder it can
   take several minutes. Later starts reuse what it learned and are quick.

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
- **Crash reports are on by default.** After a crash, Xerces sends a report to
  the Xerces crash service so we can fix it: the Xerces version and commit, the
  game build, where the crash happened (the module and offset, the start-up,
  menu or match phase, the mode and map), the launch options, the list of mods,
  basic facts about the PC such as the Windows version, the end of the logs with
  your folder names hidden, and a small crash dump with the threads' stacks. It
  goes with a random install id, not your Steam account or name, and your IP
  address is not stored. Reports are kept for 90 days. Turn this off in
  Settings > Privacy.
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

- Testers can join each other's games. The host starts a game from Home, then
  opens it to others in the game's lobby; the others type the host's address in
  Servers ("Join"). Everyone needs the same mods, in the same order: Xerces
  names any that are missing and opens them in Steam for you to subscribe.
  Nothing is downloaded from the host.
- While you host a game open to others, the game asks your router (UPnP) to
  forward its port, and closes it again when you stop. If your router has UPnP
  off, others may not reach you; then someone else hosts.
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
