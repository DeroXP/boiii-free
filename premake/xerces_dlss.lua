-- The optional NVIDIA DLSS module (src/xerces_dlss, MIT; docs/DESIGN.md 13.4
-- and D10). Only with --with-nvidia-dlss-sdk=<folder>, the DLSS SDK's
-- include\ and lib\ (NVIDIA's files, under NVIDIA's licence, kept outside the
-- repository). Never part of the GPL release: tools\release.ps1 runs premake
-- without the option, so neither project exists there. Copy the SDK's
-- lib\Windows_x86_64\rel\nvngx_dlss.dll next to the built xerces_dlss.dll.

local sdk = _OPTIONS["with-nvidia-dlss-sdk"]
if not sdk or sdk == "" then
  return
end

local root = _MAIN_SCRIPT_DIR

local function root_path(relative)
  return path.join(root, relative)
end

group("Xerces")

project("xerces_dlss")
kind("SharedLib")
language("C++")
files({ root_path("src/xerces_dlss/xerces_dlss.h"), root_path("src/xerces_dlss/xerces_dlss.cpp") })
externalincludedirs({ path.join(sdk, "include") })
libdirs({ path.join(sdk, "lib/Windows_x86_64/x64") })
links({ "nvsdk_ngx_s", "d3d11", "advapi32", "user32" })

project("xerces_dlss_check")
kind("ConsoleApp")
language("C++")
files({ root_path("src/xerces_dlss/xerces_dlss.h"), root_path("src/xerces_dlss/check.cpp") })
links({ "d3d11" })
filter("toolset:not msc*")
removebuildoptions({ "-Wl,/subsystem:windows" })
removelinkoptions({ "-Wl,/subsystem:windows" })
linkoptions({ "-Wl,/subsystem:console" })
filter("toolset:msc*")
linkoptions({ "/SUBSYSTEM:CONSOLE" })
filter({})
