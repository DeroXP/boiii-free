# Upstream rendering, performance, display and input

> Machine-generated research notes from 2026-11-04, kept as evidence for
> `docs/DESIGN.md`. Code was read at upstream `main` 5aa7fac, `mod-stack`
> ac356c3c and the fork `refs/heads/main` 7805319. `path:line` citations refer
> to those commits. Treat claims as leads: anything marked runtime still needs
> the real game.

## Summary

Upstream @5aa7fac already runs on the installed September-2026 BlackOps3.exe. I
read the PE header without modifying anything: checksum 0x6531394, TimeDateStamp
2026-08-11, 106,059,256 bytes. That checksum makes is_new_client() true, so the
first column of every {new client, legacy client, server} address triple applies
(base.cpp:36-54, ptr.hpp:62-84). Every render, display, perf and input hook in
this area is a hard-coded triple, or a single address that is the same in both
client builds (fov.cpp, Com_FPSLimit). None of them uses a signature. A reusable
scanner exists (utils::hook::signature, `_sig`), but only arxan.cpp calls it,
and that file is off-limits. The fork's perf work targeted the pre-September exe
(checksum 0x888C368, the 'legacy' column today), so its RVAs are legacy-only:
the FPS-limit byte at 0x20F8F0E, RenderSurfaceList at 0x1C53FD0 and the draw
wrapper at 0x1D00425. The legacy-to-new address shift is piecewise: 0 for low
code, -0x20 around 0x1412B-0x1413E, +0xC3D0 across the renderer region
0x141A6-0x141D1, then +0xCA90, +0xC780 and +0x58AC0 further up. Those RVAs
therefore have to be found again by pattern, not translated.

D3D11: the code has no symbol for the engine's device, context or swap chain.
The only D3D entry point is download_overlay.cpp. On the async scheduler it
creates a 1x1 hidden window plus a WARP device and swap chain, takes vtable[8]
(IDXGISwapChain::Present) and MinHook-detours it. Because the detour sits inline
on dxgi's Present body, it most likely catches every swap chain in the process.
On the first real Present it gets the device (GetDevice), the immediate context
and the HWND (GetDesc().OutputWindow). It then sets up Dear ImGui (1.93.0 WIP,
DX11 and Win32 backends, gamepad support compiled out) and subclasses the game's
WndProc with SetWindowLongPtrA. MinHook allows one hook per target:
MH_CreateHook returns MH_ERROR_ALREADY_CREATED (minhook hook.<local scratch
copy, not kept>588-589, 656-658), and utils::hook::detour::create throws on
anything other than MH_OK (hook.cpp:239-250). That is why the fork's engine_perf
had to be called from inside download_overlay's stub. ImGui is used only for the
workshop/fastdl download progress window and its Yes/No confirmations.

The overlay has these weaknesses:

- It hooks neither ResizeBuffers nor Present1.
- ImGui draws into whatever render target the engine left bound.
- It runs NewFrame/Render, two Map calls and a full DX11 state save and restore
  every frame, even when nothing is visible.
- Its WndProc handler passes all input through to the game.
- ImGui input state is written on the window thread and read on the Present
  thread. The engine's default SerializeRender=0 uses a separate render thread,
  so this is likely a data race.

I propose one gfx_hooks dispatcher that owns Present, ResizeBuffers, Present1
and ResizeBuffers1, has staged and fault-isolated callbacks, manages the
backbuffer RTV, filters to the game's swap chain, and exposes a C ABI to
plugins. One input router would sit alongside it.

Display and input: most of the display features already exist in the engine, and
boiii adds front-ends to them. config.ini exposes FullScreenMode 0/1/2 (2 =
borderless), Monitor 0-8, WindowX/Y, WindowSize, RefreshRate (1-240),
ResolutionPercent (scene render scale), Vsync (fullscreen only), MaxFPS 0-1000,
FOV 65-120 (horizontal FOV assuming 16:9, so wider screens get more FOV),
MaxFrameLatency, SerializeRender, BackbufferCount, AATechnique and per-object
MotionBlur. boiii's front-ends are:

- the -borderless, -windowed, -safe and -ultrawide flags;
- a replacement Lua graphics menu, plus the Quick Settings and Game Tweaks LUI
  menus;
- launcher fields and presets that write to boiii_players/config.ini;
- cg_fovScale (fov.cpp);
- an experimental -ultrawide that forces view dimensions to scene dimensions
  (render.cpp).

Gaps:

- The graphics-menu Lua script returns early whenever any mod is loaded. It
  compares the function Engine.UsingModsUgcName instead of its result.
- Ultrawide covers the UI only and is flag-gated; there is no HUD aspect clamp.
- The overlays are fixed-pixel and not DPI-aware.
- The launcher has no monitor or render-scale picker.
- FOV is capped at 120, and viewmodel FOV is not exposed.

Key binds use the engine's bindings_0.cfg, which covers keyboard and gamepad
buttons plus bind2. boiii persists console binds to
boiii_players/user/binds.cfg, and the launcher has a Binds tab, but batches of
more than 3 binds are dropped. Controller support is native to the engine
(`gpad_*` dvars, LUI button enums, a Steam Input template mapping to xinput).
Any controller-navigable in-game Xerces menu should therefore be LUI (Lua), not
ImGui. LUI has gamepad support built in, and C++ can drive it the same way
toast.cpp does.

Perf: upstream already does four things:

- removes the hidden secondary FPS cap by detouring Com_FPSLimit (0x140F7CFD0)
  to return UINT32_MAX, so com_maxfps governs (4ef9e224);
- stops the engine changing process priority (6-byte NOP);
- restricts process affinity to the first 4 logical CPUs for 1 s at startup (AMD
  stutter fix);
- writes a dGPU preference to the registry.

The fork added runtime-only knobs: HIGH priority, NtSetTimerResolution to the
finest step and all-cores affinity. It also added a 1-byte jge->jmp patch that
disables the whole frame-limit loop; upstream's Com_FPSLimit hook supersedes it.
On top of those it added Present, draw, Map, Wait and Sleep instrumentation.
Measured on the user's RTX 5070 Ti and 24-core CPU:

- 60 FPS (vsync) by default;
- about 200 FPS with r_vsync 0, HIGH priority and the timer change, with the GPU
  at about 58%;
- 210-300 FPS with the cap removed;
- cutting draws by 66% gained only about 5%.

The fork concluded the frame is CPU-bound per-frame work. Two things crashed:

- applying priority and timer in post_load, during fastfile load;
- every Sleep/SleepEx detour variant, including logging-only.

Upscaling: the engine separates scene resolution from display resolution
(vidConfig_t sceneWidth/Height vs displayWidth/Height; ResolutionPercent /
r_sceneResolutionMultiplier; dynamic-resolution and cubic-upsample dvars). It
generates per-object motion vectors (MotionBlur Off/Auto/On,
r_motionVectorGenerateEnable) and has a temporal AA path
(r_aaAllowTemporalMultiGpu). Camera data has a struct, refdef_t: tanHalfFov,
zNear/zFar, vieworg/lastvieworg, exposure and sceneResolutionIndex history.
However, refdef_t is not wired into cg_t and its size is not asserted. There are
no symbols for render targets, depth, velocity, jitter, projection,
resolve/upsample passes or the device. FSR1 must replace the engine's
scene-to-display upsample pass, not run at Present, because the UI is composited
at display resolution. It must ship precompiled DXBC, because the launcher can
rename the game's d3dcompiler_46.dll, and the asset override system only handles
Rawfile and ScriptParseTree. FSR2/3, DLSS and XeSS need reverse engineering of
the depth, motion-vector and jitter sources in the new exe first.

## Facts

- **verified**: The installed BlackOps3.exe is the build upstream calls the 'new
  client': PE checksum 0x6531394, TimeDateStamp 2026-08-11 18:00:53Z,
  106,059,256 bytes, SizeOfImage 0x1D75BC00. The first column of every address
  triple applies. (Read-only PE header read of the installed exe;
  src/client/game/base.cpp:36-37 (is_new_client == 0x6531394);
  src/client/main.cpp:124-125 (supported_client_checksum 0x6531394, legacy
  0x888C368))
- **verified**: Each symbol/select() is a triple {new client, legacy client,
  dedicated server}, chosen by header checksum. The fork's base (d80d308)
  treated 0x888C368 as 'client', so every fork RVA is a legacy-column address.
  (src/client/game/ptr.hpp:62-84; src/client/game/base.cpp:36-54; git show
  d80d308:src/client/game/game.cpp (is_client == 0x888C368))
- **verified**: No symbol or global exists for the engine's ID3D11Device,
  ID3D11DeviceContext or IDXGISwapChain. The only acquisition path is inside the
  Present detour: swap_chain->GetDevice, then GetImmediateContext, then
  GetDesc().OutputWindow. (src/client/component/download_overlay.cpp:351-381;
  grep for IDXGISwapChain/ID3D11Device/CreateDevice over src/ (only
  download_overlay.cpp and main.cpp:600-604 link-only reference))
- **verified**: Present is hooked by creating a 1x1 hidden window plus a WARP
  device and swap chain, taking vtable[8] and MinHook-detouring it. Setup runs
  once on scheduler::async from post_unpack. pre_destroy disables the hook and
  shuts down ImGui. (src/client/component/download_overlay.cpp:404-453 (446-448
  vtable[8]), 508-510, 512-526)
- **verified**: MinHook refuses a second hook on the same target address:
  MH_CreateHook looks the target up in its hook table and returns
  MH_ERROR_ALREADY_CREATED. utils::hook::detour::create throws on any status
  other than MH_OK. A second component therefore cannot detour Present.
  (deps/minhook/src/hook.<local scratch copy, not kept>588-589, 656-658;
  src/common/utils/hook.cpp:239-250)
- **likely**: The Present 'vtable hook' is really an inline detour of dxgi's
  Present implementation, so it fires for every swap chain of that
  implementation in the process (the dummy WARP chain, third-party overlay
  chains). (MinHook patches the target function's prologue;
  download_overlay.cpp:447-448 passes vtable[8] (the function address) to
  detour::create)
- **verified**: Nothing hooks ResizeBuffers, Present1 or ResizeBuffers1. The
  overlay instead re-reads the backbuffer size from GetDesc on every Present.
  (src/client/component/download_overlay.cpp:384-390; no other DXGI hooks in
  src/)
- **verified**: The ImGui overlay never binds a render target. imgui_impl_dx11
  never calls OMSetRenderTargets, so ImGui draws into whatever RTV the engine
  left bound at Present. (src/client/component/download_overlay.cpp:392-398;
  grep OMSetRenderTargets in deps/imgui/backends/imgui_impl_dx11.cpp = none)
- **verified**: Once initialized, the overlay runs ImGui NewFrame/Render and
  ImGui_ImplDX11_RenderDrawData on every Present, even with no window visible.
  RenderDrawData has no empty-draw-list early-out: it does 2 Map/Unmap calls and
  a full DX11 state backup and restore. The overlay also copies its two state
  structs under a lock every frame.
  (src/client/component/download_overlay.cpp:155, 224, 383-399;
  deps/imgui/backends/imgui_impl_dx11.cpp:170-232)
- **verified**: The overlay subclasses the game window with SetWindowLongPtrA
  from the Present thread and forwards input to ImGui only while a panel is
  active. ImGui_ImplWin32_WndProcHandler returns nonzero only for WM_SETCURSOR
  and ANSI WM_IME_CHAR, so the game still receives every click and key while the
  overlay is shown. (src/client/component/download_overlay.cpp:336-349, 372-375;
  deps/imgui/backends/imgui_impl_win32.cpp:808-831)
- **likely**: With the engine default SerializeRender=0, Present runs on a
  different thread from the window procedure. The current overlay therefore
  feeds ImGui IO on the window thread while calling NewFrame on the render
  thread, which is a data race. (players/config.ini:44-46 ('0 use two threads,
  one thread update frame N while the second render N-1'); fork bfca64a6
  captures the render TID inside Present)
- **verified**: Dear ImGui 1.93.0 WIP is used only by download_overlay.cpp: the
  workshop/fastdl download progress window (bytes, speed, ETA, Stop) and Yes/No
  confirmations. It is built as a static lib with the DX11 and Win32 backends
  and IMGUI_IMPL_WIN32_DISABLE_GAMEPAD. (deps/imgui/imgui.h:32-33;
  deps/premake/imgui.lua:17-36 (define at :33); callers fastdl.cpp:485-547,
  steamcmd.cpp:394-687, workshop.cpp:827-942; introduced in 12b57f64
  (2026-02-19))
- **verified**: The fork piggybacked on the overlay's Present stub with
  engine_perf::intercept_present(swap_chain, sync_interval&, flags&) because
  MinHook would not allow a second hook. download_overlay.cpp is unchanged
  upstream since the fork base apart from include order and name(), so the
  fork's integration point still applies. (git diff d80d308 refs/heads/main --
  download_overlay.cpp; engine_perf.cpp:26-30, 452-507; git diff d80d308 xerces
  -- download_overlay.cpp)
- **verified**: The scheduler's 'renderer' pipeline is driven by a detour on
  SND_EndFrame (frontend frame end, client only), not by Present. branding.cpp
  uses it to queue engine-native R_AddCmdDrawText text.
  (src/client/component/scheduler.cpp:74-80, 270-273;
  src/client/game/symbols/snd/core.hpp:429;
  src/client/component/branding.cpp:25-40, 53)
- **verified**: Display modes are engine-native: FullScreenMode/r_fullscreen
  0=Windowed, 1=Fullscreen, 2=Fullscreen Window (borderless). boiii adds
  -borderless (sets 2), -windowed (0) and -safe (0 plus r_vsync 0), applied at
  dvars_loaded. (players/config.ini:71-72;
  src/client/component/startup_flags.cpp:12-23, 34; README.md:197-198)
- **verified**: Upstream's in-game Video menu (Lua) exposes display mode,
  monitor (disabled when windowed), resolution (disabled in mode 2), refresh
  rate (mode 1 only), scene resolution ratio (r_sceneResolutionMultiplier),
  gamma, brightness, FOV 65-120 (cg_fov_default), max FPS 0-500, vsync and draw
  FPS. (`data/ui_scripts/graphics_settings/__init__.lua:5-137`)
- **verified**: Bug: the Video menu script returns early whenever a mod or
  usermap is loaded. Its guard compares the function Engine.UsingModsUgcName
  (not its result) to 'usermaps', which is always unequal.
  (`data/ui_scripts/graphics_settings/__init__.lua:1-3`)
- **verified**: The launcher (WebView2, pre-game, same process) edits engine
  keys in boiii_players/config.ini: WindowSize, RefreshRate, MaxFPS, FOV,
  MaxFrameLatency, FullScreenMode, Vsync, DrawFPS, SmoothFramerate,
  RestrictGraphicsOptions and VRAM. It also has 'smooth' and 'lowlatency'
  presets. Writes only succeed if config.ini already exists.
  (src/client/launcher/launcher.cpp:2237-2290, 2388-2420, 2632-2700;
  data/launcher/main.js:3729-3795)
- **verified**: FOV: the engine FOV setting is 'horizontal FOV assuming 16:9',
  range 65-120. fov.cpp scales fov_x, dx_dz and dy_dz by cg_fovScale after
  CG_CalcFOVfromLens when not in the UI level, makes cg_fovScale archived and
  stops it resetting. The patched addresses are identical in both client builds.
  (players/config.ini:14-15; src/client/component/fov.cpp:11-43;
  src/client/game/symbols/cg/core.hpp:10-12 (CG_CalcFOVfromLens 0x1404D6230
  both))
- **verified**: Ultrawide support is the -ultrawide flag only. It detours
  R_StoreWindowSettings so that when the scene aspect exceeds 16:9 the
  view/display dimensions equal the scene dimensions (written to vidConfig and
  cls_vidConfig), and it re-registers the safeArea dvars at 1.0 with
  cg_hudSplitscreenScale=1. The README calls it unstable. GfxWindowParms is
  commented in the source as 'very incorrect'.
  (src/client/component/render.cpp:16-71;
  src/client/game/impl/scr/place.cpp:10-22; src/client/game/structs/r.hpp:19-31;
  README.md:213; commit 1fb9459f)
- **verified**: render.cpp's guard compares the float sceneAspectRatio with
  static_cast<uint32_t>(viewAspectRatio), a truncated integer, so in effect the
  only real condition is aspect > 16:9. (src/client/component/render.cpp:29-31)
- **verified**: Multi-monitor: the engine has Monitor 0-8 (for fullscreen and
  borderless) and WindowX/WindowY. boiii sets per-monitor-v2 DPI awareness at
  process start. The splash centers on the primary monitor. The ImGui panels are
  fixed 560/620 px wide with the default font and no DPI or resolution scaling.
  (players/config.ini:75-85; src/client/main.cpp:564-574, 1005;
  src/client/component/splash.cpp:60-67;
  src/client/component/download_overlay.cpp:166, 235, 362-370)
- **verified**: Binds: the engine's bindings_0.cfg binds keyboard keys and
  gamepad buttons (BUTTON_A, BUTTON_LSHLDR, DPAD_UP...) and supports bind2.
  binds.cpp swaps the bind, unbind and unbindall function pointers in
  cmd_function_s to persist custom binds to boiii_players/user/binds.cfg, but
  drops any batch of more than 3 ops arriving within 100 ms. The launcher Binds
  tab edits the same file. (players/bindings_0.cfg:1-57;
  src/client/component/binds.cpp:37, 86-88, 140-153, 231-296;
  src/client/launcher/launcher.cpp:63-135, 1941-1980)
- **verified**: boiii contains no XInput or raw-input code. Controller support
  is engine-native: `gpad_*` dvars, a Steam Input template (controller.vdf)
  mapping to xinput buttons, and gpad_buttonsConfig/gpad_sticksConfig presets.
  boiii archives gpad_stick_deadzone_min/max and adds cl_yawspeed/cl_pitchspeed,
  which are registered with DVAR_NONE and therefore not persisted. (grep
  XInput/RegisterRawInputDevices over src/client = none; game dir
  controller.vdf; src/client/game/structs/live/settings.hpp:131-145;
  src/client/component/dvar_patches.cpp:93-94;
  src/client/component/client_patches.cpp:492-510)
- **verified**: LUI menus get native gamepad navigation and button prompts
  through `Enum.LUIButton.*` (LUI_KEY_XBA_PSCROSS, XBB_PSCIRCLE, XBY_PSTRIANGLE,
  START). C++ already runs Lua in the UI VM (toast.cpp loadstring+pcall on
  scheduler::main), which can be used to open menus.
  (data/ui_scripts/frontend_menus/datasources_gamesettingsflyout_buttons.lua:145-260;
  `data/ui_scripts/quick_settings/__init__.lua:346`;
  src/client/component/toast.cpp:39-90)
- **verified**: Upstream removes the hidden FPS cap: Com_FPSLimit (0x140F7CFD0
  in both client builds) is detoured to return UINT32_MAX, so min(com_maxfps,
  limit) equals com_maxfps. This is the same ~200 FPS secondary cap the fork
  found ('0x1407CFD0' in the fork's text, missing a digit).
  (src/client/component/client_patches.cpp:564-567;
  src/client/game/symbols/com/core.hpp:86-90; commit 4ef9e224 (2026-08-16); fork
  commit bfca64a6 message)
- **verified**: Upstream NOPs the engine's own process-priority change (6 bytes
  at new 0x1422BBB28 / legacy 0x142334C98). It also limits affinity to the first
  min(N,4) logical CPUs at the first main frame and restores the system mask 1 s
  later (AMD stutter fix). (src/client/component/client_patches.cpp:58-77, 458,
  464-465; commits 2d9ff773, feacde16)
- **verified**: Upstream writes 'GpuPreference=2;' for the boiii exe under
  HKCU/Software/Microsoft/DirectX/UserGpuPreferences if no entry exists, before
  the launcher runs. (src/client/main.cpp:600-621, 1061)
- **verified**: Fork perf (final state): runtime console commands for
  SetPriorityClass, NtSetTimerResolution (finest step), all-cores affinity,
  opt-in Sleep/SleepEx detours, and a 1-byte jge->jmp patch at legacy RVA
  0x20F8F0E. That address is Com_Frame_Try_Block_Function (legacy 0x1420F8E00) +
  0x10E, inside the frame-limit loop.
  (refs/heads/main:src/client/component/perf.cpp:40-58, 228-253, 278-308,
  347-374; src/client/game/symbols/com/core.hpp:76-77)
- **verified**: Fork crash record: HIGH priority plus timer resolution applied
  in post_load crashed during the 'core_patch' fastfile load in both runs, but
  was fine when applied at runtime. Five or more Sleep/SleepEx detour variants
  crashed, including a logging-only one during the zm_patch load. The
  NtDelayExecution hook was removed untested. A DB_LoadXAssets detour at
  0x1414236A0 stopped the game booting. printf from an early thread crashed.
  (fork commits 2211a934, 2ea11406, bfca64a6;
  refs/heads/main:src/client/component/client_patches.cpp comment near
  DB_LoadXAssets)
- **verified**: Fork measurements on the RTX 5070 Ti / 24-core rig: 60 FPS
  vsync-locked by default; about 200 FPS with r_vsync 0 + HIGH + timer (GPU
  ~58%); 243 FPS on the attract screen; 210-300 FPS in zombies with the cap
  removed. Draws were ~22M over ~9k frames, 91% from one surface loop. Cutting
  surfaces to 1/2 and 1/4 took draws 2602 -> 1467 -> 899 per frame for ~205 ->
  213 -> 214 FPS. The frontend ran 666 draws and 299 Maps per frame. Forcing
  sync_interval 0, skipping sleeps and the wait hooks each changed FPS by 0%.
  (fork commits 2ea11406, 2211a934, bfca64a6, 00127fff, fd61f80e, 9ecb1bdd
  (numbers as reported; not reproduced))
- **verified**: The engine already exposes MaxFrameLatency 0-4 (applied as
  driver queue depth), SerializeRender 0-2, BackbufferCount 2-3, SmoothFramerate
  and ShowPerformanceGraph 0-16. The fork's SetMaximumFrameLatency command
  duplicates an existing engine setting. (players/config.ini:41-46, 97, 12, 179;
  refs/heads/main:src/client/component/engine_perf.cpp:433-448)
- **verified**: The engine renders the 3D scene at a resolution separate from
  the display. vidConfig_t holds scene and display dimensions (size 0x38).
  ResolutionPercent ('Percentage of window resolution that the 3D scene renders
  at') and r_sceneResolutionMultiplier are in the menus. Dynamic-resolution and
  cubic-upsample dvars exist by name. (src/client/game/structs/r.hpp:33-66;
  src/client/game/symbols/r.hpp:31; players/config.ini:90-91;
  `data/ui_scripts/graphics_settings/__init__.lua:56-65`;
  data/lookup_tables/dvar_list.txt (`r_dynamicSceneResolution*`,
  `r_autoResolution*`, r_enableCubicUpsample, r_cubicUpsampleParam))
- **verified**: Temporal and motion signals exist by name:
  r_aaTechnique/AATechnique, r_aaAllowTemporalMultiGpu, `r_smaa*`,
  r_motionVectorGenerateEnable, r_motionVectorStaticComputeAsync,
  r_motionVectorStaticWithMB, `r_motionBlur*`, and per-object MotionBlur
  Off/Auto/On. The lookup table has no dvar names containing jitter, t2x or
  filmic. (data/lookup_tables/dvar_list.txt; players/config.ini:165-170)
- **verified**: refdef_t contains scene viewport floats, sceneResolutionIndex
  plus a 4-entry history, tanHalfFovX/Y, fov_x, vieworg/lastvieworg, zNear/zFar,
  cameraVelocity, exposure/linearExposure, enableOIT and debugResolveTarget. It
  is not wired into cg_t (that field is commented out) and has no size
  assertion. cgs_t is size-asserted (0x1E940), holds view and sceneViewport
  dimensions, and lives in boiii's static cgsArray.
  (src/client/game/structs/level/cl.hpp:1121-1258, 1405, 1943-1984;
  src/client/game/symbols/cg/core.cpp:9;
  src/client/component/client_patches.cpp:303-306)
- **verified**: `R_*` symbol coverage is minimal: R_StoreWindowSettings,
  R_Stream{Push,Pop}Disable, R_CopyTextureRegionMips (semantics unknown),
  vidConfig, R_AddCmdDrawText, the Gfx texture-pool helpers,
  Image_AssignDefaultTexture and s_world. There are no symbols for the device,
  swap chain, render targets, postfx, resolve/upsample, projection or jitter.
  (src/client/game/symbols/r.hpp:7-31; render.hpp:8-11; gfx.hpp:8-16;
  db/xasset/world.hpp:9-10; grep for projection/jitter/RenderTarget over
  src/client/game = none relevant)
- **verified**: GfxImage.texture.basemap is an `ID3D11ShaderResourceView*`.
  ugc.cpp already replaces engine image textures (img_t7_mod_preview,
  img_t7_mod_loading) with PNGs through Gfx_LoadTextureFromPng. That works for
  mod-menu preview images and also gives a route to the engine's device
  (ID3D11DeviceChild::GetDevice). (src/client/game/structs/gfx/core.hpp:152-155,
  235-269; src/client/game/impl/ugc/ugc.cpp:325-391)
- **verified**: Asset override only supports Rawfile and ScriptParseTree, so
  shaders and techniques cannot be replaced through assets. A mod can ship its
  own default_bindings.cfg rawfile. (docs/asset-override.md (Asset Support;
  Examples))
- **verified**: The game ships d3dcompiler_46.dll, and the launcher's 'reduce
  stutter' toggle renames it to .bak.
  (src/client/launcher/launcher.cpp:2530-2555; install directory listing)
- **likely**: The r_dof_enable symbol's new and legacy entries appear swapped.
  They read new 0x14AE95478 / legacy 0x14AE164F8, while nearby r_vsync,
  r_fullscreen and s_world all have new = legacy - 0x7EF80, and r_dof_enable
  sits +0x3E0 from r_vsync in both builds. dof_enabled_stub dereferences this
  symbol directly on the new exe. (src/client/game/symbols/dvar/core.hpp:97-100,
  113-114; src/client/game/symbols/db/xasset/world.hpp:9;
  src/client/component/dvar_patches.cpp:112-129, 311-312; last touched 42a5d818
  before the triple reorder in 4b4fd331)
- **likely**: The legacy-to-new address delta is piecewise. It is 0 for most
  code below ~0x1411xxxxx; -0x20 around 0x1412B4-0x1413E6; +0xC3D0 across
  0x141A6F-0x141D10 (renderer region); +0xCA90 near 0x141E00; +0xC780 at
  0x1420E0-0x1420EC; +0x58AC0 from ~0x1420F1. On that basis the fork's legacy
  findings would sit near new 0x141C47C00 (RenderSurfaceList), 0x141CF4055 (draw
  wrapper) and 0x1420EC78E (FPS-limit jge). (Computed from symbol triples:
  r.hpp:7-10, render.hpp:11, patches.cpp:573, dvar_patches.cpp:165,307,
  com/core.hpp:68-77, game_settings.cpp:76, cg/core.hpp:33, cl.hpp:13-44,
  scr/place.hpp:8; the predicted fork addresses are extrapolation)
- **verified**: A reusable pattern scanner exists (utils::hook::signature with a
  parallel/SSE scan and a _sig literal over the game image), but only arxan.cpp
  uses it. All render, perf and input hooks use hard-coded addresses.
  (src/common/utils/signature.hpp:5-46; grep _sig over src/client = arxan.cpp
  only)
- **likely**: Component post_unpack is triggered from the game's early import
  stubs (SteamAPI_RestartAppIfNecessary or SetProcessDPIAware), so post_unpack
  runs before the renderer creates its device. Device and swap-chain creation
  hooks installed there would see creation. (src/client/main.cpp:52-74, 103-110
  (creation timing itself not verified))
- **verified**: arxan.cpp replaces the game's user32!GetSystemMetrics IAT entry
  (returning 0 for SM_REMOTESESSION), so display work must not touch that IAT
  entry. (src/client/component/arxan.cpp:712-718, 836-840 (identified only, not
  analyzed))
- **verified**: The installed game's own players/config.ini currently has
  FullScreenMode=2 at 3840x2160 and RefreshRate 239.991 (documented range 1-240)
  on Monitor 1, with MaxFPS 240, Vsync 1 (fullscreen-only), MaxFrameLatency 1,
  SerializeRender 0, ResolutionPercent 100, AATechnique FXAA and MotionBlur
  Auto. The install has no boiii_players directory yet. (players/config.ini:6,
  42, 46, 72, 80, 82, 85, 88, 91, 165, 168; directory listing of the game
  folder)

## Details

# Rendering / perf / display / input: Xerces area notes

## 0. Which addresses apply

- Installed `BlackOps3.exe` (read-only PE header read): CheckSum **0x6531394**,
  TimeDateStamp 0x6A7C... = 2026-08-11 18:00:53Z, size 106,059,256, ImageBase
  0x140000000, SizeOfImage 0x1D75BC00. Upstream `is_new_client()` keys on this
  checksum (`src/client/game/base.cpp:36-37`). `main.cpp:124-125` calls it
  `supported_client_checksum`.
- Every `symbol<T>{a, b, c}` / `game::select(a, b, c)` is **{new client
  (Sept-2026 exe), legacy client (0x888C368), server (0x14C28B4)}**
  (`ptr.hpp:62-84`).
- The fork (base d80d308, 2026-04-09) identified the client as 0x888C368, so
  **all fork RVAs are legacy-column addresses**.
- Legacy-to-new deltas, computed from upstream tables. They are piecewise, so
  ported addresses must be re-found by signature:

| new-exe region                  | legacy minus new                                                        | sample (new / legacy)                                                                                                                                                                                                                |
| ------------------------------- | ----------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| < ~0x1411xxxxx (most code)      | 0                                                                       | CG_CalcFOVfromLens 0x1404D6230 both; Com_FPSLimit 0x140F7CFD0 both; fov.cpp sites 0x1404DADA7/0x14090E735/0x140926D2A both                                                                                                           |
| 0x1412B4 to 0x1413E6            | -0x20                                                                   | CG_SightTracePoint 0x1412B43A0/0x1412B4380; CL_Frame 0x141351400/0x1413513E0; ScrPlace_Init 0x1413E5650/0x1413E5630                                                                                                                  |
| 0x141A6F to 0x141D10 (renderer) | +0xC3D0                                                                 | Scr num-expected-players 0x141A6F920/0x141A7BCF0; R_RegisterDvars site 0x141C9A873/0x141CA6C43; R_StoreWindowSettings 0x141CB17C0/0x141CBDB90; R_AddCmdDrawText 0x141CCD500/0x141CD98D0; R_StreamPushDisable 0x141D03A00/0x141D0FDD0 |
| ~0x141E00                       | +0xCA90                                                                 | live_uselpc site 0x141E00411/0x141E0CEA1                                                                                                                                                                                             |
| 0x1420E0 to 0x1420EC            | +0xC780                                                                 | Com_Frame_Try_Block_Function 0x1420EC680/0x1420F8E00; gamesettings 0x1420E0907/0x1420ED087                                                                                                                                           |
| ~0x1420F1 and up                | +0x58AC0 (others further up: +0x5CB20 at 0x14226, +0x79170 at 0x1422B9) | Com_ShutdownUILevel 0x1420F1250/0x142149D10; dvar sites 0x1420F945A/0x142151F1A                                                                                                                                                      |

Predicted new-exe locations of the fork's legacy findings. These are
**unverified extrapolation**; use them only as anchors when writing signatures:

- FPS-limit `jge` at 0x1420EC78E
- RenderSurfaceList at 0x141C47C00
- RenderSurfaceRange at 0x141C47F80
- hot loop call site at 0x141C48136
- draw wrapper at 0x141CF4055

## 1. Upstream hook inventory (render / display / perf / input)

| file:line                                 | kind                                         | target {new, legacy, server}                                                                               | purpose                                                                                                       |
| ----------------------------------------- | -------------------------------------------- | ---------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| download_overlay.cpp:404-453              | MinHook detour of dummy WARP chain vtable[8] | dxgi IDXGISwapChain::Present                                                                               | ImGui overlay; lazy device/ctx/HWND capture (353-380)                                                         |
| download_overlay.cpp:372-375              | SetWindowLongPtrA(GWLP_WNDPROC)              | game HWND from swap-chain desc                                                                             | ImGui input (passthrough)                                                                                     |
| scheduler.cpp:272                         | detour                                       | SND_EndFrame {0x142215FD0, 0x142272B00, 0x140547DF0}                                                       | runs `scheduler::renderer` (frontend end-of-frame, not Present)                                               |
| scheduler.cpp:275-276                     | detour                                       | Com_Frame_Try_Block_Function {0x1420EC680, 0x1420F8E00, 0x1405020E0}                                       | `scheduler::main`                                                                                             |
| client_patches.cpp:564-567                | detour returning UINT32_MAX                  | Com_FPSLimit {0x140F7CFD0, 0x140F7CFD0, 0}                                                                 | remove hidden FPS cap (4ef9e224)                                                                              |
| client_patches.cpp:464-465                | nop 6                                        | {0x1422BBB28, 0x142334C98, 0}                                                                              | engine must not change process priority (2d9ff773)                                                            |
| client_patches.cpp:58-77, 458             | SetProcessAffinityMask                       | mask (1<<min(N,4))-1 for 1 s, then system mask                                                             | AMD stutter fix (feacde16)                                                                                    |
| client_patches.cpp:502-510                | 4 call patches                               | CL_AdjustAngles {0x1412F3344, 0x1412F3364, 0x1412F33A0, 0x1412F33C1; legacy -0x20}                         | cl_yawspeed/cl_pitchspeed (DVAR_NONE, 492-499)                                                                |
| fov.cpp:34-35 / 38-39 / 42-43             | call patch / u32 write / 0xC3                | 0x1404DADA7 / 0x14090E735 / 0x140926D2A (same in both builds)                                              | cg_fovScale multiply, ARCHIVE flag, no reset                                                                  |
| render.cpp:59-61                          | detour (only with -ultrawide)                | R_StoreWindowSettings {0x141CB17C0, 0x141CBDB90, 0x140386C30}                                              | view/display = scene dims when aspect > 16:9                                                                  |
| render.cpp:63-68                          | detour (client) / once (server)              | ScrPlace_Init {0x1413E5650, 0x1413E5630, 0}                                                                | safeArea dvars 1.0, cg_hudSplitscreenScale = 1                                                                |
| dvar_patches.cpp:311-312                  | jump to asm stub (112-129)                   | {0x141116edb, 0x141116EBB}; resumes {0x141116eeb/0x141116ECB} or CG_UpdateAdsDof {0x141116ee2/0x141116EC2} | ADS DOF follows r_dof_enable (reads the possibly-swapped symbol)                                              |
| dvar_patches.cpp:89-99                    | flags by name                                | r_dof_enable, r_lodbiasrigid, gpad_stick_deadzone_max/min, cg_drawLagometer to ARCHIVE                     | persistence                                                                                                   |
| dvar_patches.cpp:101-110, 292             | flags by name every 5 s                      | remove CHEAT on cg_drawGun, g_speed, bg_gravity, player_sustainAmmo, r_fog, timescale                      | tweaks menu                                                                                                   |
| dvars.cpp:181-182                         | detour                                       | Dvar_SetVariant {0x14226C510, 0x1422C9030}                                                                 | autosave archive dvars to boiii_players/user/config.cfg, 10 s debounce (129-149)                              |
| startup_flags.cpp:12-23                   | dvar set at dvars_loaded                     | r_fullscreen 2 / 0, r_vsync 0                                                                              | -borderless / -windowed / -safe                                                                               |
| intro.cpp:38-42                           | detour (flags only)                          | Cinematic_StartPlayback {0x1412BE3C0, 0x1412BE3A0}                                                         | -nointro / -nocinematics                                                                                      |
| branding.cpp:35-38, 53                    | renderer pipeline call                       | R_AddCmdDrawText {0x141CCD500, 0x141CD98D0}; font ptr {0x1432D2918, 0x1433519B0, 0x1410E1BE0}              | watermark (engine 2D text)                                                                                    |
| branding.cpp:56-57                        | string write                                 | {0x142FC02D8, 0x14303F3D8}                                                                                 | window title prefix 'EZZ' (rename target)                                                                     |
| lua/ui_scripting.cpp:2205-2228, 2559-2560 | detour                                       | R_CopyTextureRegionMips {0x141CAB1F0, 0x141CB75C0, 0x140383950}                                            | crash guard; calls game::readable_ptr (VirtualQuery) per call, which ptr.hpp:181-185 warns is slow on Windows |
| binds.cpp:231-242, 271-274                | function-pointer swap in cmd_function_s      | bind / unbind / unbindall                                                                                  | persist custom binds                                                                                          |
| main.cpp:564-574, 1005                    | API                                          | SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)                                                        | DPI (process start, before the game's own SetProcessDPIAware)                                                 |
| main.cpp:600-621, 1061                    | registry                                     | HKCU\Software\Microsoft\DirectX\UserGpuPreferences = 'GpuPreference=2;'                                    | dGPU preference                                                                                               |

Other display symbols available: `cls_vidConfig` {0x14536571c, 0x1453E471C,
0x1428EB3CC}, `cls_forceFullscreen` {0x14561C254, 0x14569B254, 0x142BA1F04},
`cls_exitFullscreen` {0x14561C258, 0x14569B258, 0x142BA1F08}, `CL_InitRenderer`
{0x141359710, 0x1413596F0} (cl.hpp:37-38, 74-78). `r_vsync` {0x14AE16118,
0x14AE95098, 0x147CBC1E0} and `r_fullscreen` {0x14AE15E78, 0x14AE94DF8,
0x147CBBF48} are at dvar/core.hpp:97-100. `r_dof_enable` {0x14AE95478,
0x14AE164F8, 0x147CBC5B8} (dvar/core.hpp:113-114) is likely swapped (see facts).
`s_wcd::*` are the external console window, not the game window. There is no
symbol for the game HWND.

## 2. Fork hook inventory (refs/heads/main, legacy exe only)

| fork file:line                                      | kind                                          | target                                                                                                    | purpose / result                                                                                                   |
| --------------------------------------------------- | --------------------------------------------- | --------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| download_overlay.cpp present_stub (first statement) | direct call                                   | engine_perf::intercept_present(sc, sync_interval&, flags&) (engine_perf.cpp:452-507)                      | Present count, last sync/flags, render TID, one-time stack capture, sync_interval override, SetMaximumFrameLatency |
| engine_perf.cpp:297-361                             | MinHook detours via dummy WARP context vtable | ID3D11DeviceContext: 12 DrawIndexed, 13 Draw, 14 Map, 15 Unmap, 20 DrawIndexedInstanced, 21 DrawInstanced | per-frame draw/map counts, L1/L2 caller histograms (lock-free 512-entry tables), skip list                         |
| engine_perf.cpp:248-295                             | detour                                        | RenderSurfaceList, legacy RVA 0x1C53FD0                                                                   | divide SurfaceList.count (+0x10) by a factor; broken visuals, +5%                                                  |
| engine_perf.cpp:762-777                             | detours                                       | WaitForSingleObjectEx, WaitForMultipleObjectsEx                                                           | render thread made 0 wait calls                                                                                    |
| perf.cpp:347-365                                    | detours (opt-in)                              | kernel32 Sleep / SleepEx                                                                                  | histogram/skip; crash-prone (see section 7)                                                                        |
| perf.cpp:228-253                                    | 1-byte VirtualProtect write                   | legacy RVA 0x20F8F0E: 0x7D to 0xEB                                                                        | frame-limit loop never iterates (removes com_maxfps too); superseded upstream                                      |
| perf.cpp:278-308, 367-374                           | Win32 / NT API                                | SetPriorityClass, NtSetTimerResolution(finest), SetProcessAffinityMask                                    | runtime-only knobs                                                                                                 |

The fork's reverse-engineering notes are legacy RVAs:

- frame limiter: `mov eax,0x3E8; idiv ebx` (frame time in ms), with a loop of up
  to 0x32 iterations calling a throttle function, inside
  Com_Frame_Try_Block_Function.
- RenderSurfaceRange at +0x1C54350. SurfaceList+0x08 is the surface pointer
  array; +0x10 is the count.
- 4 draw dispatch helpers: +0x1CDF520, +0x1C54C60, +0x1C54AA0 and wrapper
  +0x1D00425 (DrawIndexedInstanced at +0x1D0045E).
- L2 hot sites: +0x1C54506 (91%), +0x1C772D1 (8%), +0x1CD62FD (2%).

## 3. D3D11 / DXGI: how it works today, and the proposed dispatcher

How it works today: the game's chain is only seen inside the Present detour.
download_overlay calls `GetDevice`, then `GetImmediateContext`, then
`GetDesc().OutputWindow` (download_overlay.cpp:353-375). Another route to the
device is any engine `GfxImage.texture.basemap` (an ID3D11ShaderResourceView),
via `ID3D11DeviceChild::GetDevice`.

**The 'MinHook refuses a second hook' issue.** MinHook keys hooks by target
address (`FindHookEntry`, then `MH_ERROR_ALREADY_CREATED`; minhook hook.<local
scratch copy, not kept>588-589, 656-658). `utils::hook::detour::create` throws
on any non-OK status (hook.cpp:239-250). Consequences:

- Two components each trying to detour `vtable[8]` throws at startup.
- Plugins (plugins.cpp loads DLLs and calls p_name/post_unpack/pre_destroy) or
  Lua `package.loadlib` DLLs that ship their own MinHook can silently chain a
  second inline hook.
- Steam, Discord or RTSS overlays may also hook Present.

Vtable slots (standard dxgi.h/d3d11.h COM layout, not from the repo). Prefer
computing them with `utils::hook::get_vtable_entry(obj, &IFace::Method)`
(hook.hpp:27-50):

- IDXGISwapChain: 8 Present, 9 GetBuffer, 10 SetFullscreenState, 11
  GetFullscreenState, 12 GetDesc, **13 ResizeBuffers**, 14 ResizeTarget, 16
  GetFrameStatistics
- IDXGISwapChain1: **22 Present1**
- IDXGISwapChain2: 31 SetMaximumFrameLatency, 33 GetFrameLatencyWaitableObject
- IDXGISwapChain3: **39 ResizeBuffers1**
- ID3D11DeviceContext: 12 DrawIndexed, 13 Draw, 14 Map, 15 Unmap, 20
  DrawIndexedInstanced, 21 DrawInstanced, 33 OMSetRenderTargets

Current overlay defects:

1. No ResizeBuffers or Present1 hooks.
2. Renders into the engine's currently bound RTV.
3. Runs NewFrame/Render, two Map calls and a full DX11 state backup every frame
   even when idle (imgui_impl_dx11.cpp:170-232 has no empty early-out).
4. Input is not captured: the handler returns 0 for mouse and keyboard
   (imgui_impl_win32.cpp:808-831).
5. ImGui IO is written on the WndProc thread while NewFrame runs on the Present
   thread. SerializeRender=0 uses a separate render thread, so this is likely a
   data race.
6. ImGui is bound to whichever chain presents first (no game-chain filter).
7. Panels are fixed pixel sizes with no DPI or resolution scale.
8. The subclass uses SetWindowLongPtrA. A subclass chain using the A variant can
   change the window's Unicode-ness (unverified).

**Proposed `gfx_hooks` dispatcher.** It is the single owner; no other code hooks
DXGI or D3D11.

```cpp
namespace xerces::gfx {
enum class stage : uint8_t { begin = 0, overlay = 20, pacing = 30, end = 40 };
struct frame_info { IDXGISwapChain* sc; ID3D11Device* dev; ID3D11DeviceContext* ctx; HWND hwnd;
                    UINT w, h; DXGI_FORMAT fmt; uint64_t index; int64_t qpc, qpc_prev; };
struct present_args { UINT& sync_interval; UINT& flags; };
using present_fn = void(*)(const frame_info&, present_args&, void* user);
using resize_fn  = void(*)(const frame_info&, bool before, UINT new_w, UINT new_h, void* user);
uint32_t on_present(stage, const char* name, present_fn, void* user);
uint32_t on_post_present(const char* name, present_fn, void* user);
uint32_t on_resize(const char* name, resize_fn, void* user);
void     remove(uint32_t id);
ID3D11RenderTargetView* backbuffer_rtv();      // lazy; released in resize(before)
void     with_backbuffer_bound(void(*)(void*), void*); // OMGet / OMSet / restore + viewport
}
extern "C" __declspec(dllexport) uint32_t xerces_gfx_on_present(int stage, const char*, void*, void*); // plugin ABI
```

What it does:

- Hooks Present (8), ResizeBuffers (13), and Present1 (22) / ResizeBuffers1 (39)
  when QueryInterface succeeds.
- Binds to the game chain: OutputWindow equals the engine top-level window and
  the chain keeps presenting. It ignores WARP/dummy and foreign chains.
- Runs callbacks sorted by stage, each wrapped in SEH/try. A callback that
  faults is disabled and logged.
- ImGui becomes one context with registered panels; download_overlay becomes a
  panel. NewFrame/Render are skipped entirely when no panel is visible.
- Owns a single input router: one subclass (SetWindowSubclass or the W variant)
  feeding an SPSC queue that is drained before NewFrame. While a modal panel
  wants capture it swallows WM_INPUT, `WM_KEY*`, WM_CHAR and `WM_MOUSE*` for the
  game.
- The fork's `intercept_present` becomes a `begin`-stage telemetry callback; its
  sync override moves to the `pacing` stage.
- Upscalers do **not** use Present (see section 8).

## 4. Dear ImGui

- Single user: download_overlay.cpp (download progress window 'Downloading
  Content' with progress, MB, speed, ETA and Stop; Yes/No confirmation plus a
  blocking variant). Called from fastdl.cpp, steamcmd.cpp and workshop.cpp.
- Init flags: IniFilename/LogFilename null,
  `ImGuiConfigFlags_NoMouseCursorChange`, custom orange theme
  (download_overlay.cpp:42-152, 362-367).
- Version 1.93.0 WIP (imgui.h:32-33); static lib with DX11 + Win32 backends;
  `IMGUI_IMPL_WIN32_DISABLE_GAMEPAD` (deps/premake/imgui.lua:33). Added in
  12b57f64 (2026-02-19).
- For controller navigation it would need the define removed,
  `ImGuiConfigFlags_NavEnableGamepad` set, and the game's pad suppressed while
  open, because the engine reads the pad too. LUI is the better host for
  anything controller-facing.

## 5. Display options matrix

| feature              | engine native (config.ini / dvars)                                                                               | boiii today                                                                                                                      | gap for Xerces                                                                                                                                                                   |
| -------------------- | ---------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| FOV                  | FOV 65-120, horizontal at 16:9 (cg_fov_default); cg_fov_comp_max, cg_fov_default_nocomp, cg_fovMin exist by name | Lua Video slider 65-120; Quick Settings and Game Tweaks lists (65-120 step 5); launcher FOV field; cg_fovScale 0.5-2.0 (fov.cpp) | >120 or aspect-compensation cap not exposed; viewmodel FOV not exposed (only cg_gun_x/y/z, `cg_gun_fovcomp_*`); cg_fovScale applies to all views, not only ADS as its label says |
| Borderless           | FullScreenMode 2 'Fullscreen Window'                                                                             | -borderless; launcher dropdown; Lua dropdown                                                                                     | works; AutoRestoreFullscreen and WindowX/Y not exposed                                                                                                                           |
| Resolution / refresh | WindowSize / r_mode; RefreshRate (documented 1-240, mode 1 only)                                                 | launcher text fields; Lua dropdowns                                                                                              | >240 Hz clamp unverified; free-text inputs not validated                                                                                                                         |
| Render scale         | ResolutionPercent / r_sceneResolutionMultiplier                                                                  | Lua dropdown (in-game only)                                                                                                      | missing in launcher; needed by FSR                                                                                                                                               |
| Multi-monitor        | Monitor 0-8; WindowX/Y (vid_xpos/vid_ypos)                                                                       | Lua monitor dropdown (disabled when windowed); per-monitor-v2 DPI                                                                | no launcher monitor picker; splash on primary; overlay not DPI-aware; no spanning                                                                                                |
| Ultrawide            | Hor+ FOV                                                                                                         | -ultrawide (UI expanded to full aspect; unstable per README:213)                                                                 | HUD/menu 16:9 clamp, cinematics letterbox, in-UI toggle; GfxWindowParms struct needs fixing                                                                                      |
| Key rebinding        | Controls menu; bindings_0.cfg (keyboard + `BUTTON_*` / `DPAD_*`, bind2); gpad_buttonsConfig/sticksConfig presets | binds.cpp persistence (binds.cfg); launcher Binds tab                                                                            | no pad remap UI beyond presets; no Xerces action binds (for example 'open mods menu'); batches over 3 dropped (binds.cpp:86-88); cl_yawspeed/pitchspeed not archived             |
| VSync / FPS cap      | Vsync (fullscreen only), MaxFPS 0-1000, SmoothFramerate                                                          | Lua slider 0-500, quick list, launcher                                                                                           | none                                                                                                                                                                             |
| Latency / threading  | MaxFrameLatency 0-4, SerializeRender 0-2, BackbufferCount 2-3                                                    | launcher field and presets                                                                                                       | none (do not duplicate in code)                                                                                                                                                  |

The Video menu bug: `graphics_settings/__init__.lua:1` compares the function
`Engine.UsingModsUgcName` to 'usermaps', so the replacement menu is skipped
whenever a mod is loaded.

## 6. Dvar and setting lists

Names come from `data/lookup_tables/dvar_list.txt` (boiii's hash-to-name table).
That a name exists there does not prove the dvar is active in the new exe.

- **Display:** r_fullscreen, r_previousFullscreen, r_mode, r_monitor,
  r_monitorCount, r_currentMonitor, r_refreshRate, r_vsync,
  r_vsync_tear_window_enable/lower/upper, r_backBufferCount, r_videoMode
  (gamma), r_sceneBrightness, vid_xpos, vid_ypos, r_vid_align, r_forceAdapter,
  r_monitorTV, r_forceMonitorTV, r_uhdMode, com_wideScreen, wideScreen,
  safeArea_horizontal, safeArea_vertical, ui_safearea, UI_safeAreaOverride,
  r_scaleViewport, r_finalShiftX/Y, r_num_viewports
- **FOV:** cg_fov, cg_fov_default, cg_fov_default_nocomp,
  cg_fov_default_thirdperson, cg_fov_comp_max, cg_fovMin, cg_fovScale,
  cg_gun_fovcomp_x/y/z, cg_gun_x/y/z, r_useLensFov, r_activeLensID
- **Frame / perf:** com_maxfps, com_smoothFrames, com_maxFrameTime,
  com_drawFPS_PC, cg_drawFPS, cg_drawFPSScale, r_drawFrameDurationGraph,
  r_gpuTimers, r_gpuFrameHistogram, r_smp_backend, r_smp_worker_threads,
  r_cmdbuf_worker, r_cmdlistChain, r_asyncCompute, r_framesyncThreshold,
  r_submitDoneThreshold, r_backendSwapBuffersAssistRelease,
  r_lockFrameRateTo30Hz(_enable/_force/_dynResOff),
  r_emulateSlowOrHitchyPresent, r_useStrict30HzConditions
- **Resolution / upscaling:** r_sceneResolution, r_sceneResolutionMultiplier,
  r_dynamicSceneResolution, r_dynamicSceneResolutionMode, r_autoResolutionScale,
  r_autoResolutionThreshold, r_autoResolutionTune,
  r_autoResolutionControllerIdle, r_autoResolutionUIThreshold,
  r_autoResolutionUITune, r_lodUseDynamicResolution, r_enableCubicUpsample,
  r_cubicUpsampleParam, r_lowResolutionMode, r_fuzzSceneWidth, r_chkbMode,
  r_chkbDiffWeight, r_chkbSameWeight, r_chkbTexLodBias, r_resolveMethod,
  r_textureMipLodBias, r_texFilterQuality, r_forceTextureAniso
- **AA:** r_aaTechnique, r_aaAllowTemporalMultiGpu, r_smaa, r_smaaCompute,
  r_smaaHighQuality, r_smaaQuincunx, r_fxaaContrastThreshold,
  r_fxaaSubpixelRemoval, r_fxaaDebug. There are no t2x, filmic or jitter names.
- **Motion:** r_motionVectorGenerateEnable, r_motionVectorStaticComputeAsync,
  r_motionVectorStaticWithMB, r_motionVectorDebugDisplay, r_motionBlurEnable,
  r_motionBlurMode, r_motionBlurQuality, r_motionBlurStrength
- **Input:** gpad_enabled, gpad_button_deadzone, gpad_stick_deadzone_min/max,
  gpad_button_lstick_deflect_max, gpad_button_rstick_deflect_max,
  gpad_menu_scroll_delay_first/rest, gpad_stick_pressed,
  gpad_stick_pressed_hysteresis, gpad_debug, in_mouse, m_pitch, m_yaw, m_filter,
  m_mouseAcceleration, m_mouseFilter, m_mouseSensitivity, sensitivity,
  cl_mouseAccel, cl_freelook, cl_anglespeedkey, cl_yawspeed and cl_pitchspeed
  (boiii-registered), key_bindsDebug, keyarchiveWriteDelay, bind, bind2

Engine `players/config.ini` keys, with line numbers from the installed game's
file. The ini-to-dvar mapping is likely, not verified:

| key                                                                               | line | maps to      |
| --------------------------------------------------------------------------------- | ---- | ------------ |
| MaxFPS 0-1000                                                                     | 6    | com_maxfps   |
| DrawFPS                                                                           | 9    |              |
| SmoothFramerate                                                                   | 12   |              |
| FOV 65-120                                                                        | 15   |              |
| MaxFrameLatency 0-4                                                               | 42   |              |
| SerializeRender 0-2                                                               | 46   |              |
| VideoMemory                                                                       |      |              |
| RestrictGraphicsOptions                                                           |      |              |
| FullScreenMode                                                                    | 72   | r_fullscreen |
| WindowX / WindowY                                                                 | 75   |              |
| WindowSize                                                                        | 80   |              |
| RefreshRate 1-240                                                                 | 82   |              |
| Monitor 0-8                                                                       | 85   |              |
| Vsync (fullscreen only)                                                           | 88   |              |
| ResolutionPercent                                                                 | 91   |              |
| DisplayGamma                                                                      |      |              |
| BackbufferCount 2-3                                                               | 97   |              |
| AATechnique                                                                       | 165  |              |
| MotionBlur Off/Auto/On ('per-object')                                             | 168  |              |
| MotionBlurQuality                                                                 | 170  |              |
| OIT, OITLayers, SSAOTechnique ('GTAO Low Quality' is valid per launcher.cpp:2680) |      |              |
| AutoRestoreFullscreen                                                             | 176  |              |
| ShowPerformanceGraph 0-16                                                         | 179  |              |
| StreamMinResident                                                                 |      |              |

## 7. Performance

**Upstream:**

- Com_FPSLimit detour returns UINT32_MAX (the hidden cap the fork measured at
  ~200).
- Engine priority change NOPed.
- 4-logical-CPU affinity for 1 s at the first main frame.
- dGPU registry preference.
- Engine-native MaxFrameLatency, SerializeRender and SmoothFramerate.
- Present statistics, timer resolution and frame pacing are not implemented.

**Fork experiments:**

| experiment                                                                                            | outcome                                                                                                                                                                             |
| ----------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| HIGH priority + finest timer in post_load                                                             | crash at 'Loading fastfile core_patch', process exit, no dump, both runs (2211a934); safe when applied from the console at runtime                                                  |
| Sleep/SleepEx detours: skip, Sleep(0), SwitchToThread, 1-in-N yield, Com_IsInGame-gated, logging-only | all crashed during transitions or fastfile loads, the logging-only one during zm_patch (2ea11406); the histogram also used a std::mutex inside the hot hook (engine_perf.cpp:51-54) |
| NtDelayExecution hook                                                                                 | removed before testing as too risky (2211a934)                                                                                                                                      |
| DB_LoadXAssets detour at 0x1414236A0                                                                  | game failed to boot (fork client_patches.cpp comment)                                                                                                                               |
| printf from the cmd_watch_loop thread before the console was ready                                    | crash (bfca64a6)                                                                                                                                                                    |
| sync_interval forced to 0                                                                             | 0% change                                                                                                                                                                           |
| Sleep skip (5B+ calls suppressed)                                                                     | 0% change                                                                                                                                                                           |
| Wait hooks                                                                                            | render thread made 0 waits                                                                                                                                                          |
| RenderSurfaceList count / 2 and / 4                                                                   | 2602 -> 1467 -> 899 draws/frame, ~205 -> 213 -> 214 FPS, visuals broken                                                                                                             |
| Draw counts                                                                                           | ~22M draws over ~9k frames in zombies (~2,450/frame); frontend 666 draws and 299 Maps per frame; L2 capture costs ~4%                                                               |
| fps_unlock (1-byte patch)                                                                             | 200 -> 210-300 FPS in zombies, no animation/audio/input issues reported                                                                                                             |
| Baselines                                                                                             | 60 FPS with vsync by default (com_maxfps 240, r_vsync 1); ~200 FPS with r_vsync 0 + HIGH + timer (GPU ~58%, single-thread CPU-bound); 243 FPS on the attract screen (cap 250)       |

## 8. Upscaling feasibility, from the code

What exists:

- Scene vs display split: vidConfig_t (structs/r.hpp:33-66; symbols/r.hpp:31)
  and cls_vidConfig.
- `cgs_t.sceneViewport*` (cl.hpp:1943-1984, readable via boiii's static
  cgsArray).
- `refdef_t.sceneViewport*`, sceneResolutionIndex and a 4-entry history
  (cl.hpp:1127-1132). This is unverified layout and not wired into cg_t.
- Render-scale settings: ResolutionPercent / r_sceneResolutionMultiplier, plus
  the dynamic-resolution dvars. The engine has its own upsampler:
  r_enableCubicUpsample and r_cubicUpsampleParam.
- Camera data in refdef_t (unverified layout): tanHalfFovX/Y,
  maincamTanHalfFovY, fov_x, vieworg/lastvieworg, viewaxis, zNear/zFar,
  cameraVelocity, frametime, exposure/linearExposure, enableOIT,
  debugResolveTarget (cl.hpp:1121-1258).
- GfxCameraData has autoMB and mbStrength (gfx/core.hpp:869-899).
- Formats: GfxPixelFormat includes D24S8, D24FS8, D16, D32F, R16G16F,
  R11G11B10F, R16G16B16A16F (gfx/core.hpp:182-231).
- Engine images expose D3D11 SRVs directly (GfxImage.texture.basemap).

What is missing:

- Symbols for the render targets (scene color, depth, velocity, GBuffer), the
  resolve/upsample/postfx/UI-composite passes, the projection matrix, the TAA
  jitter sequence, and the device/swap-chain globals.
- The values of r_aaTechnique on PC (the user's config uses FXAA; the launcher
  presets only write FXAA).

Requirements by upscaler:

| need                | FSR1                                                                                                                       | FSR2/3, DLSS, XeSS                                                                                                                                   |
| ------------------- | -------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| lower render res    | ResolutionPercent                                                                                                          | same                                                                                                                                                 |
| insertion before UI | must replace the engine's scene-to-display upsample (UI is composited at display res); **not** at Present                  | same                                                                                                                                                 |
| depth               | none                                                                                                                       | need depth RT (format candidates D24S8/D32F)                                                                                                         |
| motion vectors      | none                                                                                                                       | engine makes per-object MVs (MotionBlur, r_motionVectorGenerateEnable); static/camera MVs may depend on r_motionVectorStaticWithMB; encoding unknown |
| jitter              | none                                                                                                                       | temporal AA exists (r_aaAllowTemporalMultiGpu) but jitter is not located                                                                             |
| mip bias            | recommended                                                                                                                | required: r_textureMipLodBias                                                                                                                        |
| shaders             | ship precompiled DXBC (d3dcompiler_46.dll may be renamed by launcher.cpp:2530-2555; asset override cannot replace shaders) | SDK runtime                                                                                                                                          |
| API                 | DX11 fine                                                                                                                  | external, unverified: official FSR2/3/4 SDKs target DX12/Vulkan, DLSS supports DX11, XeSS DX11 is limited                                            |

Discovery approach for the upsample pass: run with ResolutionPercent < 100 in a
dev build. Temporarily enable draw hooks through the dispatcher, then find the
draw whose bound SRV has scene dimensions while the bound RTV has display
dimensions. Derive a signature from its caller.

## 9. Input and controller

- No XInput or raw-input code in boiii. The fork claims, unverified, that BO3
  uses Raw Input scancodes (its bundle_send_key comment).
- The engine binds pad buttons in bindings_0.cfg. The game ships a Steam Input
  template (controller.vdf) mapping to xinput buttons.
- LUI menus have native pad support (`Enum.LUIButton.*`,
  `gpad_menu_scroll_delay_*`). Mods-menu path: Lua ui_scripts in the style of
  quick_settings/tweaks, plus `Engine.*` bindings exposed by ui_scripting
  (already sets Engine.IsBOIII at ui_scripting.cpp:1024-1028). Open it from C++
  the way toast.cpp does. Show preview images with the ugc.cpp texture-swap
  technique.
- Launcher: a WebView2 page (html_frame.cpp:219-285, external host object) with
  only Escape and Enter key handling (main.js:483-492, 3343-3346). Controller
  navigation would need Gamepad API polling in main.js (WebView2/Chromium
  behavior unverified), or native XInput polling in the launcher pushing
  navigation events through ExecuteScript.
- ImGui cannot capture input today. If it gains gamepad navigation, the game
  would act on the same pad presses unless pad input is gated.

## Design implications

- Create one gfx_hooks dispatcher component as the only owner of DXGI/D3D11
  hooks: Present (8), ResizeBuffers (13), and Present1 (22) / ResizeBuffers1
  (39) when available. Compute slots with utils::hook::get_vtable_entry. Give it
  staged, SEH-isolated callbacks (begin, overlay, pacing, end, post-present,
  resize before/after), a game-swapchain filter, lazy backbuffer RTV management
  with OM state save/restore, and a C ABI export so plugins never hook DXGI
  themselves.
- Move download_overlay onto the dispatcher as an ImGui panel. Use one ImGui
  context. Skip NewFrame/Render when no panel is visible. Scale fonts and panels
  to the backbuffer size and DPI.
- Build one input router: a single subclass (SetWindowSubclass or the W variant)
  feeding a thread-safe queue that is drained on the render thread before
  ImGui::NewFrame. While a modal Xerces panel wants capture, swallow keyboard,
  mouse and WM_INPUT for the game. Check whether the engine reads raw input
  through GetRawInputBuffer before relying on WndProc-level blocking.
- Build the custom in-game mods menu, mod-pack and load-order UI in LUI (Lua
  ui_scripts), following quick_settings/tweaks/graphics_settings. That gives
  keyboard, mouse and gamepad navigation and native button prompts for free.
  Expose `Engine.Xerces*` functions through ui_scripting, open menus from C++
  with the toast.cpp execute_lua pattern, and show mod preview images with the
  ugc.cpp Gfx_LoadTextureFromPng texture-swap technique. Keep ImGui for dev,
  perf and download overlays.
- Launcher controller navigation: add Gamepad API polling and spatial focus to
  main.js, or native XInput polling in the launcher pushing navigation events
  with ExecuteScript. Expose the missing engine keys there: Monitor,
  ResolutionPercent, AATechnique, MotionBlur, AutoRestoreFullscreen, WindowX/Y,
  SerializeRender, ShowPerformanceGraph. Create config.ini defaults if it does
  not exist.
- Do not reimplement engine display features (borderless = FullScreenMode 2,
  Monitor, ResolutionPercent, MaxFrameLatency, SerializeRender). Write Xerces UI
  over the engine settings. Fix the graphics_settings Lua mod guard. Turn
  -ultrawide into a real option with a 16:9 HUD/menu clamp, after correcting
  GfxWindowParms and the render.cpp comparison.
- Perf: keep upstream's Com_FPSLimit hook (it supersedes the fork's 1-byte
  limiter patch and keeps com_maxfps meaningful), the priority NOP and the AMD
  affinity window. Offer opt-in post-startup knobs only (timer resolution,
  priority class, hybrid-CPU P-core affinity for the main and render threads;
  benefit unverified). Never apply them in post_load. Drop all Sleep, Wait and
  NtDelayExecution hooks.
- Build a perf HUD from dispatcher QPC timestamps (frame time histogram, 1%/0.1%
  lows) with a cheap path through R_AddCmdDrawText on scheduler::renderer
  (branding.cpp pattern) that needs no D3D hooks. Expose the engine's
  ShowPerformanceGraph.
- Resolve offsets by signature: build a resolver on utils::hook::signature that
  finds every render, perf and input target at startup and cross-checks against
  the known triples for the three known checksums. Use the fork's legacy RVAs
  plus the piecewise deltas only as anchors when writing new-exe signatures.
  Disable features individually when their signatures fail.
- FSR1 plan: set the render scale with ResolutionPercent /
  r_sceneResolutionMultiplier and the mip bias with r_textureMipLodBias. Replace
  the engine's scene-to-display upsample draw (cubic upsample) with FSR1
  EASU+RCAS using precompiled DXBC, before UI composition and not at Present.
  Find the pass with dispatcher draw instrumentation in dev builds, then sign
  it.
- FSR2/3, DLSS and XeSS plan: first reverse-engineer and sign the depth RT,
  velocity RT (format and encoding; static vs per-object coverage) and the TAA
  jitter/projection source in the new exe. Verify the refdef_t layout or find it
  in cg_t. Treat DX11 SDK availability (DLSS yes; FSR2+/XeSS limited) as a
  gating decision, possibly needing a DX11-on-12 interop path or a different
  technique.
- Fix the r_dof_enable symbol column order, after verifying against the new exe,
  before Xerces relies on dof_enabled_stub.
- Persist cl_yawspeed and cl_pitchspeed (archive flag). Make binds.cpp's batch
  filter distinguish engine bulk loads from user or Xerces binds. Support Xerces
  actions as bindable commands, with conflict detection against bindings_0.cfg.

## Risks

- MinHook allows only one hook per target, and detour::create throws
  (hook.cpp:239-250). If any new component, or the ported fork engine_perf,
  hooks Present/ResizeBuffers separately, Xerces throws at startup. Plugin DLLs
  (plugins.cpp) and Lua package.loadlib DLLs with their own MinHook can silently
  chain on top.
- The Present detour is inline on dxgi's implementation, so it sees every swap
  chain (dummy WARP, Steam/Discord/RTSS/ReShade overlays). ImGui currently binds
  to whichever chain presents first.
- ImGui IO is written on the WndProc thread and read on the Present thread. With
  SerializeRender=0 these are likely different threads, so this is a data race
  in the current overlay.
- The overlay draws into whatever RTV the engine left bound and does not handle
  ResizeBuffers. Any RTV Xerces holds on the backbuffer will make ResizeBuffers
  fail (mode switch, alt-enter, resolution change) unless released first.
- The overlay costs 2 Map/Unmap calls plus a full DX11 state backup and restore
  per frame even when idle. That is small but measurable at 200-300 FPS, and it
  also polluted the fork's Map counts.
- Input passthrough: while ImGui panels are open the game still receives clicks,
  keys and probably raw mouse input. An ImGui-based interactive in-game menu
  would act on both the menu and the game.
- Startup-time process priority and timer-resolution changes crashed BO3 in fork
  tests, and upstream deliberately NOPs the engine's own priority change. Treat
  priority, affinity and timer changes as post-startup only and opt-in.
- Sleep/SleepEx/NtDelayExecution and `Wait*` detours crashed the game in many
  variants (even logging-only). Do not ship global kernel32 or ntdll wait/sleep
  hooks.
- Code-byte patches (like the fork's 1-byte jge->jmp) land in code that may be
  covered by the integrity-check regime owned by arxan.cpp/integrity.hpp. Do not
  touch those files; prefer the hook primitives upstream already uses and
  validate on the new exe.
- All render, perf and input addresses are hard-coded per build; the fork's are
  legacy-only. Address deltas are piecewise, so translating fork RVAs by a
  constant is unsafe and needs signatures.
- The r_dof_enable symbol's new and legacy columns look swapped. On the new exe
  dof_enabled_stub would read 8 bytes from the wrong global and dereference it
  (+0x28), which could crash on ADS or silently do nothing.
- refdef_t layout is unverified (not wired into cg_t, no size assert), and
  GfxWindowParms is explicitly marked incorrect. Building upscaler camera inputs
  on them without verification risks garbage motion or projection data.
- -ultrawide is documented as unstable. render.cpp's comparison is effectively
  only aspect > 16:9, so it applies to every wider-than-16:9 mode including
  16:10-adjacent mismatches. The Video Lua menu is disabled whenever a mod is
  loaded (comparison bug).
- binds.cpp drops any batch of more than 3 bind ops within 100 ms, so user
  scripts or Xerces bind presets can silently fail to persist.
  cl_yawspeed/cl_pitchspeed are not archived.
- Relying on runtime shader compilation for FSR fails if the launcher's 'reduce
  stutter' toggle has renamed d3dcompiler_46.dll.
- External/unverified: official FSR2/3/4 SDKs do not target DX11, and XeSS DX11
  support is limited. DLSS has a DX11 path. Windows 10 2004+/11 changed
  timer-resolution semantics (per-process, possibly ignored when occluded).
- R_CopyTextureRegionMips_Safe calls game::readable_ptr (VirtualQuery) on every
  call (ui_scripting.cpp:2205-2228). ptr.hpp:181-185 warns this throttles
  performance on Windows. Call frequency is unmeasured.
- Outside this area but it affects which address column is live: main.cpp
  contains an auto-downgrade path that downloads and replaces BlackOps3.exe from
  archive.org when the checksum is not 0x6531394 (main.cpp:118-480).

## Open questions

- Which r_aaTechnique values does the PC build accept (for example SMAA 1x/T2x,
  Filmic variants)? Does the temporal option apply sub-pixel projection jitter,
  and where in the new exe?
- Where is the scene-to-display upsample/composite pass (r_enableCubicUpsample /
  r_cubicUpsampleParam), and is UI always composited after it in every
  FullScreenMode?
- What are the velocity buffer format and encoding? Are static/camera motion
  vectors generated when MotionBlur=Off, given r_motionVectorStaticWithMB?
- Does the engine present through IDXGISwapChain::Present or Present1 in all
  modes? Which swap effect does it use? On mode or resolution change does it
  call ResizeBuffers or recreate the swap chain?
- Which threads own the game window/WndProc and Present under SerializeRender 0,
  1 and 2? Does the engine read mouse and keyboard through WM_INPUT messages or
  GetRawInputBuffer, and the pad through XInput directly?
- Is r_dof_enable's new-client address really 0x14AE164F8 (swapped entries)?
  What does dof_enabled_stub read today on the new exe?
- Is com_maxfps 0 truly 'unlimited' and safe once Com_FPSLimit returns
  UINT32_MAX (the fork disassembly shows 1000/idiv)?
- What process priority did the engine try to set at the NOPed site
  (0x1422BBB28)? Does the startup crash the fork saw relate to it?
- Should Xerces support the legacy exe (checksum 0x888C368) at all? If it only
  supports the September-2026 exe, the fork's RVAs serve only as
  reverse-engineering anchors.
- Does the engine clamp RefreshRate at 240 (config.ini documents 1-240), which
  would matter for 360/480 Hz monitors?
- Is the cg_fov_comp_max compensation the reason FOV caps out on 21:9/32:9, and
  should Xerces raise it?
- Where are refdef_t and its offset inside cg_t (cg_t is a partial definition
  with a 0x342720 size and time at 0x11A88C)?
- On the user's Core Ultra 9 285K, which logical CPUs are 0-3 (the AMD affinity
  window)? Would pinning the main and render threads to P-cores help BO3 more
  than Windows Thread Director?

## Fenced (do not modify)

- src/client/component/arxan.cpp (whole file): Arxan anti-tamper component.
  Includes the GetSystemMetrics IAT stub returning 0 for SM_REMOTESESSION
  (712-718, 836-840), the GetThreadContext stub (719+), integrity-check patching
  with signature scans (636-700) and the -dump implementation (768, 853).
  Display or perf work must not touch these, including the game's user32
  GetSystemMetrics IAT slot.
- src/client/component/integrity.hpp: list of integrity-checked code blocks
  (anti-tamper data). Some are in the renderer region (for example 0x141D0E9B4).
  Never edit it; code byte patches must not be planned around it.
- src/client/game/symbol.hpp:69-73 (symbol::call_safe) and
  src/client/game/game.hpp:14-17 (arxan::detail::set_address_to_call /
  callstack_proxy_addr): anti-tamper call-stack proxy. fov.cpp:14 uses it. Use
  as-is; do not modify.
- src/client/main.cpp:52-69 (restart_app_if_necessary_stub) and 75-111
  (patch_imports steam_api64 redirection incl.
  SteamAPI_RestartAppIfNecessary/SteamAPI_Init): Steam launch/ownership path.
  Component post_unpack is triggered from here (66-67, 72-74). Consume
  post_unpack only; do not change the trigger.
- src/client/component/steam_proxy.cpp and `src/client/steam/**`: Steam
  client/ownership integration, outside the display scope. Fence off.
- `src/common/utils/hardware_breakpoint.*` and fork commits 4df651e3 / e3fadd0d
  (all-thread hardware-breakpoint hunting): debug-register tooling that overlaps
  what arxan.cpp intercepts (GetThreadContext). Do not port into Xerces perf or
  RE tooling.
- src/client/component/client_patches.cpp:264-349 (store_tac_protected_allocs):
  workaround for Treyarch anti-cheat (TAC) protected allocations.
  Render-adjacent code depends on the static cgArray/cgsArray it creates. Leave
  unchanged.
