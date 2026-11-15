# A mod stack on many maps, one game per map, through Xerces.exe (map-check.ps1
# -Live for each), with one line per map in <OutDir>\sweep-<Tag>.txt:
#
#   tools\dev\session\sweep.ps1 -Tag rc1 -Stack 3171632315,3000417367 `
#     -Maps zm_zod,zm_tomb -ExtraFlags '-borderless','-xerces-db-pool' -PollPools
#
# (Flags in quotes: PowerShell reads a bare -name in a list as a parameter.)
#
# -PollPools runs tools\dev\re\poll_pools.py beside it: the most of the model,
# mesh, material and image pools each game used, in <OutDir>\pools-<Tag>.json
# by process id (results.jsonl names each run's pid and map).
# tools\site\sweep_to_tested.py turns both into the website's site\tested.json.
#
# -Live starts the real game: only with the user's go-ahead for that session
# (CLAUDE.md, Hard rules). A game or launcher still running from an earlier
# map is ended before the next.
param(
  [Parameter(Mandatory = $true)][string]$Tag,
  [Parameter(Mandatory = $true)][string[]]$Maps,
  [string[]]$Stack = @(),
  [string[]]$ExtraFlags = @(),
  [int]$PlaySeconds = 30,
  [string]$OutDir = 'C:\Users\Ginld\XercesDev\map-checks',
  [switch]$PollPools
)
$ErrorActionPreference = 'Continue'
$check = Join-Path $PSScriptRoot 'map-check.ps1'
$results = Join-Path $OutDir 'results.jsonl'
$out = Join-Path $OutDir "sweep-$Tag.txt"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$poller = $null
$pools = Join-Path $OutDir "pools-$Tag.json"
if ($PollPools) {
  Remove-Item -ErrorAction SilentlyContinue "$pools.stop"
  $script = Join-Path (Split-Path -Parent $PSScriptRoot) 're\poll_pools.py'
  $poller = Start-Process -PassThru -WindowStyle Hidden python -ArgumentList '-I', "`"$script`"", "`"$pools`""
}
try {
  "sweep $Tag started $(Get-Date -Format 'HH:mm:ss'): $($Stack.Count) mods, flags $($ExtraFlags -join ' ')" |
    Tee-Object -FilePath $out
  foreach ($map in $Maps) {
    if (Get-Process -Name BlackOps3, boiii, Xerces -ErrorAction SilentlyContinue) {
      Get-Process -Name boiii, Xerces -ErrorAction SilentlyContinue | Stop-Process -Force
      Start-Sleep -Seconds 3
    }
    & $check -Map $map -Stack $Stack -ExtraFlags $ExtraFlags -OutDir $OutDir -SkipCinematic `
      -PlaySeconds $PlaySeconds -Live *> $null
    $line = Get-Content $results -Encoding UTF8 | Select-Object -Last 1
    $r = $line.TrimStart([char]0xFEFF) | ConvertFrom-Json
    ('{0,-20} {1,-14} menu {2,5} s, load {3,5} s  {4}' -f $map, $r.result, $r.menu_seconds, $r.load_seconds,
      ($r.detail -replace '\s+', ' ')) | Tee-Object -FilePath $out -Append
  }
  "done $(Get-Date -Format 'HH:mm:ss')" | Tee-Object -FilePath $out -Append
} finally {
  if ($poller) {
    New-Item -ItemType File -Force "$pools.stop" | Out-Null
    Start-Sleep -Seconds 3
    if (-not $poller.HasExited) { $poller | Stop-Process -Force }
  }
}
