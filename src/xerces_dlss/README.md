# xerces_dlss: the optional NVIDIA DLSS module

MIT licence (`LICENSE` here). Not part of Xerces' GPL release: the module is
linked with NVIDIA's DLSS SDK and loads NVIDIA's `nvngx_dlss.dll`, which stay
under NVIDIA's RTX SDKs licence and outside this repository (docs/DESIGN.md 13.4
and D10).

The game client (`src/client/xerces/temporal.cpp`) loads `xerces_dlss.dll` when
the player picks NVIDIA DLSS / DLAA, through the interface in `xerces_dlss.h`.
The client offers that choice only when `xerces_dlss.dll` and `nvngx_dlss.dll`
are next to `boiii.exe` and the GPU is NVIDIA's.

## Building it (this PC only)

1. Get NVIDIA's DLSS SDK (github.com/NVIDIA/DLSS) and accept its licence. The
   module needs `include\`, `lib\Windows_x86_64\x64\nvsdk_ngx_s.lib` and
   `lib\Windows_x86_64\rel\nvngx_dlss.dll` (310.9.16 was used).
2. `premake5 ninja --with-nvidia-dlss-sdk=<the SDK folder>`, then build
   `bin/x64/Release/xerces_dlss.dll` and
   `bin/x64/Release/xerces_dlss_check.exe`.
3. Copy `nvngx_dlss.dll` next to the built module.
4. `xerces_dlss_check.exe` starts DLSS on the first hardware adapter and runs
   one DLAA frame and one upscaled frame on blank textures.

Do not hand the module or NVIDIA's DLL to anyone until the licence check D10
asks for has said how they may ship.
