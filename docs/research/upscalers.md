# Upscalers and the BO3 renderer (external research)

> Machine-generated research notes from 2026-11-04, kept as evidence for `docs/DESIGN.md`.
> Code was read at upstream `main` 5aa7fac, `mod-stack` ac356c3c and the fork `refs/heads/main` 7805319.
> `path:line` citations refer to those commits. Treat claims as leads: anything marked runtime still needs the real game.

## Summary

Versions as of 2026-11-04: AMD FSR SDK 2.4.0 "Redstone" (2026-10-28; FSR Upscaling 4.1.2 and FSR 3.1.6), NVIDIA DLSS SDK 310.9.16 and Streamline 2.14.16 (both 2026-10-16), and Intel XeSS SDK 3.0.2 (2026-07-24).

Only two kinds of upscaler run natively in BO3's Direct3D 11 renderer with first-party support:
- **Spatial shaders.** FSR 1 (EASU + RCAS) and NVIDIA Image Scaling (NIS). Both are MIT-licensed and API-neutral.
- **DLSS Super Resolution through NGX D3D11.** It is proprietary and needs an RTX GPU.

The other upscalers do not support DX11 out of the box:
- **FSR 2.2.** Officially DX12 and Vulkan, but there is an AMD-written DX11 backend patch under MIT from 2023.
- **FSR 3.1.** DX11 exists only as unofficial community ports.
- **FSR 4.** Ships only as signed DX12/Vulkan DLLs. It now runs on RX 9000 (FP8), RX 7000 and RX 6000 (INT8).
- **Cross-vendor XeSS.** Its DX11 path is Intel Arc only.

These three need a side D3D12 device: copy the inputs into shared textures and sync with a shared fence. OptiScaler cites a cost of up to about 10%.

BO3 facts:
- **Graphics API:** D3D11.
- **Anti-aliasing options:** None, FXAA, SMAA 1x, Filmic SMAA 1x, SMAA T2x, Filmic SMAA T2x.
- **Render scale:** `ResolutionPercent` runs from 50 to 200. The UI stays at screen resolution.
- **What T2x provides:** jittered rendering and motion vectors rebuilt from depth.
- **What T2x lacks:** it has only 2 hard-coded jitter positions, and its motion vectors are stored in a 16-bit nonlinear format that saturates at about 40 units.

Prior art: the Luma Framework BO3 mod already ships DLSS through NGX D3D11. It replaces the SMAA T2x resolve and patches the jitter constants in the executable (for the Sep 2026 Steam build and the 2023 build used by BOIII). Its custom-MIT license forbids commercial use without permission, so its code cannot be copied into GPL-3.0.

Licensing is what gates the plan:
- **NVIDIA:** the RTX SDK license section 4(e) bars any use that puts the SDK under an open-source license, which conflicts with GPL-3.0 linking. This is an open question.
- **AMD:** the SDK 2.x license file contradicts itself. A binary-only license is the default, yet the MIT list names every file in the repo, the signed DLLs included.

Recommended order:
- **Phase A:** FSR 1 or NIS spatial upscaling on top of `ResolutionPercent`.
- **Phase B:** the temporal input contract (jitter sequence, motion-vector decode, depth, resets, mip bias) plus FSR 2.2 or 3.1 built from MIT source on native DX11.
- **Phase <local scratch copy, not kept>** vendor ML upscalers as optional modules. DLSS native DX11 comes after a license decision; FSR 4 and XeSS go through DX12 interop.

## Findings

- **confirmed**: Latest AMD SDK is FSR SDK v2.4.0 'Redstone', published 2026-10-28. It contains FSR 2.3.4 (temporal), FSR 3.1.6, FSR Upscaling 4.1.2 (ML), FSR FG 3.1.6, FG Swapchain 3.1.7 and 4.0.0, FSR FG (ML) 4.0.2, Ray Regeneration 1.2.1 and Radiance Caching (Preview). Earlier releases: 2.3.0 (2026-06-24), 2.2.0 (2026-03-23), 2.0.0 (2025-08-20). Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.4.0 (GitHub releases API and README, read 2026-11-04)
- **confirmed**: The SDK 2.x license file (Kits/FidelityFX/docs/license.md) opens with a binary-only license (no reverse engineering; redistributions must reproduce the notices) that applies to all files except those listed. An MIT list follows. In v2.4.0 that MIT list names every one of the repo's 943 files, including the signedbin DLLs (checked by diffing the git tree against the list), so the license is internally ambiguous. SDK v1.1.4 (2025-05-08) is plain MIT throughout. Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/docs/license.md ; https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/LICENSE.txt
- **confirmed**: FSR 1 (EASU + RCAS) is MIT-licensed and ships as full shader source. GPUOpen lists DirectX 12, DirectX 11 and Vulkan support, and advises running it after tonemapping. A DX11 implementation needs only two compute or pixel passes and no SDK runtime. Source: https://gpuopen.com/fidelityfx-superresolution/ ; https://github.com/GPUOpen-Effects/FidelityFX-FSR (v1.0.2, 2021-11-16)
- **confirmed**: The public FSR 2 release (v2.2.1, 2023-06-09, MIT) ships only DX12 and Vulkan backends; its README says other backends are available on request. AMD's FidelityFX-FSR2-Unity-URP repo (MIT) contains a DX11 backend patch for FSR2 v2.2.1, authored by an AMD engineer on 2023-03-14. Source: https://github.com/GPUOpen-Effects/FidelityFX-FSR2 ; https://github.com/GPUOpen-Effects/FidelityFX-FSR2-Unity-URP/blob/main/src/patch/0001-fsr-2.2-dx11-backend.patch
- **confirmed**: FSR 3.1 through the FSR/FFX API is DX12 and Vulkan only. AMD's SDK 1.1 notes say custom backends are no longer supported through that API. No official SDK release (v1.0.0, v1.1.4, fsr3-v3.0.4, 2.x) contains a DX11 backend. Source: https://gpuopen.com/manuals/fsr_sdk/whats-new/version_1_1/ ; GitHub trees of FidelityFX-SDK tags v1.0.0, v1.1.4, fsr3-v3.0.4, v2.4.0
- **confirmed**: FSR 3.x on native DX11 exists only as unofficial ports. Examples: metarutaiga/FidelityFX-SDK-DX11 (Shader Model 5.0 patches, no license file), OptiScaler's 'FSR 3.1.2 (unofficial port to native DX11)', Luma's statically linked DX11 FSR 3, and Skyrim Community Shaders (GPL-3.0), which calls ffxGetInterfaceDX11. Source: https://github.com/metarutaiga/FidelityFX-SDK-DX11 ; https://github.com/optiscaler/OptiScaler (README) ; https://github.com/Filoppi/Luma-Framework/blob/main/Source/Core/fsr/FSR.cpp
- **confirmed**: FSR Upscaling 4.1.2 hardware support:
- RX 9000: full FP8 model.
- RX 7000: INT8, added in 4.1.1 / SDK 2.3.0.
- RX 6000: INT8, added in 4.1.2 / SDK 2.4.0.
- Ryzen AI Max 300/400: full INT8 model.
- Ryzen AI 9 300/400, Z2 A and Z2 Extreme: lightweight INT8 model.
AMD's cost table includes 1316 us at 4K on the RX 9070 XT, 2460 us at 1440p on the RX 6700 XT and 2050 us at 1440p on the RX 7800 XT. Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/docs/techniques/super-resolution-ml.md ; v2.3.0 and v2.4.0 release notes
- **confirmed**: FSR 4 supports only DX12 (CS_6_6) and Vulkan (SPIR-V 1.5); Vulkan reached parity with DX12 in 2.4.0, and there is no DX11. It must be used through the FSR API and the signed binaries: amd_fidelityfx_loader_dx12.dll plus amd_fidelityfx_upscaler_dx12.dll. Effect DLLs are looked up in this order: an already-loaded module, then the exe folder, then the loader's folder. By default the newest version wins, and that can be a newer one supplied by the driver (marked with ' *'). Passing ffxOverrideVersion pins a version and turns off automatic selection. On unsupported hardware it falls back to FSR 3.1.x. Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/docs/getting-started/ffx-api.md ; super-resolution-ml.md ; https://gpuopen.com/fidelityfx-superresolution-4/
- **confirmed**: AMD's own FSR 4 documentation is partly stale. The GPUOpen FSR 4 page still says RX 9000 only, DX12 only, version 4.0.2 (August 2025). The 'Limitations' line in super-resolution-ml.md still says RX 7000/9000, even though the 2.4.0 release notes add RX 6000. Source: https://gpuopen.com/fidelityfx-superresolution-4/ ; super-resolution-ml.md v2.4.0
- **likely**: FSR 4 source code was published by mistake, marked MIT, with SDK 2.0 around 2025-08-20. AMD then took it down, and whether the MIT grant can be revoked is disputed. No official FSR 4 source exists. The community INT8 builds derived from it are not something Xerces should redistribute. Source: https://www.tomshardware.com/pc-components/gpus/amds-already-taken-down-mistakenly-released-fsr-4-source-code-but-the-internet-never-forgets-forked-github-repositories-remain-accessible ; https://www.pcgamer.com/hardware/graphics-cards/amds-fsr-4-open-source-whoopsie-daisy-may-be-harder-to-take-back-than-initially-thought-thanks-to-the-inclusion-of-a-difficult-to-revoke-mit-license
- **confirmed**: DLSS SDK 310.9.16 was released 2026-10-16. NGX exposes a full D3D11 path (NVSDK_NGX_D3D11_Init / Init_with_ProjectID, GetCapabilityParameters, CreateFeature, NGX_D3D11_CREATE_DLSS_EXT / EVALUATE_DLSS_EXT). The programming guide lists 'DirectX11, DirectX 12, or Vulkan' engines and says NGX preserves D3D11 immediate-context state. Requirements: an NVIDIA RTX GPU and a driver from after 2022-03-03 (512.15). Source: https://github.com/NVIDIA/DLSS/releases/tag/v310.9.16 ; include/nvsdk_ngx.h and nvsdk_ngx_helpers_d3d.h ; doc/DLSS_Programming_Guide_Release.pdf (rev 310.6.0, Mar 2026) sections 2.1, 2.2, 5.2.5
- **confirmed**: Streamline 2.14.16 was released 2026-10-16. Its sl.dlss plugin manifest lists rhi d3d11, d3d12 and vk, with a minimum of Turing (TU100) and driver 512.15. Streamline also needs Windows 10 20H1 and has to be initialized before any dxgi/d3d11 call (via the interposer or manual hooks). The framework's own source is MIT; NGX and the DLSS binaries fall under the RTX SDK license. Source: https://github.com/NVIDIA-RTX/Streamline/blob/v2.14.16/source/plugins/sl.dlss/dlss.json ; dlssEntry.cpp ; README.md ; license.txt
- **confirmed**: Key terms of the NVIDIA RTX SDKs License that cover DLSS/NGX:
- Section 1(c) and 2: SDK binaries may be distributed in object code inside an application with material additional functionality, under terms at least as protective as NVIDIA's.
- Section 4(a), 4(b): no reverse engineering and no standalone redistribution.
- Section 4(e): no use that would 'cause it to become subject to an open source software license'.
- Supplement 1: the SDK may only be used on systems with NVIDIA GPUs.
- Supplement 4: notify NVIDIA before a commercial release.
- Supplement 6.1, 6.2: NVIDIA may push updates over the air and may disable a broken integration.
- Supplement 7.1(b): DLSS must be credited with NVIDIA marks on the splash screen, About box and credits. Source: https://github.com/NVIDIA/DLSS/blob/v310.9.16/LICENSE.txt
- **confirmed**: DLSS packaging rules from the guide (sections 4.2-4.2.3 and 5.2.1):
- Ship the release nvngx_dlss.dll from lib/<plat>/rel next to the exe, or point to it with a path list.
- Never ship the dev DLL, which is watermarked.
- Do not strip NVIDIA's signature; the DLL will not load without it.
- Include the third-party notices from section 9.5.
- Initialize with a GUID-like Project ID (engine type CUSTOM, checked for GUID format) or with an Application ID issued by NVIDIA. Source: https://github.com/NVIDIA/DLSS/blob/v310.9.16/doc/DLSS_Programming_Guide_Release.pdf
- **confirmed**: The FSF's GPL FAQ says two relevant things. Linking a GPL program to a library outside the system-library exception requires a permission that only the program's copyright holders can grant. Plug-ins that share function calls and data structures with the main program form a single combined program. Whether GPL-3.0 Xerces may load or redistribute NGX/DLSS (or AMD/Intel binary-only DLLs) is therefore an open legal question. Source: https://www.gnu.org/licenses/gpl-faq.html#GPLIncompatibleLibs ; https://www.gnu.org/licenses/gpl-faq.html#GPLPlugins
- **confirmed**: Two GPL-3.0 precedents handle this differently:
- OptiScaler bundles AMD FFX SDK 2.3 and XeSS 3.0.1 DLLs but not nvngx_dlss.dll; users supply DLSS from the game.
- Skyrim Community Shaders ships nvngx_dlss.dll, the Streamline DLLs and the AMD FFX DLLs, and adds GPL-3.0 section 7 'Modding' and 'Linking' exceptions in EXCEPTIONS.md. Source: https://github.com/optiscaler/OptiScaler/releases/tag/v0.9.4 (2026-07-18) ; https://github.com/community-shaders/skyrim-community-shaders/blob/HEAD/EXCEPTIONS.md
- **confirmed**: XeSS SDK 3.0.2 was released 2026-07-24 (XeLL fixes only; 3.0.0 came out 2026-03-09). XeSS Super Resolution runs on DX12 via the Intel XMX path or a cross-vendor HLSL path for any GPU with SM 6.4 and DP4a, and on Vulkan 1.1. DX11 support (added in 2.0.1) is Intel Arc only, through a separate libxess_dx11.dll. Context creation on non-Intel or Iris Xe hardware fails with XESS_RESULT_ERROR_UNSUPPORTED_DEVICE. Source: https://github.com/intel/xess/releases/tag/v3.0.2 ; https://github.com/intel/xess/blob/main/doc/xess_sr_developer_guide_english.md ; https://github.com/intel/xess/releases/tag/v2.0.1
- **confirmed**: XeSS is licensed under the Intel Simplified Software License (October 2022): binary-only use and redistribution without modification, notices must be reproduced, no endorsement, no reverse engineering. The dispatcher may load a newer XeSS Super Resolution runtime shipped with the Intel driver. Source: https://github.com/intel/xess/blob/main/LICENSE.txt ; xess_sr_developer_guide_english.md
- **confirmed**: OptiScaler's README describes its DX11 path as 'D3D11on12', but its code does not wrap the game's device. It creates a separate D3D12 device and queue on the same adapter, copies color, motion vectors, depth, exposure and reactive mask into shared textures (D3D11_RESOURCE_MISC_SHARED_NTHANDLE), and synchronizes with shared fences (ID3D11Device5::CreateFence(D3D11_FENCE_FLAG_SHARED), D3D12 queue Wait/Signal). It then copies the output back. The README cites a penalty of up to about 10%. Luma's D3D12Interop works the same way and converts depth to R32_FLOAT because DX12 libraries cannot read DX11 depth-stencil textures. Source: https://github.com/optiscaler/OptiScaler/blob/master/OptiScaler/with_dx12/dx11_with_dx12.cpp ; README ; https://github.com/Filoppi/Luma-Framework/blob/main/Source/Core/interop/D3D12Interop.h
- **confirmed**: OptiScaler only replaces upscalers in games that already support DLSS 2+, FSR 2+ or XeSS. BO3 has none of these, so OptiScaler alone cannot add upscaling to it. Its DX11 menu offers: native FSR 2.2.1, an unofficial native FSR 3.1.2, native DLSS, XeSS 2.x (Arc only), and XeSS/FSR 2/FSR 3/FSR 4 through DX12 interop. Source: https://github.com/optiscaler/OptiScaler (README, read 2026-11-04)
- **confirmed**: BO3 PC uses Direct3D 11 (Steam minimum requirements list DirectX Version 11). Source: https://store.steampowered.com/api/appdetails?appids=311210 (read 2026-11-04)
- **likely**: BO3's anti-aliasing options are None, FXAA, SMAA 1x, Filmic SMAA 1x, SMAA T2x and Filmic SMAA T2x (config key AATechnique). Render scale is ResolutionPercent, 50 to 200, default 100, described as the percentage of window resolution the 3D scene renders at. NVIDIA's guide shows 50-200% and says menus and UI stay at screen resolution. BO3 has per-object motion blur and GTAO, and no dynamic resolution option. Source: https://gist.github.com/Poomel/0b0937053af45ed8d47c (config.ini, 2016) ; https://www.nvidia.com/en-us/geforce/news/call-of-duty-black-ops-3-graphics-and-performance-guide/ (2015)
- **likely**: How BO3's SMAA T2x works, from community reverse engineering in Luma's shader dumps and code:
- Jitter: two hard-coded subpixel offsets, (-0.25, +0.25) and (+0.25, -0.25), alternate by frame parity in two separate jitter functions (static and dynamic objects).
- Camera motion vectors: rebuilt from depth using previous-frame matrices and a subpixelOffset constant.
- Motion vector storage: 16-bit SNorm with a piecewise nonlinear curve (0-10 units mapped to 0-0.5, 10-40 to 0.5-1), so it saturates at about 40 units.
- Pass order: the T2x passes run after tonemapping, i.e. on LDR color. Source: https://github.com/Filoppi/Luma-Framework/blob/main/Source/Games/Call%20of%20Duty%20Black%20Ops%203/main.cpp ; Shaders/Call of Duty Black Ops 3/Luma_CoDBO3_LinearizeVelocity.hlsl and motionvectors_0xB8A51615.ps_5_0.hlsl
- **confirmed**: Luma Framework's BO3 module (Nexus Mods BO3 mod 28; v4.02 covered by GameGPU on 2026-04-07) adds HDR and DLSS through NGX D3D11 with a self-generated Project ID GUID. How it works:
- DLSS replaces the non-Filmic SMAA T2x resolve, and the user must select non-Filmic SMAA T2x.
- It follows the in-game Render Resolution; values above 100% are untested.
- It patches the T2x jitter constants with a Halton(2,3) sequence for the Steam Sep 2026 build (BuildID 24784313) and the 2023 build used by BOIII.
- Its FSR path is disabled for BO3 because FSR corrupted the color resource.
- It warns that T7Patch is needed on vanilla Steam. Source: https://github.com/Filoppi/Luma-Framework (BO3 main.cpp, last BO3 commit 2026-09-14) ; https://en.gamegpu.com/news/igry/mod-dobavil-podderzhku-dlss-i-hdr-v-call-of-duty-black-ops-3
- **confirmed**: Luma's license is a 'Custom MIT License' with two additions: reuse must credit the authors or project, and commercial use needs permission. The commercial-use restriction makes it unsuitable to copy into GPL-3.0 code; use it only as a reference or ask the authors. Source: https://github.com/Filoppi/Luma-Framework/blob/main/LICENSE.md
- **uncertain**: Searches found no other BO3-specific FSR, XeSS or OptiScaler reports; ReShade presets for BO3 on Nexus are color presets, not upscalers. GameGPU reports that a later Luma patch added OptiScaler compatibility for choosing presets. Source: Web searches 2026-11-04 ; https://nexusmods.com/callofdutyblackops3/mods/14 ; GameGPU article above (Nexus pages returned 403)
- **confirmed**: Temporal upscalers all need the following inputs:
- jittered render-resolution color;
- depth (inverted and infinite-far flags);
- motion vectors without jitter, at render resolution;
- jitter offsets every frame;
- exposure, or auto-exposure;
- frame time delta in ms (FSR);
- a reset flag on camera cuts;
- optional reactive and transparency masks (FSR 2/3 benefit from them; FSR 4 does not need them).
Vendor guidance differs on two numbers:
- Jitter phases: DLSS needs at least 16 (32+ preferred). FSR uses ceil(8*n^2): 18/23/32/72 for 1.5x/1.7x/2x/3x. XeSS needs at least 72 at 3x.
- Mip bias: FSR and DLSS use log2(render/display) - 1. XeSS uses log2(render/display), e.g. -1.202 at 2.3x. Source: super-resolution-ml.md (FSR SDK 2.4.0) ; DLSS Programming Guide 310.6.0 sections 2.2, 3.5, 3.7 ; xess_sr_developer_guide_english.md
- **confirmed**: NVIDIA Image Scaling (NIS) is MIT-licensed, ships as HLSL/GLSL source, and integrates with DX11, DX12 and Vulkan. It is a cross-vendor alternative to FSR 1. Source: https://github.com/NVIDIAGameWorks/NVIDIAImageScaling ; https://github.com/NVIDIA/DLSS/blob/v310.9.16/README.md
- **likely**: SMAA T2x is SMAA 1x plus 2x temporal supersampling with CryEngine-style temporal reprojection (added in SMAA v2.5). Jimenez's 2017 Call of Duty talk describes default T2x as 2x diagonal supersampling, which matches BO3's +/-0.25 diagonal offsets. Source: https://www.iryoku.com/smaa/ ; https://www.activision.com/cdn/research/Dynamic_Temporal_Antialiasing_and_Upsampling_in_Call_of_Duty_v4.pdf (could not fetch directly; known from search summary)

## Details

# Upscalers for Black Ops III (D3D11): research snapshot, 2026-11-04

## 1. Versions checked today
- **AMD FSR SDK** (formerly FidelityFX SDK): **2.4.0 "Redstone"**, released 2026-10-28.
  - Contents: FSR Upscaling 4.1.2 (ML), FSR 3.1.6, FSR 2.3.4, Frame Generation 3.1.6 / 4.0.2, Frame Generation Swapchain 3.1.7 / 4.0.0, Ray Regeneration 1.2.1.
  - The last release that is plain MIT throughout is SDK 1.1.4 (2025-05-08).
- **NVIDIA DLSS SDK 310.9.16** (2026-10-16). The programming guide PDF in that tag is revision 310.6.0 (March 2026).
- **NVIDIA Streamline 2.14.16** (2026-10-16).
- **Intel XeSS SDK 3.0.2** (2026-07-24; 3.0.0 came out 2026-03-09).
- **Reference projects:**
  - OptiScaler 0.9.4 (2026-07-18, GPL-3.0).
  - Luma Framework build 785 (2026-11-02). Its BO3 module was last changed 2026-09-14.

## 2. Comparison table

| Technique | API support | GPU support | License | Redistribution | DX11 path for BO3 |
|---|---|---|---|---|---|
| **FSR 1** (EASU + RCAS, spatial) | HLSL/GLSL source. GPUOpen lists DX11, DX12, Vulkan. | Any modern GPU | MIT | Compile into Xerces and keep the notice | **Native.** Two passes on the anti-aliased output, before UI. |
| **NIS** (NVIDIA Image Scaling, spatial) | HLSL/GLSL compute: DX11, DX12, Vulkan | Any GPU | MIT | Compile in | **Native** |
| **FSR 2.2.1** (temporal, analytic) | Public backends: DX12 and Vulkan. AMD-written DX11 backend patch (2023) in GPUOpen-Effects/FidelityFX-FSR2-Unity-URP. | Any vendor with typed UAV loads | MIT | Compile in | **Native**, using AMD's patch. OptiScaler's default DX11 upscaler is a backport of it. |
| **FSR 3.1.x** (temporal, analytic) | FSR/FFX API signed DLL: DX12 and Vulkan only, custom backends unsupported. Component source is MIT. | DX12 SM 6.2+, any vendor | Source: MIT. DLL: SDK 2.x terms (see section 3). | Ship the DLL, or build from source | **Unofficial only:** metarutaiga/FidelityFX-SDK-DX11 (no license file), OptiScaler's 3.1.2 port, Luma. Otherwise DX12 interop. |
| **FSR 4.1.2** (ML) | DX12 (CS 6.6) and Vulkan (parity since SDK 2.4.0). **No DX11.** | RX 9000 (FP8), RX 7000 and RX 6000 (INT8), Ryzen AI Max / AI 9 / Z2 APUs. Other GPUs fall back to FSR 3.1. | Binary-only signed DLL; no source | Ship amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll with notices. The driver may swap in a newer version. | **DX12 interop only** |
| **DLSS SR** 310.9.16 via NGX | D3D11, D3D12, Vulkan (NVSDK_NGX_D3D11_*) | NVIDIA RTX (Turing+), driver 512.15 or newer | NVIDIA RTX SDKs License (proprietary) | nvngx_dlss.dll from lib/.../rel, signature intact, next to the exe or found via a path list. Never ship the dev DLL. Attribution required. | **Native.** Luma's BO3 mod already does this. |
| **DLSS SR via Streamline** 2.14.16 | sl.dlss supports d3d11, d3d12, vk | Same as above, plus Windows 10 20H1 | Streamline itself MIT; NGX/DLSS under the RTX license | sl.interposer/common/dlss DLLs plus nvngx_dlss.dll | **Native**, but Streamline must start before device creation (interposer or manual hooks). Heavier than NGX direct for an injected client. |
| **XeSS-SR** 3.0.x | DX12 (Intel XMX path plus cross-vendor DP4a HLSL path), Vulkan 1.1, DX11 (Intel Arc only, libxess_dx11.dll) | Arc / Iris Xe. Other vendors need SM 6.4 + DP4a, on DX12/Vulkan only. | Intel Simplified Software License (binary only, no modification, no reverse engineering) | libxess.dll (about 78 MB) or libxess_dx11.dll unmodified, plus notice. Needs MSVC runtime 14.40+. | **Native on Arc only**; DX12 interop for everyone else |
| **DX11 to DX12 side-device interop** | Hosts any DX12-only upscaler | Same adapter; needs D3D11.4 fences (ID3D11Device5 / ID3D11DeviceContext4) | OptiScaler: GPL-3.0. Luma: custom MIT with a commercial-use addendum. | n/a | Copy inputs into shared NT-handle textures, sync on a shared fence, copy the output back. **Up to about 10% cost** (OptiScaler). |

## 3. AMD details
- **License file in SDK 2.x** (`Kits/FidelityFX/docs/license.md`) contradicts itself:
  - It starts with a binary-only, no-reverse-engineering license for "all files except as noted below".
  - The MIT list that follows, in v2.4.0, names every one of the 943 repo files, including `signedbin/*.dll`.
  - Safe reading: treat the DLLs as binary-only (redistribute unmodified with notices; no reverse engineering) and ask AMD.
  - The FSR 3.1 and FSR 2 shader and host source under `upscalers/fsr3` is clearly MIT.
- **No official DX11 backend anywhere.**
  - FSR 2's README says other backends are available on request.
  - The only AMD-written DX11 code is the FSR 2.2.1 backend patch from 2023, MIT.
  - DX11 ports of FSR 3.x patch the shaders for SM 5.0, emulate wave operations through vendor extensions, and lose FP16. They are unofficial and unmaintained by AMD.
  - Luma disabled its DX11 FSR path in BO3 because it corrupted the color resource.
- **FSR 4 integration steps:**
  - Call `ffxCreateContext` with `ffxCreateContextDescUpscale`, `ffxCreateBackendDX12Desc`, and `ffxCreateContextDescUpscaleVersion` (required since SDK 2.1).
  - The loader finds effect DLLs in this order: an already-loaded module, the exe folder, then the loader's own folder. A mod can therefore keep them in a subfolder.
  - Without an override, the newest version wins, including one the driver provides (shown with a ` *` suffix).
  - Passing `ffxOverrideVersion` pins a version and disables driver upgrades.
  - AMD asks that the options UI list the driver-provided versions and keep a default entry with no override.
- **FSR 4 memory:** about 81 MB at 1080p and 318 MB at 4K.
- **FSR 4 cost, from AMD's table:** 352 us at 1080p on the RX 9070 XT (FP8), 2.0-2.5 ms at 1440p on the RX 7800 XT and RX 6700 XT (INT8).

## 4. DLSS details
- **Integration:** NGX on D3D11 needs no interop. Initialize with `NVSDK_NGX_D3D11_Init_with_ProjectID(GUID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, ...)`, or with an Application ID from NVIDIA. NGX preserves D3D11 immediate-context state.
- **Color input:** BO3's input is post-tonemap, so it fits DLSS's LDR mode (perceptual 0-1 range, with `IsHDR=0`).
- **License points that matter for an open-source client (RTX SDKs License):**
  - Section 2(a): redistribution is allowed only inside an application with material additional functionality.
  - Section 2(c): the SDK must be distributed under terms at least as protective as NVIDIA's.
  - Section 4(a), 4(b): no reverse engineering, no standalone redistribution.
  - Section 4(e): no use that would subject the SDK to an open-source license.
  - Supplement 1: the SDK is only for systems with NVIDIA GPUs.
  - Supplement 4: notify NVIDIA before a commercial release (Xerces is not commercial).
  - Supplement 7.1(b): NVIDIA attribution and marks on the splash screen, About box and credits.
  - Supplement 6.1, 6.2: NVIDIA may push updates over the air and may disable a broken integration.
- **Does GPL-3.0 conflict with this?** This is an open question. The FSF says linking non-system libraries needs a permission that only the copyright holders can grant, and that plug-ins sharing calls and data with the main program form one combined program. Two precedents:
  - OptiScaler does not bundle nvngx_dlss.dll.
  - Skyrim Community Shaders bundles it under a GPL section 7 linking exception.
  - Xerces cannot add such an exception on its own for code inherited from boiii.

## 5. XeSS details
- **APIs and GPUs:**
  - DX12: works cross-vendor through a DP4a HLSL path on SM 6.4 hardware.
  - Vulkan 1.1: supported since 2.0.1.
  - DX11: Arc only. Non-Intel hardware fails at context creation with `UNSUPPORTED_DEVICE`.
- **Separate DLLs:** the DX11 backend lives in `libxess_dx11.dll`. Functions from the two DLLs must not be mixed.
- **Rules from Intel's guide:**
  - Do not hard-code ratios, mip bias or jitter length.
  - Query `xessGetOptimalInputResolution`.
  - Mip bias is `log2(render/display)`. This differs from the DLSS/FSR formula, which subtracts 1.

## 6. Black Ops III renderer facts relevant to an upscaler
- **API and settings:**
  - D3D11; shaders are ps_5_0 and cs_5_0.
  - `AATechnique` options: None, FXAA, SMAA 1x, Filmic SMAA 1x, SMAA T2x, Filmic SMAA T2x.
  - `ResolutionPercent` runs from 50 to 200; the UI stays at screen resolution.
  - The final composite samples the scene through a bilinear-clamp sampler. That is probably how render resolution below 100% gets upscaled today (inference).
- **Pass order, reconstructed from Luma's hooks (likely):** probe cull, motion vectors (static geometry, rebuilt from depth), AO / GTAO, deferred resolve, sprites, volumetric fog, exposure, **tonemap**, SMAA T2x prep (compute), **T2x resolve**, final composite, FSFX (blood, sights, DNI), HDTV, UI.
- **What T2x provides and what it lacks:**
  - Only **2** jitter positions, (+/-0.25, -/+0.25), alternating by frame parity.
  - They are hard-coded as immediates in **two** functions (static and dynamic objects).
  - The motion-vector shader subtracts `subpixelOffset`.
  - Motion vectors are stored as 16-bit SNorm on a nonlinear curve that saturates at about 40 units.
  - The scene constants include an `isDepthHack` field. It possibly marks the weapon viewmodel and could need special handling (uncertain).
  - Filmic T2x uses a different shader, and Luma refuses to run on it.
- **Prior art (Luma BO3 module):**
  - Replaces the T2x resolve with DLSS.
  - Rewrites the jitter immediates with a Halton(2,3) sequence every frame.
  - Uses exe-specific offsets: the Steam Sep 2026 build 24784313 and the 2023 build used by BOIII.
  - Follows the Render Resolution setting.
  - Its code is a useful map of hook points, but do not copy it (license).

## 7. Retrofit pitfalls to plan for
1. **Jitter:** 2 phases is far too few. Use Halton(2,3) with ceil(8*n^2) phases (DLSS needs at least 16), patch both jitter functions consistently, and keep `subpixelOffset` in sync. Other temporal effects tuned to 2-phase T2x (for example GTAO's denoiser) may change behavior.
2. **Motion vectors:** decode them to pixels and match each SDK's sign and scale conventions. The roughly 40-unit clamp will smear fast flicks at high output resolution; consider patching the motion-vector output format.
3. **Depth:** copy or convert it to R32F, and set the inverted and infinite-far flags correctly (verify BO3's depth convention).
4. **Color is LDR, post-tonemap:** use FSR's NON_LINEAR / sRGB flags or DLSS's LDR mode. Moving the upscaler before tonemapping would be better but is much more work.
5. **Placement:** run the upscaler at the T2x resolve. FSFX and UI then stay at display resolution and need no extra work, but motion blur and bloom ordering should be checked.
6. **Mip bias:** apply a negative LOD bias to material samplers only, not to UI, shadow maps, LUTs or screen-space buffers. Vendors disagree on the formula (see section 5).
7. **History resets:** reset on map load, killcam / theater, spectator switch, menu transitions, FOV changes and resolution changes.
8. **Transparencies:** sprites, OIT and volumetric fog have no motion vectors. FSR 2/3 need a reactive mask for them; FSR 4 and DLSS cope better.
9. **Ratios:** clamp `ResolutionPercent` to 100 or below while upscaling, with 100% as a native-AA (DLAA / NativeAA) mode. Ultra Performance (33%) sits below BO3's 50% floor.
10. **Performance framing:** BO3 already runs at very high frame rates. A fixed upscaler cost of 0.35-3 ms can cancel out the gain, so the main win is image quality (replacing 2-sample T2x) and help for low-end GPUs and APUs. Measure before promising FPS.

## 8. Suggested phase order
- **Phase A, spatial (all GPUs, MIT, no DLLs).**
  - Hook the AA resolve output and replace the bilinear upscale with FSR 1 EASU plus RCAS, or NIS.
  - Add an RCAS sharpen option at 100% and a mip-bias control.
  - Set up the hook points, timing and debug views needed later.
- **Phase B, temporal input contract plus an open-source temporal upscaler (license-clean).**
  - Build jitter override, motion-vector decode, R32F depth, resets, mip bias, a forced non-Filmic T2x path and an optional reactive mask.
  - Ship FSR 2.2.1 using AMD's MIT DX11 backend, then optionally move to FSR 3.1.6 MIT source with Xerces' own DX11 backend.
  - Include a native-AA mode.
- **Phase C, vendor ML upscalers as optional modules sharing the Phase B inputs.**
  - C1, DLSS through NGX D3D11: native and simplest technically. Gated on the GPL/RTX license decision.
  - C2, FSR 4 (RX 6000/7000/9000) and cross-vendor XeSS through a DX12 interop module, plus XeSS native DX11 on Arc.

## 9. Possibly stale or second-hand facts
- GPUOpen's FSR 4 page (4.0.2, RX 9000 only, DX12 only) and the FSR 4 doc's "Limitations" line are behind the 2.4.0 release notes.
- The DLSS guide revision (310.6.0) predates SDK 310.9.16.
- The BO3 config listing is from 2016 and NVIDIA's BO3 guide is from 2015.
- Luma's executable offsets are tied to specific builds; Steam updated BO3 in 2026 per Luma's code comments.
- The Activision 2017 T2x talk PDF could not be fetched; its content is known only from a search summary.

## Recommendations

- Phase A: replace BO3's bilinear upscale (used when ResolutionPercent is below 100) with FSR 1 EASU + RCAS, or NIS, as DX11 passes placed after the AA resolve and before FSFX/UI. Both are MIT, need no proprietary DLLs, run on every GPU and carry no GPL risk. Also add a mip LOD bias control.
- Phase B: build a shared temporal input contract before adopting any vendor SDK:
- Replace T2x's 2-phase jitter with Halton(2,3) at ceil(8*n^2) phases, hooking both jitter functions found by pattern scan on the boiii executable rather than patching immediates every frame.
- Decode BO3's 16-bit nonlinear motion vectors to pixel units without jitter.
- Copy depth to R32F.
- Use the LDR/non-linear color flags.
- Reset history on killcam, spectate, map load and menu.
- Apply per-upscaler mip bias.
- Force non-Filmic T2x while upscaling is on.
- Phase B upscaler: start with FSR 2.2.1 using AMD's MIT DX11 backend patch (GPUOpen-Effects/FidelityFX-FSR2-Unity-URP), then optionally port FSR 3.1.6 MIT source to Xerces' own DX11 backend. Do not copy metarutaiga's port (no license) or Luma's code (commercial-use restriction); use them only as references. Offer a 100% native-AA mode as a T2x replacement.
- Phase C1, DLSS: integrate NGX D3D11 directly, not Streamline, using a unique GUID Project ID with engine type CUSTOM. Do this only after a licensing decision. Options: a separately licensed optional module with nvngx_dlss.dll supplied by the user, or written permission from NVIDIA. Follow the attribution rules (NVIDIA marks in About/credits) and never strip the DLL signature or ship the dev DLL.
- Phase C2: build one DX12 side-device interop module (same-adapter D3D12 device, shared NT-handle textures, shared fences, R32F depth conversion) to host FSR 4 (RX 6000/7000/9000) and cross-vendor XeSS. Use native libxess_dx11.dll on Intel Arc. Budget for up to about 10% overhead and measure it.
- For FSR 4, ship the official signed loader and upscaler DLLs from SDK 2.4.0 with AMD's notices. Do not override the version by default, so driver upgrades still apply. Never use the leaked FSR 4 source or community INT8 builds derived from it.
- Clamp ResolutionPercent to 100 or below whenever a temporal upscaler is active. Map quality modes onto BO3's range: Quality 67%, Balanced 59%, Performance 50%. Treat Ultra Performance (33%) as a later stretch that would require patching the 50% floor.
- Frame Phase C mainly as an image-quality feature (DLAA / native AA instead of 2-sample T2x) and as help for low-end GPUs, not as an FPS feature. BO3 runs very fast on modern GPUs, and an ML upscaler's fixed cost (for example about 2-2.5 ms for FSR 4 INT8 at 1440p on RX 6000/7000) can exceed the savings. Profile before promising FPS.
- Get a short legal opinion before shipping any proprietary upscaler binary in a GPL-3.0 release. Cover the RTX SDK license sections 2(c) and 4(e), the binary-only AMD and Intel DLLs, and whether a separately licensed plugin boundary is enough. A GPL section 7 exception would need consent from the upstream boiii copyright holders.

## Risks

- License conflict: the NVIDIA RTX SDKs License section 4(e) bars any use that subjects the SDK to an open-source license, and section 2(c) requires redistribution under equally protective terms. Shipping or linking NGX/DLSS in a GPL-3.0 client is legally unresolved. Under the FSF's view, loading it as a plug-in may form a combined work.
- AMD's SDK 2.x license file contradicts itself: a binary-only default, yet an MIT list naming every file, the signed DLLs included. Intel XeSS is binary-only with no modification allowed. Binary-only DLLs dynamically loaded by GPL code raise the same linking question, without an explicit anti-open-source clause.
- Third-party reference code carries license traps: Luma's custom MIT restricts commercial use, which makes it GPL-incompatible to copy, and metarutaiga's FSR 3 DX11 port has no license file. Copying either into Xerces would be a licensing problem.
- BO3-specific engineering risk:
- Jitter is hard-coded at 2 phases in two functions, and patch addresses differ per executable (Steam updated BO3 in 2026; boiii uses a 2023 build).
- Motion vectors saturate at about 40 units, causing smearing on fast flicks.
- Color reaches the upscaler post-tonemap (LDR).
- The viewmodel depth hack (isDepthHack) may break reprojection.
- Filmic T2x takes a different path.
- Other temporal effects (GTAO denoise) may assume the 2-phase jitter.
- Transparencies (sprites, OIT layers, volumetric fog) lack motion vectors. Expect ghosting with FSR 2/3 unless a reactive mask is generated.
- The DX11-to-DX12 interop needed for FSR 4 and non-Arc XeSS costs up to about 10%. It also adds VRAM, sync complexity, device-loss handling and a requirement for D3D11.4 fences. Interop bugs can show up as hitches or a poisoned GPU sync.
- Unofficial DX11 FSR 3 ports are unmaintained by AMD and have known problems; Luma disabled FSR in BO3 because it corrupted the color resource. AMD's FSR 2.2 DX11 backend dates from 2023.
- Vendor runtimes can change behavior under the client without a Xerces update: NGX over-the-air updates, NVIDIA's right to disable a broken integration, and AMD or Intel drivers substituting newer upscaler versions.
- Hardware coverage is fragmented: DLSS needs RTX; FSR 4 needs RX 6000 or newer (INT8 quality and performance below FP8); XeSS on DX11 is Arc only. Without Phase A/B, many users would get nothing.
- Performance expectations: at BO3's typical frame rates, the fixed cost of ML upscalers can outweigh render-resolution savings, so users may see small or negative FPS gains.
- Injection-based graphics hooks on official Steam multiplayer have been linked to bans (Luma and OptiScaler both warn about this). Xerces is offline/self-hosted, but users could load the module on vanilla Steam. Gate it to the Xerces runtime.
- Documentation staleness: AMD's own FSR 4 pages disagree with the 2.4.0 release notes, and the DLSS guide revision is behind the SDK. Re-verify version-specific claims at implementation time.

## Open questions

- Can a GPL-3.0 project lawfully load or redistribute nvngx_dlss.dll and link NGX, given RTX SDK License sections 2(c) and 4(e) and the FSF's plug-in position? Would a separately licensed optional module with a user-supplied DLL be enough, or is a GPL section 7 exception from all boiii copyright holders needed?
- Did AMD intend the signed DLLs to be MIT-licensed, as SDK 2.4.0's license.md literally says, or are they meant to be binary-only? Ask AMD before relying on either reading.
- Is a self-generated GUID Project ID (engine type CUSTOM) acceptable to NVIDIA for a distributed mod, as Luma does, or should Xerces request an NVIDIA Application ID?
- Which exact BlackOps3.exe build does Xerces/boiii run, and do the T2x jitter functions match Luma's 'Steam 2023 BOIII' offsets? Can they be hooked by pattern scan instead of fixed offsets?
- What depth convention does BO3 use (reversed-Z, infinite far plane)? How are viewmodel depth and motion vectors handled (isDepthHack), and are the motion vectors in render-resolution pixels?
- Does BO3 upscale render resolution below 100% bilinearly in the final composite, and can the 50% ResolutionPercent floor be lowered safely?
- Is per-object motion blur applied before or after the T2x resolve, and which other passes (GTAO denoise, volumetrics) depend on the 2-phase jitter?
- Is it worth moving the upscaler before tonemapping (HDR input) for better quality, or is the post-tonemap LDR hook point (as Luma uses) good enough?
- How much does the DX12 interop actually cost on target hardware (RX 6000/7000 INT8 FSR 4, non-Arc XeSS) at BO3's typical frame rates?
- Does Luma's BO3 mod with OptiScaler layered on top (DLSS calls swapped to FSR/XeSS) work in practice? Searches found no first-hand reports, and the Nexus pages returned 403.
