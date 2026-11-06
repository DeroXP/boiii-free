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
open, anyone who has H's address can join it unless H set a network password
(test 6), so H closes it when the test is done.

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

## 6. A password on the host

H's game checks the password itself (DESIGN.md 11.10, "The network password").
The same network is enough; over the internet it works the same. No mods. "The
console" is the game's console, where test 1 typed `connect`. Type the commands
exactly; `tiger7` stands for any word H picks.

Do the steps in this order: once J's game has given H the right password, or
joined H while H had no password, H's game lets J's address in until H's game
quits. So the refusals in steps 2 to 5 only show with a fresh start of H's game
and before step 6.

1. H: quit the game if it still runs from an earlier test, then Launch vanilla.
   In the console: `set net_password tiger7` (the password is not saved; set it
   again after each start of the game). Open the lobby.
2. J, a join that skips J's own check, as a modified client would: start the
   game (Launch vanilla), then in the console
   `set xerces_join_skip_password_check 1` and `connect <H's address>`.
   Expected: "This game needs a password, and the host's game refused the join
   ...", and J stays in the menu. Then `set xerces_join_skip_password_check 0`.
3. J: `connect <H's address>` with no password set. Expected: "This game needs a
   password. ..." (J's own game says it, before asking H).
4. J: `set net_password tiger8`, then `connect <H's address>`. Expected: "Wrong
   password. ..."; H's console shows "[Join] A joiner gave a wrong password".
5. J: repeat step 4 four more times within a minute (`connect` again each time).
   Expected: the sixth try says "Wrong password too many times ..." with a
   number of seconds. Wait that long before step 6.
6. J: `set net_password tiger7`, then `connect <H's address>`. Expected: J joins
   H's lobby; H's console shows "[Join] A joiner gave the right password".
7. H: start a match on The Giant. Expected: J goes along. J: leave to the menu,
   then `connect <H's address>` again while H plays. Expected: J joins the
   running match.
8. A raw join from J's PC, in PowerShell (Windows key, type PowerShell, Enter),
   while H's lobby is open and again while H plays a match. It sends the same
   lobby request and connect packet a modified game could, from a port that
   never passed the check. Put H's address and port in the first two lines:

   ```powershell
   $h = '192.168.1.20'
   $p = 27017
   $u = New-Object Net.Sockets.UdpClient
   $u.Client.ReceiveTimeout = 3000
   foreach ( $m in 'lobbyJoin match 00', 'connect x' ) {
       $b = [byte[]](
         @( 255, 255, 255, 255 ) + [Text.Encoding]::ASCII.GetBytes($m)
       )
       [void] $u.Send( $b, $b.Length, $h, $p )
       $from = New-Object Net.IPEndPoint( [Net.IPAddress]::Any, 0 )
       try {
           $r = $u.Receive([ref] $from)
           [Text.Encoding]::ASCII.GetString( $r, 4, $r.Length - 4 )
       } catch {
           'no answer'
       }
   }
   ```

   Expected: `lobbyDenied match password` for the lobby request, both times. For
   the connect: `no answer` while H is in the lobby (nothing runs to connect
   to), and
   `error This game needs a password, and the host's game refused the connect ...`
   while H plays. It must run on J's PC: H's game lets its own PC in.

9. H: quit the game (that forgets who passed), start it again without setting a
   password, and open the lobby. J: join with no password set. Expected: J
   joins, as in test 1.
10. Optional, if J has an older Xerces beta zip or BOIII: with H's password set,
    J joins with it. Expected: J's game stops with "Server requires a network
    password." or "Network password mismatch.", whatever J types.

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
