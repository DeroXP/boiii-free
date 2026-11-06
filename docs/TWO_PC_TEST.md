# Two-PC join test

The first test of joining another tester's game (DESIGN.md 11.10). Two PCs, each
with Black Ops III on Steam: you host (H), and your friend joins (J) with the
beta zip. Afterwards, tick the matching items in `docs/TESTING.md` ("Joining
another tester's game").

Never send game files. The console text can show IP addresses, so send it
privately.

## Before you start (both PCs)

1. Steam runs and is signed in; Black Ops III is installed and up to date.
2. Unzip the beta into a folder of its own (not the game folder) and start
   `Xerces.exe`. Home says the game was found. Both PCs use the same beta zip
   (About shows the version).
3. Pick Zombies on Home.
4. The first time the game hosts or joins, Windows Firewall asks about
   `boiii.exe`: allow it. If someone clicked Cancel earlier, allow `boiii.exe`
   in Windows Security > Firewall & network protection > Allow an app through
   firewall.
5. H subscribes in Steam to Solo Easter Eggs (1944930083), DS4C Patched
   (3171632315) and Classic Viewmodel FOV (3656779213). J subscribes to the
   first two only, for test 2. Let Steam finish downloading before starting the
   game.
6. H's address on the same network: `ipconfig` in a Command Prompt, the "IPv4
   Address", with port 27017, for example `192.168.1.20:27017`. Over the
   internet: the address the game shows H in test 5.
7. In every test H starts first. J joins from Xerces > Servers: type H's
   address, then Join vanilla (the host decides which mods load). If J's game is
   already running, close it first, or type `connect <address>` in the game's
   console.
8. "Open the lobby" means: in the game, Zombies, the online lobby (not Play
   Local, where the button does not show), then the button under the map button
   that reads DIRECT CONNECT: CLOSED. Press it; it reads DIRECT CONNECT: OPEN.

## 1. LAN, vanilla to vanilla

Same network, no mods enabled on either PC (Xerces > Mods).

1. H: Home > Launch vanilla, then open the lobby. A toast says the lobby is
   open, then another says what the router answered.
2. J: join H's LAN address. Expected: J is in H's lobby; both see each other.
3. H: start a match on a stock map (The Giant). Expected: J goes along.
4. J: leave to the menu, then join H again while H is in the match. Expected: J
   joins the running match.
5. H: end the match, back in the lobby press DIRECT CONNECT again (it reads
   CLOSED; a toast says so).
6. J: join H again. Expected: a message that H is in the menus with the lobby
   closed, naming the DIRECT CONNECT button.
7. H: open the lobby again, then leave it for the game's main menu. Note whether
   a "Closed to direct connect" toast appears within about 15 s (it should when
   the game no longer has a lobby for H).

## 2. A joiner missing one stacked mod

1. H: Xerces > Mods: enable the three, Solo Easter Eggs at the top, then DS4C,
   then Classic Viewmodel FOV. Home > Launch modded, then open the lobby.
2. J (two of the three): join H. Expected: one dialog, "Mod not installed",
   naming Workshop item 3656779213 and offering its Workshop page. Yes opens it
   in Steam. J does not join; nothing downloads or reconnects. If J launched
   modded, J's main menu still names J's own mods afterwards, not H's.
3. J: subscribe to it, quit the game, let Steam finish, join again. Expected:
   J's game loads H's three mods, then joins the lobby by itself a few seconds
   later.
4. Optional: J unsubscribes from two of them, and H starts a match before J
   joins. Expected: one dialog naming both, with "Open their 2 Workshop pages".
   Yes opens them in Steam one after the other. Check that both pages can be
   reached (Steam's Back button) and subscribed to; if only the last one shows,
   note it.

## 3. The D3 stack on both

1. Both subscribed to all three. H: Launch modded with the stack as in test 2,
   then open the lobby. J: Launch vanilla, or modded with the same three.
2. J: join H. Expected: no dialog. J's main menu says Solo Easter Eggs "(and 2
   more)". If a "Different versions" dialog appears, write down what it names,
   then answer Yes.
3. H: start a match on The Giant. Expected: J goes along, and DS4C's art and the
   wider field of view show on both PCs. Play a round.
4. If either game crashes on H's game, start Xerces again on J and note what
   Home says. Expected: J's own mods are not put into safe mode because of it,
   unless J's own mods are the same stack.
5. Optional, only if J's Home shows a "Find the culprit" test from an earlier
   crash: start that test, join H, then quit. Expected: Home says the last test
   joined another player's game and did not count, and shows the same test.

## 4. Host on a custom map (map-first)

1. Both subscribe to the same custom map, for example SANCTUM OBITUS (with DS4C
   it needs map-first, DESIGN.md 6.10). To check that J follows H, J can type
   `set xerces_map_scripts_first 0` in J's console before joining.
2. H: with the D3 stack, pick the custom map in the lobby and start the match.
3. J: join H. Expected: J loads the map and plays; J's Copy report shows
   `loader.script.map_first` lines like H's.
4. J unsubscribes from the map (or H picks a map only H has):
   - J joins H's match. Expected: "Map not installed", naming the map and its
     Workshop page.
   - J joins H's lobby while that map is picked. Expected: the same dialog.
   - J is already in H's lobby when H starts that map. Expected: J drops to the
     menu with "You don't have the map ..." and a dialog that offers its
     Workshop page. Nothing says "Reconnecting to download".

## 5. Over the internet with UPnP

The two PCs on different internet connections, for example J on a phone hotspot.
H's router needs UPnP on (most home routers have it on). While H's lobby is
open, anyone who has H's address can join it (the network password does not
cover it), so H closes it when the test is done.

1. H: Launch vanilla, then open the lobby. Within a few seconds a toast says
   what the router answered:
   - "Router port open" with an address such as `203.0.113.7:27017`: give J that
     address.
   - "Router port open" saying the router did not say its internet address: H
     looks up their public IP address (a web search for "what is my IP") and
     gives J that address with port 27017.
   - "address shared" or "another router in front": the internet cannot reach H.
     Note it, then swap who hosts.
   - "No UPnP router found" or "Router refused the port": turn UPnP on in the
     router, or forward UDP port 27017 to H's PC by hand, then close and open
     the lobby again.
2. J: join that address. Expected: J is in H's lobby; then play a match.
3. H: close the lobby (the same button). Expected: a toast that the router no
   longer forwards the port; the router's UPnP list, if it shows one, no longer
   has "Xerces lobby (Black Ops III)".
4. H: open the lobby again, then quit the game. Expected: the router's entry
   goes too.
5. Optional: J hosts and H joins.

## What to collect afterwards

On both PCs, after any step that went wrong and once at the end:

1. Xerces > Diagnostics > Copy report, pasted into a text file named after the
   PC and test, for example `J-test2.txt`.
2. The game's console window: click into its text, press Ctrl+A, then Ctrl+C,
   and paste it into the same file. Do it before closing the game; the console
   closes with it.
3. A picture of any dialog or toast (Print Screen).
4. A sentence on what you did and what happened, with the time.

Send the files to the user privately.
