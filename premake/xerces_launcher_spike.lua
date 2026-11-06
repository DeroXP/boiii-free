-- The RmlUi launcher spike (docs/DESIGN.md 7.1, decision D4): two small programs that
-- draw the C / Signal home screen with RmlUi 6.3, one on Direct3D 11 and one on
-- OpenGL 3, to measure the gate before M3 commits to RmlUi: typography at 100, 150
-- and 200 percent, transitions at 144 Hz, first frame under 150 ms, controller
-- navigation.
--
-- RmlUi and FreeType are third-party sources kept outside the repository until the
-- spike decides (their hashes are in docs/TESTING.md). Point premake at the folder
-- holding rmlui/RmlUi-6.3 and freetype/freetype-2.14.3 with
-- --xerces-third-party=<dir> or the XERCES_THIRD_PARTY environment variable; without
-- them these projects are left out. HarfBuzz (harfbuzz/harfbuzz-15.0.0 in the same
-- folder) is optional and only the launcher uses it (premake/xerces_launcher.lua).

newoption({
  trigger = "xerces-third-party",
  value = "DIR",
  description = "Xerces: folder with rmlui/RmlUi-6.3, freetype/freetype-2.14.3 and optional harfbuzz/harfbuzz-15.0.0",
})

local root = _MAIN_SCRIPT_DIR
local third_party = _OPTIONS["xerces-third-party"] or os.getenv("XERCES_THIRD_PARTY")
if not third_party then
  return
end
local rmlui = path.join(third_party, "rmlui/RmlUi-6.3")
local freetype = path.join(third_party, "freetype/freetype-2.14.3")
if not (os.isdir(rmlui) and os.isdir(freetype)) then
  print("xerces_launcher_spike: no RmlUi or FreeType under " .. third_party .. "; skipped")
  return
end

local function root_path(relative)
  return path.join(root, relative)
end

-- The launcher (premake/xerces_launcher.lua) builds on the same libraries.
XERCES_RMLUI_DIR = rmlui
XERCES_FREETYPE_DIR = freetype

-- HarfBuzz for the launcher's text shaping (docs/DESIGN.md 7.1). Without it the
-- launcher keeps RmlUi's FreeType font engine.
local harfbuzz = path.join(third_party, "harfbuzz/harfbuzz-15.0.0")
if os.isfile(path.join(harfbuzz, "src/harfbuzz.cc")) then
  XERCES_HARFBUZZ_DIR = harfbuzz
else
  print("xerces_launcher: no HarfBuzz under " .. third_party .. "; the launcher uses RmlUi's FreeType font engine")
end

group("Xerces spike")

-- FreeType 2.14.3, the module list of its own builds/windows/vc2010 project.
project("xerces_freetype")
kind("StaticLib")
language("C")
warnings("Off")
defines({ "FT2_BUILD_LIBRARY", "_CRT_SECURE_NO_WARNINGS", "_CRT_NONSTDC_NO_WARNINGS" })
includedirs({ path.join(freetype, "include") })
local freetype_sources = {
  "src/autofit/autofit.c",
  "src/base/ftbase.c",
  "src/base/ftbbox.c",
  "src/base/ftbdf.c",
  "src/base/ftbitmap.c",
  "src/base/ftcid.c",
  "src/base/ftfstype.c",
  "src/base/ftgasp.c",
  "src/base/ftglyph.c",
  "src/base/ftgxval.c",
  "src/base/ftinit.c",
  "src/base/ftmm.c",
  "src/base/ftotval.c",
  "src/base/ftpatent.c",
  "src/base/ftpfr.c",
  "src/base/ftstroke.c",
  "src/base/ftsynth.c",
  "src/base/fttype1.c",
  "src/base/ftwinfnt.c",
  "src/bdf/bdf.c",
  "src/cache/ftcache.c",
  "src/cff/cff.c",
  "src/cid/type1cid.c",
  "src/dlg/dlgwrap.c",
  "src/gzip/ftgzip.c",
  "src/lzw/ftlzw.c",
  "src/pcf/pcf.c",
  "src/pfr/pfr.c",
  "src/psaux/psaux.c",
  "src/pshinter/pshinter.c",
  "src/psnames/psmodule.c",
  "src/raster/raster.c",
  "src/sfnt/sfnt.c",
  "src/smooth/smooth.c",
  "src/sdf/sdf.c",
  "src/svg/svg.c",
  "src/truetype/truetype.c",
  "src/type1/type1.c",
  "src/type42/type42.c",
  "src/winfonts/winfnt.c",
  "builds/windows/ftdebug.c",
  "builds/windows/ftsystem.c",
}
for _, source in ipairs(freetype_sources) do
  files({ path.join(freetype, source) })
end

-- RmlUi 6.3 core with its FreeType font engine, as a static library.
project("xerces_rmlui")
kind("StaticLib")
language("C++")
warnings("Off")
defines({ "RMLUI_STATIC_LIB", "RMLUI_CORE_EXPORTS", "RMLUI_FONT_ENGINE_FREETYPE", "_CRT_SECURE_NO_WARNINGS" })
includedirs({ path.join(rmlui, "Include"), path.join(freetype, "include") })
files({
  path.join(rmlui, "Include/RmlUi/Core/**.h"),
  path.join(rmlui, "Source/Core/**.h"),
  path.join(rmlui, "Source/Core/**.cpp"),
})

-- One program per renderer: RmlUi's backends each define the same Backend:: functions.
local function spike_program(name, backend_files, libraries, extra_defines)
  project(name)
  kind("WindowedApp")
  language("C++")
  defines({ "RMLUI_STATIC_LIB", "NOMINMAX" })
  defines(extra_defines)
  includedirs({ path.join(rmlui, "Include"), path.join(rmlui, "Backends"), root_path("src") })
  files({
    root_path("src/xerces_launcher_spike/*.hpp"),
    root_path("src/xerces_launcher_spike/*.cpp"),
    path.join(rmlui, "Backends/RmlUi_Platform_Win32.cpp"),
  })
  files(backend_files)
  -- RmlUi's own backend files (a "C:" in an absolute path would break the filter).
  filter({ "files:**/Backends/**" })
  warnings("Off")
  filter({})
  links({ "xerces_rmlui", "xerces_freetype", "imm32", "user32", "gdi32", "shell32", "xinput" })
  links(libraries)
end

-- dx11/ is RmlUi's Win32_DX11 backend with timing marks added.
spike_program("xerces_spike_dx11", {
  root_path("src/xerces_launcher_spike/dx11/RmlUi_Backend_Win32_DX11.cpp"),
  path.join(rmlui, "Backends/RmlUi_Renderer_DX11.cpp"),
}, { "d3d11", "dxgi", "d3dcompiler" }, {})

spike_program("xerces_spike_gl3", {
  root_path("src/xerces_launcher_spike/gl3/RmlUi_Backend_Win32_GL3.cpp"),
  path.join(rmlui, "Backends/RmlUi_Renderer_GL3.cpp"),
}, { "opengl32" }, { "XERCES_SPIKE_GL3" })

group("")
