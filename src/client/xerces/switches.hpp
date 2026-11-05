#pragma once

// Build switches for upstream features Xerces turns off (docs/DESIGN.md D14,
// D15 and docs/ENDPOINTS.md). Each is a premake option that is off by default;
// `premake5 ninja --with-ezz-social` or `--with-upstream-downloaders` builds
// the upstream behaviour back in.
namespace xerces_switches {
#ifdef XERCES_WITH_EZZ_SOCIAL
inline constexpr bool ezz_social = true;
#else
// D14: friend presence, friend invites, NAT rendezvous (including the address
// the masters reflect back) and Ezz's Discord application send identifiers to
// Ezz's servers or to Discord. Off until Xerces' backend replaces them (M5,
// M6). The read-only public server list is not affected.
inline constexpr bool ezz_social = false;
#endif

#ifdef XERCES_WITH_UPSTREAM_DOWNLOADERS
inline constexpr bool upstream_downloaders = true;
#else
// D15: anonymous SteamCMD, the anonymous Steam CM/CDN client, the in-game
// download queue, FastDL v1 and the old launcher's Workshop downloader are
// left out of the build; xerces/no_upstream_downloaders.cpp stands in for the
// few functions other code calls. Missing Workshop items are named and opened
// in Steam instead.
inline constexpr bool upstream_downloaders = false;
#endif
} // namespace xerces_switches
