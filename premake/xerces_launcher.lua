-- Xerces.exe, the launcher (docs/DESIGN.md 3.2, 7; D4 RmlUi, D13 the name): RmlUi
-- 6.3 on Direct3D 11 with the views in src/xerces_launcher/views and the documents in
-- data/xerces/ui. It links xerces_core for everything that is not UI.
--
-- Needs the same third-party sources as the spike (xerces_launcher_spike.lua sets
-- XERCES_RMLUI_DIR when they are present); without them the project is left out.

if not XERCES_RMLUI_DIR then
  return
end

local root = _MAIN_SCRIPT_DIR
local rmlui = XERCES_RMLUI_DIR
local freetype = XERCES_FREETYPE_DIR

local function root_path(relative)
  return path.join(root, relative)
end

group("Xerces")

project("xerces_launcher")
kind("WindowedApp")
language("C++")
targetname("Xerces")
defines({ "RMLUI_STATIC_LIB", "NOMINMAX", "RAPIDJSON_HAS_STDSTRING", "ZSTD_STATIC_LINKING_ONLY" })
includedirs({
  root_path("src"),
  root_path("deps/rapidjson/include"),
  root_path("deps/zstd/lib"),
  path.join(rmlui, "Include"),
  path.join(rmlui, "Backends"),
})
files({
  root_path("src/xerces_launcher/**.hpp"),
  root_path("src/xerces_launcher/**.cpp"),
  path.join(rmlui, "Backends/RmlUi_Platform_Win32.cpp"),
  path.join(rmlui, "Backends/RmlUi_Renderer_DX11.cpp"),
})
-- RmlUi's own backend files (a "C:" in an absolute path would break the filter).
filter({ "files:**/Backends/**" })
warnings("Off")
filter({})
links({
  "xerces_core",
  "zstd",
  "zlib",
  "xerces_rmlui",
  "xerces_freetype",
  "advapi32",
  "d3d11",
  "d3dcompiler",
  "dwmapi",
  "dxgi",
  "gdi32",
  "imm32",
  "ole32",
  "shell32",
  "user32",
  "xinput",
})

group("")
