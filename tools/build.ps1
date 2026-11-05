<#
.SYNOPSIS
  Builds Xerces (docs/DESIGN.md 15.5).

.DESCRIPTION
  -Tests   generates the build files, builds xerces_tests and xerces_cli, and
           runs the unit tests and a CLI smoke test on the fixtures.
  -Client  builds the game client (boiii.exe) as well. Needs the Visual Studio
           "C++ ATL for latest v143 build tools" component.
  -Spike   builds the RmlUi launcher spike (xerces_spike_dx11.exe and
           xerces_spike_gl3.exe, DESIGN.md 7.1). Needs RmlUi 6.3 and FreeType
           2.14.3 under -ThirdPartyDir (default: the tools folder's sibling
           third_party\), laid out as rmlui\RmlUi-6.3 and freetype\freetype-2.14.3.

  LLVM 23, Ninja and premake 5.0.0 must be on PATH, or in -ToolsDir laid out
  as llvm-*\clang+llvm-*\bin, ninja-*\ and premake-*\.
  Never launches anything it builds.
#>
param(
  [switch]$Tests,
  [switch]$Client,
  [switch]$Spike,
  [string]$ToolsDir = $env:XERCES_TOOLS,
  [string]$ThirdPartyDir = $env:XERCES_THIRD_PARTY
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if ($ToolsDir -and (Test-Path $ToolsDir)) {
  $extra = @()
  $extra += Get-ChildItem $ToolsDir -Directory -Filter 'llvm-*' |
    ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter 'clang+llvm-*' } |
    ForEach-Object { Join-Path $_.FullName 'bin' }
  $extra += Get-ChildItem $ToolsDir -Directory -Filter 'ninja-*' | ForEach-Object FullName
  $extra += Get-ChildItem $ToolsDir -Directory -Filter 'premake-*' | ForEach-Object FullName
  $env:PATH = (($extra + $env:PATH) -join ';')
}

foreach ($tool in 'clang', 'ninja', 'premake5') {
  if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
    throw "$tool is not on PATH. Pass -ToolsDir or set XERCES_TOOLS."
  }
}

if (-not $Tests -and -not $Client -and -not $Spike) { $Tests = $true }

if ($Spike) {
  if (-not $ThirdPartyDir -and $ToolsDir) {
    $ThirdPartyDir = Join-Path (Split-Path -Parent $ToolsDir) 'third_party'
  }
  if (-not $ThirdPartyDir -or -not (Test-Path (Join-Path $ThirdPartyDir 'rmlui'))) {
    throw 'The spike needs -ThirdPartyDir with rmlui\ and freetype\ (or XERCES_THIRD_PARTY).'
  }
  # premake/xerces_launcher_spike.lua reads it; projects are skipped without it.
  $env:XERCES_THIRD_PARTY = $ThirdPartyDir
}

Push-Location $root
try {
  git submodule update --init --recursive
  if ($LASTEXITCODE -ne 0) { throw 'git submodule update failed' }
  premake5 ninja
  if ($LASTEXITCODE -ne 0) { throw 'premake5 failed' }

  if ($Tests) {
    ninja -C build bin/x64/Release/xerces_tests.exe bin/x64/Release/xerces_cli.exe
    if ($LASTEXITCODE -ne 0) { throw 'building the core failed' }
    & build\bin\x64\Release\xerces_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'unit tests failed' }
    & build\bin\x64\Release\xerces_cli.exe conflicts check `
      --mods-dir tests/fixtures/mods --mod fixture.alpha --mod fixture.beta `
      --stock tests/fixtures/stock
    if ($LASTEXITCODE -ne 0) { throw 'CLI smoke test failed' }
  }

  if ($Client) {
    ninja -C build Release_x64
    if ($LASTEXITCODE -ne 0) { throw 'building the client failed' }
    Write-Host 'Built build\bin\x64\Release\boiii.exe (not launched).'
  }

  if ($Spike) {
    ninja -C build bin/x64/Release/xerces_spike_dx11.exe bin/x64/Release/xerces_spike_gl3.exe
    if ($LASTEXITCODE -ne 0) { throw 'building the spike failed' }
    Write-Host 'Built build\bin\x64\Release\xerces_spike_dx11.exe and xerces_spike_gl3.exe (not launched).'
  }
} finally {
  Pop-Location
}
