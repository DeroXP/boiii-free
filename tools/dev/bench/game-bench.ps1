# One frame-time measurement in the game (docs/TESTING.md, "In-game frame
# times"): starts Black Ops III through `xerces_cli plan apply` (no launcher
# window during the run), loads a map, makes the player safe, lets the frame
# probe record one window by itself (XERCES_BENCH_AUTO), then quits.
#
#   game-bench.ps1 -Variant windowed -OutDir C:\Users\Ginld\XercesDev\bench\s1 -Live
#   game-bench.ps1 -Variant stack -Stack 1944930083,3171632315,3656779213 -OutDir ... -Live
#   game-bench.ps1 -Variant borderless -FullScreenMode 2 -OutDir ... -Live
#
# The probe writes <OutDir>\<Variant>\<run>.frames.csv, .main.csv and
# .meta.json; this script appends one JSON line per run to
# <OutDir>\results.jsonl and keeps the game log, the console text and two
# pictures (taken outside the window) in <OutDir>\runs\<run>\.
#
# Nothing is sent to the game while the window records: the console is read
# and commands are typed only before it starts and after it ends.
#
# Settings for one run only:
# - -Dvars become launcher.json "dvars", passed as +set and re-applied after
#   config.cfg, never saved into it (docs/DESIGN.md 5.6, launch_dvars). Cheats
#   for -SceneCommands come this way (boiii_allowCheats 1 for this launch).
# - -FullScreenMode edits FullScreenMode in <game>\boiii_players\config.ini
#   before the launch; the file the run started from is put back byte for byte
#   when the game has exited (the engine rewrites the file when it quits).
# - -Affinity (hex mask) and -Priority are set on boiii.exe 5 s after the menu
#   (upstream resets the affinity 1 s after the first main frame).
#
# -Live starts the real game: only with the user's go-ahead for that session.
param(
  [Parameter(Mandatory = $true)][string]$Variant,
  [Parameter(Mandatory = $true)][string]$OutDir,
  [string]$Map = 'zm_ccube',
  [string[]]$Stack = @(),
  [string]$Mode = 'zm',
  [string]$HomeDir = 'C:\Users\Ginld\XercesHome-bench',
  [string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops III',
  # ingame:<start_s>:<len_s>, counted from the first frame the player is in.
  [string]$Window = 'ingame:20:60',
  [hashtable]$Dvars = @{},
  [string[]]$ExtraFlags = @(),
  # Typed 3 s after the match starts, before the window (needs cheats, which
  # this script turns on for the launch when the list is not empty).
  [string[]]$SceneCommands = @('notarget', 'god'),
  [ValidateSet('', '0', '1', '2')][string]$FullScreenMode = '',
  [string]$Affinity = '',
  [ValidateSet('', 'Normal', 'AboveNormal', 'High')][string]$Priority = '',
  [switch]$SkipCinematic,
  [int]$MenuTimeoutSeconds = 240,
  [int]$LoadTimeoutSeconds = 480,
  [switch]$Live
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$session = Join-Path $repo 'tools\dev\session'
$bin = Join-Path $repo 'build\bin\x64\Release'
$cli = Join-Path $bin 'xerces_cli.exe'
$client = Join-Path $bin 'boiii.exe'
. (Join-Path $session 'xerces-win32.ps1')

$Stack = @($Stack | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object { $_.Trim() })
$ExtraFlags = @($ExtraFlags | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object { $_.Trim() })
if ($Window -notmatch '^ingame:(\d+(\.\d+)?):(\d+(\.\d+)?)$') { throw "-Window must be ingame:<start_s>:<len_s>, not '$Window'" }
$windowStart = [double]$Matches[1]
$windowLength = [double]$Matches[3]
if ($Variant -notmatch '^[A-Za-z0-9_.-]{1,40}$') { throw "-Variant may hold letters, digits, '.', '-' and '_' only" }
$runName = (Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Variant"
$benchDir = Join-Path $OutDir $Variant
$runDir = Join-Path $OutDir "runs\$runName"
$ini = Join-Path $GameDir 'boiii_players\config.ini'
$cfg = Join-Path $GameDir 'boiii_players\user\config.cfg'

# --- guards ---------------------------------------------------------------
if (-not $Live) { throw "game-bench starts the game: add -Live (only with the user's go-ahead for this session)" }
foreach ($path in @($cli, $client)) { if (-not (Test-Path $path)) { throw "missing build: $path" } }
foreach ($dir in @($HomeDir, $OutDir)) {
  if (-not [System.IO.Path]::IsPathRooted($dir)) { throw "$dir must be an absolute path" }
  foreach ($root in @($env:LOCALAPPDATA, $env:APPDATA, (Join-Path $env:USERPROFILE 'AppData'))) {
    if ($root -and $dir.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
      throw "$dir is under AppData (files made there land in a private copy, and the probe refuses it)"
    }
  }
  if ($dir.StartsWith($GameDir, [System.StringComparison]::OrdinalIgnoreCase) -or $dir -match '(?i)\\steamapps\\') {
    throw "$dir is in the game's or Steam's folders"
  }
}
git -C $repo merge-base --is-ancestor 62e28935 HEAD 2>$null
if ($LASTEXITCODE -ne 0) { throw 'this checkout does not contain 62e28935 (updater disabled)' }
if (Get-Process boiii, BlackOps3 -ErrorAction SilentlyContinue) { throw 'the game is already running' }
if ($env:XERCES_DRY_LAUNCH) { throw 'XERCES_DRY_LAUNCH is set: the launch would be dry' }
New-Item -ItemType Directory -Force $HomeDir, $runDir, $benchDir | Out-Null

function Say([string]$text) { Write-Host ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $text) }

function Wait-For([scriptblock]$test, [double]$seconds, [int]$stepMs = 500) {
  $deadline = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $deadline) {
    $value = & $test
    if ($value) { return $value }
    Start-Sleep -Milliseconds $stepMs
  }
  return $null
}

function Read-Journal {
  $path = Join-Path $HomeDir 'logs\journal.json'
  if (-not (Test-Path $path)) { return $null }
  try { return Get-Content $path -Raw | ConvertFrom-Json } catch { return $null }
}

function Game-Log([int]$gamePid) {
  Get-ChildItem (Join-Path $HomeDir 'logs') -Filter "*-game-$gamePid.jsonl" -ErrorAction SilentlyContinue | Select-Object -First 1
}

function Find-Events([int]$gamePid, [string]$name) {
  $log = Game-Log $gamePid
  if (-not $log) { return @() }
  $found = @()
  # The game has the file open for writing: read it with shared access.
  $text = Get-Content $log.FullName -Raw -ErrorAction SilentlyContinue
  if (-not $text) { return @() }
  foreach ($line in ($text -split "\r?\n")) {
    if (-not $line -or $line.IndexOf($name) -lt 0) { continue }
    try { $e = $line | ConvertFrom-Json } catch { continue }
    if ($e.event -eq $name) { $found += $e }
  }
  return $found
}

function Find-Dialog([int]$gamePid) {
  foreach ($h in [XercesWin]::TopWindows([uint32]$gamePid)) {
    if ([XercesWin]::IsWindowVisible($h) -and [XercesWin]::ClassOf($h) -eq '#32770') {
      return [pscustomobject]@{ Handle = $h; Title = [XercesWin]::TitleOf($h) }
    }
  }
  return $null
}

if (-not ('XercesFront' -as [type])) {
  Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class XercesFront {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  public static uint ForegroundPid() {
    uint pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), out pid);
    return pid;
  }
}
"@
}

function Foreground {
  $foregroundPid = [XercesFront]::ForegroundPid()
  $p = Get-Process -Id $foregroundPid -ErrorAction SilentlyContinue
  if ($p) { return "$($p.ProcessName) ($foregroundPid)" }
  return "pid $foregroundPid"
}

function Console-Lines {
  $game = Get-XercesGame
  if (-not $game) { return @() }
  $console = Find-XercesWindow $game 'BOIII WinConsole'
  if (-not $console) { return @() }
  $buffer = (Get-XercesConsoleControls $console).Buffer
  if (-not $buffer) { return @() }
  return [XercesWin]::ReadText($buffer) -split "\r\n|\r|\n"
}

function Capture([string]$name) {
  $path = Join-Path $runDir $name
  & (Join-Path $session 'capture.ps1') -Path $path | Out-Null
  if (Test-Path $path) { $record.pictures += $path }
}

function Machine-State {
  $state = [ordered]@{}
  try { $state.power = ((powercfg /getactivescheme) -join ' ').Trim() } catch { $state.power = "unknown: $_" }
  $smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
  if ($smi) {
    try {
      $state.gpu = ((& $smi.Source --query-gpu=name,driver_version,temperature.gpu,clocks.gr,clocks.mem,power.draw,pstate --format=csv,noheader) -join ' ').Trim()
    } catch { $state.gpu = "nvidia-smi failed: $_" }
  }
  $busy = Get-Process | Where-Object { $_.ProcessName -match '^(Xerces|xerces_cli|ninja|clang|clang\+\+|ld\.lld|lld-link|MsMpEng|SearchIndexer|OneDrive|obs64|Discord|steamwebhelper)$' } |
    Group-Object ProcessName | ForEach-Object { "$($_.Name) x$($_.Count)" }
  $state.processes = @($busy)
  return $state
}

function Hash-Of([string]$path) {
  if (-not (Test-Path $path)) { return $null }
  return (Get-FileHash $path -Algorithm SHA256).Hash
}

$record = [ordered]@{
  time = (Get-Date).ToString('s'); variant = $Variant; run = $runName; map = $Map; mode = $Mode; stack = $Stack
  window = $Window; dvars = $Dvars; extra_flags = $ExtraFlags; scene = $SceneCommands
  fullscreen_mode = $FullScreenMode; affinity = $Affinity; priority = $Priority
  result = 'unknown'; detail = ''; pid = $null; menu_seconds = $null; load_seconds = $null
  bench = $null; swapchain = $null; scene_echo = @(); foreground_start = $null; foreground_end = $null
  foreground_others = @()
  machine = $null; config_ini_restored = $null; config_cfg_unchanged = $null; pictures = @(); errors = @()
}

# The game's own config files, as this run found them.
$iniBefore = [System.IO.File]::ReadAllBytes($ini)
$iniHash = Hash-Of $ini
$cfgHash = Hash-Of $cfg
$script:iniEdited = $false

function Restore-Ini {
  if (-not $script:iniEdited) { return }
  if (Get-Process boiii -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 5 }
  if ((Hash-Of $ini) -ne $iniHash) { [System.IO.File]::WriteAllBytes($ini, $iniBefore) }
  $record.config_ini_restored = ((Hash-Of $ini) -eq $iniHash)
  $script:iniEdited = $false
}

function Finish([string]$result, [string]$detail) {
  $record.result = $result
  $record.detail = $detail
  $game = Get-XercesGame
  if ($game) {
    $lines = Console-Lines
    if ($lines.Count -gt 0) { [System.IO.File]::WriteAllLines((Join-Path $runDir 'console.txt'), $lines, (New-Object System.Text.UTF8Encoding $false)) }
    $record.errors = @($lines | Where-Object { $_ -match '(?i)\berror\b|\[Freeze\]|fatal|out of memory' } | Select-Object -First 20)
    & (Join-Path $session 'stop-game.ps1') -WaitSeconds 20 | ForEach-Object { Say $_ }
    if (Get-Process -Id $game.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $game.Id -Force }
  }
  Restore-Ini
  if ($null -eq $record.config_ini_restored) { $record.config_ini_restored = ((Hash-Of $ini) -eq $iniHash) }
  $record.config_cfg_unchanged = ((Hash-Of $cfg) -eq $cfgHash)
  if ($record.pid) {
    $log = Game-Log $record.pid
    if ($log) { Copy-Item $log.FullName $runDir }
  }
  Add-Content -Path (Join-Path $OutDir 'results.jsonl') -Value ($record | ConvertTo-Json -Depth 6 -Compress) -Encoding utf8
  Say "RESULT $result - $detail"
  Say "config.ini as before: $($record.config_ini_restored); config.cfg unchanged: $($record.config_cfg_unchanged)"
  if ($result -eq 'measured') { exit 0 } else { exit 1 }
}

# --- the launch -----------------------------------------------------------
$dvarsAll = @{}
foreach ($k in $Dvars.Keys) { $dvarsAll[$k] = [string]$Dvars[$k] }
if ($SceneCommands.Count -gt 0 -and -not $dvarsAll.ContainsKey('boiii_allowCheats')) { $dvarsAll['boiii_allowCheats'] = '1' }

$configDir = Join-Path $HomeDir 'config'
New-Item -ItemType Directory -Force $configDir | Out-Null
$config = [ordered]@{
  schema = 'xerces.config/1'; first_run_done = $true; mode = $Mode
  extra_flags = @($ExtraFlags); dvars = $dvarsAll
  profiles = @{ $Mode = [ordered]@{ mods = @($Stack | ForEach-Object { "ws.$_" }); modded = ($Stack.Count -gt 0) } }
}
[System.IO.File]::WriteAllText((Join-Path $configDir 'launcher.json'), ($config | ConvertTo-Json -Depth 5))
foreach ($name in @('logs\journal.json', 'config\culprit.json')) {
  $path = Join-Path $HomeDir $name
  if (Test-Path $path) { Move-Item $path ($path + '.' + (Get-Date -Format 'yyyyMMdd-HHmmss')) -Force }
}

try {
  if ($FullScreenMode -ne '') {
    $text = [System.Text.Encoding]::UTF8.GetString($iniBefore)
    $edited = [regex]::Replace($text, '(?m)^FullScreenMode = "\d"', "FullScreenMode = `"$FullScreenMode`"")
    if ($edited -eq $text -and $text -notmatch "(?m)^FullScreenMode = `"$FullScreenMode`"") { throw 'FullScreenMode line not found in config.ini' }
    if ($edited -ne $text) {
      $script:iniEdited = $true
      [System.IO.File]::WriteAllBytes($ini, (New-Object System.Text.UTF8Encoding $false).GetBytes($edited))
      Say "config.ini: FullScreenMode $FullScreenMode for this run (restored afterwards)"
    }
  }

  $record.machine = Machine-State
  $env:XERCES_BENCH = '1'
  $env:XERCES_BENCH_OUT = $benchDir
  $env:XERCES_BENCH_AUTO = $Window
  $arguments = @('plan', 'apply', '--home', $HomeDir, '--mode', $Mode, '--json')
  if ($Stack.Count -eq 0) { $arguments += '--vanilla' }
  $started_at = Get-Date
  Say "LIVE: $Variant - $Map, $(if ($Stack.Count) { 'mods ' + ($Stack -join ', ') } else { 'no mods' }), window $Window"
  # stdout only: Windows PowerShell turns a native program's stderr lines into
  # errors, which stop this script. plan apply's notes go to the host.
  $out = & $cli @arguments | Out-String
  Remove-Item Env:XERCES_BENCH, Env:XERCES_BENCH_OUT, Env:XERCES_BENCH_AUTO -ErrorAction SilentlyContinue
  $json = $null
  try { $json = $out.Substring($out.IndexOf('{')) | ConvertFrom-Json } catch { }
  if (-not $json -or -not $json.started) { Finish 'not_started' ("plan apply: " + $out.Trim()) }
  $gamePid = [int]$json.pid
  $record.pid = $gamePid
  Say "game started, pid $gamePid"

  $menu = Wait-For {
    if (-not (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) { return 'gone' }
    if (Find-Dialog $gamePid) { return 'dialog' }
    $j = Read-Journal
    if ($j -and -not $j.launch_id -and $j.phase -eq 'frontend') { return 'menu' }
  } $MenuTimeoutSeconds
  if ($menu -ne 'menu') { Finish 'crash_menu' "no menu ($menu)" }
  $record.menu_seconds = [math]::Round(((Get-Date) - $started_at).TotalSeconds, 1)
  $armed = Find-Events $gamePid 'perf.bench.armed'
  if (-not $armed) {
    $refused = Find-Events $gamePid 'perf.bench.refused'
    Finish 'probe_off' $(if ($refused) { "perf.bench.refused: $($refused[0].msg)" } else { 'no perf.bench.armed event' })
  }
  Say "menu after $($record.menu_seconds) s; probe armed"
  Start-Sleep -Seconds 5
  $proc = Get-Process -Id $gamePid
  if ($Affinity) { $proc.ProcessorAffinity = [IntPtr][Convert]::ToInt64($Affinity, 16); Say "affinity $Affinity" }
  if ($Priority) { $proc.PriorityClass = $Priority; Say "priority $Priority" }
  try { [void](New-Object -ComObject WScript.Shell).AppActivate($gamePid) } catch { }

  & (Join-Path $session 'console-command.ps1') -Command "map $Map" -WaitMs 1500 | Out-Null
  $load_from = Get-Date
  $script:nextSkip = (Get-Date).AddSeconds(15)
  $loaded = Wait-For {
    if ($SkipCinematic -and (Get-Date) -ge $script:nextSkip) {
      $script:nextSkip = (Get-Date).AddSeconds(6)
      & (Join-Path $session 'send-key.ps1') Space -HoldMs 1500 | Out-Null
      & (Join-Path $session 'send-key.ps1') Return | Out-Null
    }
    if (-not (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) { return 'gone' }
    if (Find-Dialog $gamePid) { return 'dialog' }
    $j = Read-Journal
    if ($j -and $j.phase -eq 'in_game') { return 'in_game' }
  } $LoadTimeoutSeconds 1000
  if ($loaded -ne 'in_game') { Finish 'crash_load' "not in the match ($loaded)" }
  $inGameAt = Get-Date
  $record.load_seconds = [math]::Round(($inGameAt - $load_from).TotalSeconds, 1)
  Say "in the match after $($record.load_seconds) s"

  # The scene, well before the window (it starts $windowStart s after the
  # player is in; the journal can be up to 1 s late).
  Start-Sleep -Seconds 3
  foreach ($command in $SceneCommands) {
    $echo = & (Join-Path $session 'console-command.ps1') -Command $command -WaitMs 700 | Where-Object { $_ } | Select-Object -Last 3
    $record.scene_echo += "$command => $($echo -join ' | ')"
  }
  try { [void](New-Object -ComObject WScript.Shell).AppActivate($gamePid) } catch { }
  $quietUntil = $inGameAt.AddSeconds($windowStart - 2)
  if ((Get-Date) -gt $quietUntil) { Say 'warning: the scene commands ran into the window' }
  while ((Get-Date) -lt $quietUntil) { Start-Sleep -Milliseconds 250 }
  $record.foreground_start = Foreground

  # The window: nothing sent to the game. Every 2 s one GetForegroundWindow
  # call notes any other window in front (the probe flags those frames).
  $windowEnd = (Get-Date).AddSeconds([math]::Ceiling($windowLength + 4))
  $gameWindow = Find-XercesWindow (Get-Process -Id $gamePid) 'CoDBlackOps'
  while ((Get-Date) -lt $windowEnd) {
    Start-Sleep -Seconds 2
    $front = [XercesFront]::GetForegroundWindow()
    if ($gameWindow -and $front -ne $gameWindow) {
      $at = [math]::Round(((Get-Date) - $quietUntil).TotalSeconds - 2, 0)
      $record.foreground_others += "+$at s: $(Foreground) '$([XercesWin]::TitleOf($front))' [$([XercesWin]::ClassOf($front))]"
    }
  }
  $record.foreground_end = Foreground
  $written = Wait-For {
    if (-not (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) { return 'gone' }
    $w = Find-Events $gamePid 'perf.bench.written'
    if ($w) { return $w[0] }
    $f = Find-Events $gamePid 'perf.bench.failed'
    if ($f) { return 'failed' }
  } 60 1000
  if ($written -is [string] -or -not $written) {
    Capture 'after-window.png'
    Finish 'not_written' "no perf.bench.written ($written)"
  }
  $record.bench = $written.data
  $chain = Find-Events $gamePid 'perf.swapchain'
  if ($chain) { $record.swapchain = $chain[0].msg }
  Capture 'after-window.png'
  $journal = Read-Journal
  if (-not $journal -or $journal.phase -ne 'in_game') { Finish 'dropped' "the match ended during the window (journal: $($journal.phase))" }

  & (Join-Path $session 'console-command.ps1') -Command 'disconnect' -WaitMs 1000 | Out-Null
  $back = Wait-For {
    if (-not (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) { return 'gone' }
    $j = Read-Journal
    if ($j -and $j.phase -eq 'frontend') { return 'menu' }
  } 120
  Finish 'measured' ("$($written.msg); back to the menu: $(if ($back -eq 'menu') { 'yes' } else { $back })")
} finally {
  Remove-Item Env:XERCES_BENCH, Env:XERCES_BENCH_OUT, Env:XERCES_BENCH_AUTO -ErrorAction SilentlyContinue
  if ($script:iniEdited) {
    $game = Get-Process boiii -ErrorAction SilentlyContinue
    if ($game) { & (Join-Path $session 'stop-game.ps1') -WaitSeconds 20 | Out-Null }
    Restore-Ini
  }
}
