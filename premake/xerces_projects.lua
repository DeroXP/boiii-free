-- Xerces' headless projects (docs/DESIGN.md 3.2): xerces_core, its tests and the CLI.
--
-- They must not include std_include.hpp or game/ headers, or call dependencies.imports(),
-- because game/symbol.hpp routes through the fenced arxan proxy. Paths are built from
-- _MAIN_SCRIPT_DIR because premake resolves relative paths against the calling script.

local root = _MAIN_SCRIPT_DIR

local function root_path(relative)
  return path.join(root, relative)
end

local function headless_includes()
  includedirs({
    root_path("src"),
    root_path("deps/rapidjson/include"),
    root_path("deps/zstd/lib"),
  })
  defines({ "RAPIDJSON_HAS_STDSTRING", "ZSTD_STATIC_LINKING_ONLY" })
end

-- The workspace links everything for the Windows subsystem; these are console programs.
local function console_app()
  kind("ConsoleApp")
  filter("toolset:not msc*")
  removebuildoptions({ "-Wl,/subsystem:windows" })
  removelinkoptions({ "-Wl,/subsystem:windows" })
  linkoptions({ "-Wl,/subsystem:console" })
  filter("toolset:msc*")
  linkoptions({ "/SUBSYSTEM:CONSOLE" })
  filter({})
end

group("Xerces")

project("xerces_core")
kind("StaticLib")
language("C++")
files({ root_path("src/xerces_core/**.hpp"), root_path("src/xerces_core/**.cpp") })
headless_includes()

project("xerces_tests")
console_app()
language("C++")
files({ root_path("src/xerces_tests/**.hpp"), root_path("src/xerces_tests/**.cpp") })
headless_includes()
-- MinHook with Xerces' thread walk (deps/premake/minhook.lua), tested on its own.
includedirs({ root_path("deps/minhook/include") })
links({ "xerces_core", "zstd", "advapi32", "minhook" })

project("xerces_cli")
console_app()
language("C++")
files({ root_path("src/xerces_cli/**.hpp"), root_path("src/xerces_cli/**.cpp") })
headless_includes()
links({ "xerces_core", "zstd", "advapi32" })

group("")

-- The RmlUi launcher spike and the launcher, only when the third-party sources
-- (RmlUi, FreeType) are given (D4).
include("xerces_launcher_spike.lua")
include("xerces_launcher.lua")

-- The game client uses xerces_core for Xerces' own parts in src/client/xerces
-- (the signature registry, the launch journal, JSON logs; DESIGN.md 3.3, 5.6).
-- premake continues the existing client project here; premake5.lua is not
-- edited.
project("client")
links({ "xerces_core" })
