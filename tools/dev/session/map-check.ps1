# Loads one custom (or stock) map through Xerces.exe with no mods, one mod or a
# stack, and records whether it reaches the match, crashes, or errors out.
#
#   tools\dev\session\map-check.ps1 -Map zm_ccube -Live
#   tools\dev\session\map-check.ps1 -Map zm_ccube -Stack 1944930083 -Live
#   tools\dev\session\map-check.ps1 -Map zm_ccube -Stack 1944930083,3171632315,3656779213 -Live
#
# The launcher starts the game with the stack (Launch modded) or none (Launch
# vanilla). At the menu the map is started from the console (-StartCommand,
# default "map <Map>"). The check then waits for the journal's in_game phase,
# stays -PlaySeconds in the match, captures the game window, disconnects back
# to the menu and quits. A crash dialog ("Ezz ERROR"), an engine error box, the
# process ending, a freeze report or the time running out ends the run early;
# the dialog is captured from the screen and the game is ended.
#
# Every run appends one JSON line to <OutDir>\results.jsonl and saves the
# console text, the game log and pictures under <OutDir>\<run>\. New crash
# dumps from the client's minidumps folder are copied to
# C:\Users\Ginld\XercesDev\dumps\map-checks\<run>\ (never into the repository).
#
# -Live starts the real game: only with the user's go-ahead for that session
# (CLAUDE.md, Hard rules). The home must be outside AppData.
param(
  [Parameter(Mandatory = $true)][string]$Map,
  [string[]]$Stack = @(),
  [string]$Mode = 'zm',
  [string]$HomeDir = 'C:\Users\Ginld\XercesHome-maps',
  [string]$OutDir = 'C:\Users\Ginld\XercesDev\map-checks',
  [string]$StartCommand = '',
  [int]$MenuTimeoutSeconds = 240,
  [int]$LoadTimeoutSeconds = 480,
  [int]$PlaySeconds = 45,
  # Launch flags for the game, such as -log-script-errors (the launcher's
  # extra_flags; they go before the +set commands).
  [string[]]$ExtraFlags = @(),
  # Skips a level's opening cinematic the way a player does (Space held, then
  # Return, every few seconds while the level loads). Stock maps such as
  # Shadows of Evil play a 3.5 minute intro otherwise.
  [switch]$SkipCinematic,
  [switch]$Live
)

$ErrorActionPreference = 'Stop'
$session = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $session '..\..\..')).Path
$bin = Join-Path $repo 'build\bin\x64\Release'
$launcher = Join-Path $bin 'Xerces.exe'
$client = Join-Path $bin 'boiii.exe'
. (Join-Path $session 'xerces-win32.ps1')

# A single "a,b,c" argument from another PowerShell arrives as one string.
$Stack = @($Stack | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object { $_.Trim() })
if (-not $StartCommand) { $StartCommand = "map $Map" }
$stackName = if ($Stack.Count -eq 0) { 'vanilla' } else { ($Stack -join '+') }
$runName = (Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Map-" + $(if ($Stack.Count -le 1) { $stackName } else { "stack$($Stack.Count)" })
$runDir = Join-Path $OutDir $runName
$dumpDir = Join-Path 'C:\Users\Ginld\XercesDev\dumps\map-checks' $runName
$minidumps = Join-Path $env:LOCALAPPDATA 'boiii\minidumps'

# --- guards ---------------------------------------------------------------
if (-not $Live) { throw "map-check starts the game: add -Live (only with the user's go-ahead for this session)" }
foreach ($path in @($launcher, $client)) { if (-not (Test-Path $path)) { throw "missing build: $path" } }
foreach ($root in @($env:LOCALAPPDATA, $env:APPDATA)) {
  if ($root -and $HomeDir.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'the home folder must be outside AppData (files made there land in a private copy)'
  }
}
git -C $repo merge-base --is-ancestor 62e28935 HEAD 2>$null
if ($LASTEXITCODE -ne 0) { throw 'this checkout does not contain 62e28935 (updater disabled)' }
if (Get-Process boiii -ErrorAction SilentlyContinue) { throw 'boiii.exe is already running' }
New-Item -ItemType Directory -Force $HomeDir, $runDir | Out-Null

$record = [ordered]@{
  time = (Get-Date).ToString('s'); map = $Map; mode = $Mode; stack = $Stack; command = $StartCommand
  result = 'unknown'; detail = ''; menu_seconds = $null; load_seconds = $null; pid = $null; exit_code = $null
  console = $null; detail_console = ''; dumps = @(); pictures = @(); errors = @()
}
function Say([string]$text) { Write-Host ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $text) }

function Wait-For([scriptblock]$test, [int]$seconds, [int]$stepMs = 500) {
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

function Find-Event([string]$path, [string]$name) {
  if (-not $path -or -not (Test-Path $path)) { return $null }
  foreach ($line in Get-Content $path) {
    if (-not $line) { continue }
    try { $e = $line | ConvertFrom-Json } catch { continue }
    if ($e.event -eq $name) { return $e }
  }
  return $null
}

function Launcher-Log($process) {
  Get-ChildItem (Join-Path $HomeDir 'logs') -Filter "*-launcher-$($process.Id).jsonl" -ErrorAction SilentlyContinue | Select-Object -First 1
}

function Write-Config {
  $config = Join-Path $HomeDir 'config'
  New-Item -ItemType Directory -Force $config | Out-Null
  $mods = ($Stack | ForEach-Object { '"ws.' + $_ + '"' }) -join ', '
  $flag = if ($Stack.Count -gt 0) { 'true' } else { 'false' }
  $flags = ''
  $extra = @($ExtraFlags | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
  if ($extra.Count -gt 0) {
    $flags = "  `"extra_flags`": [" + (($extra | ForEach-Object { '"' + $_ + '"' }) -join ', ') + "],`n"
  }
  $json = "{`n  `"schema`": `"xerces.config/1`",`n  `"first_run_done`": true,`n  `"mode`": `"$Mode`",`n" + $flags +
    "  `"profiles`": { `"$Mode`": { `"mods`": [$mods], `"modded`": $flag } }`n}`n"
  [System.IO.File]::WriteAllText((Join-Path $config 'launcher.json'), $json)
  # A fresh record each run: an unfinished journal would make Home offer safe
  # mode instead of the launch this check clicks.
  foreach ($name in @('logs\journal.json', 'config\culprit.json')) {
    $path = Join-Path $HomeDir $name
    if (Test-Path $path) { Move-Item $path ($path + '.' + (Get-Date -Format 'yyyyMMdd-HHmmss')) -Force }
  }
}

function Start-Launcher([string[]]$steps, [int]$seconds) {
  $stepsFile = Join-Path $HomeDir 'map-steps.txt'
  [System.IO.File]::WriteAllLines($stepsFile, $steps)
  $arguments = @('--home', "`"$HomeDir`"", '--skip-boot', '--view', 'home', '--script', "`"$stepsFile`"", '--seconds', $seconds)
  return Start-Process -FilePath $launcher -ArgumentList $arguments -PassThru
}

function Close-Launcher($process) {
  if (-not $process -or $process.HasExited) { return }
  [void]$process.CloseMainWindow()
  if (-not $process.WaitForExit(10000)) { Stop-Process -Id $process.Id -Force }
}

# The game's dialogs: the client's crash dialog ("Ezz ERROR", a task dialog)
# and the engine's error boxes are #32770 windows of the game process.
function Find-Dialog([int]$gamePid) {
  foreach ($h in [XercesWin]::TopWindows([uint32]$gamePid)) {
    if ([XercesWin]::IsWindowVisible($h) -and [XercesWin]::ClassOf($h) -eq '#32770') {
      return [pscustomobject]@{ Handle = $h; Title = [XercesWin]::TitleOf($h) }
    }
  }
  return $null
}

# Xerces' out-of-memory box (DESIGN.md 5.6): after OK the game must end by
# itself with exit code 0xFFFFFFFF (no crash dump). A handle is taken before
# OK, so the code can still be read once the process is gone.
function Close-OutOfMemoryBox($dialog) {
  $game = Get-Process -Id $gamePid -ErrorAction SilentlyContinue
  if (-not $game) { return }
  $null = $game.Handle
  [void][XercesWin]::PostMessage($dialog.Handle, 0x0111, [IntPtr]1, [IntPtr]::Zero)  # WM_COMMAND, IDOK
  if ($game.WaitForExit(15000)) {
    $record.exit_code = '0x{0:X8}' -f $game.ExitCode
    Say "OK pressed: the game ended by itself, exit code $($record.exit_code)"
  } else {
    $record.exit_code = 'still running 15 s after OK'
    Say 'OK pressed: the game was still running 15 s later'
  }
}

# The console's lines. Each successful read is kept, so a game that dies
# still leaves the last text this check saw (up to a second old).
$script:lastConsole = @()
function Console-Lines {
  $game = Get-XercesGame
  if (-not $game) { return @() }
  $console = Find-XercesWindow $game 'BOIII WinConsole'
  if (-not $console) { return @() }
  $buffer = (Get-XercesConsoleControls $console).Buffer
  if (-not $buffer) { return @() }
  $lines = [XercesWin]::ReadText($buffer) -split "\r\n|\r|\n"
  if ($lines.Count -gt 1) { $script:lastConsole = $lines }
  return $lines
}

# Waits until no file in the client's minidumps folder has grown for 3 s, so
# a crash zip being written is not cut short by quitting (at most 60 s).
function Wait-DumpsSettled {
  if (-not (Test-Path $minidumps)) { return }
  $last = ''
  $stableSince = Get-Date
  $deadline = (Get-Date).AddSeconds(60)
  while ((Get-Date) -lt $deadline) {
    $now = (Get-ChildItem $minidumps -File | ForEach-Object { "$($_.Name):$($_.Length)" }) -join '|'
    if ($now -ne $last) { $last = $now; $stableSince = Get-Date }
    elseif (((Get-Date) - $stableSince).TotalSeconds -ge 3) { return }
    Start-Sleep -Milliseconds 500
  }
}

function Save-Console([string]$name) {
  $lines = Console-Lines
  if ($lines.Count -le 1 -and $script:lastConsole.Count -gt 1) {
    $lines = $script:lastConsole
    $record.detail_console = 'last snapshot before the game ended'
  }
  if ($lines.Count -gt 0) {
    $path = Join-Path $runDir $name
    [System.IO.File]::WriteAllLines($path, $lines, (New-Object System.Text.UTF8Encoding $false))
    $record.console = $path
  }
  return $lines
}

function Capture([string]$name, [switch]$Screen) {
  $path = Join-Path $runDir $name
  $out = if ($Screen) { & (Join-Path $session 'capture.ps1') -Path $path -Screen } else { & (Join-Path $session 'capture.ps1') -Path $path }
  if (Test-Path $path) { $record.pictures += $path }
  return $out
}

function Dump-Files {
  if (-not (Test-Path $minidumps)) { return @() }
  return @(Get-ChildItem $minidumps -File | ForEach-Object { $_.FullName })
}

function Finish([string]$result, [string]$detail) {
  $record.result = $result
  $record.detail = $detail
  Wait-DumpsSettled
  $lines = Save-Console 'console.txt'
  # Lines worth reading first: errors, limits, script problems, freezes.
  $record.errors = @($lines | Where-Object {
      $_ -match '(?i)\berror\b|exceeded|could not|couldn''t|unresolved|compile|overflow|too many|\[Freeze\]|fatal|PMem|out of memory|default asset'
    } | Where-Object { $_ -notmatch 'Playlist error|creating default assets stubs|PLATFORM_DISCONNECTED_FROM_SERVER' } | Select-Object -First 60)
  $game = Get-XercesGame
  if ($game) {
    & (Join-Path $session 'stop-game.ps1') -WaitSeconds 20 | ForEach-Object { Say $_ }
    if (Get-Process -Id $game.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $game.Id -Force }
  }
  Close-Launcher $script:launcherProcess
  $new = @(Dump-Files | Where-Object { $script:dumpsBefore -notcontains $_ })
  if ($new.Count -gt 0) {
    New-Item -ItemType Directory -Force $dumpDir | Out-Null
    foreach ($file in $new) { Copy-Item $file $dumpDir; $record.dumps += (Join-Path $dumpDir (Split-Path $file -Leaf)) }
  }
  $gameLog = if ($record.pid) { Get-ChildItem (Join-Path $HomeDir 'logs') -Filter "*-game-$($record.pid).jsonl" -ErrorAction SilentlyContinue | Select-Object -First 1 }
  if ($gameLog) { Copy-Item $gameLog.FullName $runDir }
  $json = ($record | ConvertTo-Json -Depth 5 -Compress)
  Add-Content -Path (Join-Path $OutDir 'results.jsonl') -Value $json -Encoding utf8
  Say "RESULT $result - $detail"
  if ($record.errors.Count -gt 0) { Say 'console lines worth reading:'; $record.errors | Select-Object -First 15 | ForEach-Object { Write-Host "   $_" } }
  Say "saved in $runDir"
  if ($result -eq 'in_game') { exit 0 } else { exit 1 }
}

# --- the run --------------------------------------------------------------
$script:dumpsBefore = Dump-Files
Write-Config
Say "LIVE: Black Ops III, map $Map, $(if ($Stack.Count) { 'mods ' + ($Stack -join ', ') } else { 'no mods' })"
if ($Stack.Count -gt 0) {
  # A new home has no hash cache: one run with no steps waits for the scan.
  Say 'scanning the Workshop folder for this home (minutes the first time)...'
  $warm = Start-Process -FilePath $launcher -ArgumentList @('--home', "`"$HomeDir`"", '--skip-boot', '--view', 'home', '--dry-launch', '--seconds', 900) -PassThru
  $scanned = Wait-For { $log = Launcher-Log $warm; if ($log) { Find-Event $log.FullName 'launcher.catalog.scanned' } } 900
  Close-Launcher $warm
  if (-not $scanned) { throw 'the Workshop scan did not finish in 15 minutes' }
}
$button = if ($Stack.Count -gt 0) { 'launch-modded' } else { 'launch-vanilla' }
$started_at = Get-Date
$script:launcherProcess = Start-Launcher @("+3000 click $button") ($MenuTimeoutSeconds + $LoadTimeoutSeconds + $PlaySeconds + 300)
$started = Wait-For { $log = Launcher-Log $script:launcherProcess; if ($log) { Find-Event $log.FullName 'launcher.launch.started' } } 40
if (-not $started) {
  $log = Launcher-Log $script:launcherProcess
  $refused = if ($log) { Find-Event $log.FullName 'launcher.launch.refused' } else { $null }
  Finish 'not_started' $(if ($refused) { $refused.msg } else { 'no launcher.launch.started event' })
}
$gamePid = [int]$started.data.pid
$record.pid = $gamePid
Say "game started, pid ${gamePid}: $($started.msg)"

# The menu.
$menu = Wait-For {
  $j = Read-Journal
  if (-not (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) { return 'gone' }
  if (Find-Dialog $gamePid) { return 'dialog' }
  if ($j -and -not $j.launch_id -and $j.phase -eq 'frontend') { return 'menu' }
} $MenuTimeoutSeconds
if ($menu -ne 'menu') {
  if ($menu -eq 'dialog') { $d = Find-Dialog $gamePid; Capture 'dialog-menu.png' -Screen | Out-Null; Finish 'crash_menu' "dialog '$($d.Title)' before the menu" }
  if ($menu -eq 'gone') { Finish 'crash_menu' 'the game ended before the menu' }
  Finish 'timeout_menu' "no menu after $MenuTimeoutSeconds s"
}
$record.menu_seconds = [math]::Round(((Get-Date) - $started_at).TotalSeconds, 1)
Say "menu after $($record.menu_seconds) s"
Start-Sleep -Seconds 5
$consoleBefore = (Console-Lines).Count

# The map.
Say "console: $StartCommand"
& (Join-Path $session 'console-command.ps1') -Command $StartCommand -WaitMs 1500 | Select-Object -First 8 | ForEach-Object { Write-Host "   $_" }
$load_from = Get-Date
# An error line alone does not end the run (some are harmless); it counts when
# the match has not started 30 s after it (an error drop back to the menu).
$script:errorSeenAt = $null
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
  $tail = Console-Lines | Select-Object -Skip $consoleBefore
  if ($tail | Where-Object { $_ -match '\[Freeze\]' }) { return 'freeze' }
  if (-not $script:errorSeenAt -and ($tail | Where-Object { $_ -match '(?i)^ERROR:|Com_Error|Server script compile error|Couldn''t find map|Can''t find map|^\s*Error:' })) {
    $script:errorSeenAt = Get-Date
  }
  if ($script:errorSeenAt -and ((Get-Date) - $script:errorSeenAt).TotalSeconds -gt 30) { return 'error' }
} $LoadTimeoutSeconds 1000
$record.load_seconds = [math]::Round(((Get-Date) - $load_from).TotalSeconds, 1)
switch ($loaded) {
  'in_game' { Say "in the match after $($record.load_seconds) s" }
  'dialog' {
    $d = Find-Dialog $gamePid
    Capture 'dialog-load.png' -Screen | Out-Null
    if ($d.Title -eq 'Black Ops III ran out of memory') { Close-OutOfMemoryBox $d }
    Finish 'crash_load' "dialog '$($d.Title)' after $($record.load_seconds) s of loading"
  }
  'gone' { Finish 'crash_load' "the game ended after $($record.load_seconds) s of loading" }
  'freeze' { Capture 'freeze.png' | Out-Null; Finish 'freeze_load' "freeze report after $($record.load_seconds) s" }
  'error' {
    Start-Sleep -Seconds 3
    Capture 'error.png' | Out-Null
    if (Find-Dialog $gamePid) { $d = Find-Dialog $gamePid; Capture 'dialog-error.png' -Screen | Out-Null; Finish 'crash_load' "error, then dialog '$($d.Title)'" }
    Finish 'error_load' "an error line in the console after $($record.load_seconds) s"
  }
  default { Capture 'timeout.png' | Out-Null; Finish 'timeout_load' "not in a match after $LoadTimeoutSeconds s" }
}

# The match.
Start-Sleep -Seconds 5
Capture 'match-1.png' | Out-Null
$played = Wait-For {
  if (-not (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) { return 'gone' }
  if (Find-Dialog $gamePid) { return 'dialog' }
  $j = Read-Journal
  if ($j -and $j.phase -eq 'frontend') { return 'dropped' }
} $PlaySeconds 1000
if ($played -eq 'dialog') { $d = Find-Dialog $gamePid; Capture 'dialog-match.png' -Screen | Out-Null; Finish 'crash_match' "dialog '$($d.Title)' in the match" }
if ($played -eq 'gone') { Finish 'crash_match' 'the game ended in the match' }
if ($played -eq 'dropped') { Capture 'dropped.png' | Out-Null; Finish 'dropped_match' 'the match ended back in the menu by itself (an error drop?)' }
Capture 'match-2.png' | Out-Null

# Back to the menu, then quit.
& (Join-Path $session 'console-command.ps1') -Command 'disconnect' -WaitMs 1000 | Out-Null
$back = Wait-For {
  if (-not (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) { return 'gone' }
  if (Find-Dialog $gamePid) { return 'dialog' }
  $j = Read-Journal
  if ($j -and $j.phase -eq 'frontend') { return 'menu' }
} 120
if ($back -eq 'dialog') { $d = Find-Dialog $gamePid; Capture 'dialog-disconnect.png' -Screen | Out-Null; Finish 'crash_disconnect' "dialog '$($d.Title)' leaving the match" }
if ($back -eq 'gone') { Finish 'crash_disconnect' 'the game ended leaving the match' }
Finish 'in_game' "loaded in $($record.load_seconds) s, played $PlaySeconds s, back to the menu: $(if ($back) { 'yes' } else { 'no (timed out)' })"
