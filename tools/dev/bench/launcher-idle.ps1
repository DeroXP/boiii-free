# Measures what an idle Xerces.exe costs in each window state, from the
# launcher's own launcher.activity lines (docs/TESTING.md, Launcher speed;
# DESIGN.md 12.7). Starts nothing but Xerces.exe, with --dry-launch, and a
# stand-in for the game: a sleeping powershell.exe announced in <home>\run,
# which the launcher takes for a running game. The real game is never started.
#
#   tools\dev\bench\launcher-idle.ps1 -HomeDir C:\Users\Ginld\XercesHome-idle
#   tools\dev\bench\launcher-idle.ps1 -Exe build\bin\x64\Release\Xerces-old.exe -Label before
#
# The launcher logs launcher.activity once a minute, so every state gets one
# minute: the first minute (startup, left out of the summary), then in front
# (where Windows let the new window take the foreground), behind (a small
# empty window titled "Xerces idle check" takes the foreground), minimized,
# the stand-in game running with the launcher behind, then minimized. Then
# the stand-in exits and the time until the launcher notices
# (launcher.game.gone) is printed. About seven minutes in all. Leave the PC
# alone meanwhile: a click elsewhere changes the states (the summary says
# which state each minute was really in). The home must be outside AppData
# (CLAUDE.md) and should hold a hash cache (launcher-bench.ps1 says how to
# seed one).
param(
  [string]$Exe = '',
  [Parameter(Mandatory)] [string]$HomeDir,
  [string]$Label = 'run',
  [string]$Out = '',
  [int]$LineTimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Exe) { $Exe = Join-Path $repo 'build\bin\x64\Release\Xerces.exe' }
$Exe = (Resolve-Path $Exe).Path
if ($HomeDir -like "$env:LOCALAPPDATA*" -or $HomeDir -like "$env:APPDATA*") {
  throw "-HomeDir must be outside AppData (CLAUDE.md)"
}
if (-not $Out) { $Out = Join-Path $HomeDir 'bench' }
$runDir = Join-Path $HomeDir 'run'
New-Item -ItemType Directory -Force $Out, $runDir | Out-Null
if (-not ('XercesIdleWin' -as [type])) {
  Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class XercesIdleWin {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
}
"@
}
$SW_SHOWNOACTIVATE = 4
$SW_MINIMIZE = 6

$arguments = @('--home', "`"$HomeDir`"", '--dry-launch', '--skip-boot', '--view', 'home')
$process = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
$window = [IntPtr]::Zero
for ($i = 0; $i -lt 100 -and $window -eq [IntPtr]::Zero; $i++) {
  Start-Sleep -Milliseconds 100
  $process.Refresh()
  $window = $process.MainWindowHandle
}
$log = $null
for ($i = 0; $i -lt 100 -and -not $log; $i++) {
  Start-Sleep -Milliseconds 100
  $log = Get-ChildItem (Join-Path $HomeDir 'logs') -Filter "*-launcher-$($process.Id).jsonl" -ErrorAction SilentlyContinue | Select-Object -First 1
}
if ($window -eq [IntPtr]::Zero -or -not $log) { $process.Kill(); throw 'the launcher window or its log did not appear' }

function Events([string]$name) {
  @(Get-Content $log.FullName | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.event -eq $name })
}
function Wait-Lines([int]$count) {
  $deadline = (Get-Date).AddSeconds($LineTimeoutSeconds)
  while ((Get-Date) -lt $deadline) {
    # @() again: Windows PowerShell gives one PSCustomObject no Count.
    if (@(Events 'launcher.activity').Count -ge $count) { return $true }
    Start-Sleep -Milliseconds 250
  }
  return $false
}
function State {
  if ([XercesIdleWin]::IsIconic($window)) { 'minimized' }
  elseif ([XercesIdleWin]::GetForegroundWindow() -eq $window) { 'front' }
  else { 'behind' }
}

$sleeper = $null
$cover = $null
$entryFile = $null
$phases = @(
  @{ name = 'startup minute'; act = {} },
  @{ name = 'front'; act = {} },
  @{ name = 'behind'; act = {
      # A window of its own in front of the launcher (a new process may take
      # the foreground; this script's console may not).
      $form = 'Add-Type -AssemblyName System.Windows.Forms; $f = New-Object Windows.Forms.Form; ' +
        '$f.Text = ''Xerces idle check''; $f.Width = 360; $f.Height = 120; [void]$f.ShowDialog()'
      $script:cover = Start-Process powershell.exe -ArgumentList '-NoProfile', '-Command', $form -PassThru
      Start-Sleep -Seconds 2
    } },
  @{ name = 'minimized'; act = { [void][XercesIdleWin]::ShowWindow($window, $SW_MINIMIZE) } },
  @{ name = 'game running, behind'; act = {
      [void][XercesIdleWin]::ShowWindow($window, $SW_SHOWNOACTIVATE)
      $script:sleeper = Start-Process powershell.exe -ArgumentList '-NoProfile', '-Command', 'Start-Sleep -Seconds 1800' -WindowStyle Hidden -PassThru
      Start-Sleep -Milliseconds 300
      $entry = [ordered]@{
        api = 1; image = (Get-Process -Id $script:sleeper.Id).Path; pid = $script:sleeper.Id
        pipe = '\\.\pipe\xerces-idle-bench'; role = 'game'
        started = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
      }
      $script:entryFile = Join-Path $runDir "game-$($script:sleeper.Id).json"
      [IO.File]::WriteAllText($script:entryFile, ($entry | ConvertTo-Json -Compress))
    } },
  @{ name = 'game running, minimized'; act = { [void][XercesIdleWin]::ShowWindow($window, $SW_MINIMIZE) } }
)
$names = @()
$noticed = $null
try {
  for ($k = 0; $k -lt $phases.Count; $k++) {
    & $phases[$k].act
    Start-Sleep -Milliseconds 500
    $names += $phases[$k].name
    Write-Host ("{0:HH:mm:ss} {1}: window {2}" -f (Get-Date), $phases[$k].name, (State))
    if (-not (Wait-Lines ($k + 1))) { throw "no launcher.activity line $($k + 1) in $LineTimeoutSeconds s" }
  }
  # The stand-in game exits: how long until the launcher notices.
  $killed = (Get-Date).ToUniversalTime()
  Stop-Process -Id $sleeper.Id -Force
  $deadline = (Get-Date).AddSeconds(15)
  while (-not $noticed -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 100
    $gone = Events 'launcher.game.gone' | Select-Object -First 1
    if ($gone) { $noticed = ([DateTime]::Parse($gone.ts).ToUniversalTime() - $killed).TotalMilliseconds }
  }
} finally {
  if ($sleeper -and -not $sleeper.HasExited) { Stop-Process -Id $sleeper.Id -Force }
  if ($cover -and -not $cover.HasExited) { Stop-Process -Id $cover.Id -Force }
  if ($entryFile -and (Test-Path $entryFile)) { Remove-Item $entryFile }
  Start-Sleep -Seconds 1
  if (-not $process.HasExited) {
    [void]$process.CloseMainWindow()
    if (-not $process.WaitForExit(20000)) { $process.Kill() }
  }
}

$rows = @()
$lines = Events 'launcher.activity'
for ($k = 1; $k -lt [math]::Min($lines.Count, $names.Count); $k++) {
  foreach ($state in 'front', 'behind', 'minimized', 'game_running') {
    $s = $lines[$k].data.$state
    if (-not $s -or $s.seconds -lt 30) { continue } # the few seconds a change takes
    $wakes = if ($null -ne $s.wakes) { [math]::Round($s.wakes * 60 / $s.seconds) } else { $null }
    $rows += [pscustomobject][ordered]@{
      phase = $names[$k]; state = $state; seconds = $s.seconds; frames = $s.frames
      frames_per_minute = $s.per_minute; wakes_per_minute = $wakes
    }
  }
}
$rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
if ($null -ne $noticed) { Write-Host ("Game exit noticed after {0:N0} ms" -f $noticed) } else { Write-Host 'Game exit not noticed in 15 s' }
$summary = [ordered]@{
  label = $Label; exe = $Exe; home = $HomeDir; log = $log.FullName
  started = (Get-Date -Format 'yyyyMMdd-HHmmss'); states = $rows; game_exit_noticed_ms = $noticed
}
$file = Join-Path $Out ("launcher-idle-{0}-{1}.json" -f $Label, $summary.started)
[IO.File]::WriteAllText($file, ($summary | ConvertTo-Json -Depth 5))
Write-Host "Saved $file"
