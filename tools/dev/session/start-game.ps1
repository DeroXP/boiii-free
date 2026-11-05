# Starts this repository's client build for a supervised session
# (docs/TESTING.md): from the Black Ops III folder, with XERCES_DATA_PATH set to
# the repository's data/, intros skipped and upstream's launcher bypassed.
#
#   tools\dev\session\start-game.ps1 -GamePath "D:\...\Call of Duty Black Ops III"
#   tools\dev\session\start-game.ps1 -GamePath $bo3 -Mod 1944930083 -ExtraMods 3171632315,3656779213 -Safe
#
# Flags always go before the +set commands: the engine joins everything after
# "+set <dvar>" up to the next "+" into the value, so "+set fs_game 123 -safe"
# sets fs_game to "123 -safe".
#
# Never start a build whose updater is not disabled (commit 62e28935 disables
# it); this script only starts build\bin\x64\Release\boiii.exe from this
# repository.
param(
  [Parameter(Mandatory = $true)][string]$GamePath,
  [string]$Mod = "",
  [string[]]$ExtraMods = @(),
  [switch]$Safe,
  [string[]]$Flags = @()
)

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$exe = Join-Path $repo "build\bin\x64\Release\boiii.exe"
$data = Join-Path $repo "data"
if (-not (Test-Path $exe)) { throw "no client build at $exe (tools\build.ps1 -Client)" }
if (-not (Test-Path (Join-Path $GamePath "BlackOps3.exe"))) { throw "no BlackOps3.exe in $GamePath" }
if (Get-Process boiii -ErrorAction SilentlyContinue) { throw "boiii.exe is already running" }

$arguments = @("-launch", "-nointro")
if ($Safe) { $arguments += "-safe" }
$arguments += $Flags
if ($Mod) { $arguments += @("+set", "fs_game", $Mod) }
if ($ExtraMods.Count -gt 0) { $arguments += @("+set", "fs_extramods", ($ExtraMods -join ",")) }

$env:XERCES_DATA_PATH = $data
$game = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $GamePath -PassThru
"started boiii.exe (pid $($game.Id)): $($arguments -join ' ')"
"XERCES_DATA_PATH=$data"
