# Builds and runs tools\dev\launcher\font-width-check.cpp: the launcher's
# HarfBuzz font engine must never draw a line of text wider than RmlUi lays it
# out (docs/DESIGN.md 7.1, docs/TESTING.md, Launcher text shaping). Run it after
# changing src\xerces_rmlui_harfbuzz or updating RmlUi or HarfBuzz. Starts
# nothing but the check itself.
#
#   tools\dev\launcher\font-width-check.ps1 -ToolsDir C:\Users\Ginld\XercesDev\tools
#
# Needs the launcher built with HarfBuzz first (tools\build.ps1 -Launcher with
# third_party\harfbuzz\harfbuzz-15.0.0): it links the libraries that build left
# in build\bin\x64\Release. The strings are every key of
# data\xerces\i18n\template.json; the check measures them in each Inter Tight
# weight and IBM Plex Mono at 16 sizes from 12 to 120 px. -Out (default
# build\font-width-check in the repository) holds the program. Exit code 0
# when every line fits.
param(
  [string]$ToolsDir = $env:XERCES_TOOLS,
  [string]$ThirdPartyDir = $env:XERCES_THIRD_PARTY,
  [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Out) { $Out = Join-Path $repo 'build\font-width-check' }
if (-not $ThirdPartyDir -and $ToolsDir) { $ThirdPartyDir = Join-Path (Split-Path -Parent $ToolsDir) 'third_party' }
if ($ToolsDir -and (Test-Path $ToolsDir)) {
  $llvm = Get-ChildItem $ToolsDir -Directory -Filter 'llvm-*' |
    ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter 'clang+llvm-*' } |
    ForEach-Object { Join-Path $_.FullName 'bin' }
  $env:PATH = ((@($llvm) + $env:PATH) -join ';')
}
if (-not (Get-Command clang++ -ErrorAction SilentlyContinue)) { throw 'clang++ is not on PATH. Pass -ToolsDir or set XERCES_TOOLS.' }

$rmlui = Join-Path $ThirdPartyDir 'rmlui\RmlUi-6.3'
$freetype = Join-Path $ThirdPartyDir 'freetype\freetype-2.14.3'
$harfbuzz = Join-Path $ThirdPartyDir 'harfbuzz\harfbuzz-15.0.0'
foreach ($dir in $rmlui, $freetype, $harfbuzz) {
  if (-not (Test-Path $dir)) { throw "$dir is missing (-ThirdPartyDir or XERCES_THIRD_PARTY)" }
}
$lib = Join-Path $repo 'build\bin\x64\Release'
$libraries = 'xerces_rmlui_harfbuzz.lib', 'xerces_rmlui.lib', 'xerces_harfbuzz.lib', 'xerces_freetype.lib' |
  ForEach-Object { Join-Path $lib $_ }
foreach ($library in $libraries) {
  if (-not (Test-Path $library)) { throw "$library is missing: build the launcher with HarfBuzz first (tools\build.ps1 -Launcher)" }
}

New-Item -ItemType Directory -Force $Out | Out-Null

# The flags of the launcher's own build (premake5.lua and premake\xerces_launcher.lua).
$exe = Join-Path $Out 'font-width-check.exe'
$arguments = @(
  '-std=c++20', '-O2', '-m64', '-fms-runtime-lib=static', '-fuse-ld=lld', '-w',
  '-DRMLUI_STATIC_LIB', '-DNOMINMAX', '-DNDEBUG', '-D_WINDOWS', '-DWIN32',
  "-I$rmlui\Include", "-I$rmlui\Source\Core", "-I$freetype\include", "-I$harfbuzz\src",
  "-I$rmlui\Samples\basic\harfbuzz\src",
  (Join-Path $PSScriptRoot 'font-width-check.cpp'), '-o', $exe
) + $libraries + @('-luser32')
& clang++ @arguments
if ($LASTEXITCODE -ne 0) { throw 'building font-width-check failed' }

& $exe (Join-Path $repo 'data\xerces\fonts') (Join-Path $repo 'data\xerces\i18n\template.json')
exit $LASTEXITCODE
