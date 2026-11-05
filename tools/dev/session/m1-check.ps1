# Runs the in-game M1 checks of docs/TESTING.md ("Loader and manifest")
# through Xerces.exe, one case per run, and prints what passed.
#
#   tools\dev\session\m1-check.ps1 -Case modded              # dry launch only
#   tools\dev\session\m1-check.ps1 -Case vanilla -Live       # starts the game
#
# Cases:
#   vanilla  Launch vanilla: signatures resolve, the journal reaches frontend,
#            quitting leaves clean_exit.
#   modded   Launch modded with -Stack: the plan is applied, fs_game and
#            fs_extramods match it, quitting leaves clean_exit.
#   crash    Launch modded, end boiii.exe at the menu: the journal stays
#            unfinished and the next launcher start offers safe mode (a
#            capture of Home is saved).
#   restart  After "crash": start the game the way the crash dialog's
#            "Restart game" does (XERCES_HOME, no launch id): the game must
#            leave the crashed journal alone.
#   cli      Launch modded, then xerces_cli status (and with -CliReload,
#            load --extras and unload) against the running game.
#
# Without -Live nothing but Xerces.exe --dry-launch runs: it builds the plan
# file and the command line and starts nothing. -Live starts the real game:
# only with the user's go-ahead for that session (CLAUDE.md, Hard rules).
# The home folder must be outside AppData; it is a test home that this script
# writes freely (config, journal, logs).
param(
  [ValidateSet('vanilla', 'modded', 'crash', 'restart', 'cli')]
  [string]$Case = 'vanilla',
  [string]$HomeDir = 'C:\Users\Ginld\XercesHome-m1',
  [string[]]$Stack = @('1944930083', '3171632315', '3656779213'),
  [string]$Mode = 'zm',
  [int]$TimeoutSeconds = 240,
  [switch]$Live,
  [switch]$CliReload
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$bin = Join-Path $repo 'build\bin\x64\Release'
$launcher = Join-Path $bin 'Xerces.exe'
$client = Join-Path $bin 'boiii.exe'
$cli = Join-Path $bin 'xerces_cli.exe'
$results = New-Object System.Collections.ArrayList

function Note([string]$check, [string]$result, [string]$detail = '') {
  [void]$results.Add([pscustomobject]@{ Check = $check; Result = $result; Detail = $detail })
  $line = '{0,-5} {1}' -f $result.ToUpper(), $check
  if ($detail) { $line += ': ' + $detail }
  Write-Host $line
}

function Wait-For([scriptblock]$test, [int]$seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $deadline) {
    $value = & $test
    if ($value) { return $value }
    Start-Sleep -Milliseconds 500
  }
  return $null
}

function Read-Journal {
  $path = Join-Path $HomeDir 'logs\journal.json'
  if (-not (Test-Path $path)) { return $null }
  try { return Get-Content $path -Raw | ConvertFrom-Json } catch { return $null }
}

function Read-Events([string]$path) {
  if (-not $path -or -not (Test-Path $path)) { return @() }
  $out = @()
  foreach ($line in Get-Content $path) {
    if (-not $line) { continue }
    try { $out += ($line | ConvertFrom-Json) } catch { }
  }
  return $out
}

function Newest-Log([string]$pattern, [datetime]$since) {
  Get-ChildItem (Join-Path $HomeDir 'logs') -Filter $pattern -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $since } |
    Sort-Object LastWriteTime | Select-Object -Last 1
}

function Find-Event([string]$path, [string]$name) {
  Read-Events $path | Where-Object { $_.event -eq $name } | Select-Object -First 1
}

function Write-Config([bool]$modded) {
  $config = Join-Path $HomeDir 'config'
  New-Item -ItemType Directory -Force $config | Out-Null
  $mods = ($Stack | ForEach-Object { '"ws.' + $_ + '"' }) -join ', '
  $flag = if ($modded) { 'true' } else { 'false' }
  $json = "{`n  `"schema`": `"xerces.config/1`",`n  `"first_run_done`": true,`n  `"mode`": `"$Mode`",`n" +
    "  `"profiles`": { `"$Mode`": { `"mods`": [$mods], `"modded`": $flag } }`n}`n"
  [System.IO.File]::WriteAllText((Join-Path $config 'launcher.json'), $json)
}

# Moves the last journal and any culprit search aside, so Home starts from a
# clean record instead of offering safe mode.
function Reset-Journal {
  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  foreach ($name in @('logs\journal.json', 'config\culprit.json')) {
    $path = Join-Path $HomeDir $name
    if (Test-Path $path) { Move-Item $path ($path + '.' + $stamp) }
  }
}

function Start-Launcher([string[]]$steps, [int]$seconds) {
  $stepsFile = Join-Path $HomeDir 'm1-steps.txt'
  [System.IO.File]::WriteAllLines($stepsFile, $steps)
  $arguments = @('--home', "`"$HomeDir`"", '--skip-boot', '--view', 'home',
    '--script', "`"$stepsFile`"", '--seconds', $seconds)
  if (-not $Live) { $arguments += '--dry-launch' }
  return Start-Process -FilePath $launcher -ArgumentList $arguments -PassThru
}

# A new home has no hash cache, and the first Workshop scan takes minutes
# (about 195 s for 59 GB here); until it ends no mod counts as installed. A
# scan cut short keeps what it hashed, so the cache file alone does not say
# the scan is complete: one dry launcher run with no steps waits for the
# scan to finish (a second or two once cached), then closes.
function Warm-Catalog {
  Write-Host 'Scanning the Workshop folder for this home (minutes the first time)...'
  $from = Get-Date
  $arguments = @('--home', "`"$HomeDir`"", '--skip-boot', '--view', 'home', '--dry-launch', '--seconds', 900)
  $process = Start-Process -FilePath $launcher -ArgumentList $arguments -PassThru
  $scanned = Wait-For {
    $log = Newest-Log '*launcher*.jsonl' $from
    if ($log) { Find-Event $log.FullName 'launcher.catalog.scanned' }
  } 900
  Close-Launcher $process
  if ($scanned) { Note 'Workshop scan for this home' 'info' $scanned.msg }
  else { throw 'the Workshop scan did not finish in 15 minutes' }
}

function Close-Launcher($process) {
  if (-not $process -or $process.HasExited) { return }
  [void]$process.CloseMainWindow()
  if (-not $process.WaitForExit(10000)) { Stop-Process -Id $process.Id -Force }
}

function Wait-Frontend([int]$gamePid) {
  $journal = Wait-For {
    $j = Read-Journal
    if ($j -and -not $j.launch_id -and ($j.phase -eq 'frontend' -or $j.phase -eq 'in_game')) { $j }
  } $TimeoutSeconds
  if ($journal) {
    Note 'journal taken over by the game and at the menu' 'pass' "phase $($journal.phase), plan '$($journal.plan_id)'"
  } else {
    $j = Read-Journal
    $phase = if ($j) { $j.phase } else { 'no journal' }
    Note 'journal taken over by the game and at the menu' 'fail' "after $TimeoutSeconds s: $phase"
  }
  return $journal
}

function Check-GameLog([int]$gamePid, [bool]$modded) {
  $log = Get-ChildItem (Join-Path $HomeDir 'logs') -Filter "*-game-$gamePid.jsonl" -ErrorAction SilentlyContinue |
    Select-Object -First 1
  if (-not $log) { Note 'game log' 'fail' "no *-game-$gamePid.jsonl in $HomeDir\logs"; return }
  Note 'game log' 'info' $log.FullName
  $resolved = Find-Event $log.FullName 'xerces.signatures.resolved'
  $miss = Find-Event $log.FullName 'xerces.signatures.miss'
  if ($resolved) { Note 'signatures resolved' 'pass' $resolved.msg }
  elseif ($miss) { Note 'signatures resolved' 'fail' $miss.msg }
  else { Note 'signatures resolved' 'fail' 'no xerces.signatures.resolved event' }
  if ($modded) {
    foreach ($name in @('xerces.feature.on', 'loader.plan.applied')) {
      $event = Find-Event $log.FullName $name
      if ($event) { Note $name 'pass' $event.msg } else { Note $name 'fail' 'not in the game log' }
    }
    $mismatch = Find-Event $log.FullName 'loader.plan.mismatch'
    if ($mismatch) { Note 'loader.plan.mismatch' 'fail' $mismatch.msg }
  }
  $dvars = Find-Event $log.FullName 'xerces.dvars.reapplied'
  if ($dvars) { Note 'launcher settings re-applied' 'info' $dvars.msg }
}

function Console-Value([string]$dvar) {
  $lines = & (Join-Path $PSScriptRoot 'console-command.ps1') -Command $dvar -WaitMs 1000
  return ($lines | Where-Object { $_ -match [regex]::Escape($dvar) }) -join ' / '
}

function Quit-Game($game) {
  & (Join-Path $PSScriptRoot 'stop-game.ps1') -WaitSeconds 30 | ForEach-Object { Note 'quit' 'info' $_ }
  $journal = Wait-For { $j = Read-Journal; if ($j -and $j.phase -eq 'clean_exit') { $j } } 15
  if ($journal) { Note 'quitting leaves clean_exit' 'pass' }
  else { $j = Read-Journal; Note 'quitting leaves clean_exit' 'fail' "phase $($j.phase)" }
}

# --- guards ---------------------------------------------------------------
if (-not (Test-Path $launcher)) { throw "no launcher build at $launcher (tools\build.ps1 -Launcher)" }
foreach ($root in @($env:LOCALAPPDATA, $env:APPDATA)) {
  if ($root -and $HomeDir.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "the home folder must be outside AppData (files made there land in a private copy)"
  }
}
if ($Live) {
  if (-not (Test-Path $client)) { throw "no client build at $client (tools\build.ps1 -Client)" }
  # Never start a build whose updater is not disabled (62e28935).
  git -C $repo merge-base --is-ancestor 62e28935 HEAD 2>$null
  if ($LASTEXITCODE -ne 0) { throw 'this checkout does not contain 62e28935 (updater disabled)' }
  if (Get-Process boiii -ErrorAction SilentlyContinue) { throw 'boiii.exe is already running' }
  Write-Host "LIVE: this starts Black Ops III (case $Case, home $HomeDir)."
} elseif ($Case -eq 'restart' -or $Case -eq 'cli') {
  throw "-Case $Case needs the game running: it has no dry form (add -Live with the user's go-ahead)"
}
New-Item -ItemType Directory -Force $HomeDir | Out-Null
$since = (Get-Date).AddSeconds(-2)

# --- restart: no launcher; the game alone, as the crash dialog starts it ---
if ($Case -eq 'restart') {
  $before = Read-Journal
  if (-not $before -or $before.phase -eq 'clean_exit') {
    throw 'no unfinished journal to protect: run -Case crash first'
  }
  $raw = Get-Content (Join-Path $HomeDir 'logs\journal.json') -Raw
  $gamePath = (Get-Content (Join-Path $HomeDir 'config\launcher.json') -Raw | ConvertFrom-Json).game_dir
  if (-not $gamePath) {
    $detected = Newest-Log '*launcher*.jsonl' ([datetime]::MinValue)
    $event = if ($detected) { Find-Event $detected.FullName 'launcher.game.detected' } else { $null }
    $prefix = 'Black Ops III found at '
    if ($event -and $event.msg.StartsWith($prefix)) {
      # Logs write the profile folder as %USERPROFILE%.
      $gamePath = $event.msg.Substring($prefix.Length).Replace('%USERPROFILE%', $env:USERPROFILE)
    }
  }
  if (-not $gamePath -or -not (Test-Path (Join-Path $gamePath 'BlackOps3.exe'))) {
    throw 'cannot tell where the game is (no game_dir in launcher.json, no launcher.game.detected event)'
  }
  $env:XERCES_HOME = $HomeDir
  $env:XERCES_DATA_PATH = Join-Path $repo 'data'
  Remove-Item Env:XERCES_LAUNCH_ID -ErrorAction SilentlyContinue
  $arguments = @('-launch', '-nointro', '+set', 'fs_game', $Stack[0])
  if ($Stack.Count -gt 1) { $arguments += @('+set', 'fs_extramods', (($Stack | Select-Object -Skip 1) -join ',')) }
  $game = Start-Process -FilePath $client -ArgumentList $arguments -WorkingDirectory $gamePath -PassThru
  Note 'game started without a launch id' 'info' "pid $($game.Id)"
  $skipped = Wait-For {
    $log = Get-ChildItem (Join-Path $HomeDir 'logs') -Filter "*-game-$($game.Id).jsonl" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($log) { Find-Event $log.FullName 'loader.journal.skipped' }
  } $TimeoutSeconds
  if ($skipped) { Note 'the game leaves the crashed journal alone' 'pass' $skipped.msg }
  else { Note 'the game leaves the crashed journal alone' 'fail' 'no loader.journal.skipped event' }
  Start-Sleep -Seconds 5
  $after = Get-Content (Join-Path $HomeDir 'logs\journal.json') -Raw
  if ($after -eq $raw) { Note 'journal unchanged' 'pass' "still $($before.phase), plan '$($before.plan_id)'" }
  else { Note 'journal unchanged' 'fail' 'journal.json changed' }
  & (Join-Path $PSScriptRoot 'stop-game.ps1') -WaitSeconds 30 | ForEach-Object { Note 'quit' 'info' $_ }
  $after = Get-Content (Join-Path $HomeDir 'logs\journal.json') -Raw
  if ($after -eq $raw) { Note 'journal unchanged after the restarted game quit' 'pass' }
  else { Note 'journal unchanged after the restarted game quit' 'fail' 'the quit wrote the journal' }
  $results | Format-Table -AutoSize | Out-String -Width 200
  exit 0
}

# --- the launcher cases ---------------------------------------------------
$modded = $Case -ne 'vanilla'
Write-Config $modded
Reset-Journal
if ($modded) { Warm-Catalog; $since = (Get-Date).AddSeconds(-2) }
$button = if ($modded) { 'launch-modded' } else { 'launch-vanilla' }
$steps = @("+3000 click $button")
if (-not $Live) { $steps += @('+2500 quit') }
$launcherProcess = Start-Launcher $steps ($TimeoutSeconds + 180)

if (-not $Live) {
  [void]$launcherProcess.WaitForExit(30000)
  $log = Newest-Log '*launcher*.jsonl' $since
  $dry = if ($log) { Find-Event $log.FullName 'launcher.launch.dry' } else { $null }
  $refused = if ($log) { Find-Event $log.FullName 'launcher.launch.refused' } else { $null }
  if ($dry) {
    Note 'dry launch command' 'pass' $dry.msg
    if ($modded) {
      $expected = '+set fs_game ' + $Stack[0]
      if ($dry.msg.Contains('-xerces-plan') -and $dry.msg.Contains($expected)) { Note 'plan and main mod on the command line' 'pass' }
      else { Note 'plan and main mod on the command line' 'fail' "expected -xerces-plan and '$expected'" }
      $plan = Get-ChildItem (Join-Path $HomeDir 'config\plans') -Filter '*.json' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
      if ($plan) { Note 'plan file' 'pass' $plan.FullName } else { Note 'plan file' 'fail' 'none in config\plans' }
    }
  } elseif ($refused) {
    Note 'dry launch command' 'fail' "refused: $($refused.msg)"
  } else {
    Note 'dry launch command' 'fail' 'no launcher.launch.dry event in the launcher log'
  }
  if (Read-Journal) { Note 'a dry launch writes no journal' 'fail' 'logs\journal.json exists' }
  else { Note 'a dry launch writes no journal' 'pass' }
  $results | Format-Table -AutoSize | Out-String -Width 200
  exit 0
}

$started = Wait-For {
  $log = Newest-Log '*launcher*.jsonl' $since
  if ($log) { Find-Event $log.FullName 'launcher.launch.started' }
} 30
if (-not $started) {
  $log = Newest-Log '*launcher*.jsonl' $since
  $refused = if ($log) { Find-Event $log.FullName 'launcher.launch.refused' } else { $null }
  $why = if ($refused) { $refused.msg } else { 'no launcher.launch.started event' }
  Note 'launcher started the game' 'fail' $why
  Close-Launcher $launcherProcess
  $results | Format-Table -AutoSize | Out-String -Width 200
  exit 1
}
$gamePid = [int]$started.data.pid
Note 'launcher started the game' 'pass' "pid $gamePid; $($started.msg)"
$game = Get-Process -Id $gamePid -ErrorAction SilentlyContinue

$journal = Wait-Frontend $gamePid
Check-GameLog $gamePid $modded
if ($journal) {
  Start-Sleep -Seconds 3 # the console answers once the menu is up
  Note 'fs_game' 'info' (Console-Value 'fs_game')
  Note 'fs_extramods' 'info' (Console-Value 'fs_extramods')
}

switch ($Case) {
  'crash' {
    Stop-Process -Id $gamePid -Force
    Note 'game ended from outside (a crash)' 'info' "pid $gamePid"
    $exited = Wait-For {
      $log = Newest-Log '*launcher*.jsonl' $since
      if ($log) { Find-Event $log.FullName 'launcher.game.exited' }
    } 20
    if ($exited) { Note 'launcher saw the exit' 'pass' $exited.msg } else { Note 'launcher saw the exit' 'fail' 'no launcher.game.exited event' }
    $j = Read-Journal
    if ($j -and $j.phase -ne 'clean_exit') { Note 'journal records the crash' 'pass' "phase $($j.phase)" }
    else { Note 'journal records the crash' 'fail' 'phase clean_exit or no journal' }
    Close-Launcher $launcherProcess
    $shot = Join-Path $HomeDir 'm1-safe-mode.png'
    $again = Start-Launcher @("+2500 shot $shot", '+1000 quit') 30
    [void]$again.WaitForExit(40000)
    if (Test-Path $shot) { Note 'Home after the crash (should offer safe mode)' 'info' $shot }
    else { Note 'Home after the crash' 'fail' 'no capture' }
  }
  'cli' {
    $env:XERCES_HOME = $HomeDir
    $status = & $cli status 2>&1
    $code = $LASTEXITCODE
    $text = ($status | Out-String).Trim()
    if ($code -eq 0 -and $journal -and $text.Contains($journal.plan_id)) { Note 'xerces_cli status' 'pass' $text }
    else { Note 'xerces_cli status' 'fail' "exit $code`: $text" }
    if ($CliReload -and $Stack.Count -gt 1) {
      $extras = ($Stack | Select-Object -Skip 1) -join ','
      $load = & $cli load --extras $extras 2>&1
      Note 'xerces_cli load --extras' $(if ($LASTEXITCODE -eq 0) { 'pass' } else { 'fail' }) (($load | Out-String).Trim())
      Start-Sleep -Seconds 20
      $unload = & $cli unload 2>&1
      Note 'xerces_cli unload' $(if ($LASTEXITCODE -eq 0) { 'pass' } else { 'fail' }) (($unload | Out-String).Trim())
      Start-Sleep -Seconds 20
    }
    Quit-Game $game
    Close-Launcher $launcherProcess
  }
  default {
    Quit-Game $game
    $exited = Wait-For {
      $log = Newest-Log '*launcher*.jsonl' $since
      if ($log) { Find-Event $log.FullName 'launcher.game.exited' }
    } 20
    if ($exited) { Note 'launcher saw the exit' 'info' $exited.msg }
    Close-Launcher $launcherProcess
  }
}

$results | Format-Table -AutoSize | Out-String -Width 200
if ($results | Where-Object { $_.Result -eq 'fail' }) { exit 1 }
