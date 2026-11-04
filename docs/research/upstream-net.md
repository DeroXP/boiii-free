# Upstream networking, servers and social

> Machine-generated research notes from 2026-11-04, kept as evidence for `docs/DESIGN.md`.
> Code was read at upstream `main` 5aa7fac, `mod-stack` ac356c3c and the fork `refs/heads/main` 7805319.
> `path:line` citations refer to those commits. Treat claims as leads: anything marked runtime still needs the real game.

## Summary

Transport. Upstream uses raw IPv4 Winsock UDP. It does not use Steam networking or a networking library of its own. boiii creates one non-blocking UDP socket itself (network.cpp:102-140): SO_BROADCAST is set, it binds INADDR_ANY:net_port (default 3074) and tries up to 10 ports. It then writes the socket into the engine's ip_socket global, and on clients into the listen-server socket global too. The engine's own bind() is neutralised (network.cpp:429-431). Engine netchan traffic still uses the engine paths: NET_SendPacket to sendto, and Sys_GetPacket from recvfrom. Both Winsock calls pass through the game-image IAT hooks that demonware.cpp installs (demonware.cpp:352-405), which pass non-Demonware traffic through. The engine checksum is stripped and a 1-byte socket/netID header is used. boiii's own control plane is a set of connectionless out-of-band (OOB) commands: 0xFFFFFFFF + name + separator + payload. They are sent with a direct sendto on the same socket (network::send_data) and dispatched by intercepting the engine's connectionless-packet dispatcher (client call site select(0x14134D166,...), server 0x14018E698). ISteamNetworking P2P is stubbed. Demonware is emulated in-process. There is no encryption, no relay and no IPv6 (netadr_t is IPv4-only). Every engine lobby message from a non-loopback source is dropped (network.cpp:179-182). security.hpp, which holds lobby-packet inspectors, is no longer included anywhere and only has legacy-client offsets.

Connect and join. The connect command is replaced by party::connect_stub. The client first sends getInfo with a random challenge. It then checks the infoResponse: protocol 8, sub_protocol 1 or 0, gamename T7, a client-side-only net_password_hash compare, map/gametype, and modId/workshop_id/sv_wwwBaseURL. Missing usermaps come from FastDL (HTTP manifest.json plus SHA1) or a steamcmd prompt; missing mods come from steamcmd. The client loads the host's single mod (setup_same_mod_as_host), then calls CL_ConnectFromLobby with the initial connstate patched to CHALLENGING. A reimplemented CL_CheckForResend sends getchallenge. The server answers with getChallengeResponse carrying a random 32-byte challenge kept per source address for 10 minutes. The client then sends a fragmented OOB connect: its ECC public key, an ANSI-X9.62 signature of SHA-512(challenge), its profile DDL blob, and the userinfo string (including the password dvar). The server checks the signature, checks that the userinfo xuid equals the first 8 bytes of SHA1(pubkey), and checks the name characters. It then calls SV_DirectConnect, which is rate-limited per IP on dedicated servers only. After that it broadcasts playerXuid, profileInfo and name-override sync, which clients accept only from the host's source address. The identity key is derived deterministically from hardware IDs and stored at LOCALAPPDATA/boiii/user/ezz-private.key. A dedicated server uses a new random GUID every run.

Discovery, NAT traversal and friends. Server listing uses the DP-master style protocol: getservers T7 8 full empty / getServersResponse and the dedicated-side heartbeat T7. The masters are master.ezz.lol:20810 and m.ezz.lol:20810, editable in boiii_players/user/master_servers.txt. Favorites and recent servers are kept in text files. Each listed server is then queried with getInfo. The same Ezz masters also act as a UDP rendezvous for "friends can join" (nat.cpp): privRegister, privJoin, privPeer and privReject, then direct punch/punchAck on the game socket, with a 12 s timeout and fallback to a direct IP. Friend presence uses friendPublish and friendLookup, keyed by the "BOIII Friend Code", which is the identity key hash. upnp.cpp maps the UDP port through Windows IUPnPNAT COM. There is no relay and no TURN. The friends list is a local friends.json with at most 200 entries, managed by the launcher and by Lua. The real Steam client (steamclient64.dll through steam_proxy) is used only for rich presence, persona names and a partly wired invite path; the Steam join callback and the boiii_game_info presence path are dormant. Discord is presence-only through discord-rpc (ariloc buttons fork, app id 967371125573177474).

Protections and gaps. Existing protections:
- Dedicated servers: per-IP connect rate limit (5 per 10 s, -noratelimit disables it); chat sanitization; Cmd_ParseArgs RET patch; TeamOps arbitrary-write NOP; luinotify dedup (-mitigatepacketspam); rcon with a 500 ms per-address limit and a plaintext password; mute commands.
- All builds: reliableAcknowledge sanity check.
- Clients: party_count clamp, OOB echo disabled, error-string truncation, voice/mic killed, Netchan div-by-zero fix.
- FastDL: path-traversal checks and a user confirmation.

Missing:
- There is no general OOB rate limiting. getInfo and getchallenge are answered by every boiii process, including non-hosting clients, which leaks the Friend Code.
- getChallengeResponse is accepted from any sender.
- Game traffic is unauthenticated plaintext.
- There is no ban system.

Lua networking. Axios (plain HTTP only) and NET (inert flags) sit behind a single session-wide unsafe-Lua approval. That approval is automatic on dedicated servers. game.httpget/game.httppost (libcurl HTTPS) and the social/kick/economy game.* functions are not gated at all, so any LUI script can call them.

Boundaries for Xerces. Demonware emulation works through IAT Winsock hooks that redirect seven DW hostnames to in-process servers; Xerces must treat it as a frozen compatibility shim. Steam ownership, licensing, auth-ticket and VAC/anticheat code, and Arxan/integrity code, are fenced (see off_limits). Every networking hook in this area uses a hardcoded per-exe address chosen by a 3-way select (new client / legacy client / dedicated server, picked by PE checksum) or a server-only _g address. None uses the existing signature scanner, so every site listed in details_markdown needs a pattern-scan port. The best insertion points for an encryption, relay or hole-punch layer are the boiii-owned socket creation, the game-image sendto/recvfrom IAT slots (already hooked), and network::send_data. Engine-level NET_SendPacket/Sys_GetPacket are a second option; a full NET_SendPacket reimplementation already exists but is commented out. The DeroXP fork (refs/heads/main) touches none of these networking files. nat.cpp and upnp.cpp were added upstream after the fork base (b493c14a, 2026-07-27).

## Facts

- **verified**: Game networking is raw IPv4 UDP over one socket that boiii creates (non-blocking, SO_BROADCAST, bound INADDR_ANY:net_port with up to 10 increments) and injects into the engine's ip_socket global; on clients the same socket is also written into the listen-server socket global. (src/client/component/network.cpp:102-140, 418; symbols/net/core.hpp:10 (ip_socket {0x157DF68E8, 0x157E75818, 0x14A640988}); network.cpp:137 select(0x14A5B09D8, 0x14A640988, 0))
- **verified**: The engine is prevented from binding its own sockets (bind pointer slot replaced with bind_stub returning 0). (network.cpp:191-194, 429-431 select(0x15AA6A38C, 0x15AAE9344, 0x14B4BD828))
- **verified**: If net_port is busy, create_ip_socket binds port+1..+9 but never writes the real port back, so nat/upnp/friends advertise and map game::port() (the dvar value), not the bound port. (network.cpp:119-133; nat.cpp:110-113 local_port(); nat.cpp:256 upnp::open_port(local_port()); party.cpp:495; game/utils.hpp:253-259)
- **verified**: Engine packet checksum is disabled on both send and receive. Engine packets carry a 1-byte header ((netsrc & 0xF) | (localNetID & 0xF) << 4). boiii OOB packets omit that byte and the receiver synthesizes NS_SERVER|(NS_SERVER<<4) when the first 4 bytes are 0xFFFFFFFF. (network.cpp:147-161, 359-371; header layout shown in the disabled reimplementation net_sendpacket_stub network.cpp:317-349 (343-345))
- **verified**: boiii's own OOB sends bypass the engine: network::send prefixes 0xFFFFFFFF + command + separator and send_data calls sendto directly on *ip_socket (from boiii's module, so not through the game-image IAT hooks). (network.cpp:221-252; demonware.cpp:352-370 hooks only the game module IAT (utils::nt::library{} = game image because main.cpp:535-540 sets PEB ImageBaseAddress to the loaded game))
- **verified**: Inbound OOB commands are intercepted inside the engine's connectionless dispatcher, case-insensitively, with exceptions swallowed. A handled command stops engine processing. Client hook: call site select(0x14134D166, 0x14134D146, -) via handle_command_stub. Server hook: call site 0x14018E698 via cl_dispatch_connectionless_packet_stub. (network.cpp:29-95, 400-409)
- **verified**: Every engine lobby message (party/lobby/voice/migration LobbyMsg) from a non-loopback source is dropped wholesale by the handle_packet_internal detour ('Kill lobby system'). (network.cpp:169-189, 420-423 select(0x141EEB860, 0x141EF7FE0, 0x1404A5B90); commit 8bfe71f3 (2026-03-05) replaced the call to ezzse<local scratch copy, not kept>:InspectPacket with an unconditional drop)
- **verified**: security.hpp (lobby packet inspectors) is dead code: no file includes it and InspectPacket is never called. Its function pointers are select(0x0, legacy, 0x0), so they would be null on the current client and on the server. (grep finds no reference to security.hpp/InspectPacket outside the file; security.hpp:127-167, 977-1034)
- **verified**: Steam P2P networking (ISteamNetworking) is entirely stubbed. No Steam Datagram Relay or networking sockets are used. (src/client/steam/interfaces/networking.cpp:6-112 (all return false/0/NULL))
- **verified**: Demonware is emulated in-process. The game image's Winsock imports (send, recv, sendto, recvfrom, connect, select, closesocket, ioctlsocket, gethostbyname, getaddrinfo, freeaddrinfo, getpeername, getsockname) are IAT-hooked. Lookups of stun.{us,eu,jp,au}.demonware.net, ops3-pc-auth3.prod.demonware.net, ops3-pc-lobby.prod.demonware.net and prod.umbrella.demonware.net are routed to local servers; all other traffic passes through to real Winsock. (demonware.cpp:93-350, 352-370, 380-405)
- **verified**: The game's own curl has TLS peer/host verification disabled and DW URLs rewritten to http://. prod.uno.demonware.net is rewritten but is not registered as an emulated host. (demonware.cpp:411-435 (uno at 428-429), registry 380-389)
- **likely**: Game traffic has no encryption or message authentication after connect, and the engine checksum is removed. Host-only OOB messages (playerXuid, profileInfo, nameoverride) are trusted purely by source address. (network.cpp send/recv paths contain no crypto; auth.cpp:518-532 and profile_infos.cpp:220-237 gate on party::is_host(addr); name.cpp:47 is_syncable_address)
- **verified**: Player identity is a libtomcrypt ECC key generated with ecc_make_key(keysize 512/8) from Yarrow seeded by SMBIOS UUID, MachineGuid, HwProfileGuid, <local scratch copy, not kept>volume serial and a constant. It is persisted at LOCALAPPDATA/boiii/user/ezz-private.key with a .bak copy, and the public key goes to ezz-public.key. The -alias flag selects a second key. XUID/GUID is the first 8 bytes of SHA1(public key). (auth.cpp:32-87, 91-197, 539-547; utils/cryptography.cpp:242-249, 258-266; utils/properties.cpp:109-127; game/flags.cpp:93-96)
- **verified**: The emulated SteamUser::GetSteamID, LiveUser_GetXuid, LiveUser_UserGetXuid and LiveUser_GetLocalXuid all return the boiii key hash. The launcher shows the same value as the 'BOIII Friend Code'. (steam/interfaces/user.cpp:10-20; auth.cpp:597-601, 731-735; live.cpp:49-51, 214-216; launcher.cpp:2030-2049)
- **verified**: Dedicated servers use a fresh random GUID per process (0x110000100000000 | random), so they have no persistent identity. (auth.cpp:549-557)
- **verified**: Challenge handshake: any boiii process answers getchallenge with a raw 'getChallengeResponse ' + 32 random bytes. The challenge is stored per source netadr, stays valid 10 minutes, is not consumed on use, and stale entries are evicted every 5 minutes. (auth.cpp:342-445, 629, 637)
- **verified**: The client accepts getChallengeResponse from any sender without checking it is the server being connected to. It copies the 32 bytes and flips CHALLENGING to CONNECTING. (auth.cpp:208-234 (target parameter unused))
- **verified**: Connect packet: the client builds userinfo (name, protocol 18532, netfieldchk, sessionmode, engine challenge int, invited, xuid hex, qport, migrating, password, clanAbbrev). It sends string(pubkey) + string(signature over SHA-512 of the challenge) + profile_info{int32 version, string ddl} + string('connect "<userinfo>"'), fragmented into 1 KiB OOB 'connect ' packets through NET_OutOfBandData. (game/impl/cl/cl.cpp:163-268; auth.cpp:236-284; fragment_handler.cpp:27, 114-135; utils/cryptography.cpp:269-291)
- **verified**: Server-side connect verification checks the ECDSA signature against the challenge issued to that address, that the userinfo xuid equals the key hash, and that the name is non-empty without '%', '~' or characters outside 32-126. It answers 'error Bad signature/Bad XUID/Bad name'. On success it distributes profile info, calls SV_DirectConnect and sets client.xuid plus playerXuid broadcasts. (auth.cpp:447-516, 286-336)
- **verified**: The engine's native connect OOB handler is skipped, and the client connect state starts at CHALLENGING instead of the DW-based flow. (auth.cpp:631-633 select(0x1421F73CA, 0x142253EFA, 0x14053714A); network.cpp:377-380 CL_ConnectFromLobby+select(0x170,0x170,0x154))
- **verified**: CL_CheckForResend is fully reimplemented because Arxan mutates the original at runtime. All known call sites plus the function itself are redirected. Only local client 0 sends getchallenge, every 3 s. (client_patches.cpp:512-557; game/impl/cl/cl.cpp:50-311 (270-303))
- **verified**: Pre-connect validation: getInfo must report protocol==8, sub_protocol 1 or 0, gamename T7, non-empty mapname and gametype, and a matching net_password_hash when the server publishes one. (party.cpp:164-249; game/structs/core.hpp:12-13 (PROTOCOL=8, SUB_PROTOCOL=1))
- **verified**: net_password is enforced only by the joining client. The server publishes an unsalted FNV-1a-64 of the password in getInfo and never validates anything from the client, so it is isolation/filtering, not access control. (network_password.cpp:20-35 (TODO at 27); getinfo.cpp:159-162; party.cpp:212-233; no server-side check found in repo)
- **likely**: The server password (g_password) is sent in cleartext as the 'password' userinfo key and is presumably checked by the engine in SV_DirectConnect. getInfo exposes only isPrivate. (cl.cpp:230-237; auth.cpp:643-648 (password dvar DVAR_USERINFO); getinfo.cpp:133; game/utils.hpp:194-198; data/ui_scripts/server_password/__init__.lua:88-89)
- **verified**: Mod negotiation is single-mod. The server advertises modId (loaded mod publisher id), modName, workshop_id/usermapId and sv_wwwBaseURL/sv_wwwBaseUrl. The client loads the same single mod via UGC_LoadModByPublisherId_Impl, or prompts for a steamcmd download. (getinfo.cpp:148-157; party.cpp:251-292; workshop.cpp:873-910, 986-1011)
- **verified**: FastDL covers usermaps only. The client fetches <sv_wwwBaseURL>/usermaps/<map>/manifest.json (array of [name,size,sha1]) and asks the user via MessageBox. Files go sequentially into ./usermaps/<map>/ with size and SHA1 checks (workshop.json last). Paths are traversal-checked, redirects are followed and http:// is allowed. The manifest is server-controlled, so it gives integrity, not authenticity. (workshop.cpp:799-815; fastdl.cpp:88-109, 120-180, 182-309, 370-437 (curl opts 223-238))
- **verified**: FastDL treats .dll as a map file type and will write server-supplied DLLs into usermaps/<map>/. boiii does not auto-load them (plugins load only from appdata/plugins and boiii/plugins), but unsafe-approved Lua package.loadlib could. (fastdl.cpp:76-86; plugins.cpp:33-38; ui_scripting.cpp:1348, 1750-1807)
- **verified**: sv_wwwDownload and sv_wwwDlDisconnected are registered but never read. FastDL is enabled solely by a non-empty sv_wwwBaseURL. (getinfo.cpp:77-81; grep finds no other use; workshop.cpp:799)
- **verified**: Master servers default to master.ezz.lol:20810 and m.ezz.lol:20810. They are read once (call_once) from boiii_players/user/master_servers.txt, which is written with defaults if missing. (server_list.cpp:228-229, 286-329)
- **verified**: Server list protocol: OOB 'getservers T7 8 full empty' goes to every master. The 'getServersResponse' payload is parsed as repeated 7-byte records [IPv4 4B][port 2B BE]['\\'], results are merged across masters, and the timeout is 2 s. (server_list.cpp:42-124, 331-359, 430-456)
- **verified**: Per-server info queries use 'getInfo <16-hex random>'. The answer 'infoResponse' (info string, newline separator) is matched by host and challenge, with a 1 s timeout. At most 39 sends go out per 100 ms tick, and only while Com_IsRunningUILevel(). (party.cpp:382-456, 528; getinfo.cpp:164)
- **verified**: getInfo is answered by every boiii process (generic component, no host check), including non-hosting clients. The reply includes xuid, which is the Friend Code of a client or the random GUID of a dedicated server, and it is not rate-limited. (getinfo.cpp:70, 94-165 (xuid at 131))
- **verified**: Dedicated servers send 'heartbeat T7' to all masters at start (+5 s), on every G_InitGame, every 5 minutes and on the 'heartbeat' command. sv_lanOnly suppresses it. (dedicated/command.cpp:17-31, 66-87)
- **verified**: Favorites (favorite_servers.txt) and recent servers (recent_servers.txt, at most 50) persist via ISteamMatchmaking::AddFavoriteGame. lan_servers.txt (lan_add command) is written but never read, and RequestLANServerList is a stub. PlayerDetails/ServerRules are unimplemented and there is no getstatus handler. (server_list.cpp:150-283, 361-421, 469-476; matchmaking.cpp:85-87; matchmaking_servers.cpp:297-301, 661-669)
- **verified**: Rendezvous/hole-punch protocol (client to Ezz master). privRegister <hostToken> [local] goes out every 5 s while hosting; the master replies privRegisterAck <reflected ip:port>. privJoin <token> [local] goes out every 1 s until a privPeer <token> <cand...> (at most 16 candidates) or privReject arrives. Peers exchange punch/punchAck <token> every 250 ms on the game socket. The join completes by party::connect(sender); after 12 s the client falls back to a direct address or shows an error popup. (nat.cpp:25-27, 154-164, 221-357, 430-448, 497-514)
- **verified**: Rendezvous replies are accepted only if the sender IP:port equals a configured master address, and tokens must be 8-64 chars [A-Za-z0-9_-]. (nat.cpp:90-97, 147-152, 286, 298, 333, 362, 393)
- **verified**: Host/join tokens have at most 32 bits of entropy: a fresh std::mt19937 seeded with one random_device value is used per token. (nat.cpp:99-108)
- **verified**: Friend presence: hosts publish 'friendPublish 1 <friendCode> <hostToken> [local]' unsigned. Clients send 'friendLookup 1 <requestId> <friendCode> [local]' for each saved friend and collect friendPresence (5 fields incl. token and endpoint) or friendOffline over a 2 s window. (nat.cpp:166-181, 359-413, 458-489)
- **verified**: No relay or TURN exists anywhere. When hole punching fails, the only alternatives are the UPnP-mapped endpoint, the master-reflected endpoint, or the LAN endpoint. (nat.cpp:221-231, 420-428; upnp.cpp)
- **verified**: UPnP uses Windows IUPnPNAT/IStaticPortMappingCollection COM to map the UDP port to the local IPv4 ('BOIII friend match') asynchronously, and records the external IP. Mappings are removed on close or shutdown. There is no NAT-PMP/PCP. (upnp.cpp:46-137)
- **verified**: External dependencies: Ezz masters (UDP 20810), api.ipify.org (public IP, 1 s timeout), UDP connect() route tricks to 1.1.1.1:53 and 8.8.8.8:53 (no packets sent) for local IP, the local Steam client, the local Discord client, api.steampowered.com and steamcmd for workshop. (server_list.cpp:287-288; friends.cpp:84-95, 295-313; nat.cpp:115-145; upnp.cpp:17-40; workshop.cpp:719-722; steamcmd.cpp:735-739)
- **verified**: The friends list is local: boiii_players/user/friends.json [{steam_id, name}], deduplicated, at most 200 entries. Despite the field name, entries are BOIII Friend Codes (identity key hashes) added in the launcher ('BOIII Friend Code') or via Lua game.addfriend. Steam's friend list is not used (access_steam_friends is never called). (friends.cpp:30-31, 135-240, 330-354; launcher.cpp:2027-2140; data/launcher/main.js:4185-4306; steam_proxy.cpp:527-563 unused)
- **verified**: The server browser 'Friends' tab is backed by nat friend lookups. Offline friends appear as RFC 5737 TEST-NET placeholder rows 192.0.2.x, which party::connect_stub maps back to the friend via browser routes before any connection is attempted. (matchmaking_servers.cpp:192-245, 303-345, 500-618; party.cpp:335-340; friends.cpp:697-730)
- **verified**: The Steam invite/join path is dormant. invite_to_game (Lua game.invitefriend, not called by shipped UI) passes the Friend Code as a SteamID to the real InviteUserToGame. The 'boiii_game_info' rich-presence key is read but never written. The GameRichPresenceJoinRequested callback hook is only installed inside get_pending_game_invite, which nothing calls. (friends.cpp:401-443, 479-512, 557-582; steam_proxy.cpp:62-92, 565-635; grep of data/ and src/ for callers)
- **verified**: Real-Steam proxied functionality via the user's steamclient64.dll (IClientEngine interfaces and SteamClient017/SteamFriends015): rich presence set/clear (the game's own SetRichPresence is forwarded), persona name (seeds the player name), friend names, friend rich-presence get/request, InviteUserToGame, UGC subscribed items. (steam_proxy.cpp:191-225, 376-433, 444-520, 565-587, 641-654, 749-774; steam/interfaces/friends.cpp:118-124; name.cpp:130; steam/steam.cpp:94-107)
- **verified**: Discord integration is presence only. It uses discord-rpc (submodule ariloc/discord-rpc-buttons), app id 967371125573177474, ready/errored/disconnected handlers, callbacks every 1 s and presence every 5 s. Presence covers map, mode, score and rounds from Lua setters, party size/max, and buttons to discord.gg/ezz and forum.ezz.lol. There are no join/spectate secrets, and the DiscordRPC Lua library is stubbed. (discord.cpp:106, 229-376, 401-416, 425-440; .gitmodules deps/discord-rpc)
- **verified**: rcon exists only on dedicated servers (server_component). Format: OOB 'rcon <password> <command>'. The password is compared in plaintext (!=), the limit is 1 request per rcon_timeout ms (default 500, range 100-10000) per source address, and output is captured and returned in one 'print' OOB. The engine's SVC_RemoteCommand restrictions are removed. (rcon.cpp:16-100; network.cpp:396-398 (0x140538D5C))
- **unverified**: boiii's rcon has no explicit guard against an empty rcon_password. If the engine tokenizer yields an empty argv for a quoted empty string, 'rcon "" <cmd>' would pass the check when no password is set. (rcon.cpp:18-31 (no empty check); tokenizer is engine Cmd_TokenizeStringKernel (command.cpp:82-84))
- **verified**: Dedicated protections: per-IP SV_DirectConnect limit of 5 attempts per 10 s (cleanup every 30 s, disabled by -noratelimit); G_Say sanitization (control chars, printf specifiers, 0x80-0x9F, 512-char cap); Cmd_ParseArgs patched to RET at 0x1404B2E00 (RCE fix); TeamOps arbitrary write NOPed at 0x1401155D5; a disconnect drops only that client, not every client from the IP; luinotify reliable-command dedup behind -mitigatepacketspam. (dedicated/networking.cpp:23-136, 138-241, 288-368)
- **verified**: The connect rate limit runs inside SV_DirectConnect, i.e. after fragment reassembly and ECDSA verification, keys on IPv4 only, and exists only on dedicated servers. Listen servers (private matches) have none. (auth.cpp:497 then dedicated/networking.cpp:140-147; networking is a server_component)
- **verified**: Other exploit fixes: reliableAcknowledge sanity check before SV_ExecuteClientMessage (all builds); party_count clamp in presence parsing (client); OOB 'echo' disabled and 'error' string truncated (client); voice packet handler replaced with 'xor rax,rax; ret' and mixerOpen stubbed; Netchan_AllocMessage divide-by-zero guard; non-loopback lobby messages dropped. (patches.cpp:383-392, 566-568; live_presence_patch.cpp:17-47; network.cpp:403-413, 425-427; client_patches.cpp:383-390, 467-469; network.cpp:179-182)
- **verified**: There is no ban system. Admin tooling is limited to engine kick commands, Lua game.kickplayer (host only, SV_DropClient) with the kick_menu UI, muteclient/unmuteclient console commands, server say/tell, the status column patch, rcon, and the g_log games_mp.log (IW4M compatible). (grep for ban finds nothing; ui_scripting.cpp:664-819; chat.cpp:52-78, 199-243; status.cpp; game_log.cpp:42-88; dedicated/command.cpp:74-75)
- **verified**: Lua Axios (Get*, Post*) and NET (Disable/Enable/IsDemonware/Steam) are wrapped in lua_state::unsafe_function. That allows the call if is_server(), if -unsafe-lua was passed, or after the user clicks Yes once in a process-wide MessageBox; the approval then applies to every script and every unsafe API. (lua_state.hpp:38-47; axios.cpp:84-103; net.cpp:53-71; ui_scripting.cpp:55-91, 1294-1296)
- **verified**: Axios supports only plain http:// (HTTPRequest.hpp rejects other schemes) and runs synchronously on the Lua thread. Axios.Ping is a stub. The NET library flags are inert (nothing outside net.cpp reads them). (utils/HTTPRequest.hpp:916-917, 968-969; axios.cpp:13-75; net.cpp:12-43 + grep)
- **verified**: game.httpget(url) and game.httppost(url, body) are NOT gated. They call libcurl (HTTPS with verification, redirects followed; file:// disabled at build) from any LUI script. (ui_scripting.cpp:821-837; utils/http.cpp:87-189; deps/premake/curl.lua:27-44 (CURL_DISABLE_FILE at 32))
- **verified**: Other ungated game.* Lua functions with side effects: addfriend, removefriend, invitefriend, connecttofriend, connectsocialfriend, refreshfriends (master traffic), kickplayer (host), savestats, purchasecodpoints/vials/distills, cookgobblegumrecipe, resetgobblegums, setcurrenciesmaxed, hotreloadstart(path), getappdatapath, setDiscord*. (ui_scripting.cpp:570-949)
- **likely**: Mod/usermap LUI code runs in the same primary Lua VM as the boiii ui_scripts and can therefore reach the game.* globals and the unsafe-gated libraries. (ui_scripting.cpp:1008-1032 (globals set in primary VM, __newindex removed by enable_globals 951-964); KNOWN_MALICIOUS_MODS logic 966-1006 exists because mod Lua reads Engine.IsBOIII)
- **verified**: patch_unsafe_lua_functions runs on clients only. On the current (new) client the os.* functions are not hooked because their addresses are 0x0. Dedicated servers load lobby_scripts with no gating. (ui_scripting.cpp:1285-1349 (os_* only if is_legacy_client, 1306-1314), 2588-2609; symbols/lua/hks.hpp:243, 247; ui_scripting.cpp:1038)
- **verified**: Lua DLL loading goes through a load_dll detour with a name blacklist (T7Overcharged, discord_game_sdk, AAE, T7Recharged, CoDAxiosUtility, discord-rpc, LibCurlShim, arxan, dependencies). -disable-loadlib stubs every load; package.loadlib is additionally unsafe-gated. (ui_scripting.cpp:1750-1807, 2565, 1348; game/flags.cpp:83-86)
- **verified**: No networking hook in this area uses pattern scanning. All use hardcoded addresses via game::select(new, legacy, server), chosen by PE checksum, or server-only _g constants. utils::hook::signature exists but is used only by arxan.cpp. (game/base.cpp:36-49; game/ptr.hpp:62-84, 237; src/common/utils/signature.hpp; grep shows only arxan.cpp includes signature.hpp)
- **verified**: The DeroXP fork (refs/heads/main) does not modify any file in this area. Its changes are limited to client_patches, console_logger, download_overlay, engine_perf, patches, perf, script, updater, workshop, symbols.hpp and tools/*.py. (git diff --name-only d80d308 refs/heads/main)
- **verified**: nat.cpp, nat.hpp, upnp.cpp and upnp.hpp did not exist at the fork base d80d308. They arrived with the friends-system rewrite in b493c14a (XFrozedyX, 2026-07-27). (git cat-file -e d80d308:src/client/component/nat.cpp fails; git show --stat b493c14a)

## Details

# Upstream networking, servers, social: reference for Xerces (upstream 5aa7fac)

All paths are relative to `src/client/` unless shown otherwise. Addresses are written `(new client, legacy client, server)` as passed to `game::select`. `_g` means a single absolute address, relocated, and almost always a dedicated-server address. Exe variants are picked by PE optional-header CheckSum: new `0x6531394`, legacy `0x888C368`, server `0x14C28B4` (`game/base.cpp:36-49`).

---
## 1. Transport

### 1.1 Socket model
- **One** IPv4 UDP socket for everything. boiii creates it in `network::create_ip_socket` (`component/network.cpp:102-140`), scheduled once on the main pipeline at line 418:
  - `socket(AF_INET, SOCK_DGRAM)`, SO_BROADCAST, non-blocking.
  - bind `INADDR_ANY:net_port`, then +1 up to 10 attempts. The real port is printed but **not written back** to `net_port`.
  - Stored into the engine `ip_socket` symbol `{0x157DF68E8, 0x157E75818, 0x14A640988}` (`game/symbols/net/core.hpp:10`). On clients it is also stored into the listen-server socket global `select(0x14A5B09D8, 0x14A640988, -)` (network.cpp:135-139).
- Engine `bind` is disabled: the pointer slot `select(0x15AA6A38C, 0x15AAE9344, 0x14B4BD828)` is set to `bind_stub` (network.cpp:429-431).
- Default ports: client `net_port` 3074 (`game/utils.hpp:253-259`). The connect/LAN default for servers is 27017 (`party.cpp:29`, `server_list.cpp:242`).
- Other sockets: temporary UDP sockets that only `connect()` to 1.1.1.1:53 or 8.8.8.8:53 to learn the local IP (nat.cpp:115-145, upnp.cpp:17-40, friends.cpp:295-313). HTTP uses libcurl (TCP/TLS) and HTTPRequest.hpp (plain TCP). Demonware TCP and UDP are emulated in-process. Steam uses steamclient IPC and Discord uses an IPC pipe.
- No IPv6: `netadr_t` = {ipv4 union, port, type, localNetID} (`game/structs/net/core.hpp:90-99`), with types NA_BOT, NA_BAD, NA_LOOPBACK, NA_RAWIP, NA_IP.

### 1.2 Wire formats
| Kind | Layout | Evidence |
|---|---|---|
| Engine packet (netchan or engine OOB) | `[1 byte: (netsrc&0xF) \| (localNetID&0xF)<<4]` + engine payload; engine checksum disabled | network.cpp:343-345 (disabled reimpl.), 359-371 |
| boiii OOB | `FF FF FF FF` + command + separator (' ' default, '\n' for infoResponse) + data; no socket byte. The receiver detects the missing byte (`read_socket_byte_stub`, 147-157) | network.cpp:221-229 |
| Fragmented OOB (`connect `, `profileInfo`) | `[u64 id][u32 count][u32 index][<=0x400 bytes]` per packet. Reassembled per source netadr. MAX_FRAGMENTS=100 (fragments per id, and ids per address); 5 s expiry; `clean()` every 5 s | game/fragment_handler.cpp:7, 27, 41-87, 89-112, 114-135; network.cpp:357 |

Note: `fragment_handler.cpp:49` checks `fragment_index > fragment_count` where it should be `>=`. An out-of-range index is stored and later makes `.at()` throw, which `handle_command` catches. The global map has no cap across source addresses, so spoofed sources can grow memory for up to 5 s.

### 1.3 Hook points (all hardcoded today)
| # | Purpose | Site | file:line |
|---|---|---|---|
| H1 | Socket creation/injection | `create_ip_socket`; `ip_socket` symbol; client server-socket `select(0x14A5B09D8,0x14A640988,-)` | network.cpp:102-140 |
| H2 | Engine bind disabled | `select(0x15AA6A38C,0x15AAE9344,0x14B4BD828)` = bind_stub | network.cpp:429-431 |
| H3 | Inbound read | `Sys_GetPacket {0x1422B9040,0x1423321B0,0x140596CF0}` (symbols/sys.hpp:21): +0xD3 call read_socket_byte_stub, +0x106 nop4, +0x111 call verify_checksum_stub | network.cpp:359-368 |
| H4 | Outbound send | `NET_SendPacket {0x1422B9240,0x1423323B0,0x140596E40}` +0xEE = 0 (no checksum). Full replacement `net_sendpacket_stub` exists but is commented out | network.cpp:317-349, 370-375 |
| H5 | Winsock IAT of game image | send/recv/sendto/recvfrom/connect/select/closesocket/ioctlsocket/gethostbyname/getaddrinfo/freeaddrinfo/getpeername/getsockname to demonware `io::*_stub` (pass-through unless the destination is an emulated DW host) | demonware.cpp:93-350, 352-370, 391-405 |
| H6 | boiii OOB send | `network::send/send_data`: direct `sendto(*ip_socket)` from boiii module | network.cpp:221-252 |
| H7 | OOB dispatch (client) | call in CL_DispatchConnectionlessPacket `select(0x14134D166,0x14134D146,-)` to handle_command_stub; 'echo' disabled `select(0x14134D11B,0x14134D0FB,-)`=0xEB; OOB 'error' to com_error_oob_stub `select(0x14134D226,0x14134D206,-)` | network.cpp:403-413 |
| H8 | OOB dispatch (server exe) | call `0x14018E698` to cl_dispatch_connectionless_packet_stub; SVC_RemoteCommand `0x140538D5C` to con_restricted_execute_buf_stub | network.cpp:395-401 |
| H9 | Engine 'connect' OOB handler skipped | `select(0x1421F73CA,0x142253EFA,0x14053714A)`=0xEB | auth.cpp:631-633 |
| H10 | Lobby messages killed | detour `select(0x141EEB860,0x141EF7FE0,0x1404A5B90)`: drop every non-loopback | network.cpp:169-189, 420-423 |
| H11 | Initial connstate CHALLENGING | `CL_ConnectFromLobby {0x14134C590,0x14134C570,0x14018E420}` + `select(0x170,0x170,0x154)` | network.cpp:377-380 |
| H12 | Don't kick clients without DW handle | `select(0x1421F137D,0x14224DEAD,0x1405315F9)`=0xEB | network.cpp:382-384 |
| H13 | NetAdr_ToString: skip DW, NA_IP to NA_RAWIP | `select(0x14211A432,0x142172EF2,0x140515881)`, `select(0x14211A414,0x142172ED4,0x140515864)` | network.cpp:386-393 |
| H14 | Voice killed / mic killed | `select(0x141359330,0x141359310,0x14018FE40)` = `48 31 C0 C3`; mixerOpen slot `select(0x15AA6A29C,0x15AAE9254,-)` | network.cpp:425-427; client_patches.cpp:467-469 |
| H15 | cl_maxpackets max 100 | `select(0x1412FF362,0x1412FF342,0x140177A32)` | network.cpp:433-435 |
| H16 | `connect` command | jump `select(0x141ED9670,0x141EE5FE0,-)` to party::connect_stub | party.cpp:523-524 |
| H17 | CL_CheckForResend replaced | detour `{0x14134B990,0x14134B970,-}` + call sites `select(0x141359DD4,0x141359DB4,-)`, `select(0x1413514DE,0x1413514BE,-)` | client_patches.cpp:548-557; game/impl/cl/cl.cpp:50-311 |
| H18 | Signed connect send | cl.cpp:259-261 to auth::send_fragmented_connect_packet | auth.cpp:264-284 |
| H19 | SV_DirectConnect `{0x1421ECD30,0x142249880,0x14052EC60}` | dedicated rate-limit detour; bot call site `select(0x1421EC58C,0x1422490DC,0x14052E582)` | dedicated/networking.cpp:138-147, 302-306; auth.cpp:640-641 |
| H20 | reliableAck sanity | call `select(0x1421F7D6C,0x14225489C,0x140537C4C)` | patches.cpp:383-392, 566-568 |
| H21 | Server-browser Lua table | detour `select(0x141F13590,0x141F1FD10,-)` | server_list.cpp:458-460 |
| H22 | Presence party_count clamp | detour `select(0x141E84D90,0x141E91820,-)` | live_presence_patch.cpp:44-47 |
| H23 | Netchan_AllocMessage div0 | `Netchan_AllocMessage {0x14211AED0,0x142173990,0x140516000}` +0x31 | client_patches.cpp:383-390 |
| H24 | Dedicated security | G_Say `{0x141976610,0x1419765F0,0x140299170}` detour; `0x1404B2E00`=RET; `0x1401155D5` nop7; SV_Live_RemoveAllClientsFromAddress `{0x1421F7B00,0x142254630,0x1405379E0}`; SV_AddServerCommand `{0x1421F6930,0x142253460,0x140536660}` | dedicated/networking.cpp:288-368 |
| H25 | Chat / status / bots | chat.cpp:192, 202, 220, 240, 264, 274; status.cpp:38-53; bots.cpp:158-165 | (see files) |
| H26 | DW patches | demonware.cpp:411-449 (11 sites) | demonware.cpp |
| H27 | Other used symbols | NET_OutOfBandData `{0x14211AB40,0x142173600,0x140515C80}`, NET_OutOfBandPrint `{0x14211AC50,0x142173710,0}`, NET_StringToAdr `{0x142119CC0,0x142172780,0x140515110}`, CL_DisconnectPacket `{0x14134CBB0,0x14134CB90,0}`, CL_Disconnect `{0x14135D880,0x14135D860,0}`, SV_AddTestClient `{0x1421EC3F0,0x142248F40,0x14052E3E0}`, ClientCommand `{0x14193DFC0,0x14193DFA0,0x140295C40}` | game/symbols/** |

### 1.4 Where an encryption, relay or hole-punch layer fits
1. **Winsock shim (recommended).** Chain a Xerces transport in front of the existing game-image IAT `sendto`/`recvfrom` stubs (H5). Every engine datagram (netchan and engine OOB) passes through here, and the engine never sees the change. Also route `network::send_data` (H6) through the same shim, because boiii's own OOB bypasses the IAT.
   - Encryption: AEAD per peer, keyed after the handshake (section 4).
   - Relay: encapsulate `[peer-id][ciphertext]` to a relay endpoint. On receive, strip the header and rewrite `from` to a **synthetic IPv4 netadr**, since the engine only knows IPv4:port. Upstream already uses this trick in the friends browser with 192.0.2.x placeholders (matchmaking_servers.cpp:216-222).
   - NAT keepalives.
2. **Engine level:** detour `NET_SendPacket` (H4; the re-implementation shows the header semantics) and `Sys_GetPacket` (H3). This is more invasive because Sys_GetPacket already has 3 inline patches.
3. **Socket level:** `create_ip_socket` (H1) is boiii code, so Xerces can own socket creation (dual-stack, a socket pair, a virtual socket) without engine patches.
4. **Control plane:** new OOB commands via `network::on` (H7/H8). Only datagrams that start with `FF FF FF FF` are dispatched.
5. **Hole punching** must stay on `ip_socket` so the NAT binding matches game traffic. nat.cpp already does this.

---
## 2. OOB command catalog
| Command | Direction | Handler | Gate / validation |
|---|---|---|---|
| `getInfo <hex8>` | anyone to any boiii process | getinfo.cpp:94-165 | none (no host check, no rate limit). Replies `infoResponse` |
| `infoResponse` | server to querier | party.cpp:390-419 | must match a pending (host, challenge) |
| `getservers T7 8 full empty` | client to master | server_list.cpp:350-351 | - |
| `getServersResponse` | master to client | server_list.cpp:104-124, 430-437 | only from a master being queried |
| `heartbeat T7` | dedicated to masters | dedicated/command.cpp:17-31 | skipped if sv_lanOnly |
| `getchallenge` | client to server (any boiii process answers) | auth.cpp:398-428, 637 | per-netadr 32 B challenge, 10 min |
| `getChallengeResponse <32B>` | server to client | auth.cpp:208-234, 636 | **no sender check** |
| `connect ` (fragmented) | client to server | auth.cpp:501-516 then 447-499 | only if server_running; ECDSA, xuid==keyhash, name charset |
| `error <msg>` | server to client | engine (truncated by com_error_oob_stub) | - |
| `playerXuid` | server to clients | auth.cpp:518-532 | client: `!server_running && party::is_host(src)` |
| `profileInfo` (fragmented) | server to clients | profile_infos.cpp:220-237 | `party::is_host(src)` |
| `nameoverride` / `nameoverridereq` | server to client / client to server | name.cpp:568-695 | trusted host / connected client address |
| `rcon <pw> <cmd>` | admin to dedicated | rcon.cpp:73-88 | 1 per rcon_timeout ms per address; plaintext compare; reply `print` |
| `privRegister`/`privRegisterAck`, `privJoin`/`privPeer`/`privReject`, `friendPublish`, `friendLookup`/`friendPresence`/`friendOffline` | client to/from Ezz master | nat.cpp | inbound only from master IP:port; token charset/length |
| `punch`/`punchAck <token>` | peer to peer | nat.cpp:338-357 | token equals current join/host token; sender `is_connectable_address` (network.cpp:305-308) |

---
## 3. Connect / join flow (client to dedicated or listen server)
1. **Entry.** The console `connect`, the server browser, or Lua `Engine.Exec("connect ...")` reaches the engine connect handler, which is replaced by `party::connect_stub` (party.cpp:331-380; hook 523-524).
   - The address is normalized: localhost becomes 127.0.0.1 and the default port is 27017.
   - A friends-browser placeholder goes to `friends::connect_to_friend`.
   - 0.0.0.0 is rejected.
   - Resolution is async (NET_StringToAdr on a detached thread, network.cpp:270-276).
2. **connect_finish** (297-329) stores connect_host and clears profile infos. If a friend's Steam rich presence `boiii_game_info` matches the address (dormant path), it goes straight to step 5. Otherwise it calls `query_server`.
3. **getInfo.** The challenge is `random::get_challenge()`, 8 random bytes as hex (utils/cryptography.cpp:647-652). Queries are queued and only sent while `Com_IsRunningUILevel()` (party.cpp:421-456).
4. **handle_connect_query_response** (164-295) checks:
   - protocol == 8 and sub_protocol is 1 or 0 (`game/structs/core.hpp:12-13`)
   - gamename T7
   - `net_password_hash` against the local hash, client-side only
   - mapname and gametype present

   It then reads modId, workshop_id/usermapId and sv_wwwBaseURL, records the pending reconnect, and runs `workshop::check_valid_usermap_id` (FastDL, or a steamcmd prompt) and `check_valid_mod_id` (steamcmd prompt) (workshop.cpp:776-910).
5. **connect_to_lobby_with_mode_internal** (132-162) forces LobbyBase network mode LIVE (or keeps LAN) and the session mode, using Com_SwitchMode with a single retry after 5 s. It sets game mode MATCHMAKING_PLAYLIST.
6. **connect_to_lobby** (70-99) clears stored XUIDs and the challenge and calls `workshop::setup_same_mod_as_host`, which loads ONE mod by publisher id with a filesystem reinit and waits (workshop.cpp:986-1011). It then calls `CL_ConnectFromLobby(controller, empty XSESSION_INFO, addr, ...)`; connstate starts at CHALLENGING (H11).
7. **CHALLENGING** (cl.cpp:270-303): every 3 s, local client 0 only, `NET_OutOfBandPrint "getchallenge"`, after the `dwGetConnectionTaskStatus` check.
8. **Server `send_challenge`** (auth.cpp:398-428) replies with a raw `getChallengeResponse ` + 32 bytes.
9. **Client `set_challenge`** (208-234) copies the bytes and flips the used local clients to CONNECTING.
10. **CONNECTING** (cl.cpp:163-268) builds the userinfo: name, protocol 18532, netfieldchk, sessionmode, challenge (engine int), invited, xuid, qport, migrating, `password` (password dvar, DVAR_USERINFO, auth.cpp:643-648), clanAbbrev. It sends `connect "<info>"` through `send_fragmented_connect_packet`. The payload is:
    - string pubkey
    - string ECDSA-X9.62(SHA-512(challenge))
    - profile_info {int32 version (3), string ddl from `boiii_players/user/profile_info`}
    - string connect
11. **Server** `handle_connect_packet_fragment` (501-516) reassembles and runs `dispatch_connect_packet` on the server pipeline (447-499):
    - verify the signature against `get_challenge(target)` (the challenge is not consumed)
    - parse the profile and tokenize the userinfo
    - require xuid == key hash, and a name that is non-empty with chars 32..126 except `%` `~`
    - `add_and_distribute_profile_info`
    - `SV_DirectConnect(target)`, rate-limited on dedicated, which presumably also checks g_password
    - `handle_new_player`: set client.xuid and send `playerXuid` to and about everyone (286-336)
12. The engine connect continues. CONFIRMLOADING sends `cfl` + xuid + qport. SENDINGDATA sends `stats` in 0x4C0-byte slices of up to 0x25800 bytes of `Storage_SerializeTransferData` (cl.cpp:101-161).
13. After connect: profileInfo broadcast, name-override snapshots (name.cpp), playerXuid. On disconnect, the stored challenge is cleared (auth.cpp:603-619, 736-739).

**Join-time mod negotiation today:** one `modId`, plus `usermapId`/`workshop_id` and `sv_wwwBaseURL`. There are no versions, hashes, load order or multiple mods.

---
## 4. Identity and keys
- Key generation: `ec<local scratch copy, not kept>:generate_key(512, entropy)` via libtomcrypt `ecc_make_key(keysize=64)` on a Yarrow PRNG.
  - Entropy: SMBIOS UUID x2 + MachineGuid + HwProfileGuid + <local scratch copy, not kept>volume serial + `ezz-boiii-auth-v2` (or `-alias-v2`), + controller index (auth.cpp:64-87).
  - Fully random if no hardware IDs are available.
- Files: `LOCALAPPDATA/boiii/user/ezz-private[-alias][-N].key` (+ `.bak`, atomic write/verify) and `ezz-public*.key` (auth.cpp:91-197; utils/properties.cpp:109-127).
- XUID / Friend Code = first 8 bytes of SHA1(public key) (utils/cryptography.cpp:242-249). It is shown as a decimal 'BOIII Friend Code' in the launcher (launcher.cpp:2030-2049).
- Signatures: SHA-512 then ECDSA ANSI X9.62 (cryptography.cpp:269-317). libtomcrypt also exposes `ecc_shared_secret`/`ecc_encrypt_key`, so it can serve an ECDH session-key derivation.
- Dedicated server GUID: random per process (auth.cpp:549-557). There is no server key or fingerprint.

---
## 5. Server discovery
- Masters: `boiii_players/user/master_servers.txt` (one host:port per line; defaults `master.ezz.lol:20810`, `m.ezz.lol:20810`). Resolved once via `call_once` (server_list.cpp:286-329). The Ezz master implementation is **not in this repo**.
- Internet tab: `RequestInternetServerList` (matchmaking_servers.cpp:249-295) sends getservers, then `getInfo` for each server, and `create_server_item` maps the info string to `gameserveritem_t` (57-108). Tags include gametype, dedicated, hardcore, zombies, campaign, playerCount, bots, rounds, modName; appid 311210 only if sub_protocol matches; `m_steamID` = xuid.
- Favorites/History: `favorite_servers.txt` and `recent_servers.txt` (max 50) via the emulated `ISteamMatchmaking::AddFavoriteGame` (matchmaking.cpp:76-103). LAN: `lan_add` writes `lan_servers.txt`, but nothing reads it and `RequestLANServerList` is a stub. There is no broadcast discovery even though SO_BROADCAST is set.
- Friends tab: backed by nat lookups (section 6).
- A Lua custom browser exists: `game.getrawservercount/getrawserverinfo/isserverlistrefreshing` (ui_scripting.cpp:892-948).
- Not implemented: PlayerDetails, ServerRules, any getstatus/player-list query.
- Heartbeat: dedicated sends `heartbeat T7` (dedicated/command.cpp:17-31, 78-86). The master learns the address from the UDP source.

---
## 6. NAT traversal (nat.cpp) and UPnP (upnp.cpp)
**Trigger.** The private-match options menu calls `friends_open` (data/ui_scripts/party/datasources_start_menu_game_options.lua:19-24). The command handler (friends.cpp:742-763) calls `nat::set_open_to_friends`, which only works when `getinfo::is_host()` (`server_running`) (nat.cpp:450-456). `set_open` sets the dvars `nat_open` and `friends_open`, sets `com_pauseSupported=0`, and calls `upnp::open_port` (249-266).

**Host loop (every 5 s, nat.cpp:268-281):**
- `privRegister <token16hex> [localIP:port]`; the master replies `privRegisterAck <reflected ip:port>`.
- `friendPublish 1 <friendCode> <token> [local]`. This is unsigned and anyone can publish any Friend Code client-side; whether the master checks anything is unknown.

**Join** (`friends::connect_to_friend`, which calls `nat::begin_join(token, fallback)`, nat.cpp:430-448):
- `privJoin <token> [local]` immediately and every 1 s until candidates arrive.
- `privPeer <token> <ip:port>...`: the joiner keeps at most 16 connectable candidates; the host adds probes valid for 10 s (296-329). `privReject` aborts.
- Both sides send `punch <token>` every 250 ms (221-247). The receiver answers `punchAck`. The joiner calls `complete_join(sender)`, which calls `party::connect(sender)` and enters the normal connect flow (section 3).
- After 12 s: connect to the fallback (presence endpoint) if it is connectable, otherwise show "network may block UDP hole punching" (214-219).

**Presence lookup:** `friendLookup 1 <reqId> <friendCode> [local]` per friend. Replies are `friendPresence 1 <req> <code> <token> <endpoint>` or `friendOffline 1 <req> <code>`, finished after 2 s (458-489, 359-413).

**Endpoint preference** (`get_host_endpoint`, 420-428): UPnP external, then master-reflected, then local. This getter and `get_host_token` are unused.

**UPnP:**
- `CoCreateInstance(CLSID_UPnPNAT)` then `get_StaticPortMappingCollection`, then `Add(port, "UDP", port, localIPv4, TRUE, "BOIII friend match")`, run async with one operation at a time.
- The external IP comes from the mapping. `Remove` runs on close and in `pre_destroy` (upnp.cpp:46-137).
- The mapped port is `game::port()`, not the actually-bound port.
- No NAT-PMP/PCP, no lease time, no IGD discovery of its own (Windows SSDP is used).

**No relay exists.** Infrastructure needed: the UDP rendezvous with a stable IP:port, because replies must come from the queried address (`from_rendezvous`, 147-152).

---
## 7. Friends, presence, invites, Discord
- **List:** `boiii_players/user/friends.json`, `[{"steam_id": <FriendCode>, "name"}]`. The field name `steam_id` is legacy; the values are Friend Codes. Max 200, deduplicated, own code filtered out (friends.cpp:30-31, 135-240, 389-399).
  - Written by the launcher (`readFriendIdentity`, `readFriends`, `writeFriends`, `addFriend`, launcher.cpp:2027-2140; main.js:4185-4306) and by Lua `game.addfriend/removefriend`.
- **States:** offline and in_game only; `online` is never set.
  - `refresh_presence` (601-655) and the browser friends tab (matchmaking_servers.cpp:303-345, 500-618) use nat lookups.
  - In-game UI: `data/ui_scripts/social_friends/__init__.lua` (game.getfriendcount/getfriend/refreshfriends/issocialfriend/connectsocialfriend).
- **connect_to_friend** (522-597): join token leads to NAT join; otherwise Steam rich presence `boiii_game_info` (never published anywhere, so dormant); otherwise `connect ip:port` via Cbuf.
- **Invites:** `invite_to_game` (401-443) builds `addr|map|gametype|mode|mod|senderCode|senderName`, sets rich presence `boiii_invite`, and calls the **real Steam** `InviteUserToGame` with the Friend Code used as a SteamID. That is an ID-space mismatch. Only Lua `game.invitefriend` reaches it, and the shipped UI never calls it.
  - Incoming Steam join requests: a `Steam_BGetCallback` detour for callback 337 (GameRichPresenceJoinRequested) is installed lazily inside `get_pending_game_invite` (steam_proxy.cpp:589-635), which has no callers, so it is dormant.
- **Own connect address** (friends.cpp:243-320): prefers a Radmin VPN 26.x or Hamachi 25.x adapter, then the in-game server address, then the public IP from api.ipify.org, then the route-trick local IP.
- **Steam (proxied, steam_proxy.cpp):** loads `tier0_s64`, `vstdlib_s64`, `gameoverlayrenderer64` and `steamclient64` from the Steam install, and gets IClientEngine interfaces (user 8, utils 14, friends 13, ugc 62) (191-225).
  - `SteamAPI_Init` also creates SteamClient017 and SteamFriends015 (417-433).
  - Used for: SetRichPresence/ClearRichPresence (376-407; the game's own presence via the emulated ISteamFriends, steam/interfaces/friends.cpp:118-124), persona name (409-415, name.cpp:130), friend name and rich presence (641-654, 749-774), InviteUserToGame (565-587), UGC subscribed items (444-520).
  - `get_own_steam_id`, `access_steam_friends` and `clear_invite_presence` are unused.
- **Discord** (discord.cpp): discord-rpc from `ariloc/discord-rpc-buttons`, app id `967371125573177474`.
  - Handlers: ready (sets env `discord_user`), errored and disconnected. Callbacks run every 1 s; presence updates every 5 s on main.
  - Presence fields: details/state (mode, map, gametype, score from Lua `game.setDiscordPlayerScore/EnemyScore/RoundsPlayed`, rounds), images keyed by map, partySize/partyMax, buttons to discord.gg/ezz and forum.ezz.lol.
  - No join or spectate secrets. The `DiscordRPC` Lua library is stubbed (401-416).

---
## 8. Existing protections and admin tooling
| Protection | Scope | Evidence |
|---|---|---|
| Drop all non-loopback lobby messages | all | network.cpp:179-182 |
| Signed connect (ECDSA over per-address challenge), xuid == key hash, name charset | listen + dedicated | auth.cpp:447-499 |
| Per-IP connect rate limit 5/10 s (`-noratelimit` disables) | dedicated only; runs after crypto | dedicated/networking.cpp:23-84, 138-147, 302-306, 328-331 |
| Chat sanitization (G_Say) | dedicated | dedicated/networking.cpp:86-136, 289 |
| Cmd_ParseArgs RET (RCE), TeamOps arbitrary write NOP | dedicated | dedicated/networking.cpp:308-315 |
| Per-IP disconnect only removes that client | dedicated | dedicated/networking.cpp:149-155, 317-326 |
| luinotify dedup (`-mitigatepacketspam`) | dedicated | dedicated/networking.cpp:157-241, 363-368 |
| reliableAcknowledge sanity | all | patches.cpp:383-392, 566-568 |
| party_count clamp (presence BoF) | client | live_presence_patch.cpp:17-47 |
| OOB echo disabled, error truncation | client | network.cpp:403-413 |
| Voice handler / mic killed | client | network.cpp:425-427; client_patches.cpp:467-469 |
| Netchan_AllocMessage div0 | client | client_patches.cpp:383-390 |
| Host-only OOB (playerXuid/profileInfo/nameoverride) by source address | client | auth.cpp:521; profile_infos.cpp:223; name.cpp:47 |
| Fragment caps (100 ids/address, 100 frags, 5 s) | all | fragment_handler.cpp |
| FastDL traversal/size/SHA1 + MessageBox | client | fastdl.cpp:88-109, 254-269, 402-412 |
| Lua OpenURL disabled, unsafe gate, DLL blacklist, KNOWN_MALICIOUS_MODS hides IsBOIII | client | ui_scripting.cpp:1284-1349, 1750-1807, 966-1006 |
| rcon rate limit (rcon_timeout 500 ms) | dedicated | rcon.cpp:51-71, 98-99 |

**Missing:**
- no OOB rate limiting except rcon and connect (getInfo is a roughly 30x reflection amplifier; getchallenge roughly 3.5x)
- `issued_challenges` is unbounded per spoofed source
- getChallengeResponse is accepted from anyone (connect DoS)
- no session encryption or MAC
- net_password not enforced by the server
- rcon plaintext, with a possible empty-password acceptance (unverified)
- no ban list
- no listen-server rate limits

**Flags:**
- `-noratelimit`, `-mitigatepacketspam`: dedicated
- `-unsafe-lua`, `-disable-loadlib`: Lua
- `-nosteam`: steam_proxy
- `-alias`: second identity
- `-noplugins`: plugins
- `-dedicated`
- dvars: `sv_lanOnly`, `rcon_timeout`, `net_password`, `nat_open`/`friends_open`, `sv_sayname`, `g_deadChat`, `sv_wwwBaseURL`

**Admin tooling:** engine kick commands; Lua `game.kickplayer` (host only, `SV_DropClient` EXE_PLAYERKICKED) with `data/ui_scripts/kick_menu`; `muteclient`/`unmuteclient`; server `say`/`tell` overrides with `sv_sayname` (chat.cpp:199-258); `status` xuid column (status.cpp); `g_log` games_mp.log (game_log.cpp); IW4M tell fix (dedicated/command.cpp:74-75); bots `spawnBot` (host only).

---
## 9. Lua networking and capability surface
| API | Transport | Gate | Evidence |
|---|---|---|---|
| `Axios.Get/GetEncrypted/GetRetrieve/GetUpdate/GetUpdateEncrypted` | HTTPRequest.hpp, **http:// only**, synchronous | unsafe_function | axios.cpp:13-33, 84-103; HTTPRequest.hpp:916-917 |
| `Axios.Post/PostEncrypted` | same, JSON content-type | unsafe | axios.cpp:35-56 |
| `Axios.Ping` | stub returning "" | unsafe | axios.cpp:71-75 |
| `NET.*Demonware/*Steam` | none (inert flags) | unsafe | net.cpp |
| `game.httpget(url)`, `game.httppost(url, body)` | libcurl HTTP/HTTPS, redirects, TLS verify, file:// compiled out | **NONE** | ui_scripting.cpp:821-837; deps/premake/curl.lua:32 |
| `FileIO.*` (no path sandbox, `path::normalize`) | local FS | unsafe | lua/fileio.cpp:405-445 |
| `io.*`, `load/loadfile`, `string.dump`, `package.loadlib`, `serialize.*` | - | unsafe (client only); `os.*` hooked only on legacy client; debug library blocked | ui_scripting.cpp:1285-1349 |
| DLL loads (`load_dll`) | - | name blacklist; `-disable-loadlib` | ui_scripting.cpp:1750-1807, 2565 |
| `game.addfriend/removefriend/invitefriend/connecttofriend/connectsocialfriend/refreshfriends/kickplayer/getkickableplayers/savestats/purchase*/cook*/reset*/setcurrenciesmaxed/hotreloadstart/getappdatapath/setDiscord*` | - | **NONE** | ui_scripting.cpp:570-949 |

- **Who can call:**
  - scripts from `LOCALAPPDATA/boiii/data/ui_scripts` and `<game>/boiii/ui_scripts`; on dedicated servers, `lobby_scripts` (ui_scripting.cpp:1034-1041)
  - mod/usermap LUI loaded from fastfiles in the same VM (likely)
- **Approval:** process-wide `unsafe_lua_approved_for_session`, either a one-time MessageBox (55-91) or `-unsafe-lua`. On dedicated servers everything is always allowed (lua_state.hpp:40) and the hooks are not installed (2588-2609).
- **GSC comparison:** GSC has `writefile`/`readfile` sandboxed to `boiii/scriptdata` and no networking (gsc/gsc_funcs.cpp:788-840).

---
## 10. Steam: emulated, proxied, stubbed
| Category | What |
|---|---|
| Emulated (src/client/steam/**) | SteamAPI_* exports (steam.cpp); ISteamUser identity (GetSteamID = boiii key hash), voice stubs; ISteamFriends (persona = boiii name, friend list empty, SetRichPresence forwarded to the proxy, GetFriendRichPresence "", InviteUserToGame false); ISteamMatchmaking (favorites/history from boiii files; lobbies are local fakes); ISteamMatchmakingServers (Ezz master + getInfo + nat friends); ISteamGameServer (stubs; heartbeats and master packets are no-ops); UGC/remote storage (workshop area) |
| Stubbed | ISteamNetworking P2P (all false) |
| Proxied to real Steam | via steam_proxy, see section 7 |
| Fenced | apps.cpp, user.cpp/game_server.cpp auth-ticket and licensing functions, steam_proxy ownership code, auth.cpp Steam auth patch block (see off_limits) |

---
## 11. Demonware / official matchmaking boundary
- **Emulator:** IAT Winsock hooks (H5) plus in-process servers.
  - `auth3_server` (fenced), `lobby_server` hosting bd* services (lobby_server.cpp:12-40), `umbrella_server`, and `stun_server`, which returns the caller's IP and port 3074 (stun_server.cpp:30-58).
  - `bdStorage` stores user files at `boiii_players/user/<name>` and serves publisher files from embedded resources.
  - `bdMatchMaking` returns the user's own XUID as session id and an empty `find_sessions`.
  - `bdPresence` and `bdRichPresence` return empty replies.
- **Game patches** (demonware.cpp:411-452): curl TLS verification off; DW URLs switched to http (`prod.uno.demonware.net` is rewritten but not emulated); publisher-file signature skipped (fenced); FFOTD name; LPC_File_SafeWrite/DeleteStale killed; reportReward loop released; "Emulating Online Service" string.
- **Live hooks** (live.cpp:197-235): LiveConnect queue functions faked, `Live_OnDWDisconnect` no-op, storage pump made thread-safe, `Live_IsDemonwareFetchingDone` pump.
- The native lobby, matchmaking and host-migration network messages are dropped for remote peers (H10).
- **Rule for Xerces:** treat `component/demonware.cpp`, `game/demonware/**`, the DW-related hooks in `live.cpp` and the Steam lobby/game-server emulation as frozen compatibility shims. Add no services, no matchmaking or playlists, no stats or leaderboards, and do not tunnel Xerces data through DW or Steam lobbies. Xerces online features go over its own OOB commands on `ip_socket` and its own HTTPS backend.

---
## 12. Dead code, inconsistencies and bugs found
1. `security.hpp` is unreferenced and has legacy-only offsets.
2. `net_sendpacket_stub` is commented out.
3. `lan_servers.txt` is write-only; the LAN list is a stub.
4. `sv_wwwDownload` and `sv_wwwDlDisconnected` are never read.
5. `party::join_session` is declared but not defined (party.hpp:24-25).
6. `nat::get_host_token` and `nat::get_host_endpoint` are unused.
7. `steam_proxy::get_pending_game_invite/get_own_steam_id/access_steam_friends/clear_invite_presence` are unused, and `boiii_game_info` is read but never written.
8. fragment index off-by-one (fragment_handler.cpp:49).
9. Bound port is not written back to `net_port` (network.cpp:125-133).
10. `set_challenge` has no sender check (auth.cpp:208-234).
11. Tokens have at most 32 bits of entropy (nat.cpp:99-108).
12. Server queries are only sent at the UI level (party.cpp:424).
13. The first lobby-drop check is redundant (network.cpp:174-177).
14. Friend Codes are passed to Steam APIs as SteamIDs (friends.cpp:423-438, 486-488).
15. The friends `online` state is never set.

## Design implications

- Build a Xerces transport shim at the game-image Winsock boundary: chain it in front of demonware's sendto/recvfrom IAT stubs and route network::send_data through it. The shim owns AEAD, relay encapsulation and keepalives. Relayed peers get synthetic IPv4 netadrs (netadr_t is IPv4-only), for example from 100.64.0.0/10, mapped back in recvfrom.
- Keep the single-socket model. Hole punching, rendezvous and game traffic must share ip_socket so the NAT bindings match. Take ownership of create_ip_socket (it is boiii code) and write back the actually bound port.
- Session security: after the existing ECDSA connect auth, add an ECDH handshake (libtomcrypt ecc_shared_secret is available) carried in the challenge and connect messages to derive per-connection keys. MAC the host-only OOB messages. Make set_challenge verify the sender, and consume challenges.
- Give dedicated servers a persistent identity keypair, replacing the random per-process GUID, and publish its fingerprint via the backend. This lets favorites, friend joins and relays pin servers (TOFU).
- Add a generic per-source token bucket in network::handle_command covering all OOB commands, global caps on fragment_handler and issued_challenges, rate limiting before any crypto, and the same protections on listen servers. Consider answering getInfo only to challenge-validated or rate-limited peers.
- Xerces protocol negotiation: extend getInfo and connect userinfo with the Xerces protocol version, the ordered mod list (ids, versions, content hashes), a load-order hash, a mod-pack share code and the required-download manifest. Use a separate PROTOCOL/sub_protocol or a new key so Xerces and BOIII clients do not cross-connect silently. Replace setup_same_mod_as_host (single modId) with a mod-set apply step.
- FastDL v2: signed manifests (server or publisher key), HTTPS by default, an extension allowlist (no .dll or executables), size caps, no http downgrade on redirect, content-addressed storage, and support for mod packs, not just usermaps.
- Friends and presence backend: an HTTPS API (Railway works for this) for the friend graph with mutual acceptance, presence, invites and mod-pack share codes, with requests signed by the identity key (timestamp + nonce). Run the UDP rendezvous/reflection and a TURN-like relay on a UDP-capable host with relay tokens issued by the API. Keep DP-master getservers/heartbeat compatibility or move listing to HTTPS.
- Mod capability system: per-mod, manifest-declared capabilities (for example net.http with an allowlisted host list, fs scoped to a per-mod data directory, ui.social, server.admin, no loadlib). Enforce them on Axios, NET, FileIO, io/package and on the currently ungated game.* functions (httpget/httppost first). Attribute calls to a mod via the chunk name or a loader tag, replace the process-wide unsafe approval, and apply the policy on dedicated servers too.
- Keep Demonware emulation frozen. Implement all Xerces online features as new network::on OOB commands plus the Xerces HTTPS backend; never as DW services, Steam lobbies or official matchmaking.
- Steam: use official ISteamFriends rich presence (connect key) and InviteUserToGame with real SteamIDs only, as a separate ID space from the Xerces identity. Workshop goes through official UGC/steamcmd. The dormant BGetCallback join hook is not needed if Steam's standard +connect launch path is used.
- Discord: optionally add join via Discord join secrets or the Social SDK mapped to Xerces invite tokens. Replace the Ezz app id and buttons.
- Admin tooling: rcon v2 (challenge-response HMAC, required non-empty password, lockout), a ban list keyed by Xerces identity plus IP, and server-enforced private-server passwords (HMAC challenge instead of the published net_password hash).
- Port every hook in the details hook table to pattern-scanned symbols, with the existing utils::hook::signature as the base, and verify per exe at startup. Decide whether Xerces supports legacy-client and dedicated exes (3-way select) or only the current client. Leave the fenced Steam and Arxan sites exactly as upstream has them.
- Delete or quarantine security.hpp. Only port its inspectors to the current exe if Xerces ever re-enables remote lobby messages; otherwise keep dropping them.

## Risks

- Every networking hook site and symbol in this area (H1-H27 in details) is a hardcoded per-exe address chosen by a 3-way select or a server-only _g constant. A game update silently breaks connect, OOB dispatch, socket injection and security patches. The fenced auth.cpp Steam patch block must be carried over even though it cannot be moved to pattern scanning.
- Game traffic is plaintext and unauthenticated after connect, and the engine checksum is removed. On-path attackers can read or alter it. Off-path attackers who spoof the server's source address can inject host-only OOB messages (playerXuid, profileInfo, nameoverride), which are trusted by source IP.
- getChallengeResponse is accepted from any sender (auth.cpp:208-234). Anyone who can reach a connecting client can overwrite its stored challenge and make the connect fail with 'Bad signature'.
- Every boiii process, including idle clients that are not hosting, answers getInfo and getchallenge without rate limiting. getInfo leaks the player's Friend Code (xuid) and is a large UDP reflection amplifier.
- Connect-path resource exhaustion: issued_challenges grows per spoofed source address (10 min lifetime), the fragment map has no global cap, and ECDSA verification runs before the per-IP rate limit. Listen servers have no rate limiting at all.
- rcon sends its password in plaintext UDP with a simple != compare and has no explicit guard when rcon_password is empty. Whether an empty quoted password token is accepted is unverified.
- net_password gives no access control: the server publishes an unsalted FNV-1a-64 hash and never checks the client. Users may believe it protects a server.
- FastDL trusts a server-controlled manifest. http:// and redirects are allowed, the .dll extension is accepted, and the only integrity check is SHA1 against the server's own manifest. A malicious server plus Lua approved as unsafe (package.loadlib) gives code execution.
- Lua: game.httpget/httppost (HTTPS, any URL including LAN addresses) and the side-effecting game.* social, economy and kick functions are callable by any LUI script with no gate, and mod LUI likely shares the VM. The unsafe gate itself is all-or-nothing per process, and on the current client os.* is not hooked (address 0x0). Whether os.* exists in that exe is unknown.
- Rendezvous and presence spoofing: friendPublish is unsigned, join tokens have at most 32 bits of entropy, and the client does not check that a looked-up friend has added you back. Server-side master rules are unknown, so 'friends can join' may mean 'anyone who knows your Friend Code can join'.
- Hard dependency on Ezz infrastructure: master.ezz.lol and m.ezz.lol on UDP 20810 for the server list, heartbeat, NAT rendezvous and presence; the Ezz Discord app id and links. The master code is not in the repo, so a rename to Xerces needs replacement infrastructure and a protocol spec.
- The planned Railway backend may not accept inbound UDP. Rendezvous, reflection and relay need a UDP-reachable host with a stable IP:port, because nat.cpp only accepts replies from the exact queried address. Verify before committing to Railway for anything beyond HTTPS.
- The bound-port fallback is not written back to net_port, so UPnP and the advertised endpoints can point at the wrong port when two instances run.
- The Friend Code and SteamID spaces are mixed: the Friend Code is passed to Steam InviteUserToGame and GetFriendPersonaName. Building on the existing Steam invite path would invite or look up invalid IDs.
- prod.uno.demonware.net is rewritten to http but not emulated. Any request the game makes to it would hit real DNS in cleartext (unverified whether BO3 calls it).
- The CL_CheckForResend reimplementation must stay in sync with all call sites. Its original is mutated by Arxan at runtime, so detouring or patching the original prologue may crash.
- Re-enabling engine lobby messages (party, voice, host migration) would reopen the lobby exploit class. security.hpp's inspectors only have legacy-client offsets and are dead code.
- Outside this area but relevant to the hard boundary: src/client/main.cpp:122-131 and the patch-install flow download a 'compatible' BlackOps3.exe from archive.org and replace the user's installed exe. Flag it for the launcher/legal review.
- The SV_DirectConnect rate limiter also wraps bot connects (direct_connect_bots_stub calls the detoured function). If bot netadrs share an addr, adding more than 5 bots in 10 s on a dedicated server could be silently rejected. Unverified.

## Open questions

- What exactly does the Ezz master (master.ezz.lol/m.ezz.lol:20810) implement server-side for privRegister/privJoin/privPeer/friendPublish/friendLookup? Does it check signatures or mutual friendship, and does it probe servers? May Xerces keep using it, or must it run its own?
- Does Railway currently support inbound UDP for public services? If not, which host should run the UDP rendezvous and relay?
- Does SV_DirectConnect in the current exe enforce g_password against the 'password' userinfo key on the boiii connect path?
- Does the Sept 2026 exe still contain the HKS os.* functions (upstream new-client address is 0x0, so they are ungated), and can require() of a C module reach load_dll without going through the gated package.loadlib?
- Does the engine tokenizer turn rcon "" <cmd> into an empty argv[0], making boiii rcon accept it when rcon_password is empty?
- Do bot connects (NA_BOT netadr) hit the dedicated SV_DirectConnect rate limiter when more than 5 bots are added within 10 s?
- Does BO3 issue requests to prod.uno.demonware.net in boiii mode (rewritten to http but not emulated)?
- What are the engine names and semantics of the hook sites select(0x141ED9670,...) (connect command), select(0x141EEB860,...) (lobby packet handler), select(0x14134D166,...) and 0x14018E698 (connectionless dispatch call sites)? They are needed to write robust signatures.
- Will Xerces support the legacy client exe and the dedicated server exe (BlackOps3_UnrankedDedicatedServer.exe), or only the current Steam client build?
- Can mod/usermap LUI loaded from fastfiles always reach the boiii game.* globals, or are there VM or sandbox differences between the frontend and in-game Lua states?

## Fenced (do not modify)

- src/client/component/steam_proxy.cpp:96-101, 270-330, 343-349 (ownership_state enum, start_mod_unsafe/start_mod, evaluate_ownership_state and the post_unpack call that runs them; also the -nosteam is_disabled path at 103-106 as used by start_mod): Steam ownership-state evaluation and launch-under-app mechanism. Carry over verbatim and do not build on it, including the IClientFriends app-480 fallback in set_rich_presence/clear_rich_presence (383-405), which belongs to the same mechanism.
- src/client/steam/interfaces/apps.cpp (whole file: BIsSubscribed:8, BIsVACBanned:14, BIsSubscribedApp:20, BIsDlcInstalled:33, GetEarliestPurchaseUnixTime:37, DLC functions 43-54, GetAppOwner:75, BIsSubscribedFromFamilySharing:96): license, subscription, DLC and VAC answers.
- src/client/steam/interfaces/user.cpp:65-140 (GetAuthSessionTicket, BeginAuthSession, EndAuthSession, CancelAuthTicket, UserHasLicenseForApp, RequestEncryptedAppTicket, GetEncryptedAppTicket): Steam auth tickets and licensing. GetSteamID at 17-20 is identity only and not fenced.
- src/client/steam/interfaces/game_server.cpp:65-100 (SendUserConnectAndAuthenticate, CreateUnauthenticatedUserConnection, GetAuthSessionTicket, BeginAuthSession, EndAuthSession, CancelAuthTicket, UserHasLicenseForApp): server-side Steam user authentication and licensing.
- src/client/component/auth.cpp:650-744 ('Patch steam id bit check' jump list: LiveSteamAuth_Pump, LiveSteamServer_GetAuthDataById, LiveSteamServer_EndAllClientAuthSessions, LiveSteamServer_SteamServersDisconnected_Handle, LiveSteamLobby_*/LiveSteam_Lobby_RequestJoin, LiveSteam_Friend_AddByID/Overlay, XUID_Valid, LiveMetPlayer_AddRecent and the 11 server-exe sites 0x1404747C6-0x140477365): Steam auth-session and Steam-ID validation patches. Inherit unchanged; do not port them into the Xerces pattern-scan system or extend them.
- src/client/component/live.cpp:131-145 and 170-172 (clientinfo_activateallcontent, LobbyActiveList_SetClientInfo_ActivateAllContent, return_all_content; DLC entitlement helpers, defined but currently unhooked), 186 and 219-221 (LiveAntiCheat_ConsoleDetailsReported to true), 188 and 231-232 (LiveSteam_NotVacBanned to true): entitlement, anti-cheat and VAC status.
- src/client/game/demonware/servers/auth3_server.cpp: Demonware auth-ticket emulation (license id field, account_type steam), adjacent to ownership verification.
- src/client/game/demonware/services/bdAnticheat.cpp: Demonware anti-cheat service stubs.
- src/client/component/demonware.cpp:437-439 ('Skip publisher file signature stuff'): bypass of the DW publisher-file signature (integrity) check.
- src/client/component/dedicated/networking.cpp:279-286 (NOPs at 0x1401A18E1 and 0x1401A1B5D disabling the server exe's deliberate crash on 'authload failure'): auth/anti-tamper-adjacent; carry over unchanged.
- src/client/component/arxan.cpp and src/client/component/integrity.hpp: Arxan anti-tamper and integrity-check handling.
- src/client/game/symbol.hpp:6-9 and 69-73 (arxan::detail::set_address_to_call / callstack_proxy_addr used by symbol::call_safe), plus the arxan references in src/client/game/game.hpp: Arxan callstack proxy plumbing.
- src/client/component/client_patches.cpp:264-349 (store_tac_protected_allocs and the Hunk_UserAlloc_ReturnStatic* helpers): workaround for Treyarch anti-cheat (TAC)-protected allocations. The CL_CheckForResend replacement at 512-557 is a networking function and may be changed, but it exists because of Arxan runtime mutation, so keep the redirect-all-call-sites approach.
