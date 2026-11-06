-- Xerces.exe, the launcher (docs/DESIGN.md 3.2, 7; D4 RmlUi, D13 the name): RmlUi
-- 6.3 on Direct3D 11 with the views in src/xerces_launcher/views and the documents in
-- data/xerces/ui. It links xerces_core for everything that is not UI.
--
-- Needs the same third-party sources as the spike (xerces_launcher_spike.lua sets
-- XERCES_RMLUI_DIR when they are present); without them the project is left out.
--
-- Text shaping (DESIGN.md 7.1): with HarfBuzz 15.0.0 in the same third-party folder
-- (XERCES_HARFBUZZ_DIR, also set there) the launcher shapes text with RmlUi's HarfBuzz
-- font engine, which reads OpenType (GPOS) kerning; without it, with RmlUi's own
-- FreeType engine. XERCES_LAUNCHER_HARFBUZZ tells the code which one it has.

if not XERCES_RMLUI_DIR then
  return
end

local root = _MAIN_SCRIPT_DIR
local rmlui = XERCES_RMLUI_DIR
local freetype = XERCES_FREETYPE_DIR
local harfbuzz = XERCES_HARFBUZZ_DIR

local function root_path(relative)
  return path.join(root, relative)
end

-- RmlUi 6.3's HarfBuzz font engine lives in its samples, not in RmlUi's core.
local harfbuzz_engine = path.join(rmlui, "Samples/basic/harfbuzz/src")

group("Xerces")

if harfbuzz then
  -- HarfBuzz 15.0.0 ("Old MIT" licence) from its amalgamated source with the
  -- FreeType integration, as BUILD.md describes: no ICU, GLib, Cairo, Graphite or
  -- platform shapers. HAVE_FT_GET_VAR_BLEND_COORDINATES, which HarfBuzz's own builds
  -- set for any recent FreeType, is left out on purpose: with it, HarfBuzz 15.0.0
  -- copies the FreeType face's variation into the font as normalized coordinates
  -- (whose design values it then records as NaN) and passes those NaNs back to
  -- FreeType when the font changes, so every Inter Tight weight but Regular was
  -- measured at its thinnest. Without it the named instance comes from the face
  -- index alone, in both HarfBuzz and FreeType.
  project("xerces_harfbuzz")
  kind("StaticLib")
  language("C++")
  warnings("Off")
  defines({ "HAVE_FREETYPE", "HAVE_FT_GET_TRANSFORM" })
  includedirs({ path.join(harfbuzz, "src"), path.join(freetype, "include") })
  files({ path.join(harfbuzz, "src/harfbuzz.cc") })

  -- The engine from RmlUi's sample (MIT, RmlUi's LICENSE.txt), built from RmlUi's
  -- folder except FontFaceHandleHarfBuzz.cpp, whose copy in src/xerces_rmlui_harfbuzz
  -- keeps the pen's fractions within words (the file says what changed).
  -- As the sample's CMakeLists.txt says, it uses RmlUi's private Core headers and
  -- the FreeType helpers inside RmlUi's core, which a Windows DLL build of RmlUi
  -- does not export; xerces_rmlui is a static library, so they link. The sample's
  -- folder comes after Source/Core, so RmlUi's own headers (it has a
  -- TextureLayout.h too) win there.
  project("xerces_rmlui_harfbuzz")
  kind("StaticLib")
  language("C++")
  warnings("Off")
  defines({ "RMLUI_STATIC_LIB", "_CRT_SECURE_NO_WARNINGS" })
  includedirs({
    path.join(rmlui, "Include"),
    path.join(rmlui, "Source/Core"),
    path.join(freetype, "include"),
    path.join(harfbuzz, "src"),
    harfbuzz_engine,
  })
  files({
    path.join(harfbuzz_engine, "*.h"),
    path.join(harfbuzz_engine, "*.cpp"),
    root_path("src/xerces_rmlui_harfbuzz/*.cpp"),
  })
  removefiles({ path.join(harfbuzz_engine, "main.cpp"), path.join(harfbuzz_engine, "FontFaceHandleHarfBuzz.cpp") })
end

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
if harfbuzz then
  defines({ "XERCES_LAUNCHER_HARFBUZZ" })
  includedirs({ harfbuzz_engine, path.join(harfbuzz, "src") })
  links({ "xerces_rmlui_harfbuzz", "xerces_harfbuzz" })
end

group("")
