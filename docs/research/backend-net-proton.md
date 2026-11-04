# Backend, NAT traversal, Discord and Proton (external research)

> Machine-generated research notes from 2026-11-04, kept as evidence for `docs/DESIGN.md`.
> Code was read at upstream `main` 5aa7fac, `mod-stack` ac356c3c and the fork `refs/heads/main` 7805319.
> `path:line` citations refer to those commits. Treat claims as leads: anything marked runtime still needs the real game.

## Summary

As of 2026-11-04, Railway still has no public UDP ingress. Its documented public ingress is HTTP/HTTPS plus a TCP proxy, and the request for inbound UDP has been "under review" since February 2026. Railway works well for the HTTP API, WebSocket presence and signaling (no WebSocket time limits) and Postgres. Postgres there is a container template that Railway maintains, with volume backups, point-in-time recovery since May 2026, HA, and cron. Other points to know: health checks run only at deploy time, there is no log drain, monitors (alerts) need the Pro plan, and native backups probably do too (forum answers say Pro-only). Pricing is usage-based: $20 per vCPU-month, $10 per GB of RAM per month, $0.05 per GB of egress. You can set a soft limit (email alert) and a hard limit, but the hard limit takes every workload offline. Railway also had a security incident on 2026-10-20 to 22 in which some encrypted environment variables were decrypted, so plan to rotate secrets.

UDP game traffic (STUN, TURN, relay) has to run somewhere else. The options are Cloudflare Realtime TURN (managed; 1,000 GB a month free, then $0.05 per GB), coturn on Fly.io (UDP needs a dedicated IPv4 at $2 a month), or a Hetzner VPS. Running TURN over TCP through Railway's TCP proxy is only a degraded last resort.

For the C++ Windows client:
- **libjuice** (MPL-2.0) is the leanest ICE/STUN/TURN library. It has supported ICE-TCP since 1.7.0, but its TURN client talks to the server over UDP only.
- **GameNetworkingSockets** (BSD-3-Clause) v1.6.x is a full alternative: since June 2026 its built-in ICE needs no external dependencies, and it supports TURN and has its own encryption.
- **libnice** depends on GLib, which is heavy on Windows.
- **coturn** (server) is BSD-3-Clause.
- **For connection security**, use netcode-style connect tokens from the backend (BSD-3-Clause, XChaCha20-Poly1305) or a Noise handshake, built on libsodium (ISC) or Monocypher (CC0/BSD-2).

Expect about 70–83% of hole-punch attempts to succeed directly. You need a relay when both peers are behind hard (symmetric) NAT, when peers share a CGNAT, and on networks that block UDP. Direct P2P also reveals players' IP addresses to each other.

For a solo developer, the recommended backend is TypeScript on Node LTS with Fastify or Hono and Postgres, with Go as the lean alternative. Use Argon2id at OWASP parameters, short-lived asymmetric access tokens (PASETO v4.public, or JWT with a pinned EdDSA algorithm), and opaque refresh tokens that rotate on every use with reuse detection (RFC 9700). Collect as little data as possible and support self-serve deletion.

Discord: discord-rpc (MIT) is deprecated and the Game SDK is archived. The Social SDK is free but proprietary, and linking it in-process with GPL-3.0 boiii code is a licensing problem. Talking to the Discord client's local IPC directly (as discord-rpc does) is the GPL-safe route. Under Proton it needs a bridge such as rpc-bridge.

Steam Deck: WebView2 under Wine/Proton is still fragile. Upstream Ezz BOIII moved its launcher to WebView2 in July 2026, the launcher became unusable under Wine, and they fixed it by detecting Wine and falling back to the legacy MSHTML control. Under Proton that control is rendered by the bundled Wine Gecko 2.47.4, so it is an old engine. For a launcher that behaves the same on Windows and Proton, RmlUi (MIT) fits best. It added DX11 and DX12 backends in 6.3 (August 2026) and supports flexbox, transitions, keyframe animations, transforms and gradients.
- **Dear ImGui** suits tools and overlays.
- **CEF** is heavy: the runtime package is about 177 MB compressed.
- **Ultralight** is proprietary and incompatible with GPL.

On Proton, hooks on the DX11 Present call work through DXVK; DXVK 3.1 improved compatibility for overlays that hook DXGI. Don't ship DLL proxies, and treat upscalers that need D3D12 interop, and DLSS, as Windows-only extras. On the Deck, gamescope already provides FSR1/NIS upscaling.

## Findings

- **confirmed**: Railway's documented public networking is HTTP/HTTPS plus a TCP Proxy (for databases, game servers, custom protocols); no UDP ingress option is documented, and no 2026 changelog entry through 2026-10-30 mentions UDP. Source: https://docs.railway.com/networking/tcp-proxy ; https://docs.railway.com/networking/public-networking/specs-and-limits ; https://railway.com/llms-changelog.md (all accessed 2026-11-04)
- **likely**: A Railway feature request for inbound UDP (opened 2026-02-22) is still in 'under_review' status with no staff response; community answers say UDP would need a separate proxy because Railway only runs TCP proxies. Source: https://station.railway.com/feedback/adding-inbound-udp-fad19847 ; https://station.railway.com/questions/udp-not-working-anymore-24a1898e (accessed 2026-11-04)
- **confirmed**: Railway TCP Proxy assigns a domain and random public port (e.g. *.proxy.rlwy.net:15140), load-balances randomly across replicas in the closest region, and supports custom domains via CNAME (port unchanged; Cloudflare proxying must be off). Source: https://docs.railway.com/networking/tcp-proxy (accessed 2026-11-04)
- **likely**: Railway's TCP proxy is a raw L4 relay that does not preserve the client source IP and offers no PROXY protocol header, so STUN-style address discovery or IP-based logic behind it is impossible. Source: https://station.railway.com/questions/proxy-protocol-or-source-ip-visibility-f-91a52bc3 ; https://station.railway.com/questions/client-i-ps-via-tcp-proxy-proxy-protoco-9fbd1fe8 (forum/bot answers, 2026)
- **confirmed**: Railway WebSockets (HTTP/1.1) are exempt from the request duration and inactivity limits; HTTP requests may run 15 min while transferring (5 min idle); limits are 10,000 concurrent connections per service, ~11,000 RPS per domain, 60 s idle HTTP/1.1 keep-alive; TLS 1.2/1.3. Source: https://docs.railway.com/networking/public-networking/specs-and-limits (accessed 2026-11-04)
- **confirmed**: Railway private networking uses WireGuard tunnels with SERVICE.railway.internal DNS; environments created after 2025-10-16 are dual-stack IPv4+IPv6 (older ones IPv6-only); any IPv4/IPv6 traffic including UDP is allowed between services; available only at runtime (not builds); no cross-project or cross-environment traffic; private traffic is not billed as egress. Source: https://docs.railway.com/networking/private-networking/how-it-works (accessed 2026-11-04)
- **confirmed**: Railway has four deploy regions: US West Metal (California), US East Metal (Virginia), EU West Metal (Amsterdam), Southeast Asia Metal (Singapore). Source: https://docs.railway.com/deployments/regions (accessed 2026-11-04)
- **confirmed**: Railway pricing: Free $0 with $1/month credit; Hobby $5/month including $5 usage; Pro $20/month including $20 usage. Container rates: CPU $20/vCPU-month, RAM $10/GB-month, volume $0.15/GB-month, network egress $0.05/GB; builds free. Hobby per-service caps: 6 replicas, 48 vCPU/48 GB, 5 GB volume. Source: https://docs.railway.com/reference/pricing/plans (accessed 2026-11-04)
- **confirmed**: Railway usage limits: a soft limit only sends an email alert, while hitting the Compute hard limit takes all workloads offline (with reminders at 75/90/100%); the minimum hard limit is $10; limits can be managed with `railway usage limit` CLI commands. Source: https://docs.railway.com/reference/usage-limits (accessed 2026-11-04)
- **confirmed**: Railway Postgres is a template-deployed container (Railway's SSL image on the official Postgres image) that Railway maintains; it can be converted to HA (Patroni/etcd/HAProxy), supports one-click major upgrades, offers PgBouncer and extension templates (pgvector, PostGIS, TimescaleDB), is private by default, and public access goes through a billed TCP proxy. Source: https://docs.railway.com/databases/postgresql (accessed 2026-11-04); changelog 2026-09-11 one-click major upgrades
- **confirmed**: Railway volume backups: manual plus daily (kept 6 days), weekly (27 days), monthly (89 days); incremental copy-on-write billed at volume price; restore creates a new volume and only works within the same project+environment; wiping a volume deletes all backups; manual backups capped at 50% of volume size. Source: https://docs.railway.com/reference/backups (accessed 2026-11-04)
- **likely**: Volume backups appear to require the Pro plan (Hobby backup limit reportedly zero); the official docs do not state the plan gating. Source: https://station.railway.com/questions/backup-pitr-unavailable-for-staging-post-911535d8 and related Station threads (forum, 2026)
- **confirmed**: Railway Postgres PITR (launched 2026-05-15): pgBackRest archives WAL to a Railway bucket, weekly full + daily differential backups, last 4 fulls kept (about 4-week window, starting after enablement), restore creates a sibling service with manual cutover; WAL can be dropped if a 5 GiB queue cap trips during object-storage outages; billed via bucket storage and egress. Source: https://docs.railway.com/volumes/point-in-time-recovery (accessed 2026-11-04)
- **confirmed**: Railway cron jobs: runs must be at least 5 minutes apart, schedules are UTC, a run is skipped if the previous one is still running, and the job must exit; CLI gained `railway cron list/run` in Oct 2026. Source: https://docs.railway.com/reference/cron-jobs ; https://docs.railway.com/reference/cli-api (accessed 2026-11-04)
- **confirmed**: Railway healthchecks are deploy-time only (not continuous monitoring); default timeout 300 s (RAILWAY_HEALTHCHECK_TIMEOUT_SEC); probes come from healthcheck.railway.app; services with volumes have brief downtime on redeploy. Source: https://docs.railway.com/reference/healthchecks (accessed 2026-11-04)
- **confirmed**: Railway logs are retained 3 days (Free), 7 (Trial/Hobby), 30 (Pro), up to 90 (Enterprise); logging is capped at 500 lines/s per replica; Railway has no log drain setting (use Vector, Fluent Bit or OpenTelemetry). Source: https://docs.railway.com/observability/logs (accessed 2026-11-04)
- **confirmed**: Railway Monitors (threshold alerts on CPU, RAM, disk, egress via email, in-app and webhooks) require the Pro plan; webhooks cover deploy/crash events; tracing exists. Source: https://docs.railway.com/observability.md (accessed 2026-11-04)
- **confirmed**: Railway Edge Rules (Aug 2026) can match client IP/CIDR, host, path or request headers and Block, Redirect or Challenge; rate limiting is not mentioned, so API rate limiting must be done in-app (or via Cloudflare in front). Source: https://railway.com/changelog/2026-08-20-railway-anon.md (2026-08-21)
- **likely**: Railway redeploys support overlap and draining windows (RAILWAY_DEPLOYMENT_OVERLAP_SECONDS, RAILWAY_DEPLOYMENT_DRAINING_SECONDS: SIGTERM then SIGKILL); long-lived WebSocket clients must handle reconnects across deploys. Source: https://docs.railway.com/deployments/deployment-teardown (accessed 2026-11-04)
- **confirmed**: Railway CLI installs via npm (@railway/cli), Scoop on Windows, Homebrew or shell script; key commands include railway up/logs/run/ssh/connect/variable set/usage limit status/cron; CI uses RAILWAY_TOKEN (project) or RAILWAY_API_TOKEN (workspace). Source: https://docs.railway.com/reference/cli-api (accessed 2026-11-04)
- **confirmed**: SMTP and static outbound IPs are Pro-only on Railway. Source: https://docs.railway.com/reference/outbound-networking (accessed 2026-11-04)
- **confirmed**: Railway security incident (2026-10-20 to 10-22): attacker used CI credentials and decrypted a subset of encrypted environment variables; at most 1.85% of workspaces (~100,000) potentially affected; Railway advises rotating sensitive variables set before 2026-10-21 and removing public database access. Source: https://blog.railway.com/p/preliminary-report-october-security-incident ; https://railway.com/changelog/2026-10-23-october-21-security-incident (2026-10-23)
- **confirmed**: Fly.io supports public UDP only by binding to fly-global-services on a dedicated IPv4 (no UDP over public IPv6); about 72 bytes of per-packet overhead, so budget ~1300-byte payloads. Prices: shared-cpu-1x 256 MB $2.19/mo, dedicated IPv4 $2/mo, egress $0.02/GB (NA/EU), $0.04 (APAC/Oceania/SA), $0.12 (Africa/India); no free tier for new orgs. Source: https://docs.fly.io/networking/udp-and-tcp ; https://docs.fly.io/about/pricing (accessed 2026-11-04)
- **confirmed**: Cloudflare Realtime TURN: $0.05/GB (edge-to-client) after a 1,000 GB/month free tier shared with SFU; STUN at stun.cloudflare.com is free/unlimited; TURN on UDP 3478/443, TCP 3478/80, TLS 5349/443; anycast-homed allocations; API-issued credentials with TTL up to 48 h; IPv4 relay addresses only; no RFC 6062 TCP relaying; possible loss above ~50-100 Mbps or ~5-10 kpps per client. Source: https://developers.cloudflare.com/realtime/turn/faq/ ; https://developers.cloudflare.com/realtime/turn/ (accessed 2026-11-04)
- **likely**: Hetzner raised cloud prices twice in 2026 (2026-04-01 and 2026-06-15 for new orders/rescales); cloud plans now start at EUR 5.99/month excl. VAT (Cost-Optimized line); included-traffic figures by location (EU 20 TB vs less in US/Singapore) vary between secondary sources. Source: https://www.hetzner.com/cloud/ (accessed 2026-11-04); https://www.tomshardware.com/tech-industry/hetzner-to-raise-prices-by-up-to-37-percent-from-april-1 ; https://privatedevops.com/news/hetzner-june-2026-cloud-price-increase-what-to-do
- **confirmed**: libjuice is MPL-2.0, written in C with no required dependencies, builds with MSVC, implements ICE (RFC 8445), STUN (RFC 8489), TURN client (RFC 8656), a lightweight embedded STUN/TURN server, single-port mux and IPv4/IPv6; ICE-TCP was added in v1.7.0 (2025-11-15); latest v1.7.4 (2026-09-26). Source: https://github.com/paullouisageneau/libjuice ; https://api.github.com/repos/paullouisageneau/libjuice/releases (accessed 2026-11-04)
- **likely**: libjuice's TURN client reaches TURN servers over UDP only (TURN over TCP/TLS is not documented), so it cannot use a TCP-only TURN path such as one behind Railway's TCP proxy. Source: https://github.com/paullouisageneau/libjuice README ('Only UDP is supported as transport protocol'); release notes v1.6-v1.7.4
- **confirmed**: libnice is dual-licensed LGPL-2.1(-or-later) OR MPL-1.1, GLib-based, implements ICE RFC 8445, a TURN client (RFC 5766), partial ICE-TCP and a TCP-over-UDP reliable stream; 0.1.24 was packaged in 2026-09. Source: https://libnice.freedesktop.org/ ; https://sources.debian.org/src/libnice/0.1.23-2/COPYING ; https://archlinux.org/packages/extra/x86_64/libnice
- **confirmed**: Valve GameNetworkingSockets is BSD-3-Clause, provides reliable/unreliable messages with AES-GCM-256 + Curve25519, pluggable custom signaling and symmetric-connect; v1.6.0 (2026-06-03) made a dependency-free native ICE client the default and added TURN (RFC 5766) for symmetric NATs plus IPv6; v1.6.1 (2026-10-03) fixed STUN/TURN failover; WebRTC ICE is now opt-in. Source: https://github.com/ValveSoftware/GameNetworkingSockets/releases/tag/v1.6.0 ; https://api.github.com/repos/ValveSoftware/GameNetworkingSockets/releases (accessed 2026-11-04)
- **confirmed**: GameNetworkingSockets needs protobuf plus one crypto backend (OpenSSL 1.1.1+, libsodium, or Windows bcrypt); Steam Datagram Relay is not available outside Steam, so off-Steam relaying must use your own TURN/relay; whether its TURN client supports TCP/TLS transport is not documented. Source: https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/BUILDING.md ; https://raw.githubusercontent.com/ValveSoftware/GameNetworkingSockets/master/README_P2P.md
- **confirmed**: coturn is BSD-3-Clause (Citrix copyright); latest 4.18.0 (2026-09-08); client transports UDP/TCP/TLS/DTLS, RFC 6062 TCP relaying, TURN REST API time-limited shared-secret credentials, requires OpenSSL 3.0+, Docker images available; CVE-2020-26262 (loopback relay via 0.0.0.0/[::]) shows why denied-peer-ip should be set explicitly. Source: https://github.com/coturn/coturn ; https://raw.githubusercontent.com/coturn/coturn/master/LICENSE ; https://api.github.com/repos/coturn/coturn/releases ; https://access.redhat.com/security/cve/CVE-2020-26262
- **confirmed**: netcode (mas-bandwidth/netcode, BSD-3-Clause, standard 1.02) is a secure client/server UDP protocol: backend-issued connect tokens whose private part is sealed with XChaCha20-Poly1305 (libsodium), carrying 1-32 server addresses, per-connection keys, 256 bytes of user data and expiry; it has no NAT traversal; servers hold a 32-byte private key that must never ship in the client. Source: https://github.com/mas-bandwidth/netcode ; https://github.com/mas-bandwidth/netcode/blob/main/STANDARD.md (accessed 2026-11-04)
- **confirmed**: noise-c (MIT) describes itself as a reference implementation and its last commit was 2023-12-06; libhydrogen (ISC) offers a Noise-based key exchange using Curve25519 + Gimli, is C99 with Windows support, and was active in 2026-09. Source: https://github.com/rweather/noise-c ; https://api.github.com/repos/rweather/noise-c/commits?per_page=1 ; https://github.com/jedisct1/libhydrogen
- **confirmed**: libsodium is ISC-licensed, current 1.0.22-stable, supports Windows (MSVC/MinGW, x86/x64/arm64); crypto_pwhash defaults to Argon2id since 1.0.15 (INTERACTIVE 64 MiB, MODERATE 256 MiB, SENSITIVE 1 GiB). Source: https://libsodium.gitbook.io/doc ; https://libsodium.gitbook.io/doc/password_hashing/default_phf (accessed 2026-11-04)
- **confirmed**: Monocypher 4.0.3 is CC0-1.0 or BSD-2-Clause, providing XChaCha20-Poly1305, X25519, EdDSA, BLAKE2b and Argon2 (Argon2id via CRYPTO_ARGON2_ID, without real parallelism). Source: https://monocypher.org/ ; https://monocypher.org/manual/argon2
- **likely**: MPL-2.0 code can be combined into a (L)GPL larger work under MPL section 3.3 unless the author marked it 'Incompatible With Secondary Licenses'; BSD/ISC/MIT/CC0 libraries are GPL-3.0-compatible. Source: https://www.mozilla.org/en-US/MPL/2.0/FAQ/ (Q14, Q25)
- **confirmed**: A 2025-26 measurement of libp2p/IPFS DCUtR (4.4M attempts, 85k networks, 167 countries; accepted at IMC '26) found ~70% +/-7.1% hole-punch success, TCP and QUIC statistically indistinguishable, 97.6% of successes on the first attempt. Source: https://arxiv.org/abs/2510.27500 (2025-10-31) ; https://arxiv.org/abs/2604.12484 (2026-04-14)
- **uncertain**: Other hole-punch figures: ProbeLab reports ~82.9% success for port-restricted cone NATs and ~40% for symmetric NAT on a small, doubtful sample; Ford et al. (2005) found ~82% of tested NATs supported UDP hole punching (~64% TCP); WebRTC practitioners commonly cite 10-20% of sessions needing TURN (methodology unclear). Source: https://probelab.io/blog/can-libp2p-punch-through-nats/ ; https://arxiv.org/abs/cs/0603074 ; https://www.nojitter.com/know-where-turn-when-deploying-webrtc
- **confirmed**: Hole punching fails when both sides use endpoint-dependent ('hard'/symmetric) mapping; one hard side needs impractical birthday-paradox port probing; peers behind the same CGNAT need hairpinning; UPnP/NAT-PMP/PCP are often disabled or broken by double NAT; relays guarantee connectivity including on UDP-blocking networks at a latency cost. Source: https://tailscale.com/blog/how-nat-traversal-works
- **confirmed**: Relaying through Valve's SDR is used precisely because relayed traffic never reveals IP addresses, protecting players from DoS; direct P2P exposes peers' IPs. Source: https://partner.steamgames.com/doc/features/multiplayer/steamdatagramrelay
- **confirmed**: Ezz BOIII (the boiii fork, GPL-3.0, active 2026-11-03) dedicated servers default to UDP 27017; the README tells players to port-forward or use ZeroTier/Radmin VPN/Hamachi, and lists Windows 10/11 64-bit as the only platform. Source: https://github.com/Ezz-lol/boiii-free ; https://api.github.com/repos/Ezz-lol/boiii-free (accessed 2026-11-04)
- **confirmed**: OWASP's Argon2id minimums are equivalent configurations m=46 MiB/t=1, m=19 MiB/t=2, m=12 MiB/t=3, m=9 MiB/t=4, m=7 MiB/t=5 (p=1); the summary recommendation is 19 MiB, t=2, p=1; a pepper is optional defense in depth and must be stored separately from hashes. Source: https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html
- **confirmed**: RFC 9700 (OAuth 2.0 Security BCP, Jan 2025) requires public-client refresh tokens to be sender-constrained or rotated; with rotation a new refresh token is issued on each refresh, the old one invalidated, and reuse of an old token causes revocation of the active one. Source: https://www.rfc-editor.org/rfc/rfc9700.html (sections 2.2.2, 4.14.2)
- **confirmed**: RFC 8725 (JWT BCP) requires pinning accepted algorithms, using each key with exactly one algorithm, validating issuer/audience, and recommends explicit typing. Source: https://www.rfc-editor.org/rfc/rfc8725.html
- **confirmed**: PASETO replaces JWT algorithm agility with versioned protocols; v4.local = XChaCha20 + BLAKE2b-MAC, v4.public = Ed25519; it cannot prevent replay by itself and is a community specification. Source: https://github.com/paseto-standard/paseto-spec ; https://github.com/paseto-standard/paseto-spec/blob/master/docs/01-Protocol-Versions/README.md
- **likely**: Current TypeScript/Go ecosystem pieces are maintained: Fastify 5.12.8, Hono 4.13.16, Bun 1.4.4 (built-in Postgres/Redis clients since 1.3), @node-rs/argon2 2.2.2 (MIT), panva/jose and panva/paseto (MIT, pushed 2026-11-02), coder/websocket for Go (ISC, pushed 2026-11-04); Node 26 entered LTS on 2026-10-28 per secondary sources. Source: https://registry.npmjs.org/fastify/latest ; https://registry.npmjs.org/hono/latest ; https://registry.npmjs.org/bun/latest ; https://registry.npmjs.org/@node-rs/argon2/latest ; https://api.github.com/repos/panva/jose ; https://api.github.com/repos/coder/websocket ; https://bun.com/blog/bun-v1.3 ; https://infoq.com/news/2026/06/nodejs-release-changes
- **confirmed**: GDPR anchors for retention and deletion: data minimisation (Art. 5(1)(c)), storage limitation (Art. 5(1)(e)), and erasure 'without undue delay' when grounds apply (Art. 17). Source: https://gdpr-info.eu/art-5-gdpr/ ; https://gdpr-info.eu/art-17-gdpr/
- **confirmed**: discord-rpc (MIT) carries a deprecation notice in favor of the Game SDK; the repo is not archived and was last pushed 2024-06-13. Source: https://github.com/discord/discord-rpc ; https://api.github.com/repos/discord/discord-rpc
- **confirmed**: Discord's docs say the Game SDK has been archived, existing projects keep working, and new projects should use the Discord Social SDK; Game SDK UpdateActivity is limited to 5 updates per 20 seconds. Source: https://docs.discord.com/developers/developer-tools/game-sdk (accessed 2026-11-04)
- **confirmed**: Discord Social SDK (launched 2025-03-17, free) provides rich presence, game invites (powered by presence party + join secret, SendActivityInvite, RegisterLaunchCommand, 180 s join hold), lobbies, friends, DMs, voice, account linking and provisional accounts; Rich Presence can be set without authentication when the desktop client is running. Source: https://techcrunch.com/2025/03/17/discord-launches-sdk-to-help-developers-enhance-social-experiences-in-their-games ; https://docs.discord.com/developers/discord-social-sdk/development-guides/setting-rich-presence ; https://docs.discord.com/developers/discord-social-sdk/development-guides/managing-game-invites
- **confirmed**: Social SDK platforms: Windows x64/ARM64 GA, macOS GA, Linux experimental (glibc 2.31+), Android/iOS GA, Xbox/PlayStation with approval; standalone C++ requires C++20 from SDK 1.9; Unity 2021.3+, Unreal 5.5+. Source: https://docs.discord.com/developers/discord-social-sdk/core-concepts/platform-compatibility.md (accessed 2026-11-04)
- **likely**: The Social SDK is proprietary: the terms (effective 2025-03-17) grant a limited, revocable, non-sublicensable license and allow distribution only as integrated into your application; Discord's developer terms bar embedding developer credentials in open-source projects. The terms page returned 403, so this is from search snippets. Source: https://support-dev.discord.com/hc/en-us/articles/30225844245271-Discord-Social-SDK-Terms ; https://docs.redpoint.games/docs/setup/platforms/discord
- **confirmed**: Linking a GPL program to a non-system proprietary library requires permission from the program's copyright holders (impractical for boiii-derived code with many contributors), while pipes/sockets normally indicate separate programs. Source: https://www.gnu.org/licenses/gpl-faq.html (GPLIncompatibleLibs, MereAggregation)
- **confirmed**: Under Wine/Proton, Windows games cannot reach a native Linux Discord client's IPC socket without a bridge; rpc-bridge (MIT, active 2026-04) creates \\.\pipe\discord-ipc-0 in the prefix and forwards it to the Unix socket; Flatpak Steam needs xdg-run/discord-ipc-0 access. Source: https://github.com/EnderIce2/rpc-bridge ; https://www.mado-hosting.com/satisfactory-mods/FG_DiscordRP
- **confirmed**: Wine bug 56378 (Edge/WebView2 not working without --no-sandbox) was closed FIXED in Wine 11.1 on 2026-01-23; earlier workarounds were setting msedgewebview2.exe to Windows 7/8.1 and passing --no-sandbox. Source: https://bugs.winehq.org/show_bug.cgi?id=56378
- **likely**: Proton 11 is rebased on Wine 11.0 (beta April 2026), so the Wine 11.1 WebView2 fix may not be included unless backported. The Proton 11 build bundles wine-gecko 2.47.4 and wine-mono 11.2.0. Source: https://raw.githubusercontent.com/ValveSoftware/Proton/proton_11.0/Makefile.in ; https://www.tweaktown.com/news/111106/valve-proton-11-beta-unlocks-more-playable-games-and-boosts-performance-for-steam-deck-linux-fans/index.html
- **likely**: WebView2 under Wine remains fragile in late 2026: the runtime is not in a fresh prefix and is installed manually (often pinned, with a Windows-version override), and reports include blank first frames and GPU-process E_NOTIMPL aborts (CrossOver 26). Source: https://github.com/khoipm06/wuwa-tappo-wine ; https://github.com/broowens/ascension-macos/issues/3 ; https://lutris.net/games/install/40011/view
- **confirmed**: Ezz BOIII moved its launcher to WebView2 in mid-2026; issue #274 (2026-07-27) reported that the UI rendered but was not interactive under Wine/Proton. Commit 8621e832 added a Wine-compatible launcher: on Wine it uses the legacy WebBrowser control rendered by Wine Gecko, with legacy-JS fixes, Linux font fallbacks and an X-UA-Compatible IE=edge meta tag, and SteamCMD workshop downloads are disabled. Current code never tries WebView2 on Wine; on Windows only E_NOTIMPL falls back, and other WebView2 errors show a dialog and close. Source: https://github.com/Ezz-lol/boiii-free/issues/274 ; https://api.github.com/repos/Ezz-lol/boiii-free/commits/8621e832 ; https://raw.githubusercontent.com/Ezz-lol/boiii-free/main/src/client/launcher/html/html_frame.cpp
- **likely**: Under Wine's MSHTML, layout comes from Wine Gecko (2.47.x) while most scripts run on Wine's own jscript.dll, so launcher HTML/JS must stay ES5-era and avoid modern CSS. Source: https://list.winehq.org/mailman3/hyperkitty/list/wine-commits@winehq.org/message/FJEH45DDXXSX4LHHCXA4VQU43WAKXCJJ (2012 commit) ; upstream commit 8621e832 'legacy JavaScript compatibility'
- **confirmed**: Black Ops III (311210) on ProtonDB: tier Gold, trending Platinum, best reported Platinum, 416 reports, confidence strong. Source: https://www.protondb.com/api/v1/reports/summaries/311210.json (accessed 2026-11-04)
- **uncertain**: Community reports for boiii on Linux/Deck are mixed: some players say it works (with Proton-GE, 2024), others hit prefix/launch failures (2024); one 2024 comment reports a Proton Experimental black screen on Deck; controller input was not detected via Steam Big Picture on Deck (issue #96, 2024); a dedicated server under Proton crashed on a sound read error (June 2026). Source: https://github.com/Ezz-lol/boiii-free/issues/49 ; https://github.com/Ezz-lol/boiii-free/issues/96 ; https://api.github.com/search/issues?q=repo:Ezz-lol/boiii-free+linux ; https://redlib.reallyaweso.me/r/linux_gaming/s/bi8Gcp9dRx
- **confirmed**: RmlUi (MIT) 6.3 (2026-08-22) added user-contributed DirectX 11 (Win32_DX11, GLFW_DX11) and DirectX 12 backends plus CSS variables and @font-face, and requires C++17. Its DX11 filters are fully featured but have open issues. Source: https://api.github.com/repos/mikke89/RmlUi/releases ; https://github.com/mikke89/RmlUi (accessed 2026-11-04)
- **confirmed**: RmlUi's RCSS supports flexbox, transitions (only when a class or pseudo-class changes), @keyframes animations with named tweening functions, fully interpolated transforms, linear/radial/conic gradients (as decorators), box-shadow (not animatable), filter/backdrop-filter, masks and data binding. It is based on CSS2 with selected CSS3 features. Source: https://github.com/mikke89/RmlUi ; https://mikke89.github.io/RmlUiDoc/pages/rcss/animations_transitions_transforms.html ; https://mikke89.github.io/RmlUiDoc/pages/rcss.html
- **confirmed**: Dear ImGui is MIT-licensed and current at v1.93.0 (2026-10-14), with an official DX11 backend; styling is through ImGuiStyle rather than CSS. Source: https://api.github.com/repos/ocornut/imgui ; https://api.github.com/repos/ocornut/imgui/releases
- **likely**: CEF's Windows x64 runtime package (chromiumembeddedframework.runtime.win-x64 156.0.15, updated 2026-11-03) is 177.34 MB compressed. CEF is BSD-style licensed and multi-process Chromium; under Wine it likely faces the same sandbox and GPU-process issues as WebView2. Source: https://www.nuget.org/packages/chromiumembeddedframework.runtime.win-x64 ; https://api.github.com/repos/chromiumembedded/cef
- **likely**: Ultralight is proprietary. The Free tier requires turnover and funding under $200K and is capped at 60 FPS with no JS JIT or media. Indie is $395 per title (under $1M), Pro $4,500 per title (under $10M), and source access is Enterprise-only, so it cannot be linked into GPL-3.0 boiii-derived code without an exception. Source: https://ultralig.ht/pricing (accessed 2026-11-04) ; https://www.gnu.org/licenses/gpl-faq.html
- **confirmed**: DXVK 3.1 (2026-08-28) improved compatibility with external overlays that hook specific DXGI entry points and added a dxvk.enableNvCudaInterop option whose disabling also disables DLSS; DXVK 3.1.1 (2026-09-15) followed. Source: https://api.github.com/repos/doitsujin/dxvk/releases
- **confirmed**: DXVK has implemented D3D11On12 (a D3D11 device created from a D3D12 device) since 2.2, which requires vkd3d-proton 2.9+. Proton-CachyOS 11 added experimental OptiScaler integration in May 2026 (PROTON_USE_OPTISCALER=1), which its developer says will not work with all games. Source: https://github.com/doitsujin/dxvk/releases/tag/v2.2 ; https://github.com/HansKristian-Work/vkd3d-proton/releases/tag/v2.9 ; https://www.gamingonlinux.com/2026/05/proton-cachyos-11-adds-initial-optiscaler-integration-and-lots-of-other-fixes/page=1/
- **uncertain**: Official FSR 2 targets DX12/Vulkan; DX11 is only available via an unofficial patch (from AMD's Unity URP tutorial). XeSS DX11 support is contested in public sources. Source: https://gpuopen.com/learn/fsr2-for-unity-urp-dx11/ ; https://forums.flightsimulator.com/t/fsr-2-support-for-directx-11-if-possible/549319 ; https://github.com/intel/xess
- **confirmed**: DLSS for D3D11/D3D12 under Proton goes through dxvk-nvapi, which needs an NVIDIA Turing-or-newer GPU on the proprietary driver and is enabled by default since Proton 9.0. On non-NVIDIA GPUs (such as the Steam Deck) it reports a Pascal architecture to prevent DLSS initialization. Source: https://github.com/jp7677/dxvk-nvapi
- **likely**: Steam Deck's gamescope compositor applies FSR 1.0 or NIS spatial upscaling to any game, so in-game spatial upscalers are optional on Deck. Source: https://wiki.archlinux.org/title/Gamescope ; https://pcgamer.com/valves-steam-deck-now-comes-with-amd-fsr-upscaling-as-standard-on-any-game

## Details

# Xerces infrastructure research (snapshot 2026-11-04)

Legend: **[S]** = from secondary/forum sources or likely to change soon. Re-check Railway's weekly changelog and Proton/DXVK releases before relying on these details.

---

## 1. Railway

### 1.1 Feature check

| Area | Status | Details |
|---|---|---|
| **Public UDP ingress** | **No** | Public ingress is HTTP/HTTPS plus TCP Proxy only. The inbound-UDP request (opened 2026-02-22) is still "under review". There is no UDP entry in the changelog through 2026-10-30. |
| TCP Proxy | Yes | Railway assigns a `*.proxy.rlwy.net:<port>` endpoint and load-balances randomly across replicas in the nearest region. A custom domain works via CNAME (the port stays the same; Cloudflare proxying must be off). It does **not** pass the client IP and has no PROXY protocol [S]. |
| WebSockets | Yes (HTTP/1.1) | Exempt from the duration and idle limits. Other limits: HTTP requests up to 15 min while transferring and 5 min idle, 10k concurrent connections per service, ~11k RPS per domain, 60 s HTTP/1.1 keep-alive. |
| Private networking | Yes | WireGuard, `<svc>.railway.internal`. Dual-stack for environments created after 2025-10-16 (older ones IPv6-only). UDP is allowed *between services*. Runtime only (not during builds), same project and environment only, no egress charge. |
| Regions | 4 | US West (California), US East (Virginia), EU West (Amsterdam), SE Asia (Singapore). |
| Postgres | Template container | Railway-maintained SSL image. Optional HA (Patroni/etcd/HAProxy), one-click major-version upgrades (2026-09), PgBouncer, extension templates. Private by default; public access means a TCP proxy and billed egress. |
| Backups | Volume snapshots | Daily (kept 6 d), weekly (27 d), monthly (89 d) and manual (up to 50% of the volume size). Incremental, billed at the volume rate. Restores only into the same project and environment. **Wiping the volume deletes its backups.** Forum answers say this is Pro-only [S]. |
| PITR | Yes (since 2026-05) | pgBackRest archives WAL to a Railway bucket: weekly full plus daily differential backups, last 4 fulls kept (about a 4-week window). A restore creates a sibling service and the cutover is manual. WAL can be dropped if the 5 GiB queue cap trips during a storage outage. |
| Cron | Yes | At least 5 min between runs, UTC. A run is skipped if the previous one is still going, and the job must exit. CLI: `railway cron list/run`. |
| Health checks | Deploy-time only | Not continuous. Default timeout 300 s, probed from `healthcheck.railway.app`. Services with volumes have a brief outage on redeploy. |
| Logs | Built in | Kept 3 d on Free, 7 d on Hobby, 30 d on Pro, up to 90 d on Enterprise. Capped at 500 lines/s per replica. **No log drain**; use Vector, Fluent Bit or the OTel collector template. |
| Alerts | Monitors (Pro) | Thresholds on CPU, RAM, disk and egress, delivered by email, in-app or webhook. Deploy and crash webhooks and tracing are also available. |
| Edge | Edge Rules (2026-08) | Match on IP/CIDR, host, path or header, then Block, Redirect or Challenge. No rate limiting. |
| Redeploys | Overlap and draining | `RAILWAY_DEPLOYMENT_OVERLAP_SECONDS` and `RAILWAY_DEPLOYMENT_DRAINING_SECONDS` (SIGTERM, then SIGKILL). WebSocket clients must reconnect. |
| Pricing | Usage-based | Free $0 (+$1 credit), Hobby $5 (includes $5 of usage), Pro $20 (includes $20). CPU $20/vCPU-month, RAM $10/GB-month, volume $0.15/GB-month, egress $0.05/GB. Builds are free. |
| Spend control | Soft and hard limits | The soft limit only sends an email. The hard limit takes **all workloads offline**. Reminders at 75/90/100%, minimum $10. CLI: `railway usage limit`. |
| Pro-only features | | SMTP, static outbound IPs, Monitors, and (probably) backups. |
| CLI | npm `@railway/cli`, Scoop, Homebrew, shell script | `railway up`, `logs`, `run`, `ssh`, `connect`, `variable set`, `usage limit status`. CI authenticates with `RAILWAY_TOKEN` (project) or `RAILWAY_API_TOKEN` (workspace). |
| Security | 2026-10-20 to 22 incident | Stolen CI credentials were used to decrypt a subset of environment variables in up to 1.85% of workspaces. Rotate any secrets set before 2026-10-21. |

**Small bill example (assumed load):**
- API service at 0.1 vCPU and 256 MB: $2 + $2.50
- Postgres at 0.05 vCPU and 256 MB: $1 + $2.50
- 1 GB volume: $0.15
- 10 GB egress: $0.50
- **Total: about $8.65/month.** On Hobby you pay about $8.65. Pro is a flat $20 until usage passes $20.

### 1.2 Getting UDP (game relay, STUN, TURN) anyway

| Option | Quality / latency | Cost (2026 list prices) | Verdict |
|---|---|---|---|
| coturn on Railway, reached by TURN over TCP/TLS through the TCP Proxy | See notes below | Railway egress $0.05/GB plus compute | Last resort only |
| WebSocket relay on Railway, DERP-style (like Tailscale's fallback) | Same TCP head-of-line problem, but easy to authenticate and gets through UDP-blocking networks | $0.05/GB | Acceptable as a fallback that guarantees connectivity, never as the primary path |
| **Cloudflare Realtime TURN** (managed) | Anycast allocation near the client. Ports: UDP 3478/443, TCP 3478/80, TLS 5349/443. IPv4 relay addresses only. No RFC 6062 TCP relaying. Loss possible above ~5–10 kpps or ~50–100 Mbps per client. | 1,000 GB/month free (shared with SFU), then $0.05/GB from edge to client. STUN is free. | **Default choice.** Test interop with libjuice or GNS first. |
| coturn on Fly.io | UDP via `fly-global-services` on a dedicated IPv4. Plan for ~1300-byte payloads. No public IPv6 UDP. | VM $2.19/mo + IPv4 $2/mo + egress $0.02/GB (NA/EU), $0.04 (APAC/SA), $0.12 (Africa/India) | Good self-hosted option, can run in multiple regions |
| coturn or a custom relay on a Hetzner VPS | Native UDP in EU, US and SG locations | From about €5.99/mo excl. VAT after the two 2026 price rises. Large included traffic in the EU (exact figures vary) [S]. | Cheapest bandwidth, but you run the box (patching, DDoS) |
| Direct only (port forwarding plus UPnP via miniupnpc, BSD-3) | Best latency when it works | $0 | Not enough alone: players behind hard NAT or CGNAT can't host |

Notes on coturn behind Railway's TCP Proxy:
- TCP head-of-line stalls on any packet loss, over two TCP legs routed through one of only 4 regions.
- STUN is impossible: there is no inbound UDP, and the proxy hides the client IP.
- Relayed UDP addresses can't be reached from the internet, so both peers must allocate on the same coturn.
- No port 443 is available.
- libjuice can't do TURN over TCP, so it can't use this path at all.

**Relay bandwidth estimate (an assumption, not measured for BO3):**
- Assume ~100 kbit/s each way per relayed player: about 45 MB/h each way, about 90 MB/h through the relay.
- 200 relayed player-hours a month is about 18 GB.
- That costs about $0.36 on Fly (NA/EU), about $0.90 on Railway, and nothing within Cloudflare's free 1 TB.
- If a **host** sits behind hard NAT, every joiner is relayed, so multiply accordingly. Measure real BO3 packet rates before sizing.

---

## 2. NAT traversal and connection security (C++ Windows client)

| Library | License / GPL-3.0 fit | What it provides | Windows build | Relay and TCP | Status |
|---|---|---|---|---|---|
| **libjuice** | MPL-2.0 (combinable with GPL under MPL §3.3) | ICE (RFC 8445), STUN (8489), TURN client (8656), embedded lightweight STUN/TURN server, single-port mux, dual-stack | C, MSVC, no required deps | TURN reached over UDP only [likely]. ICE-TCP (RFC 6544) since 1.7.0 (2025-11-15). | v1.7.4 (2026-09-26) |
| libnice | LGPL-2.1+ OR MPL-1.1 (OK) | Full ICE, TURN client, partial ICE-TCP, TCP-over-UDP streams | Pulls in GLib (heavy) | TURN | 0.1.24 (packaged 2026-09) |
| **GameNetworkingSockets** | BSD-3-Clause (OK) | Full transport: reliable/unreliable messages, fragmentation, AES-GCM-256 + Curve25519, custom signaling, symmetric connect. **Native ICE with no external deps is the default since v1.6.0 (2026-06-03), with TURN (RFC 5766).** | Needs protobuf plus OpenSSL 1.1.1+, libsodium or bcrypt | TURN yes. Steam Datagram Relay is not available off Steam. TCP/TLS TURN transport is not documented. | v1.6.1 (2026-10-03) |
| coturn (server) | BSD-3-Clause | UDP/TCP/TLS/DTLS listeners, RFC 6062, TURN REST API (time-limited shared-secret credentials) | Linux / Docker | n/a | 4.18.0 (2026-09-08). Needs OpenSSL 3. Set `denied-peer-ip` explicitly (CVE-2020-26262 history). |
| netcode | BSD-3-Clause | Secure client/server UDP: connect tokens sealed with XChaCha20-Poly1305 (libsodium), encrypted and signed packets | C / CMake | **No NAT traversal** | Standard 1.02 |
| noise-c | MIT | Noise reference implementation | ? | n/a | Stale (last commit 2023-12) |
| libhydrogen | ISC | Small library with a Noise-based key exchange (Curve25519 + Gimli) | C99, Windows | n/a | Active (2026-09) |
| **libsodium** | ISC | Argon2id (`crypto_pwhash` default since 1.0.15), XChaCha20-Poly1305, `crypto_kx`, Ed25519 | MSVC/MinGW, x86/x64/arm64 | n/a | 1.0.22-stable |
| Monocypher | CC0-1.0 OR BSD-2 | XChaCha20-Poly1305, X25519, EdDSA, BLAKE2b, Argon2i/d/id | Single C file | n/a | 4.0.3 |

### Hole-punch success rates

- **IPFS/libp2p measurement (IMC '26; 4.4M attempts, 85k networks, 167 countries):** about **70% ±7.1%** success, conditional on the earlier steps. TCP and QUIC are statistically the same, and 97.6% of successes come on the first try.
- **ProbeLab:** about 82.9% for port-restricted cone NATs, and about 40% for symmetric NAT on a small, doubtful sample.
- **Ford et al. (2005):** about 82% of tested NATs supported UDP hole punching.
- **WebRTC rule of thumb:** 10–20% of sessions need TURN [S].

### When a relay is needed

- Both peers are behind hard (endpoint-dependent, "symmetric") NAT.
- One peer is hard and port prediction fails.
- Both peers are behind the same CGNAT, which doesn't support hairpinning.
- The network blocks UDP; this needs TURN over TCP/TLS on port 443.
- UPnP, NAT-PMP or PCP are disabled, or double NAT breaks them.

For listen-server hosting, the **host's** NAT dominates: if the host is behind hard NAT, every joiner has to relay. Direct P2P also **reveals IP addresses** to other players; a relay hides them.

### Suggested client shape for BO3 (design guidance, unsourced)

- **Leave BO3's netcode alone and add a local UDP tunnel.**
  - The joiner's game connects to `127.0.0.1:<port>`.
  - The tunnel carries datagrams over an ICE path (libjuice), or over GNS unreliable messages, to the host's game port (27017 by default), with TURN as the fallback.
- **Authentication.** Over the authenticated WebSocket, the backend gives the host a fresh 32-byte key for each session (netcode-style). Joiners receive connect tokens sealed with that key, valid for about 30–60 s. A malicious host can only mint tokens for its own session.
- **Alternative.** A Noise XX/IK handshake (libsodium or libhydrogen) using static keys signed by the backend.
- **Encryption.** Encrypt tunnel datagrams with XChaCha20-Poly1305 plus a replay window, or use GNS's built-in encryption with your own CA.

---

## 3. Backend stack (accounts, friends, presence over WebSocket, invites, rate limiting)

| | **TypeScript** (Node LTS or Bun; Fastify / Hono) | **Go** | **Rust** |
|---|---|---|---|
| Solo-dev velocity | Highest; largest ecosystem | High; small language with a strong standard library | Lowest at first |
| Versions (2026-11) | Fastify 5.12.8, Hono 4.13.16, Bun 1.4.x, Node 26 LTS since 2026-10-28 [S] | Go 1.2x | axum / tokio |
| WebSocket | `@fastify/websocket` (ws), Hono helpers, Bun native WebSocket with pub/sub | `coder/websocket` (ISC, active) | axum ws, tokio-tungstenite |
| Postgres | pg, postgres.js or Bun.sql, with Kysely or Drizzle | pgx + sqlc | sqlx |
| Argon2id | `@node-rs/argon2` 2.2.2 (MIT, prebuilt binaries) | `x/crypto/argon2.IDKey` | `argon2` crate (RustCrypto) |
| Tokens | `jose` (EdDSA JWT) and `paseto` v4, both MIT and active 2026-11 | go-jose, PASETO libraries | jsonwebtoken, pasetors |
| RAM on Railway ($10/GB-month) | Highest | Low | Lowest |
| Main risks | npm supply chain; blocking the event loop | Verbose code | Learning curve, slow builds |

**Pick:** TypeScript on Node LTS with Fastify (or Hono), Postgres, one replica to start. If you add replicas, use Postgres LISTEN/NOTIFY or Railway Redis for presence fan-out. Go is the lean alternative. Rust only makes sense if you're already fluent. If the developer is most at home in Python, FastAPI with asyncpg is also viable; the security design below doesn't depend on the language.

### Auth design

- **Password hashing:** Argon2id at OWASP minimums: m=19 MiB, t=2, p=1 (or 46 MiB with t=1, 12 MiB with t=3, and so on). Unique salt per user. Optional pepper, kept outside the database. Limit how many hashes run at once and rate-limit logins.
- **Access tokens:** 5–15 minutes, using an asymmetric scheme so relay and tunnel servers on other providers can verify them offline. Either:
  - PASETO v4.public (Ed25519), or
  - JWT with EdDSA, a pinned algorithm list, and checks on `typ`, `iss` and `aud` (RFC 8725).
- **Refresh tokens:** opaque 256-bit values, stored hashed and tied to a device or session row. Rotate on every use. If an old token is reused, revoke the whole family (RFC 9700 §4.14.2). Set both an idle timeout and an absolute lifetime.
- **WebSocket:**
  - Authenticate in the first frame, not in the URL query (keeps tokens out of logs).
  - Re-authenticate before the access token expires.
  - Send server pings.
  - Reconnect with jitter after each Railway redeploy.
- **Join and relay tickets:** separate, audience-scoped, valid for 60 s or less. TURN credentials should last minutes to a few hours (Cloudflare allows at most 48 h).
- **Rate limiting:** per account and per IP, using a token bucket in memory for a single replica, or in Redis/Postgres. Use Edge Rules to block known-bad IP/CIDR ranges. Put Cloudflare in front only if you need a WAF. Railway says its HTTP edge overwrites `X-Real-IP`, but some users report otherwise [S].
- **Secrets:** assume environment variables can leak (see the 2026-10 incident). Use key IDs (`kid`), run old and new keys side by side during rotation, and avoid static TURN credentials.

### Privacy and retention

| Data | Policy |
|---|---|
| Account: username, Argon2id hash, optional email for recovery | Delete on account deletion. Reserve the username for a grace period. Offer a JSON data export. |
| IP addresses / ICE candidates | Never store them in the database. Rate-limit keys live in memory with a TTL. Logs follow Railway retention (7 d on Hobby, 30 d on Pro). Exchange candidates only after both sides consent (invite or friendship). Offer a relay-only privacy mode. |
| Presence | Ephemeral, in memory only. Support "appear offline", friends-only visibility and blocking. |
| Invites / friend requests | Expire after minutes (invites) or days (requests). Purge with a Railway cron job. |
| Sessions | Revoke all on deletion or password change. |
| Backups | State in the privacy policy that deleted data can persist until backups expire: up to 89 d for volume backups, about 4 weeks for PITR. |

GDPR anchors: data minimisation (Art. 5(1)(c)), storage limitation (Art. 5(1)(e)), and erasure "without undue delay" (Art. 17). This is not legal advice.

---

## 4. Discord

| | discord-rpc | Game SDK | Social SDK |
|---|---|---|---|
| Status | Deprecated in favor of the Game SDK. Not archived; last push 2024-06. | Archived (per Discord docs); existing integrations keep working | Current (launched 2025-03-17, free) |
| License | **MIT** | Proprietary | Proprietary. Limited, revocable license; ship only as part of your app; no standalone redistribution [terms page returned 403] |
| Rich presence | Yes, via the client's local IPC | Yes (5 updates per 20 s) | Yes; works without authentication when the desktop client is running |
| Invites / party | Join secrets (legacy) | Invites, join, spectate | Invites built on presence: party + join secret, `SendActivityInvite`, `RegisterLaunchCommand`, 180 s join hold |
| Social features | No | n/a | Lobbies, friends, DMs, voice, account linking, provisional accounts (need a server-side bot token) |
| Platforms | Windows / macOS / Linux | Windows / macOS / Linux | Windows GA, macOS GA, Linux experimental (glibc 2.31+), mobile GA, consoles. C++20 required since 1.9. |
| GPL-3.0 fit | Good | Poor | Poor in-process: needs a linking exception that only the copyright holders can grant. A separate helper process is a legal gray area. |
| Under Proton | All three need an IPC bridge (rpc-bridge, MIT) to reach a native Linux Discord client | | |

**Recommendation:** implement rich presence and join secrets by talking to the local Discord IPC yourself, or vendor the MIT discord-rpc. Only the public Application ID goes in the client. Never ship a client secret or bot token: Discord's developer terms bar embedding credentials in open-source projects.

---

## 5. Steam Deck / Proton

### WebView2 under Wine/Proton

- Wine bug 56378 (Edge/WebView2 needing `--no-sandbox`) was **fixed in Wine 11.1** (2026-01-23).
- Proton 11 is based on Wine 11.0. Whether the fix was backported is **unverified**.
- The runtime is not present in a fresh Proton prefix. Community installers add it by hand, often pinning a version and setting a Windows-version override.
- Late-2026 reports include blank first frames and GPU-process `E_NOTIMPL` aborts (CrossOver 26).

**Upstream evidence from Ezz BOIII:**
- In mid-2026 the launcher moved to WebView2. Issue #274 (2026-07-27) reported that under Wine/Proton the UI rendered but did nothing when clicked.
- Commit 8621e832 fixed this: when Wine is detected, the launcher uses the **legacy WebBrowser/MSHTML control**, which Wine renders with **Wine Gecko**.
  - It added legacy-JS fixes, Linux font fallbacks and an `X-UA-Compatible` meta tag.
  - It disabled SteamCMD workshop downloads under Wine.
- Proton 11 bundles **wine-gecko 2.47.4** (and wine-mono 11.2.0). Layout is a Firefox-47-era engine and most scripts run on Wine's own jscript, so any HTML launcher must target ES5-era JS and CSS on Deck [likely].

### boiii-style clients on the Deck today

- **No official support:** the README lists Windows 10/11 only.
- **The base game runs well:** ProtonDB rates BO3 Gold, trending Platinum (416 reports).
- **boiii itself is mixed** [S]:
  - Works for some players with Proton-GE (2024).
  - Prefix and launch problems for others (2024).
  - A Proton Experimental black screen reported on Deck (2024).
  - Controller input not detected via Big Picture (issue #96, 2024).
  - A dedicated server under Proton crashed (2026-06).
  - SteamCMD workshop downloads are off under Wine.
- PatchOpsIII v1.0.3 (2025-03) ships Linux/Deck builds for setting up the T7 patch.

### Launcher UI options

| Option | License / GPL-3.0 fit | Size and startup | Proton behavior | Styling |
|---|---|---|---|---|
| WebView2 | Microsoft runtime (system component) | No bundle (Evergreen runtime) | Fragile; upstream had to bypass it | Modern CSS/JS |
| Legacy MSHTML | Windows component | Tiny | Works through Wine Gecko 2.47.4 | Old: IE11 on Windows, Gecko 47 on Wine; ES5 |
| CEF | BSD-style (compatible) | Win x64 runtime package 177 MB compressed; multi-process | Likely the same sandbox and GPU-process issues as WebView2 | Modern CSS/JS |
| Ultralight | Proprietary. Free tier only under $200K turnover and funding (60 FPS cap, no JS JIT). **Incompatible with GPL-3.0.** | Small | Draws through D3D, so through DXVK on Proton | HTML subset |
| **RmlUi** | **MIT (compatible)** | Small library; no browser process | D3D11 runs through DXVK; GL3 runs through Wine's OpenGL | See below |
| Dear ImGui | MIT (compatible) | Tiny | D3D11 runs through DXVK | ImGuiStyle settings; animation is manual; no CSS |

RmlUi styling (RCSS, which is CSS2 plus some CSS3):
- Flexbox.
- Transitions, which fire only when a class or pseudo-class changes.
- `@keyframes` animations.
- Transforms.
- Linear, radial and conic gradients (as decorators).
- Box-shadow (cannot be animated).
- `filter` and `backdrop-filter`.
- CSS variables (added in 6.3).
- Data binding.
- Gamepad and keyboard navigation [likely].

RmlUi's DX11 and DX12 backends are new: user-contributed in 6.3 (2026-08-22), and the DX11 filters have open issues. That makes **GL3, the reference renderer, the safer launcher backend**; use DX11 for anything drawn inside the game.

### DXVK / vkd3d-proton implications for DX11 upscaler hooks

- **How the game runs:** on Proton, BO3's `d3d11.dll`/`dxgi.dll` are DXVK DLLs translating to Vulkan.
- **Present hooks work:** COM vtable hooks on `IDXGISwapChain::Present` and `ResizeBuffers` work under DXVK. DXVK 3.1 (2026-08-28) improved compatibility with overlays that hook DXGI entry points.
- **No DLL proxies:** don't drop `dxgi.dll`/`d3d11.dll` proxies into the game folder, because they collide with Proton's DXVK overrides (ReShade's guidance is to keep only one dxgi.dll). Hook at runtime, or chain-load.
- **Precompiled shaders:** ship DXBC rather than compiling with `D3DCompile` at runtime, because Wine's shader compiler differs [likely].
- **D3D12-interop upscalers (FSR 3.1+/4, XeSS FG):** these need D3D11↔D3D12 interop, which on Proton means DXVK↔vkd3d-proton.
  - DXVK has had D3D11On12 since 2.2 (needs vkd3d-proton 2.9 or newer).
  - OptiScaler's Proton integration is experimental (Proton-CachyOS, 2026-05).
  - Treat this whole path as Windows-first.
- **FSR 2 and XeSS on DX11:** official FSR 2 targets DX12/Vulkan (DX11 only via an unofficial patch); XeSS DX11 support is contested.
- **DLSS:** via dxvk-nvapi it needs an NVIDIA Turing-or-newer GPU, so it never works on a Deck.
- **Most portable option:** a spatial pass (FSR1 or CAS) written as plain D3D11 shaders, combined with BO3's render scale. On the Deck, gamescope already provides FSR1/NIS.

---

## 6. Staleness notes

- Railway ships changes weekly. UDP support, plan gating of backups, and Edge Rules features can change.
- Proton, GE-Proton and DXVK release often. WebView2 behavior depends on the exact Proton build.
- The Discord Social SDK terms could not be fetched (HTTP 403), so the license summary comes from search snippets.
- Hetzner traffic allowances and the hole-punch percentages come from secondary or contextual sources.

## Recommendations

- Use Railway for the HTTP API, WebSocket presence and ICE-candidate signaling, cron jobs and Postgres. Design on the assumption that Railway has no UDP ingress, and keep the database on private networking (no public TCP proxy).
- Use Cloudflare Realtime TURN as the default relay: the backend mints short-lived credentials (minutes to hours, at most 48 h), and STUN is free. First run a one-day interop test with libjuice or GameNetworkingSockets. Keep coturn on Fly.io (dedicated IPv4) or Hetzner as the self-hosted fallback, using TURN REST shared-secret credentials and an explicit denied-peer-ip for loopback and private ranges.
- Do not route game traffic through TURN over TCP on Railway's TCP proxy. At most, run a DERP-style WebSocket relay on Railway as a last resort for networks that block UDP.
- For the client, add a local UDP tunnel that leaves BO3's netcode untouched. Use libjuice (MPL-2.0, light) for ICE, plus libsodium XChaCha20-Poly1305 encryption with backend-issued tokens for each session. Alternatively, evaluate GameNetworkingSockets 1.6.1 (BSD-3) as an all-in-one option (ICE, TURN and encryption) if the protobuf dependency is acceptable. Add miniupnpc for hosts.
- Add a NAT-type probe to the launcher, and warn or auto-relay when a would-be host is behind hard NAT or CGNAT. Offer a relay-only privacy mode, since direct P2P reveals players' IPs.
- Authenticate peer joins with short-lived tickets (60 s or less) issued by the backend. Either use netcode-style connect tokens sealed with a fresh 32-byte key per hosted session, or a Noise XX/IK handshake via libsodium or libhydrogen. Avoid noise-c, which has had no commits since 2023-12.
- Build the backend in TypeScript on Node LTS with Fastify (or Hono), Postgres via Kysely or Drizzle, ws, @node-rs/argon2 and jose or paseto, on one replica at first. Choose Go instead if you want a lean static binary; choose Rust only if you're already fluent.
- Hash passwords with Argon2id at OWASP minimums (19 MiB, t=2, p=1) and limit concurrent hashing. Issue 5–15-minute asymmetric access tokens (PASETO v4.public, or EdDSA JWT with a pinned algorithm and typ/iss/aud checks). Use opaque refresh tokens stored hashed, rotated on every use, with family revocation when an old one is reused (RFC 9700). Authenticate WebSockets in the first frame rather than the URL.
- Choose the Railway plan deliberately. Either take Pro ($20) for native backups, PITR and monitors, or stay on Hobby and run a nightly pg_dump cron to off-Railway storage. Set a soft email alert and set the hard limit carefully, since hitting it takes everything offline.
- After Railway's October 2026 incident: rotate any secret created before 2026-10-21, keep secrets few and give them key IDs with rotation, use short TTLs on TURN and session credentials, and write a key-rotation runbook.
- Privacy: collect only a username, password hash and optional email. Don't store IPs or ICE candidates. Make invites and friend requests expire, with cron purges. Offer self-serve deletion and data export. Disclose that deleted data persists until backups expire (up to 89 days for volume backups, about 4 weeks for PITR).
- Discord: implement rich presence and join secrets over the Discord client's local IPC (your own small implementation or the vendored MIT discord-rpc), not the proprietary Social SDK in-process. Document rpc-bridge for Proton/Deck users, and keep any bot token server-side.
- Launcher UI: migrate to RmlUi (MIT) with the GL3 renderer for the launcher window and DX11 for in-game overlays, with gamepad navigation. Keep a direct-launch path that skips the launcher for the Deck. If you stay on HTML, keep upstream's Wine fallback to the legacy WebBrowser control and target ES5 JS with Gecko-47-era CSS.
- Upscaling: implement FSR1 or CAS as plain D3D11 shaders with precompiled DXBC, hooking Present at runtime with no DLL proxies. Treat D3D12-interop upscalers and DLSS as optional Windows-only features, and rely on gamescope FSR1/NIS on the Deck.
- Test on Windows 10/11 (with and without the WebView2 runtime) and on the Deck with Proton 11 stable, Proton Experimental and GE-Proton 11. For networking, test CGNAT (mobile hotspot), symmetric NAT (cloud VM) and a UDP-blocked network.

## Risks

- Railway has no public UDP, and the inbound-UDP request has no ETA. Any design that assumes Railway can carry game, STUN or TURN traffic will fail. TURN over TCP via the TCP proxy hides client IPs, suffers TCP head-of-line stalls, and can't be used by libjuice.
- Railway's hard spend limit takes ALL workloads offline, database included. A misjudged cap means a community-wide outage, and soft limits only send email.
- Railway's 2026-10-20 to 22 incident decrypted a subset of environment variables in up to 1.85% of workspaces. Signing keys, TURN secrets and database URLs stored as environment variables need a rotation plan.
- Data-loss risk: native backups and monitors appear to be Pro-gated. Restores only go into the same project and environment, and wiping a volume deletes its backups. PITR can lose WAL during object-storage outages.
- Abuse of the relay or TURN server: an open relay can become a proxy into private ranges (see the coturn CVE-2020-26262 history). Static TURN credentials in an open-source client would leak, and Cloudflare TURN starts dropping packets above roughly 5–10 kpps per client.
- Privacy and safety: direct P2P exposes player IPs, enabling DDoS and harassment. P2P hosts behind hard NAT force every joiner onto the relay, raising relay load.
- GPL-3.0 compliance: proprietary in-process SDKs (Discord Social SDK, Ultralight) conflict with boiii-derived GPL code, and only all the copyright holders could grant a linking exception. Modified libjuice files carry MPL-2.0 file-level source obligations.
- Steam Deck/Proton fragility: WebView2 under Wine is unreliable, and the fallback MSHTML/Wine Gecko 2.47 engine is old. Behavior can change between Proton builds. Controller input and SteamCMD workshop downloads were already problems upstream.
- DXVK and Proton updates can break hooks. DLL-proxy approaches collide with DXVK overrides. Upscalers that rely on D3D12 interop are experimental under Proton, and DLSS is unavailable on the Deck.
- RmlUi's DX11 and DX12 backends were added only in 2026-08 as user contributions, with open issues in DX11 filters, so rendering bugs are possible.
- A central account backend for a mod client of a commercial game raises visibility and takedown/terms-of-service exposure. Keep offline and LAN play fully functional without the backend.
- Data protection duties: EU users bring GDPR obligations (erasure, minimisation, retention disclosure). BO3 is M-rated, so decide on a minimum age and avoid collecting age data you don't need. This is not legal advice.
- Staleness: Railway ships weekly, and Proton/DXVK release monthly. Several figures (Hetzner traffic, hole-punch rates, Discord terms) come from secondary sources.

## Open questions

- Will Railway ship public UDP ingress? The feature request has been 'under review' since 2026-02-22 with no staff response.
- Are Railway volume backups and Postgres PITR available on Hobby, or Pro-only? The docs are silent and forum answers say Pro.
- Does Proton 11 (Wine 11.0 base), Proton Experimental or GE-Proton 11 include the Wine 11.1 WebView2 sandbox fix, and does a WebView2 launcher actually work on a Steam Deck today?
- Does libjuice's TURN client (and GNS 1.6's native ICE) interoperate with Cloudflare Realtime TURN credentials, and does GNS support TURN over TCP/TLS transport?
- What are BO3's real packet rates and bandwidth per player (host and client) for relay cost and Cloudflare kpps sizing?
- What exactly do the Discord Social SDK Terms say about open-source or copyleft distribution? The terms page returned HTTP 403.
- Should Xerces bridge game traffic through a localhost tunnel, or hook the engine's own socket? How does each affect boiii's server browser, IP-based bans and per-IP connection limits?
- Which features truly need the backend (friends, presence, invites) versus staying local or LAN-only, given the project's self-hosted and offline-play goals?
- Where are the players located, relative to Railway's 4 regions and to the relay locations (Cloudflare anycast, Fly or Hetzner regions)?
- What minimum age and data-collection policy (email or no email) fit an M-rated game's community?
