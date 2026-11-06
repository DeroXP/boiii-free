# Checks that an idle Xerces.exe draws nothing and wakes up rarely, and that
# what should still draw does (DESIGN.md 12.7; docs/TESTING.md, Launcher
# speed). Each case is a short dry-launch run with a script; the frames and
# passes between two `activity` steps come from the launcher.activity line
# the second one logs. Starts nothing but Xerces.exe, always with
# --dry-launch, and for the game case a stand-in game: a sleeping
# powershell.exe announced in <home>\run, as
# tools\dev\bench\launcher-idle.ps1 does. The real game is never started.
# About a minute once the home has a hash cache.
#
#   tools\dev\launcher\check-idle.ps1 -HomeDir C:\Users\Ginld\XercesHome-idle
#
# -HomeDir must be outside AppData (CLAUDE.md). A new home's first Workshop
# scan takes a while; the first run waits for it, as m1-check.ps1 does.
param(
  [Parameter(Mandatory)] [string]$HomeDir,
  [string]$Exe = ''
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Exe) { $Exe = Join-Path $repo 'build\bin\x64\Release\Xerces.exe' }
if (-not (Test-Path $Exe)) { throw "no launcher build at $Exe (tools\build.ps1 -Launcher)" }
$Exe = (Resolve-Path $Exe).Path
if ($HomeDir -like "$env:LOCALAPPDATA*" -or $HomeDir -like "$env:APPDATA*") {
  throw '-HomeDir must be outside AppData (CLAUDE.md)'
}
$logs = Join-Path $HomeDir 'logs'
$runDir = Join-Path $HomeDir 'run'
New-Item -ItemType Directory -Force $HomeDir, $logs, $runDir | Out-Null

function Launcher-Log($process, [datetime]$from) {
  # Process ids come back: the newest log of this id written since the start.
  Get-ChildItem $logs -Filter "*-launcher-$($process.Id).jsonl" -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $from.AddSeconds(-1) } |
    Sort-Object LastWriteTime | Select-Object -Last 1
}

function Events($log) {
  @(Get-Content $log.FullName | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
}

# A new home has no hash cache: one run with no steps waits for the scan.
$from = Get-Date
$warm = Start-Process -FilePath $Exe -PassThru -ArgumentList @(
  '--home', "`"$HomeDir`"", '--dry-launch', '--skip-boot', '--view', 'home', '--seconds', 900)
$deadline = (Get-Date).AddSeconds(900)
$scanned = $false
while (-not $scanned -and -not $warm.HasExited -and (Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 500
  $log = Launcher-Log $warm $from
  $scanned = $log -and (Events $log | Where-Object { $_.event -eq 'launcher.catalog.scanned' })
}
if (-not $warm.HasExited) {
  [void]$warm.CloseMainWindow()
  if (-not $warm.WaitForExit(10000)) { $warm.Kill() }
}
if (-not $scanned) { throw 'the Workshop scan did not finish' }

# Each case: the screen, the steps (the first at 1 s; "+ms verb arg") and
# what the span between the two activity steps must show. `state` is the
# launcher.activity state the span is counted in. Passes come from the
# 100 ms waits (a second behind a running game), plus a few more for
# Windows' own timer messages in the first seconds after start.
$cases = @(
  @{ name = 'in front, idle'; view = 'home'; state = 'front'
    steps = @('+0 front on', '+2000 activity', '+4000 activity')
    frames = @(0, 0); wakes_per_second = 15 },
  @{ name = 'in front, a caret blinks'; view = 'servers'; state = 'front'
    steps = @('+0 front on', '+500 focus address', '+1500 activity', '+4000 activity')
    frames = @(3, 12); wakes_per_second = 15 },
  @{ name = 'behind, a focused text field'; view = 'servers'; state = 'behind'
    steps = @('+0 front off', '+500 focus address', '+1500 activity', '+4000 activity')
    frames = @(0, 0); wakes_per_second = 15 },
  @{ name = 'minimized, a screen shown'; view = 'home'; state = 'minimized'
    steps = @('+1000 minimize', '+500 activity', '+300 view mods', '+2500 activity')
    frames = @(0, 0); wakes_per_second = 15 },
  @{ name = 'a game running, launcher behind'; view = 'servers'; state = 'game_running'; game = $true
    steps = @('+0 front off', '+500 focus address', '+5000 activity', '+5000 activity')
    frames = @(0, 0); wakes_per_second = 2 },
  @{ name = 'in front, a transition draws while it runs'; view = 'home'; state = 'front'
    steps = @('+0 front on', '+500 focus launch-modded', '+1500 activity', '+0 focus launch-vanilla', '+1000 activity')
    frames = @(10, 1000); wakes_per_second = 1000 }
)

$failed = 0
foreach ($case in $cases) {
  $sleeper = $null
  $entryFile = $null
  try {
    if ($case.game) {
      $sleeper = Start-Process powershell.exe -ArgumentList '-NoProfile', '-Command', 'Start-Sleep -Seconds 300' -WindowStyle Hidden -PassThru
      Start-Sleep -Milliseconds 300
      $entry = [ordered]@{
        api = 1; image = (Get-Process -Id $sleeper.Id).Path; pid = $sleeper.Id
        pipe = '\\.\pipe\xerces-idle-check'; role = 'game'
        started = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
      }
      $entryFile = Join-Path $runDir "game-$($sleeper.Id).json"
      [IO.File]::WriteAllText($entryFile, ($entry | ConvertTo-Json -Compress))
    }
    $stepsFile = Join-Path $HomeDir 'idle-steps.txt'
    [IO.File]::WriteAllLines($stepsFile, [string[]]($case.steps + '+0 quit'))
    $from = Get-Date
    $process = Start-Process -FilePath $Exe -PassThru -ArgumentList @(
      '--home', "`"$HomeDir`"", '--dry-launch', '--skip-boot', '--view', $case.view,
      '--script', "`"$stepsFile`"", '--seconds', 60)
    if (-not $process.WaitForExit(90000)) { $process.Kill(); throw "$($case.name): the launcher did not exit" }
    $log = Launcher-Log $process $from
    $lines = @(if ($log) { Events $log | Where-Object { $_.event -eq 'launcher.activity' } })
    $span = if ($lines.Count -ge 2) { $lines[1].data } else { $null }
    $counted = if ($span) { $span.($case.state) } else { $null }
    if (-not $counted -or $counted.seconds -le 0) {
      $verdict = 'FAIL'
      $detail = "no $($case.state) span between the activity steps"
    } else {
      $perSecond = $counted.wakes / [math]::Max($span.seconds, 0.1)
      $ok = $counted.frames -ge $case.frames[0] -and $counted.frames -le $case.frames[1] -and
        $perSecond -le $case.wakes_per_second
      $verdict = if ($ok) { 'ok' } else { 'FAIL' }
      $detail = '{0} frames, {1} passes in {2} s ({3:N1} a second)' -f $counted.frames, $counted.wakes, $span.seconds, $perSecond
    }
  } finally {
    if ($sleeper -and -not $sleeper.HasExited) { Stop-Process -Id $sleeper.Id -Force }
    if ($entryFile -and (Test-Path $entryFile)) { Remove-Item $entryFile }
  }
  if ($verdict -ne 'ok') { $failed++ }
  '{0,-4} {1,-44} {2}' -f $verdict, $case.name, $detail
}
exit ([int]($failed -gt 0))
