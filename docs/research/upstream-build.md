# Upstream build, CI, licences and tests

> Machine-generated research notes from 2026-11-04, kept as evidence for
> `docs/DESIGN.md`. Code was read at upstream `main` 5aa7fac, `mod-stack`
> ac356c3c and the fork `refs/heads/main` 7805319. `path:line` citations refer
> to those commits. Treat claims as leads: anything marked runtime still needs
> the real game.

## Summary

Upstream builds with premake5. One workspace, `boiii`, generates into ./build
and uses the `msc-clangcl` toolset on Windows (VS PlatformToolset `clangcl`). It
is C++20, x64 only, has Debug and Release configs and links the CRT statically.
There are three first-party projects: `common` (a StaticLib of Windows
utilities), `client` (a WindowedApp with targetname `boiii`, which builds
boiii.exe) and `tlsdll` (a CRT-less SharedLib embedded into boiii.exe as RCDATA
and extracted to %TEMP% at runtime). Twenty `deps/premake/*.lua` scripts add 13
dependency static libs plus several header-only deps. Only the icon, the splash
image, the Demonware-emulation blobs (including an Activision fastfile) and
tlsdll.dll are compiled into the exe. The launcher HTML/JS, Lua UI scripts and
GSC scripts are loose files in repo `data/`. CI uploads them to Cloudflare R2,
and the client's updater mirrors them into %LOCALAPPDATA%/boiii/data. The local
scripts and CI generate different solutions: generate.bat/build.bat use `vs2022`
and build/boiii.sln, while CI uses VS 2026, `premake5 vs2026` and
build/boiii.slnx. Upstream beta has already moved to premake 5.0.0 + ninja +
LLVM 23. The build script has verified latent bugs. The 'version.h for tls.dll'
prebuild actually lands on zstd.vcxproj. Filters written as `'toolset: msc*'`
(with a space) never match. Debug mixes /MTd and /MDd.

CI has four workflows. Only check-formatting (stylua, clang-format 22 on src/,
prettier) runs on push/PR; nothing is built or tested on PRs. A tag push of `v*`
builds Release on windows-2025, then publishes boiii.exe + data/ to R2 with a
JSON manifest of [name, size, SHA-1]. It also creates a GitHub Release with
boiii.exe as the only asset, using the archived marvinpinto action. Beta is a
manual dispatch that publishes to R2 boiii/beta/ + boiii-beta.json. BuildNew is
a manual Debug/Release matrix. Nothing is signed: no Authenticode, no signed
manifest, no checksum file. Two paths download and execute exes with no
verification at all: the dedicated-server beta exe and the launcher's version
picker, which pulls GitHub release assets. The updater's behaviour matters most
for Xerces. In Release builds it self-replaces the running exe with upstream's
boiii.exe whenever the hash differs. Its cleanup pass also deletes everything
under %LOCALAPPDATA%/boiii that is not in upstream's manifest; only `user` and
`data` survive, and within data/ any file not in the manifest is deleted too.
Xerces must own its update channel and app-data root before shipping anything.

There are no unit tests, test projects or test frameworks on main or on upstream
beta. The only automated correctness checks are compile-time struct-layout
assertions (about 481 ASSERT_SIZE and 157 ASSERT_OFFSET uses, plus about 35
static_asserts). Versioning comes from `git describe --tags --always` via
`premake5 generate-buildinfo`. That writes build/src/version.h and version.hpp:
tag numbers give VERSION_PRODUCT (3.0.0) and the git rev count is appended for
VERSION_FILE (3.0.0.2666). Tags are lightweight vX.Y.Z (51 tags). A stray tag
named `main` and the fork's v0.0.x-alpha tags can confuse describe.

LICENSE is the verbatim GPL-3.0 text, committed by the original author momo5502
in 2022. There is no 'or later' grant, no per-file headers, and README has no
license section. Most dependencies are permissive and GPL-3.0-compatible.
Problem items: src/common/str.hpp carries Linux-kernel code tagged SPDX GPL-2.0,
which the FSF considers incompatible with GPLv3. src/tlsdll/tlssup.cpp is
Microsoft CRT source. The Steamworks SDK mirror (v1.37 headers) has no license.
WebView2 is linked as a binary-only static lib. Activision/Treyarch data is
embedded or shipped. Upstream ships no third-party notices, no GPL 'Appropriate
Legal Notices' in the UI, and no credit to the original author. Upstream beta
and mod-stack add AGPL-3.0 sqlite_orm. Among upscalers, FSR1 and FSR2 are MIT;
DLSS, XeSS and the FidelityFX SDK 2.x/FSR4 binaries are proprietary and
binary-only, which raises open GPL linking questions.

For Xerces, new projects must be declared before group('Dependencies'). Headless
code must avoid dependencies.imports(), the client PCH, and game/ headers,
because game/symbol.hpp routes through the fenced arxan::detail proxy. Tests
belong under src/ but outside src/client, since the client globs every .cpp
there. Dev-only code needs new option names: DEV_BUILD/NO_CHECK feed only the
fenced ownership gate. Fenced areas: arxan.cpp/integrity.hpp, `steam_proxy.*`,
`src/client/steam/**`, dump.cpp, the arxan::detail proxy, the
--dev-build/--no-check options, and auth/licensing-adjacent structs
(classification pending).

## Facts

- **verified**: The build is premake5-generated: workspace 'boiii' located in
  ./build, objdir build/obj, targetdir build/bin/<platform>/<config>,
  startproject client. (premake5.lua:260-269)
- **verified**: Toolset is msc-clangcl on Windows (generated VS PlatformToolset
  'clangcl') and clang elsewhere; language standard C++20 (generated
  LanguageStandard stdcpp20). (premake5.lua:261-265,273-274;
  build/client.vcxproj:25,31,72,110)
- **verified**: Global settings: configurations Debug/Release, platform x64
  only, staticruntime On, symbols On (PDB in Release too), warnings Extra,
  characterset ASCII, incrementallink Off, multiprocessorcompile On.
  (premake5.lua:271-301)
- **verified**: Release optimizes for size with /LTCG (`msc*`) or full LTO/ICF
  (clang), and compiles for baseline x86-64 with SSE4.1/SSE4.2 code generation
  disabled. (premake5.lua:307-362)
- **verified**: First-party projects: common (StaticLib from `src/common/**`),
  client (WindowedApp, targetname boiii, PCH std_include.hpp, dependson tlsdll,
  linker base 0x170000000) and tlsdll (SharedLib, symbols/exceptions/CRT/LTO
  off). (premake5.lua:407-543)
- **verified**: Thirteen dependency StaticLib projects are generated: asmjit,
  brotli, curl, discord-rpc, hksc, imgui, libtomcrypt, libtommath, minhook,
  minizip (from zlib/contrib), udis86, zlib, zstd. Header-only deps: argparse,
  frozen, gtl, rapidjson, stb, SteamworkSDK public headers, and concurrentqueue
  (no premake script; resolved via the client's ./deps include dir). WebView2
  contributes headers plus a prebuilt static lib. (`build/*.vcxproj` listing;
  `deps/premake/*.lua`; premake5.lua:454; src/client/game/log.cpp:6)
- **verified**: dependencies.imports() is called for both common and client,
  injecting every dependency's includes, defines and links. common.lib's Lib
  step lists crypt32.lib and WebView2LoaderStatic.lib as AdditionalDependencies,
  so they are merged into common.lib. (premake5.lua:53-59,429,488;
  build/common.vcxproj:76-78,108-110)
- **verified**: The trailing 'create version.h for tls.dll' prebuild block runs
  after dependencies.projects(), so it attaches to the last dependency project
  (zstd), not tlsdll. (premake5.lua:545-559; grep generate-buildinfo hits only
  build/client.vcxproj and build/zstd.vcxproj; build/tlsdll.vcxproj has no
  PreBuildEvent)
- **verified**: Filters written as `'toolset: msc*'` (with a space) never match,
  so their -Qunused-arguments/-Wno-dangling-else options are silently dropped.
  (premake5.lua:411,435,500; -Qunused-arguments occurs 0 times in
  build/common.vcxproj, client.vcxproj, tlsdll.vcxproj)
- **verified**: Debug mixes CRT flags: staticruntime On emits RuntimeLibrary
  MultiThreadedDebug while buildoptions add /MDd and linkoptions add
  /NODEFAULTLIB:libcmt.lib /NODEFAULTLIB:libucrt.lib -l libcmtd.lib /MTd.
  (premake5.lua:281,368-385; build/client.vcxproj:69-71,84)
- **verified**: Premake options: --copy-to (post-build copy of boiii.exe),
  --dev-build (defines DEV_BUILD), --no-check (defines NO_CHECK); the CI
  environment variable defines CI. DEV_BUILD and NO_CHECK are consumed only at
  steam_proxy.cpp:315 (the fenced ownership gate).
  (premake5.lua:69-83,286-296,484-486; grep DEV_BUILD/NO_CHECK in src)
- **verified**: generate.bat syncs and initializes submodules recursively and
  runs `premake5 %* vs2022`. It aborts if premake5 is not on PATH, contradicting
  README's claim that it downloads a pinned premake. (generate.bat:5-17;
  README.md:414-415)
- **verified**: build.bat calls generate.bat, then builds build/boiii.sln
  Release|x64 with MSBuild located through vswhere, and pauses at the end.
  (build.bat:6-19,32)
- **verified**: CI (main) builds on windows-2025 with VS 2026 (setup-msbuild
  vs-version '[18.0,)'), 'premake5 vs2026' via Jarod42/install-premake5@v7 with
  no premake version pin, and 'msbuild /p:PlatformToolset=ClangCL
  build/boiii.slnx'. (build-and-publish.yml:33-70;
  build-and-publish-beta.yml:26-55; build-new.yml:13-57)
- **verified**: The local machine has premake5 5.0.0-beta8 (WinLibs) and VS
  Build Tools 2022 17.14.37516; the in-progress local build generated
  build/boiii.sln (vs2022). (premake5 --version; vswhere -latest; ls build/)
- **verified**: Resources compiled into boiii.exe: icon, splash.jpg, six
  Demonware emulation blobs (including the fastfile core_ffotd_tu32_593.ff) and
  tlsdll.dll read from ../../build/bin/x64/<cfg>/. At runtime tlsdll is
  extracted to %TEMP%/boiii-tlsdll.dll. (src/client/resource.r<local scratch
  copy, not kept>94-108; src/client/resource.hpp:3-13;
  src/client/loader/tls.cpp:12; src/common/utils/binary_resource.cpp:9-41)
- **verified**: Launcher HTML/CSS/JS, Lua UI scripts (27 .lua), GSC scripts (140
  .gsh + 4 .gsc), lookup tables and gamesettings are not embedded. They are 179
  loose files in repo data/, uploaded to R2 by CI and mirrored by the updater
  into %LOCALAPPDATA%/boiii/data. The launcher opens
  file:///.../data/launcher/main.html in WebView2, with a legacy IE fallback.
  (data/ (find: 179 files); src/client/game/game.cpp:30-42;
  launcher.cpp:2957-2959,2976-2980; html_frame.cpp:358-375;
  ui_scripting.cpp:1038-1039,1093-1094; gsc_compiler.cpp:57;
  script.cpp:806,1240-1243)
- **verified**: Lua source in mods/overrides is compiled to bytecode at runtime
  by the hksc static lib. (deps/premake/hksc.lua:18-41;
  src/client/component/script.cpp:27,940-957; docs/asset-override.md:10-13)
- **verified**: Only check-formatting.yml runs on push/pull_request.
  build-and-publish runs on tags `v*` and manual dispatch;
  build-and-publish-beta and build-new are manual only. No workflow builds or
  tests on PRs. (check-formatting.yml:3; build-and-publish.yml:3-13;
  build-and-publish-beta.yml:3-9; build-new.yml:3-4)
- **verified**: Releases are published to a Cloudflare R2 bucket via rclone
  (boiii/ + boiii.json; beta boiii/beta/ + boiii-beta.json), served at
  https://r2.ezz.lol/. A GitHub Release is created via
  marvinpinto/action-automatic-releases@latest with secrets.TOKEN, attaching
  only boiii.exe. (build-and-publish.yml:19-28,129-181;
  build-and-publish-beta.yml:15-21,112-137; file_updater.cpp:17-25)
- **verified**: Update integrity is a JSON array of [name, size, uppercase
  SHA-1] that the client compares against. There is no signature, no
  Authenticode signing, and no checksum file on GitHub Releases.
  (build-and-publish.yml:101-127; file_updater.cpp:63-89,108-110; no
  sign/checksum/attest step in any workflow (grep))
- **verified**: The latest upstream release, v3.0.0, has a single asset
  boiii.exe (6,157,824 bytes, uploader Skwll) with a GitHub-computed digest
  sha256:e6c49cc3ecb145a05f92bd1051ebcafd9b201cf8c522b26308cce18156491a62. No
  BOIII-Full.zip is produced, although README:61 lists it. (gh api
  repos/Ezz-lol/boiii-free/releases/latest; README.md:61)
- **verified**: Beta channel: the 'selectedVersion' property set to beta (or the
  -beta flag on a dedicated server) switches to boiii-beta.json and boiii/beta/.
  On a dedicated server, -beta downloads https://r2.ezz.lol/boiii/beta/boiii.exe
  to <game>/versions/boiii-beta.exe and runs it without any hash or signature
  check. (file_updater.cpp:31-61; main.cpp:924-948 (URL at 935))
- **verified**: The launcher version picker lists Ezz-lol/boiii-free GitHub
  releases and downloads each release's boiii.exe asset to
  <game>/versions/boiii-<tag>.exe, which is then executed without verification.
  (data/launcher/main.js:4466,4491,4499,4527-4528;
  src/client/launcher/launcher.cpp:1315-1354)
- **verified**: In Release builds the updater replaces the running host exe when
  its SHA-1 differs from the manifest entry 'boiii.exe'. Debug skips this unless
  -update is passed; it is also skipped when a non-latest version is selected.
  (file_updater.cpp:25,497-509,536-543; component/updater.cpp:40-56)
- **verified**: Updater cleanup deletes every entry in %LOCALAPPDATA%/boiii that
  is not in the manifest, except the 'user' and 'data' directories (directories
  included because list_files defaults to includeDirectories=true). It also
  deletes every file under data/ not in the manifest. This wipes the appdata
  plugins folder that plugins.cpp loads from. (file_updater.cpp:189-201,637-712;
  src/common/utils/io.hpp:35-36; plugins.cpp:34)
- **verified**: There are no unit tests, test projects or test frameworks on
  upstream main or upstream beta, and CI runs no tests. (find/grep for test dirs
  and gtest/catch/doctest outside deps; git ls-tree upstream/beta; workflows)
- **verified**: The only automated correctness checks are compile-time ones:
  ASSERT_SIZE (481 uses), ASSERT_OFFSET (157), ASSERT_CPP03_POD (27),
  ASSERT_PACKED (5) and ASSERT_ALIGNMENT (2), plus 35 static_asserts.
  (src/client/game/structs/macros.hpp:57-140; grep counts)
- **verified**: Formatting CI runs 'stylua --check .' (stylua version latest),
  the clang-format check on src via jidicula/clang-format-action@v4.18.0
  (default clang-format 22), and prettier 3.9.6 via 'yarn check'. Ignore files
  exclude deps/ (and build/). (check-formatting.yml:13-34; jidicula
  action.yml@v4.18.0 default '22'; package.json:5-11; .clang-format-ignore;
  .styluaignore; .prettierignore)
- **verified**: pre-commit hook bug: prettier files are assigned with '='
  instead of '+=', and are not counted in num_files_to_format, so commits that
  stage only prettier files are never formatted. (hooks/pre-commit:239,248)
- **verified**: scripts/win/format.ps1 contains mangled commands ('cmd / c',
  'clang-format -i --style = file') that cannot work as written.
  (scripts/win/format.ps1:24,106,121)
- **verified**: Versioning: GIT_DESCRIBE = 'git describe --tags --always'.
  VERSION_PRODUCT is the tag's numbers (3.0.0); VERSION_FILE is the tag plus
  'git rev-list --count HEAD' (3.0.0.2666). These are written to
  build/src/version.h and version.hpp, and the commit hash goes to
  build/version.txt (consumed by nothing). (premake5.lua:1,20-37,114-256;
  build/src/version.h; grep version.txt (no consumers))
- **verified**: version.h is regenerated only when GIT_DESCRIBE changes, so
  GIT_DIRTY and GIT_BRANCH can go stale. The #undef list omits VERSION_FILE_RC
  and SHORTVERSION. (premake5.lua:138-140,192-202)
- **verified**: Tags are lightweight vX.Y.Z (51 total, v1.0.0 2023-12-07 to
  v3.0.0 2026-09-27 at HEAD 5aa7fac). A lightweight tag named 'main' (7cd24652)
  is an ancestor of HEAD. The fork tags v0.0.1-alpha and v0.0.2-alpha exist, and
  the fork branch describes as v0.0.2-alpha. (git for-each-ref refs/tags; git
  describe on HEAD and refs/heads/main; git merge-base --is-ancestor)
- **verified**: Where version strings surface: getinfo 'shortversion' (server
  info), console and launcher titles 'EZZ BOIII V' SHORTVERSION, the dedicated
  title 'BOIII V%s', the branding watermark 'EZZ: ' VERSION, and the crash
  report 'Version:'. (getinfo.cpp:152; launcher.cpp:1283,1366,1478;
  console.cpp:1328-1329,1582; dedicated/info.cpp:40; branding.cpp:36;
  exception.cpp:486)
- **verified**: LICENSE is the verbatim GPL-3.0 text (674 lines, 35,149 bytes
  LF-normalized, md5 1ebbd3e34237af26da5dc08a4e440464), committed by momo5502 on
  2022-05-21 (16a5d98e). There is no 'or any later version' grant or per-file
  license header anywhere, and README has no license section. (LICENSE; git log
  -- LICENSE; grep SPDX/GPL in src; grep license in README.md)
- **verified**: Copyright holders are many: 2,666 commits, including
  momo5502/Maurice Heumann 773, Adam Killam 864, Skwll 279, XFrozedyX 157 and
  dependabot 157. README credits and the launcher credits popup do not name the
  original author. (git shortlog -sn HEAD; README.md:441-452;
  data/launcher/main.html:3264-3331)
- **verified**: VERSIONINFO declares CompanyName 'Ezz', ProductName 'BOIII' and
  LegalCopyright 'Copyright (C) 2026 Ezz. All rights reserved.'; FILETYPE is
  VFT_DLL even though the file is an exe. The tlsdll resource says 'All rights
  reserved.' (src/client/resource.r<local scratch copy, not kept>65-79;
  src/tlsdll/resource.r<local scratch copy, not kept>63-69)
- **verified**: src/common/str.hpp contains a strscpy family copied from
  linux/lib/string.c and tagged 'SPDX-License-Identifier: GPL-2.0' (GPL-2.0-only
  in SPDX terms). It was introduced 2026-06-30 and is used in about 10 files.
  (src/common/str.hpp:12-22 (104-line file); git log e81ec4a4; grep strscpy
  users)
- **verified**: src/tlsdll/tlssup.cpp is MSVC CRT source ('Copyright (c)
  Microsoft Corporation. All rights reserved.'). src/client/launcher/xxhash.h is
  BSD-2-Clause and src/common/str.cpp is ISC (OpenBSD strlcpy).
  (src/tlsdll/tlssup.cpp:1-8; src/client/launcher/xxhash.h:6;
  src/common/str.cpp:1-17)
- **verified**: No third-party license notices or GPL legal or warranty notices
  are shipped in data/ or shown in the launcher UI. (grep for dependency author
  names, 'third party', 'license', 'warranty' in data/ and
  data/launcher/main.html returned nothing relevant)
- **verified**: deps/SteamworkSDK is the rlabrecque/SteamworksSDK mirror pinned
  at SDK v1.37 (2016). The mirror has no license (GitHub license API 404); only
  public/ headers are on the include path; dependabot deliberately excludes it.
  (deps/premake/SteamworkSDK.lua:5-13; deps/SteamworkSDK/Readme.txt:3,12;
  .github/dependabot.yml:8-11; gh api)
- **verified**: Microsoft.Web.WebView2 1.0.4191.47 is vendored in-tree (not a
  submodule) under a BSD-3-Clause LICENSE.txt with a NOTICE.txt. The build links
  the prebuilt 10.7 MB WebView2LoaderStatic.lib. (git log 097557ba;
  deps/Microsoft.Web.WebView2/LICENSE.txt, NOTICE.txt;
  deps/premake/Microsoft.Web.WebView2.lua:5-12)
- **likely**: curl is pinned to tag/branch curl-8_4_0 (October 2023) and built
  with Schannel, zstd, brotli and zlib. Because .gitmodules tracks that branch,
  dependabot will not move it to newer curl releases. (.gitmodules:32-35; git
  submodule status (curl-8_4_0); deps/premake/curl.lua:27-48,75-81)
- **verified**: Upstream beta (f2fdc83d, 2026-11-03, 464 commits ahead of main)
  switches CI to LLVM 23, premake 5.0.0 and ninja. It changes 804 lines of
  premake5.lua, adds premake/module/{ninja,llvm,gcc}.lua, Dockerfile.dev,
  .clang-tidy and 6 submodules, including AGPL-3.0 sqlite_orm (used by
  local_db.cpp). (git diff --stat upstream/main upstream/beta; git show
  upstream/beta:.gitmodules; gh api repos/fnc12/sqlite_orm/license; git grep
  sqlite_orm upstream/beta)
- **verified**: The upstream mod-stack branch (ac356c3c, 2026-10-15, 208 commits
  ahead, not merged into beta) adds mod stacking: mod_stack.cpp (832 lines), the
  fs_extramods dvar, a Mods-menu stack and host-synced mod order. It also
  carries local_db/sqlite_orm. (git show --stat c6676a31; git show
  upstream/mod-stack:docs/mod-stack.md; git merge-base --is-ancestor)
- **verified**: Upstream mostly uses hardcoded per-build addresses: about 767
  symbol definitions with hex literals (client/legacy/server triples) versus
  about 8 signature uses. The game build is identified by a whitelist of PE
  CheckSums (new 0x6531394, legacy 0x888C368, server 0x14C28B4). The installed
  BlackOps3.exe has CheckSum 0x6531394 and TimeDateStamp 2026-08-11.
  (src/client/game/symbol.hpp:12-56; src/client/game/ptr.hpp:62-84;
  src/client/game/base.cpp:36-49; regex counts; PE header read of the installed
  exe)
- **verified**: Plugin ABI: DLLs are loaded from %LOCALAPPDATA%/boiii/plugins
  and <game>/boiii/plugins, with optional exports p_name, post_load, post_unpack
  and pre_destroy. -noplugins disables loading on the client (not on dedicated
  servers). The '-plugins' flag is declared but never read.
  (src/client/component/plugins.cpp:21-142; src/common/utils/flags.cpp:135,174)
- **verified**: Components register through static initializers
  (REGISTER_COMPONENT). Every .cpp under src/client is compiled into boiii.exe
  because of the files glob. (src/client/loader/component_loader.hpp:26-40;
  premake5.lua:447-452)
- **verified**: The DeroXP fork (refs/heads/main) changed no build or CI files
  versus its base d80d308. It added `tools/*.py` (Python with capstone) and
  disabled the updater in e5968d07, and it published GitHub releases
  v0.0.1-alpha and v0.0.2-alpha with boiii.exe and `bo3-bundle-boiii-*.zip`
  assets. (git diff --stat d80d308 refs/heads/main -- premake5.lua deps/premake
  .github ...; git show e5968d07; gh api repos/DeroXP/boiii-free/releases)
- **verified**: Upscaler SDK licenses as of 2026-11-04: FidelityFX-FSR (FSR1)
  MIT; FidelityFX-FSR2 MIT; NVIDIA Streamline core MIT-style; NVIDIA DLSS under
  the 'NVIDIA RTX SDKs LICENSE' (proprietary, non-sublicensable, object-code
  distribution); Intel XeSS under the 'Intel Simplified Software License'
  (binary only, no modification or reverse engineering); FidelityFX SDK 2.x
  prebuilt components (FSR 3.1/4) under an AMD binary-only license with no
  reverse engineering (samples MIT). (gh api license and contents of
  GPUOpen-Effects/FidelityFX-FSR, FidelityFX-FSR2, NVIDIAGameWorks/Streamline,
  NVIDIA/DLSS, intel/xess, GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
  docs/license.md)
- **verified**: Building blocks for signed updates already exist: the
  libtomcrypt submodule has Ed25519 sign/verify with LTC_CURVE25519 enabled, and
  utils::cryptography wraps sha256/sha512. (`deps/libtomcrypt/src/pk/ed25519/*`;
  deps/libtomcrypt/src/headers/tomcrypt_custom.h:451;
  src/common/utils/cryptography.hpp:369-385)
- **verified**: CI depends on archived or unpinned third parties:
  marvinpinto/action-automatic-releases is archived;
  ammaraskar/msvc-problem-matcher is pinned to @master; stylua uses 'version:
  latest'; rclone-current .deb is downloaded without a checksum.
  (build-and-publish.yml:66-67,129-132,174; check-formatting.yml:17; gh api
  repos/marvinpinto/action-automatic-releases (archived=true))
- **likely**: GitHub does not evaluate path filters for tag pushes, so
  paths-ignore in build-and-publish.yml has no effect. GitHub's auto-generated
  tag source archives do not include submodule contents.
  (build-and-publish.yml:5-13; GitHub Actions/Releases platform behaviour (not
  re-verified in this session))

## Details

# Upstream build system, CI, releases, licensing and tests (main @5aa7fac = v3.0.0)

Snapshot. The working tree is on branch `xerces` = upstream main 5aa7fac
(lightweight tag v3.0.0, 2026-09-27, 2,666 commits). `build/` was generated by
the background build using premake5 5.0.0-beta8 (`vs2022`, so
`build/boiii.sln`); the local toolchain is VS Build Tools 2022 17.14.37516. The
installed BlackOps3.exe has PE CheckSum 0x6531394, which is upstream's
`is_new_client()` (src/client/game/base.cpp:36-38), and TimeDateStamp
2026-08-11.

---

## (a) Projects, toolchain and outputs

### Workspace-wide settings (premake5.lua)

| Setting            | Value                                                                                                                                                                                          | Lines        |
| ------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------ |
| workspace / dirs   | `boiii`; location `./build`; objdir `build/obj`; targetdir `build/bin/%{cfg.platform}/%{cfg.buildcfg}`                                                                                         | 260-269      |
| toolset            | `msc-clangcl` on Windows (VS `clangcl`, needs the "C++ Clang tools for Windows" VS component); `clang` elsewhere                                                                               | 261-265      |
| configs / platform | Debug, Release / x64 (`architecture x86_64`)                                                                                                                                                   | 271, 276-277 |
| language           | C++; `cppdialect C++20` (generated `stdcpp20`); dependencies are C                                                                                                                             | 273-274      |
| runtime / misc     | `staticruntime On`, `symbols On`, `warnings Extra`, `characterset ASCII`, `systemversion latest`, incremental link off, MP compile on                                                          | 279-301      |
| global defines     | `_WINDOWS;WIN32` (x64); `DEV_BUILD` (--dev-build); `NO_CHECK` (--no-check); `CI` (env CI)                                                                                                      | 286-305      |
| Release            | `optimize Size`, `NDEBUG`; ms<local scratch copy, not kept>-Wno-*`, `/clang:-march=x86-64 -mno-sse4.1 -mno-sse4.2`, link `/LTCG /IGNORE:4702`; clang: `-flto=full`, whole-program vtables, ICF | 307-362      |
| Debug              | `optimize Debug`, `DEBUG;_DEBUG;_CRT_DEBUG`; ms<local scratch copy, not kept>k `/NODEFAULTLIB:libcmt.lib /NODEFAULTLIB:libucrt.lib -l libcmtd.lib /MTd` (mixes CRT flavours)                   | 364-405      |
| options/actions    | `--copy-to=PATH` (client post-build copy), `--dev-build`, `--no-check`; actions `premake5 version`, `premake5 generate-buildinfo`                                                              | 69-256       |

### Projects

| Project            | Kind           | Output                                                                                                        | Key facts                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| ------------------ | -------------- | ------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| common             | StaticLib      | `build/bin/x64/<cfg>/common.lib`                                                                              | `src/common/**`. Calls `dependencies.imports()` (429), which pulls every dependency's includes, defines and links. The generated `<Lib>` merges `crypt32.lib;WebView2LoaderStatic.lib` (build/common.vcxproj:77,109). Includes no game headers.                                                                                                                                                                                                                                                        |
| client             | WindowedApp    | `boiii.exe` + `boiii.pdb`                                                                                     | `targetname boiii` (442). PCH `std_include.hpp`/`std_include.cpp` (444-445). Files `src/client/**.rc/.hpp/.cpp` + `resources/**` (447-452). `dependson tlsdll` (464). Links common and all dependencies (466, 488). Linker `/base:0x170000000` (489-494) keeps the loader clear of the game image it maps (loader.cpp:173-184; main.cpp:535-541; std_include.hpp:3,6). Prebuild `premake5 generate-buildinfo` when version headers are missing at generate time (468-482; build/client.vcxproj:88-92). |
| tlsdll             | SharedLib      | `tlsdll.dll`                                                                                                  | `src/tlsdll/**`; symbols/exceptions/runtime checks off; `/NODEFAULTLIB`, `/Z<local scratch copy, not kept>hreadSafeInit-`; LTO removed (496-543). Embedded into boiii.exe and extracted to `%TEMP%/boiii-tlsdll.dll` (tls.cpp:12; binary_resource.cpp:9-41).                                                                                                                                                                                                                                           |
| Dependencies group | 13 × StaticLib | asmjit, brotli, curl, discord-rpc, hksc, imgui, libtomcrypt, libtommath, minhook, minizip, udis86, zlib, zstd | Created by `dependencies.projects()` (545-546) from `deps/premake/*.lua`, which are auto-included (43-51, 258).                                                                                                                                                                                                                                                                                                                                                                                        |

Header-only or prebuilt dependencies: argparse, frozen, gtl, rapidjson
(`RAPIDJSON_HAS_STDSTRING`), stb, SteamworkSDK (`public/` only), WebView2
(headers + `WebView2LoaderStatic.lib`), and concurrentqueue (no premake file;
included as `concurrentqueue/...` via the client's `./deps` include dir,
log.cpp:6).

### Resources: what is embedded and what ships as loose data

- Embedded through `src/client/resource.rc`:
  - VERSIONINFO (48-87).
  - `ID_ICON` icon.ico and `IMAGE_SPLASH` splash.jpg (94-95).
  - Demonware emulation blobs (97-102): `experiments_tu32.gz`,
    `featured_cards-english_tu32.gz`, `playlists_tu32.gz`,
    `core_ffotd_tu32_593.ff` (an Activision fastfile), `keys.txt`,
    `qosconfig4.csv`.
  - `TLS_DLL` = `../../build/bin/x64/{Debug|Release}/tlsdll.dll`. This hardcodes
    the output path (104-108).
- Not embedded: repo `data/` (179 files):
  - `launcher/` (main.html 130 KB, main.js 154 KB, main.css, bigboiii.jpg 1.2
    MB, verification.json)
  - `ui_scripts/` (27 .lua)
  - `scripts/` (140 .gsh, 4 .gsc)
  - `lookup_tables/`, `gamesettings/`
- How the data reaches users and gets read:
  - CI uploads `data/` to R2. The updater writes it to
    `%LOCALAPPDATA%/boiii/data` (game.cpp:30-42; file_updater.cpp:536-543).
  - The launcher loads `file:///…/data/launcher/main.html`
    (launcher.cpp:2957-2959, 2976-2980) in WebView2, with its user-data dir at
    `%LOCALAPPDATA%/EZZ BOIII/WebView2` (html_frame.cpp:49-56) and an IE/MSHTML
    fallback (html_frame.cpp:358-375).
  - Lua and GSC are read from the same tree, with `<game>/boiii/...` fallbacks
    (ui_scripting.cpp:1038-1039, 1093-1094; script.cpp:806, 1057, 1240-1243;
    gsc_compiler.cpp:57).
  - Lua source is compiled at runtime by hksc (script.cpp:27, 940-957).
- Dev consequence: a local build has no data unless `data/` is copied into app
  data, and the upstream updater will overwrite or delete it (see (b)).

### Verified premake pitfalls

1. **Scope leak.** Anything after `dependencies.projects()` applies to the last
   dependency project. The "version.h for tls.dll" prebuild (548-559) is emitted
   into `build/zstd.vcxproj`; `tlsdll.vcxproj` has no PreBuildEvent.
2. **Dead filters.** `filter({ "toolset: msc*" })` (411, 435, 500) never
   matches; `-Qunused-arguments` is absent from every generated vcxproj.
3. **CRT flag mix in Debug.** RuntimeLibrary `MultiThreadedDebug` together with
   `/MDd` and `/MTd -l libcmtd.lib` (build/client.vcxproj:69-71, 84).
4. **Incomplete `#undef` list.** It omits `VERSION_FILE_RC` and `SHORTVERSION`
   (192-202).
5. **VS-only macros.** `resincludedirs({"$(ProjectDir)src"})` (427/462/543) only
   works with VS generators; beta replaced it for ninja.
6. **Inconsistent rapidjson config.** `RAPIDJSON_ASSERT` throws only in the
   client PCH (std_include.hpp:116-121); `common` translation units use the
   default assert.

### Local scripts vs CI

|           | Local (generate.bat/build.bat)                          | CI on main                                            | Upstream beta CI                   |
| --------- | ------------------------------------------------------- | ----------------------------------------------------- | ---------------------------------- |
| Generator | `premake5 %* vs2022` (generate.bat:17)                  | `premake5 vs2026` (install-premake5@v7, unpinned)     | premake `v5.0.0`, `premake5 ninja` |
| Build     | MSBuild `build\boiii.sln` Release x64 (build.bat:12-19) | `msbuild /p:PlatformToolset=ClangCL build/boiii.slnx` | `ninja Release_x64 -Cbuild`        |
| Compiler  | VS clang-cl                                             | VS 2026 (`vs-version [18.0,)`), windows-2025          | LLVM 23 (ZhongRuoyu/setup-llvm@v0) |

Cross-compiling: `scripts/unix/cross.sh` runs
`premake5 --os=windows --cc=clang ... gmake` (454-475) against an MSVC sysroot
(`WINDOWS_MSVC_SYSROOT`, 628-689) and builds `boiii.exe` plus `tlsdll.dll`
(785-796). It also supports `--tidy` (clang-tidy) and `--exec`.

---

## (b) CI, releases and channels

| Workflow                   | Trigger                                                                          | What it does                                                                                                                                                                                                                                                         | Artifacts                             | Publishes to         |
| -------------------------- | -------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------- | -------------------- |
| check-formatting.yml       | `push`, `pull_request`                                                           | `stylua --check .` (stylua `latest`); `jidicula/clang-format-action@v4.18.0` on `src` (default clang-format 22); `yarn install --frozen-lockfile && yarn check` (prettier 3.9.6)                                                                                     | none                                  | none                 |
| build-and-publish.yml      | tags `v*` + manual (`paths-ignore` lines 10-13 are ineffective for tags, likely) | build (windows-2025, Release) → publish (ubuntu: SHA-1 manifest, rclone to R2 `boiii/` + `boiii.json`, forced `application/octet-stream`) → create_release (marvinpinto@latest, `secrets.TOKEN`, tag = `GITHUB_REF_NAME`, `prerelease: false`, asset boiii.exe only) | `build-artifacts` (boiii.exe + data/) | R2 + GitHub Releases |
| build-and-publish-beta.yml | manual, input `branch` (default `beta`)                                          | Same build; publishes R2 `boiii/beta/` + `boiii-beta.json`; concurrency `beta-build`                                                                                                                                                                                 | `beta-build-artifacts`                | R2 only              |
| build-new.yml              | manual                                                                           | Matrix Debug/Release build; uploads Release `boiii.pdb`, `boiii.exe`, `data/*`; create_release with tag = `git describe --tags --abbrev=0` (the latest existing tag, so a manual run likely recreates and overwrites that release)                                   | Release Symbols / Binary / data       | GitHub Releases      |

- **Secrets:** `R2_BUCKET`, `R2_ACCOUNT_ID`, `R2_ACCESS_KEY_ID`,
  `R2_SECRET_ACCESS_KEY`, `TOKEN` (a personal access token; the v3.0.0 asset
  uploader is Skwll).
- **Manifest format:** a JSON array
  `[["boiii.exe", size, "SHA1"], ["data/...", size, "SHA1"], ...]` built with
  shell `echo` (build-and-publish.yml:101-127). The workflow comment couples
  `R2_FOLDER`/`MANIFEST_FILE` to the `UPDATE_*` macros in file_updater.cpp
  (24-28).
- **Client side:**
  - Endpoints: `UPDATE_SERVER https://r2.ezz.lol/`, `boiii.json`, `boiii/`,
    `boiii-beta.json`, `boiii/beta/`, `UPDATE_HOST_BINARY "boiii.exe"`
    (file_updater.cpp:17-25).
  - When updates run: always, unless `-noupdate` is passed or the launcher's
    `autoUpdate` setting is false; `-update`/`-beta` force them
    (component/updater.cpp:14-56).
  - Hashing: SHA-1 compare (108-110, 497-534).
  - Host exe: replaced in Release only; Debug requires `-update` (497-509).
  - Cleanup: deletes non-manifest entries in the appdata root (keeps only `user`
    and `data`) and non-manifest files in `data/` (189-201, 637-712).
- **Checksums/signatures:** only the SHA-1 manifest. No Authenticode, no signed
  manifest, no SHA256SUMS, no attestations. GitHub computes a sha256 digest per
  release asset (v3.0.0 boiii.exe `sha256:e6c49cc3…91a62`, 6,157,824 bytes), but
  nothing consumes it.
- **Beta channel:**
  - Launcher users pick "Beta (Experimental)", which stores
    `selectedVersion=beta` and switches manifest and folder
    (file_updater.cpp:31-61).
  - Dedicated servers with `-beta` download
    `https://r2.ezz.lol/boiii/beta/boiii.exe` to
    `<game>/versions/boiii-beta.exe` and relaunch it with no hash check
    (main.cpp:924-948).
- **Old-version picker:** main.js:4466 queries
  `api.github.com/repos/Ezz-lol/boiii-free/releases`, maps each release's
  `boiii.exe` asset to `versions/boiii-<tag>.exe` (4491-4499), and
  launcher.cpp:1315-1354 downloads and runs it unverified.
- **Dependabot:** `gitsubmodule` monthly with a limit of 10 PRs; excludes
  `deps/SteamworkSDK` (engine-matched API). There is no github-actions or npm
  ecosystem entry.
- **Fork releases:** DeroXP/boiii-free published v0.0.1-alpha and v0.0.2-alpha
  manually (boiii.exe + `bo3-bundle-boiii-*.zip`); the fork has no CI.

---

## (c) Tests and formatting

- **Tests:** none. There are no test projects, no frameworks (gtest, Catch2,
  doctest), no test scripts and no CI test step, on main or on upstream beta.
- **Compile-time checks:**
  - Layout macros in `src/client/game/structs/macros.hpp:57-140`: `ASSERT_SIZE`
    ×481, `ASSERT_OFFSET` ×157, `ASSERT_CPP03_POD` ×27, `ASSERT_PACKED` ×5,
    `ASSERT_ALIGNMENT` ×2.
  - About 35 raw `static_assert`s.
  - `warnings("Extra")` with `FatalCompileWarnings` commented out (310).
- **Formatting configuration:**
  - `.clang-format`: 4,313 lines covering several languages; for Cpp it is
    LLVM-based, ColumnLimit 80, braces Attach, IndentWidth 2, SortIncludes
    CaseInsensitive, Standard Latest. `.editorconfig` says
    `brace_style = next_line`, which contradicts it.
  - `.stylua.toml` and `.prettierrc.json` (with the toml and powershell
    plugins).
  - Ignore files exclude `deps/` and `build/`.
- **Formatting tooling:**
  - `hooks/pre-commit` is installed by `scripts/unix/install-hooks.sh` or
    `scripts/win/install-hooks.ps1`, which copy it into .git/hooks.
  - `scripts/unix/format.sh` and `scripts/win/format.ps1` format the whole repo.
- **Known tooling bugs:** pre-commit lines 239 and 248 (prettier-only commits
  are skipped); format.ps1 lines 24, 106 and 121 are mangled.
- **Version drift:** CI formats with clang-format 22, while local VS-bundled
  clang-format may differ.

---

## (d) Versioning

- `GIT_VERSIONING_COMMAND = git describe --tags --always` (premake5.lua:1).
- `premake5 generate-buildinfo` (114-256) writes `build/src/version.h` with:
  - `GIT_DESCRIBE`, `GIT_DIRTY`, `GIT_HASH`, `GIT_TAG` (from
    `describe --abbrev=0`), `GIT_BRANCH` (CI fallback parses
    `git show -s --pretty=%d`).
  - `VERSION_PRODUCT(_RC)` = the tag's digits (3 parts).
  - `VERSION_FILE(_RC)` = the tag's digits plus `git rev-list --count HEAD`.
  - `VERSION` = `GIT_DESCRIBE`; `SHORTVERSION` = `VERSION_PRODUCT`.
- It also writes `build/src/version.hpp`, a wrapper the RC compiler can use, and
  `build/version.txt` (the commit hash; nothing consumes it).
- Current generated values: `"v3.0.0"`, dirty 0, hash 5aa7fac…, branch
  `"xerces"`, `3.0.0`, `3.0.0.2666`.
- Regeneration happens only when `GIT_DESCRIBE` changes (138-140).
- `premake5 version` prints describe, plus `-<branch>` for branches other than
  develop/master/main (85-112).
- The release version is the pushed tag name (build-and-publish.yml:59-64). Tags
  are lightweight (51; v1.0.0 2023-12-07 … v3.0.0 2026-09-27).
- Hazards:
  - A tag literally named `main` (7cd24652, an ancestor of HEAD).
  - The fork's `v0.0.x-alpha` tags; the fork describes as `v0.0.2-alpha`.
  - `vertonumarr` drops non-numeric suffixes, so a tag without digits (such as
    `main`) yields 0.0.0.
- Consumers:
  - Server info `shortversion` (getinfo.cpp:152), which may matter for interop.
  - UI and console titles (launcher.cpp:1283, 1366, 1478; console.cpp:1328-1329,
    1582).
  - Branding watermark (branding.cpp:36) and crash report (exception.cpp:486).
  - All of them use `#if __has_include("version.hpp")` with a fallback to "0".

---

## (e) License and attribution

- **LICENSE:** the unmodified GPL-3.0 text (LF-normalized md5 1ebbd3e3…0464),
  added by momo5502 on 2022-05-21 (16a5d98e).
  - No "or any later version" statement, no per-file headers, and no README
    license section. GitHub reports GPL-3.0.
- **Copyright holders:** all contributors. Top authors: Adam Killam 864,
  momo5502/Maurice Heumann 773, Skwll 279, XFrozedyX 157.
  - README credits (441-452) and the launcher "Credits" popup
    (main.html:3264-3331, which lists Skwll, Frozedy and amkillam and links to
    contributors) do not name the original author.
- **Obligations a rename must keep:**
  1. Keep LICENSE and the license and warranty notices. Give recipients a copy
     (§4, §5).
  2. Add prominent "modified by … on <date>" notices (§5(a)).
  3. Show "Appropriate Legal Notices" in interactive UIs (§5(d)): copyright, no
     warranty, and how to view the license. The launcher and console have none
     today.
  4. Provide Corresponding Source for every binary (§6), including build scripts
     and submodule sources. GitHub tag archives omit submodules (likely), so
     attach a full source tarball.
  5. Reproduce third-party notices for BSD, MIT, curl, Apache and Zlib code.
     Upstream ships none.
  6. Fix the VERSIONINFO copyright string ("Copyright (C) 2026 Ezz. All rights
     reserved.", resource.r<local scratch copy, not kept>77).
  7. Courtesy and good practice: credit momo5502 and upstream contributors.
- **Code already in-tree with issues:**
  - `src/common/str.hpp:12-22`: GPL-2.0 Linux code. GPL-2.0-only is incompatible
    with GPL-3.0 per the FSF; it is easy to rewrite (about 80 lines, about 10
    users).
  - `src/tlsdll/tlssup.cpp:1-8`: Microsoft CRT source.
  - Activision/Demonware blobs embedded in the exe, and Treyarch-style `.gsh`
    headers in `data/scripts`.
- **New Xerces modules:** anything linked into boiii.exe becomes part of the
  GPL-3.0 combined work, so it must be GPL-3.0-compatible. Xerces may license
  its own new files as GPL-3.0-or-later or as a GPL-compatible permissive
  license. It can grant §7 additional permissions only for code it owns, not for
  upstream's code.
- **Proprietary SDK linking (open questions, not conclusions):**
  - GPLv3 §1 excludes "System Libraries" (Major Component = OS kernel or window
    system, the compiler, and similar) from Corresponding Source. Candidates:
    the static MSVC CRT, D3D11/DXGI, perhaps the driver-provided NGX core.
  - DLSS (`nvngx_dlss.dll` or the Streamline DLSS plugin), XeSS (`libxess.dll`)
    and FidelityFX SDK 2.x DLLs are shipped by the application, are binary-only,
    and forbid reverse engineering. Whether loading them (statically, or as
    separate runtime DLLs behind a plugin boundary) creates a combined work with
    the GPL code is unresolved.
  - The same question applies to the WebView2 binary static lib and the
    proprietary Steamworks headers already in upstream.
  - FSR1 and FSR2 are MIT and can be compiled in.
  - If Xerces rebases onto beta or mod-stack: sqlite_orm is AGPL-3.0. GPLv3 §13
    allows the combination, but AGPL's network-interaction clause may then apply
    to modified dedicated servers.

---

## Dependency and license table

| Dependency                    | Pin (submodule describe)                        | Integration                                             | Used by                           | License (local file)                                                                                                | GPL-3.0 compatible?                 | Notes                                                                    |
| ----------------------------- | ----------------------------------------------- | ------------------------------------------------------- | --------------------------------- | ------------------------------------------------------------------------------------------------------------------- | ----------------------------------- | ------------------------------------------------------------------------ |
| minhook                       | 8af6b4a (v1.3.4-17)                             | StaticLib                                               | common/utils/hook.cpp, client PCH | BSD-2-Clause (+ HDE portions BSD-2), deps/minhook/LICENSE.txt                                                       | Yes                                 | Binary notice required                                                   |
| asmjit                        | dffd8b1                                         | StaticLib (`ASMJIT_STATIC`)                             | 6 client files                    | Zlib, LICENSE.md                                                                                                    | Yes                                 |                                                                          |
| zlib                          | d81c2d7 (v1.3.2-90, branch develop)             | StaticLib                                               | compression, curl                 | Zlib                                                                                                                | Yes                                 | Tracks develop                                                           |
| minizip                       | in zlib/contrib                                 | StaticLib (minizip.lua)                                 | steamcmd.cpp, scr/gdb.cpp         | Zlib                                                                                                                | Yes                                 |                                                                          |
| libtomcrypt                   | ecc3686 (v1.18.2-1020, develop)                 | StaticLib                                               | utils/cryptography                | Unlicense ("LibTom license")                                                                                        | Yes                                 | Has Ed25519 (`LTC_CURVE25519`)                                           |
| libtommath                    | ae40a87 (v1.3.0-418, develop)                   | StaticLib                                               | libtomcrypt                       | Unlicense                                                                                                           | Yes                                 |                                                                          |
| rapidjson                     | 24b5e7a (v1.1.0-763)                            | header-only                                             | 16 files                          | MIT (`include/`); msinttypes BSD-3; JSON License only in `bin/jsonchecker` (not compiled, but in the checkout)      | Yes as used                         | Exclude jsonchecker from any source bundle if desired                    |
| udis86                        | 56ff6c8 (v1.7.2-84)                             | StaticLib + `deps/extra/udis86` itab                    | exception.cpp, PCH                | BSD-2-Clause                                                                                                        | Yes                                 |                                                                          |
| discord-rpc                   | 382148c (v3.1.0-44), ariloc/discord-rpc-buttons | StaticLib                                               | discord.cpp                       | MIT (Discord 2017)                                                                                                  | Yes                                 | Fork of a deprecated library                                             |
| stb                           | 2c980bb                                         | header-only                                             | utils/image.cpp                   | MIT OR Unlicense                                                                                                    | Yes                                 |                                                                          |
| curl                          | d755a5f (curl-8_4_0)                            | StaticLib (Schannel, zstd/brotli/zlib)                  | utils/http, fastdl, steamcmd      | curl license (MIT-like); inet_ntop/pton ISC; krb5.c BSD-3; BSD-4-Clause-UC only in tests/server/tftpd.c (not built) | Yes                                 | Pinned to an Oct-2023 release, a security risk                           |
| imgui                         | 5a0ac16 (v1.62-5647)                            | StaticLib (dx11 + win32 backends)                       | download_overlay.cpp              | MIT                                                                                                                 | Yes                                 |                                                                          |
| SteamworkSDK                  | 20509c3 (SDK v1.37, 2016)                       | headers only (`public/`)                                | game/structs/steam.hpp etc.       | Proprietary Valve Steamworks SDK terms; mirror has no license (gh 404); Readme "All rights reserved"                | No / unclear                        | Restrictive redistribution; dependabot-excluded; next to the fenced area |
| argparse                      | d924b84 (v3.2-2)                                | header-only                                             | utils/flags                       | MIT                                                                                                                 | Yes                                 |                                                                          |
| zstd                          | ee650ef                                         | StaticLib                                               | curl/http                         | BSD-3-Clause OR GPL-2.0-only (README:238)                                                                           | Yes (choose BSD)                    | COPYING (GPLv2) may confuse scanners                                     |
| brotli                        | a3abcbe                                         | StaticLib (`c/`)                                        | curl                              | MIT                                                                                                                 | Yes                                 |                                                                          |
| frozen                        | b07a5c8 (1.2.0-16)                              | header-only                                             | gsc, asset_limits (8 files)       | Apache-2.0                                                                                                          | Yes (GPLv3 only)                    | Ship a license copy; no NOTICE file                                      |
| gtl                           | 086db66 (v1.2.0-42)                             | header-only                                             | common/structs/concurrent.hpp     | Apache-2.0 (+ licenses/license_folly Apache, license_lamerman BSD)                                                  | Yes                                 |                                                                          |
| concurrentqueue               | 683b9e3 (v1.0.5-6)                              | header-only via `./deps`                                | game/log.cpp                      | BSD-2-Clause OR BSL-1.0 (+ Zlib lightweightsemaphore.h)                                                             | Yes                                 | No premake script                                                        |
| hksc                          | afa5ace                                         | StaticLib (CLI files removed) + `deps/hksc-config`      | script.cpp                        | MIT (Lua license), COPYRIGHT                                                                                        | Yes                                 |                                                                          |
| Microsoft.Web.WebView2        | 1.0.4191.47 vendored in-tree (097557ba)         | headers + prebuilt `WebView2LoaderStatic.lib` (10.7 MB) | launcher/html/html_frame.cpp      | BSD-3-Clause (Microsoft) + NOTICE (ANTLR3, StringTemplate4 BSD-3)                                                   | Permissions yes; source unavailable | Binary-only lib vs §6 is an open question; runtime is an OS component    |
| (in-src) xxhash.h             | n/a                                             | header                                                  | launcher                          | BSD-2-Clause (line 6)                                                                                               | Yes                                 |                                                                          |
| (in-src) str.cpp              | n/a                                             | common                                                  | strlcpy                           | ISC (OpenBSD)                                                                                                       | Yes                                 |                                                                          |
| (in-src) str.hpp              | n/a                                             | common                                                  | strscpy family                    | GPL-2.0 (Linux)                                                                                                     | **No** (FSF: v2-only vs v3)         | Rewrite                                                                  |
| (in-src) tlssup.cpp           | n/a                                             | tlsdll                                                  | TLS stub                          | Microsoft CRT source, all rights reserved                                                                           | Unclear                             | Clean-room replacement candidate                                         |
| (assets) `resources/dw/*`     | n/a                                             | embedded RCDATA                                         | Demonware emulation               | Activision content (fastfile, playlists, etc.)                                                                      | n/a                                 | Redistribution risk                                                      |
| (assets) `data/scripts/*.gsh` | n/a                                             | shipped via R2                                          | GSC compile                       | Treyarch-derived headers (likely)                                                                                   | n/a                                 | Redistribution risk                                                      |

Beta-only additions, relevant if Xerces rebases: opus (BSD-3-Clause), portaudio
(MIT-style PortAudio license), sqlite (public domain; the rhuijben amalgamation
repo is BSD-3), **sqlite_orm (AGPL-3.0 or a paid MIT license; used by
local_db.cpp)**, xz (liblzma 0BSD), stb_image_resize (MIT/Unlicense).

Upscaler SDKs (planning):

| SDK                                     | License                                                                                                    | Implication                                                           |
| --------------------------------------- | ---------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| FidelityFX FSR1                         | MIT                                                                                                        | Compile into GPL binary                                               |
| FidelityFX FSR 2.2                      | MIT                                                                                                        | Compile in (DX11 backend is not official; another area should verify) |
| FidelityFX SDK 2.x (FSR 3.1/4 prebuilt) | AMD binary-only, no reverse engineering (docs/license.md)                                                  | Proprietary runtime DLLs                                              |
| NVIDIA Streamline                       | MIT-style core                                                                                             | Its DLSS feature DLLs are proprietary                                 |
| NVIDIA DLSS SDK                         | NVIDIA RTX SDKs License (non-sublicensable, object-code distribution, "material additional functionality") | Proprietary                                                           |
| Intel XeSS                              | Intel Simplified Software License (Oct 2022), binary only, no modification or reverse engineering          | Proprietary                                                           |

CI actions: actions/checkout@v4/@v5, microsoft/setup-msbuild@v3,
Jarod42/install-premake5@v7 (no license file),
ammaraskar/msvc-problem-matcher@master (Apache-2.0), actions/upload-artifact@v4,
actions/download-artifact@v4, marvinpinto/action-automatic-releases@latest (MIT,
**archived**), JohnnyMorganz/stylua-action@v5.0.0,
jidicula/clang-format-action@v4.18.0 (MIT), actions/setup-node@v4. None is
pinned by SHA.

---

## (f) Adding new projects cleanly

General rules:

- Declare first-party projects **before** `group("Dependencies")`
  (premake5.lua:545), for example right after tlsdll (543). Otherwise later
  statements attach to `zstd`.
- Never call `dependencies.imports()` in a minimal project; import only what you
  need.
- Do not place code under `src/client/` unless it belongs in boiii.exe: the
  client compiles every `src/client/**.cpp`.
- Put new C++ under `src/`: CI clang-format checks only `src`.
- New premake tables for test-only dependencies should **not** be
  `table.insert`ed into `dependencies`, or `common` and `client` will import
  them too.
- Use generator-neutral paths, not `$(ProjectDir)`; upstream beta uses ninja.
- New executables inherit the Debug CRT flag mix. Verify they link and run, or
  override with `removebuildoptions`.

```lua
-- 1) headless core: no game, no client PCH, no Win32 requirement
project("xerces-core")
kind("StaticLib"); language("C++")
files({ "./src/xerces-core/**.hpp", "./src/xerces-core/**.cpp" })
includedirs({ "./src/xerces-core" })
rapidjson.import()            -- header-only; explicit, not dependencies.imports()

-- 2) unit tests
project("xerces-tests")
kind("ConsoleApp"); language("C++")
files({ "./src/xerces-tests/**.cpp" })
includedirs({ "./src/xerces-core", "./src/xerces-tests" })
links({ "xerces-core" })
doctest_import()              -- deps/premake/doctest.lua defining a local table (not added to `dependencies`)
rapidjson.import()

-- 3) CLI (pack/share-code/manifest linter)
project("xerces-cli")
kind("ConsoleApp"); language("C++")
files({ "./src/xerces-cli/**.cpp" })
includedirs({ "./src/xerces-core" })
links({ "xerces-core" })      -- add curl.import() only if HTTP is needed (pulls zstd/zlib/brotli + crypt32)
-- no /base:0x170000000 and no TLS_DLL: those exist only because the client maps BlackOps3.exe

-- 4) dev-only module, behind its own option (never --dev-build / --no-check)
newoption({ trigger = "xerces-dev", description = "Build Xerces developer-only modules" })
if _OPTIONS["xerces-dev"] then
  defines({ "XERCES_DEV" })   -- workspace-wide define; in-exe components use #ifdef XERCES_DEV + REGISTER_COMPONENT
  project("xerces-devplugin")
  kind("SharedLib"); language("C++")
  files({ "./src/xerces-devplugin/**.cpp" })  -- exports p_name/post_load/post_unpack/pre_destroy (plugins.cpp ABI)
end
```

- Unit-test candidates that can be built without the game:
  - Manifest and load-order resolution, conflict maps, stringtable row/key
    merge, share-code encode/decode.
  - `utils::hook::signature` over synthetic buffers (signature.hpp:5-43).
  - The GSC lexer and parser (gsc_lexer.cpp/gsc_parser.cpp only include
    `<std_include.hpp>` plus std headers, so a test-local shim header works).
    The emitter is excluded because it needs `game/game.hpp`.
- Not testable headlessly: anything including `game/*.hpp`. `symbol.hpp` routes
  through the fenced `arxan::detail` proxy.
- Version headers: new binaries needing `version.hpp` must include
  `%{prj.location}/src` and either `dependson({"client"})` or run
  `premake5 generate-buildinfo` as an explicit CI step before building.
- CI sketch: add a push/PR workflow on windows-2025 that does checkout with
  submodules, pins premake (`ref: v5.0.0`), generates, runs
  `msbuild ... /p:Configuration=Debug` and `Release`, then runs
  `build/bin/x64/<cfg>/xerces-tests.exe`. Keep the release workflows uploading
  explicit paths only, so dev artifacts never ship.
- Plugin caveats: the appdata plugins folder is deleted by upstream's updater
  cleanup, so use `<game>/<root>/plugins` or Xerces's own updater. Plugins get
  no host API (only lifecycle exports) and are loaded without any signature or
  allowlist.

---

## Rename touchpoints (build, CI and release scope)

- Build:
  - `workspace("boiii")` (premake5.lua:260), which determines
    `build/boiii.sln/.slnx` referenced by build.bat:18 and every workflow.
  - `targetname("boiii")` (442).
  - cross.sh:35-36 (`BOIII_EXE`, `TLS_DLL`).
- Resources: resource.r<local scratch copy, not kept>73-79 (CompanyName,
  FileDescription, LegalCopyright, OriginalFilename, ProductName) and
  tlsdll/resource.r<local scratch copy, not kept>63-69.
- Runtime names:
  - Temp DLL name (tls.cpp:12).
  - App-data root `%LOCALAPPDATA%/boiii` (game.cpp:42) and the WebView2 dir "EZZ
    BOIII" (html_frame.cpp:56).
  - Game subfolder `boiii/` (plugins.cpp:37; ui_scripting.cpp:1094;
    script.cpp:1243) and `boiii_players` (plugins.cpp:26).
- Update and release endpoints:
  - Updater macros (file_updater.cpp:17-25) and the beta URL (main.cpp:935).
  - Launcher JS (main.js:4466, 4491, 4499, 4527-4528).
  - Workflow R2 folder and manifest names; the issue template label "BOIII
    Version".
- Infrastructure and metadata: master servers `master.ezz.lol:20810`,
  `m.ezz.lol:20810` (server_list.cpp:287-288); package.json name and repository.
- Scale of string changes: about 202 "boiii", 63 "BOIII", 29 "Ezz", 18 "EZZ" and
  25 "ezz.lol" occurrences across src/, data/, premake5.lua and .github.

## Fenced inventory (identified only, not analysed)

See off_limits. Summary: `component/arxan.cpp` + `integrity.hpp`;
`component/steam_proxy.*` (DEV_BUILD/NO_CHECK gate at :315);
`src/client/steam/**`; `component/dump.cpp` + `-dump`; `game/symbol.hpp` /
`game.hpp` `arxan::detail` proxy; Arxan workaround blocks in
`client_patches.cpp`/`patches.cpp`; premake `--dev-build`/`--no-check`;
auth/licensing-adjacent structs pending classification.

## Design implications

- Own the update channel before the first Xerces binary: new base URL and
  manifest names (file_updater.cpp:17-25, main.cpp:935, main.js:4466/4527), a
  new app-data root (game.cpp:42), and workflow R2_FOLDER/MANIFEST_FILE kept in
  sync (build-and-publish.yml:24-28). Until then, ship with the updater
  disabled, as the fork did in e5968d07.
- Signed releases: SHA-256 per file (utils::cryptography::sha256 exists) and an
  Ed25519-signed manifest (libtomcrypt ed25519_sign/verify already in the
  submodule) with the public key embedded in the exe. Verify every exe download
  (beta, version picker, self-update). Publish SHA256SUMS and a signature next
  to each GitHub Release, Authenticode-sign if a certificate is available, and
  archive PDBs per release for crash triage.
- Keep Xerces user state (mod manifests, packs, share-code caches, load order)
  out of any updater-mirrored directory. If legacy %LOCALAPPDATA%/boiii
  overrides are honoured, read them only and never write there.
- Premake hygiene for every new project: declare it before
  group('Dependencies'); fix the trailing prebuild (premake5.lua:553-559) and
  the `'toolset: msc*'` filters (411/435/500) while there; avoid $(ProjectDir)
  so a later move to upstream's ninja generator is painless.
- Headless core library (xerces-core): no dependencies.imports(), no client
  std_include.hpp, no game/ headers; import header-only deps explicitly; define
  RAPIDJSON_ASSERT consistently via premake defines rather than a PCH. This is
  where manifest parsing, deterministic load order, the asset ownership map,
  conflict detection, stringtable merge and share codes live.
- Tests: an xerces-tests ConsoleApp under src/ (covered by the clang-format CI
  path) but outside src/client (whose glob would compile it into the exe), using
  doctest (MIT) or Catch2 (BSL-1.0). Add a push/PR workflow that builds Debug
  and Release and runs the tests on windows-2025. Include signature-scanner
  tests on synthetic buffers and GSC lexer/parser tests through a std_include
  shim.
- Enforce the pattern-scanning-only offset policy in CI with a lint that rejects
  new hex-literal game symbols in Xerces-owned code (upstream has about 767).
  Real signature validation can only run locally against the developer's own
  game install; game binaries must never enter CI or the repo.
- CLI tool (xerces-cli) as a ConsoleApp linking xerces-core, without
  /base:0x170000000 and without TLS_DLL embedding; add curl.import() only if it
  needs the network.
- Dev-only code behind a new option and define (for example --xerces-dev /
  XERCES_DEV), never DEV_BUILD/NO_CHECK. Use either #ifdef'd REGISTER_COMPONENT
  components or a SharedLib plugin on the existing plugins.cpp ABI, loaded from
  <game>/<root>/plugins. If plugins become a feature, design a versioned C ABI
  and a signing or allowlist policy.
- Versioning: use a Xerces tag namespace (for example xerces-vX.Y.Z) with
  `git describe --tags --match 'xerces-v*'`. Record the upstream base
  (v3.0.0@5aa7fac) in a separate macro, regenerate version.h when the dirty
  state changes, and decide deliberately what getinfo 'shortversion' reports for
  server interop.
- Licensing deliverables: keep LICENSE; add a NOTICE file ('modified from BOIII
  v3.0.0 by ...' with dates), AUTHORS/CREDITS (momo5502, Ezz-lol maintainers,
  git shortlog), THIRD_PARTY_NOTICES for every dependency, and a launcher
  About/Legal page (GPLv3 section 5(d)). Fix the VERSIONINFO LegalCopyright,
  attach a full source archive including submodules to every release, rewrite
  the str.hpp GPL-2.0 code, and decide on tlssup.cpp provenance.
- Release Xerces-authored code as GPL-3.0-or-later, or a GPL-compatible
  permissive license for code that will be shared with the Railway backend. The
  backend is a separate networked program and is not bound by the client's GPL
  unless it incorporates this code.
- Upscaling licensing path: compile in FSR1 (MIT) first, FSR2 (MIT) next. Treat
  DLSS/XeSS/FSR4 as optional runtime DLLs behind a narrow interface, never
  vendored into the repo or the release zip, pending the system-library and
  section 7 questions.
- Pin and align toolchains: pin the premake version, VS/LLVM versions and
  actions by SHA; make generate.bat/build.bat match the CI generator; document
  the required VS components (clang-cl); plan for upstream's ninja/LLVM 23
  switch.
- Add a dev deploy step that copies data/ into the Xerces app-data data
  directory after build, because launcher/Lua/GSC data is not embedded in the
  exe.
- Asset hygiene: Xerces-owned directories and test fixtures must contain no
  Activision/Treyarch-derived binaries or scripts. Decide whether the inherited
  embedded fastfile and the .gsh headers stay.
- Branch strategy: decide whether Xerces tracks upstream main, beta or mod-stack
  before writing multi-mod code, because upstream mod-stack already implements
  fs_extramods stacking and would conflict with or replace parts of the planned
  loader.

## Risks

- Shipping any Xerces build that still uses upstream's updater: Release builds
  self-replace the exe with upstream boiii.exe (file_updater.cpp:497-509), and
  cleanup deletes non-manifest files and folders under %LOCALAPPDATA%/boiii,
  including plugins/ and any Xerces data (file_updater.cpp:637-712).
- Unverified executable downloads: the dedicated -beta path (main.cpp:924-948)
  and the launcher version picker (launcher.cpp:1315-1354, main.js:4466-4528)
  download and execute exes with no hash or signature check.
- Update trust rests on TLS plus SHA-1 manifests with no signature; anyone
  controlling the R2 bucket or domain controls every client.
- No automated tests and no build on PRs; only formatting runs on push/PR, so
  compile breaks and regressions surface only after tag builds or manual runs.
- GPL-2.0-only Linux code in src/common/str.hpp inside a GPL-3.0 work (FSF
  considers them incompatible); needs legal review, but a clean rewrite is
  small.
- Corresponding Source gaps: GitHub tag archives omit submodules (likely);
  WebView2LoaderStatic.lib is binary-only; Steamworks headers have no license;
  tlssup.cpp is Microsoft CRT source. The same exposure applies to the DeroXP
  fork's published v0.0.x-alpha binaries.
- Redistributed game data: an Activision fastfile and Demonware blobs are
  embedded in boiii.exe (resource.r<local scratch copy, not kept>97-102), and
  Treyarch-derived .gsh headers ship via R2 (data/scripts).
- curl 8.4.0 (Oct 2023) in a networked client that Xerces will extend (friends
  backend, NAT relay); several later curl security advisories are likely
  unpatched.
- CI supply chain: archived marvinpinto action, msvc-problem-matcher@master,
  stylua 'latest', rclone .deb without checksum, a personal access token as
  secrets.TOKEN, and no actions pinned by SHA.
- build-new.yml create_release reuses the latest existing tag (git describe
  --abbrev=0) with an action that recreates the release for that tag, so a
  manual run can overwrite the current latest release (likely).
- Premake scope leaks and dead `'toolset: msc*'` filters silently drop or
  misplace flags; new projects can inherit the same surprises and the Debug
  /MTd-/MDd mix.
- Toolchain drift: local scripts use vs2022 and .sln, CI uses vs2026 and .slnx
  with unpinned premake, and upstream beta moves to ninja + LLVM 23 + premake
  5.0.0 with 804 changed lines in premake5.lua, so heavy premake edits will
  conflict on rebase.
- Rebasing onto upstream beta or mod-stack brings AGPL-3.0 sqlite_orm
  (local_db.cpp) plus 5 new submodules.
- Upstream already implements mod stacking on the mod-stack branch
  (fs_extramods, Mods menu, host-synced order): duplicate effort and possible
  protocol divergence from BOIII servers.
- Partial rename: brand strings and endpoints are spread across code, data, CI
  and JS (about 202 'boiii', 63 'BOIII', 29 'Ezz', 18 'EZZ', 25 'ezz.lol');
  missing one can leave paths or endpoints pointing at upstream infrastructure.
- The plugin loader runs any DLL found in two folders with no allowlist or
  signature, and only lifecycle exports are available.
- clang-format version drift (CI defaults to clang-format 22) can make the
  formatting check fail on code formatted locally with VS's bundled
  clang-format.
- Version confusion: the stray 'main' tag, fork alpha tags and upstream vX tags
  share one namespace, so git describe may report misleading versions for Xerces
  builds.

## Open questions

- Is the project GPL-3.0-only or GPL-3.0-or-later? There is no grant statement
  anywhere; the answer affects relicensing options and compatibility reasoning.
- Does the GPL-2.0-tagged code in src/common/str.hpp make current upstream
  binaries non-distributable? Xerces should rewrite it regardless; confirm with
  counsel whether past fork releases need remediation.
- Can a GPL-3.0 executable load proprietary upscaler runtimes (nvngx_dlss.dll or
  the Streamline DLSS plugin, libxess.dll, FidelityFX SDK 2.x DLLs)? This
  depends on the scope of the GPLv3 System Library exclusion (D3D11/DXGI,
  driver-provided NGX core) and on the separate-DLL/plugin arm's-length
  argument. Section 7 additional permissions can only come from copyright
  holders; Xerces can grant them for its own code but not for upstream's 2,666
  commits. Needs legal review.
- Are the Steamworks v1.37 headers (no license in the mirror), the binary-only
  WebView2LoaderStatic.lib and the Microsoft CRT tlssup.cpp acceptable in
  Xerces's Corresponding Source? Or should Xerces replace them with its own
  Steam type declarations, the dynamic WebView2Loader.dll and a clean-room TLS
  stub?
- Should Xerces keep embedding Activision/Demonware data
  (core_ffotd_tu32_593.ff, playlists, keys.txt) and shipping Treyarch .gsh
  headers in data/?
- Which base does Xerces track: upstream main (v3.0.0), beta (+464 commits,
  ninja/LLVM, AGPL sqlite_orm) or mod-stack (+208 commits, upstream mod
  stacking)?
- Where are Xerces releases hosted (R2, GitHub Releases, both), and who holds
  the Ed25519 manifest-signing key and any Authenticode certificate?
- Will Xerces remain wire-compatible with BOIII infrastructure
  (master.ezz.lol/m.ezz.lol, the getinfo 'shortversion' field, protocol), or run
  its own masters? This determines which version strings must stay stable.
- Should headless tests also run on Linux runners? That needs a separate
  workspace or filters, because _WINDOWS/WIN32 are global defines. The
  alternative is Windows runners only.
- How should the DeroXP fork's already-published v0.0.1-alpha and v0.0.2-alpha
  binaries be brought into GPL source compliance (full source archive including
  submodules, or a written offer)?
- Which DX11 paths do DLSS, XeSS and FSR2+/FSR4 actually support? The FidelityFX
  SDK 2.x/FSR4 appears DX12-oriented. This belongs to the upscaling area but
  affects which proprietary SDKs are even relevant.
- README.md:61 advertises BOIII-Full.zip, but no workflow produces it and v3.0.0
  has only boiii.exe. Is a full package expected for Xerces releases (exe +
  data + notices)?

## Fenced (do not modify)

- src/client/component/arxan.cpp - Arxan anti-tamper handling (has its own
  highest component priority 'arxan', component_interface.hpp:17); fence the
  whole file.
- src/client/component/integrity.hpp - integrity-check handling included only by
  arxan.cpp; fence together with arxan.cpp.
- src/client/component/steam_proxy.cpp and steam_proxy.hpp - Steam
  ownership/licensing gate; DEV_BUILD and NO_CHECK are consumed only at
  steam_proxy.cpp:315; fence.
- `src/client/steam/**` (notably interfaces/user.cpp:91-93 UserHasLicenseForApp,
  interfaces/game_server.cpp:98, interfaces/apps.cpp, auth-ticket methods) -
  Steam API emulation that answers license/ownership/subscription/ticket
  queries; fence.
- premake5.lua:75-83 and 286-292 (--dev-build -> DEV_BUILD, --no-check ->
  NO_CHECK) - their only consumer is the ownership gate; do not modify, reuse or
  wire into Xerces projects; Xerces needs new option and define names.
- src/client/game/symbol.hpp:6-9,70-71 and src/client/game/game.hpp:14-17 -
  arxan::detail callstack proxy that every game::symbol call goes through; use
  as-is and never modify; this is also why game/ headers cannot enter headless
  libraries.
- src/client/component/dump.cpp and the -dump flag
  (src/common/utils/flags.cpp:169) - dumps the game executable after Arxan
  unpack; fence.
- Arxan workaround blocks inside otherwise-normal files:
  src/client/component/client_patches.cpp around lines 517-527,
  src/client/component/patches.cpp around line 548, and the context of
  src/client/component/dedicated/sound.cpp:188 - fence those specific blocks
  only.
- Classification pending, treat as fenced until a human reviews them:
  src/client/component/auth.cpp;
  src/client/game/demonware/servers/auth3_server.cpp (m_licenseID in emulated
  auth tickets); src/client/game/structs/live/core.hpp:41-59
  (appLicenseVerification bits); src/client/game/structs/dw/core.hpp
  (AUTH_INCORRECT_LICENSE_CODE); src/client/game/structs/sys.hpp;
  src/client/game/symbols/lua/cod/api.hpp - all matched license/ownership
  keywords.
- deps/SteamworkSDK pin and its dependabot exclusion
  (.github/dependabot.yml:8-11) - leave untouched.
- README.md:37-49 positioning ('removes Steam ownership verification', 'No Steam
  ownership required') - content-level fence: do not carry it into Xerces docs
  or branding.
