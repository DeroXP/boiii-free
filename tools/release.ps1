<#
.SYNOPSIS
  Builds a tester package of Xerces from the current commit (docs/DESIGN.md
  16.1): dist\xerces-<version>\, dist\xerces-<version>.zip and
  dist\symbols-<version>.zip.

.DESCRIPTION
  1. Refuses a tree with changes or submodules off their recorded commits: a
     package is built from a commit, so its SOURCE.txt and About's source link
     name the exact source. Warns when HEAD is not on origin (as of the last
     fetch; nothing is fetched): the link only works once it is pushed.
  2. Generates the build files (premake writes the version from VERSION, the
     commit and a clean-tree flag into build\src\xerces_version.h, checked
     here), builds xerces_tests and xerces_cli and runs the unit tests and
     tools\ci\cli_smoke.ps1, then builds Xerces.exe and boiii.exe (Release).
     tools\ci\check_switches.py must pass first: upstream's updater and the
     -beta download stay off.
  3. Stages dist\xerces-<version>\: the three programs, data\ and compat\ (the
     files Git tracks), LICENSE, THIRD_PARTY_NOTICES.txt and SOURCE.txt
     (tools\release\notices.py), README.txt (docs\TESTERS.md) and
     KNOWN_ISSUES.txt (docs\KNOWN_ISSUES.md), then SHA256SUMS.txt over every
     file. No .lib, .pdb, spike or test programs go in.
  4. Zips the folder, zips the PDBs into symbols-<version>.zip (kept by the
     team, never handed out with the package), and writes
     dist\SHA256SUMS-<version>.txt for the two zips.

  Never launches anything it builds, downloads nothing and pushes nothing.
  LLVM 23, Ninja and premake 5.0.0 come from PATH or -ToolsDir (laid out as
  tools\build.ps1 expects); RmlUi and FreeType from -ThirdPartyDir; their
  source archives, hashed into SOURCE.txt, from -DownloadsDir.
#>
param(
  [string]$ToolsDir = $env:XERCES_TOOLS,
  [string]$ThirdPartyDir = $env:XERCES_THIRD_PARTY,
  [string]$DownloadsDir = $env:XERCES_DOWNLOADS,
  [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }
# This PC's layout (CLAUDE.md) when nothing else is given.
$dev = 'C:\Users\Ginld\XercesDev'
if (-not $ToolsDir -and (Test-Path "$dev\tools")) { $ToolsDir = "$dev\tools" }
if (-not $ThirdPartyDir -and (Test-Path "$dev\third_party")) { $ThirdPartyDir = "$dev\third_party" }
if (-not $DownloadsDir -and (Test-Path "$dev\downloads")) { $DownloadsDir = "$dev\downloads" }

function Step([string]$text) { Write-Host "== $text" }

function Invoke-Checked([string]$what, [scriptblock]$command) {
  & $command
  if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

# Writes text with Windows line ends, UTF-8 without a byte-order mark.
function Write-Crlf([string]$path, [string]$text) {
  $text = ($text -replace "`r`n", "`n") -replace "`n", "`r`n"
  [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding $false))
}

# Windows PowerShell reads files without a byte-order mark as ANSI; these are UTF-8.
function Read-Utf8([string]$path) {
  [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
}

# Zips a folder with '/' in the entry names, as the zip format asks: Windows
# PowerShell's ZipFile.CreateFromDirectory writes '\'. prefix: the folder the
# entries go under, or none.
function New-Zip([string]$folder, [string]$zip, [string]$prefix) {
  Add-Type -AssemblyName System.IO.Compression
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  if (Test-Path $zip) { Remove-Item -Force -LiteralPath $zip }
  $archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
  try {
    foreach ($file in (Get-ChildItem -Recurse -File $folder | Sort-Object FullName)) {
      $entry = $file.FullName.Substring($folder.Length + 1) -replace '\\', '/'
      if ($prefix) { $entry = "$prefix/$entry" }
      [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $archive, $file.FullName, $entry, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
  } finally {
    $archive.Dispose()
  }
}

function Get-Sha256([string]$path) {
  (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
}

Push-Location $root
try {
  Step 'Checking the tree'
  $changes = git status --porcelain
  if ($LASTEXITCODE -ne 0) { throw 'git status failed' }
  if ($changes) {
    $changes | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }
    throw 'The tree has changes. Commit them first: a package is built from a commit.'
  }
  $submodules = git submodule status
  $off = @($submodules | Where-Object { $_ -match '^[-+U]' })
  if ($off.Count) {
    $off | ForEach-Object { Write-Host "  $_" }
    throw 'Submodules are missing or not at the commits HEAD records (git submodule update, no changes).'
  }
  $commit = (git rev-parse HEAD).Trim()
  $onOrigin = @(git branch -r --contains HEAD | Where-Object { $_ -match '^\s*origin/' })
  if (-not $onOrigin.Count) {
    Write-Warning "HEAD $commit is not on origin (as of the last fetch). SOURCE.txt and About link to it on GitHub: push it before handing the package out."
  }

  $version = (Read-Utf8 (Join-Path $root 'VERSION')).Trim()
  if ($version -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$') { throw "VERSION is not a version: '$version'" }
  $name = "xerces-$version"
  Step "Xerces $version at $commit"

  if ($ToolsDir -and (Test-Path $ToolsDir)) {
    $extra = @()
    $extra += Get-ChildItem $ToolsDir -Directory -Filter 'llvm-*' |
      ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter 'clang+llvm-*' } |
      ForEach-Object { Join-Path $_.FullName 'bin' }
    $extra += Get-ChildItem $ToolsDir -Directory -Filter 'ninja-*' | ForEach-Object FullName
    $extra += Get-ChildItem $ToolsDir -Directory -Filter 'premake-*' | ForEach-Object FullName
    $env:PATH = (($extra + $env:PATH) -join ';')
  }
  foreach ($tool in 'clang', 'ninja', 'premake5', 'python') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool is not on PATH. Pass -ToolsDir or set XERCES_TOOLS." }
  }
  if (-not $ThirdPartyDir -or -not (Test-Path (Join-Path $ThirdPartyDir 'rmlui'))) {
    throw 'Xerces.exe needs -ThirdPartyDir with rmlui\ and freetype\ (or XERCES_THIRD_PARTY).'
  }
  if (-not $DownloadsDir -or -not (Test-Path $DownloadsDir)) {
    throw 'SOURCE.txt needs -DownloadsDir with the RmlUi and FreeType source archives (or XERCES_DOWNLOADS).'
  }
  $env:XERCES_THIRD_PARTY = $ThirdPartyDir

  Step 'Lints'
  Invoke-Checked 'check_switches.py' { python -I tools\ci\check_switches.py }
  Invoke-Checked 'check_hosts.py' { python -I tools\ci\check_hosts.py }

  Step 'Generating the build files'
  Invoke-Checked 'premake5' { premake5 ninja }
  $versionHeader = Read-Utf8 (Join-Path $root 'build\src\xerces_version.h')
  if ($versionHeader -notmatch ('#define XERCES_GIT_HASH "' + $commit + '"') -or
      $versionHeader -notmatch '#define XERCES_GIT_DIRTY 0' -or
      $versionHeader -notmatch ('#define XERCES_VERSION "' + [regex]::Escape($version) + '"')) {
    throw 'build\src\xerces_version.h does not name this clean commit and VERSION.'
  }
  Invoke-Checked 'notices.py --check' { python -I tools\release\notices.py --check }

  Step 'Tests'
  Invoke-Checked 'building the tests and the CLI' { ninja -C build bin/x64/Release/xerces_tests.exe bin/x64/Release/xerces_cli.exe }
  Invoke-Checked 'unit tests' { & build\bin\x64\Release\xerces_tests.exe }
  $smoke = & (Join-Path $PSScriptRoot 'ci\cli_smoke.ps1') 6>&1 | ForEach-Object { "$_" }
  $smoke | Select-Object -Last 3 | ForEach-Object { Write-Host "  $_" }
  if (-not ($smoke | Select-Object -Last 1) -or ($smoke | Select-Object -Last 1) -notmatch 'ok$') { throw 'CLI smoke test failed' }

  Step 'Building Xerces.exe and boiii.exe'
  Invoke-Checked 'building the programs' { ninja -C build bin/x64/Release/Xerces.exe bin/x64/Release/boiii.exe bin/x64/Release/xerces_cli.exe }

  Step "Staging dist\$name"
  $bin = Join-Path $root 'build\bin\x64\Release'
  New-Item -ItemType Directory -Force $OutDir | Out-Null
  $stage = Join-Path $OutDir $name
  if (Test-Path $stage) { Remove-Item -Recurse -Force -LiteralPath $stage }
  New-Item -ItemType Directory -Force $stage | Out-Null
  foreach ($exe in 'Xerces.exe', 'boiii.exe', 'xerces_cli.exe') {
    Copy-Item -LiteralPath (Join-Path $bin $exe) -Destination $stage
  }
  foreach ($file in (git -c core.quotepath=off ls-files -- data compat)) {
    $target = Join-Path $stage ($file -replace '/', '\')
    New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $root $file) -Destination $target
  }
  Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $stage
  Invoke-Checked 'notices.py' {
    python -I tools\release\notices.py --notices (Join-Path $stage 'THIRD_PARTY_NOTICES.txt') `
      --source (Join-Path $stage 'SOURCE.txt') --downloads $DownloadsDir
  }
  Write-Crlf (Join-Path $stage 'README.txt') (Read-Utf8 (Join-Path $root 'docs\TESTERS.md'))
  Write-Crlf (Join-Path $stage 'KNOWN_ISSUES.txt') (Read-Utf8 (Join-Path $root 'docs\KNOWN_ISSUES.md'))

  $unwanted = @(Get-ChildItem -Recurse -File $stage | Where-Object {
      $_.Extension -in '.lib', '.pdb', '.ilk', '.exp', '.obj' -or $_.Name -like 'xerces_tests*' -or $_.Name -like 'xerces_spike*'
    })
  if ($unwanted.Count) { throw "Files that do not belong in the package: $($unwanted.FullName -join ', ')" }
  foreach ($need in 'data\launcher\main.html', 'data\ui_scripts', 'data\xerces\ui\shell.rml', 'compat\builds.json', 'compat\signatures.json') {
    if (-not (Test-Path (Join-Path $stage $need))) { throw "The package lacks $need" }
  }

  $files = Get-ChildItem -Recurse -File $stage | Sort-Object FullName
  $sums = foreach ($file in $files) {
    $relative = $file.FullName.Substring($stage.Length + 1)
    "$(Get-Sha256 $file.FullName)  $relative"
  }
  Write-Crlf (Join-Path $stage 'SHA256SUMS.txt') (($sums -join "`n") + "`n")

  Step 'Zips'
  $zip = Join-Path $OutDir "$name.zip"
  New-Zip $stage $zip $name

  $symbolsDir = Join-Path $OutDir "symbols-$version"
  if (Test-Path $symbolsDir) { Remove-Item -Recurse -Force -LiteralPath $symbolsDir }
  New-Item -ItemType Directory -Force $symbolsDir | Out-Null
  foreach ($pdb in 'Xerces.pdb', 'boiii.pdb', 'xerces_cli.pdb', 'tlsdll.pdb') {
    $path = Join-Path $bin $pdb
    if (-not (Test-Path $path)) { throw "$pdb is missing from build\bin\x64\Release" }
    Copy-Item -LiteralPath $path -Destination $symbolsDir
  }
  Write-Crlf (Join-Path $symbolsDir 'BUILD.txt') "Symbols for Xerces $version, commit $commit.`nFor reading crash dumps of this package only; not for testers.`n"
  $symbols = Join-Path $OutDir "symbols-$version.zip"
  New-Zip $symbolsDir $symbols ''
  Remove-Item -Recurse -Force -LiteralPath $symbolsDir

  $zipSums = foreach ($file in $zip, $symbols) { "$(Get-Sha256 $file)  $(Split-Path -Leaf $file)" }
  Write-Crlf (Join-Path $OutDir "SHA256SUMS-$version.txt") (($zipSums -join "`n") + "`n")

  Step 'Done'
  Write-Host "  $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB, $($files.Count + 1) files)"
  Write-Host "  $symbols"
  $zipSums | ForEach-Object { Write-Host "  $_" }
  if (-not $onOrigin.Count) { Write-Warning "Not on origin yet: push $commit before handing the package out." }
} finally {
  Pop-Location
}
