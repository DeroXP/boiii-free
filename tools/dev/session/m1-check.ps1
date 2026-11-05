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
#   safeoff  After "crash": Home's "Launch with mods off" reaches the menu with
#            no mods, the journal marks it as safe mode, and quitting clears
#            the safe-mode offer.
#   reopen   Launch modded, close Xerces.exe while the game runs and open it
#            again: the game keeps running, the new launcher does not call it
#            a crash, refuses a second launch, and sees the game exit.
#   dvars    Launch modded with a launcher setting (-DvarName, default
#            cg_fov_default, set to a value config.cfg does not have): the game
#            sets it again after config.cfg, and config.cfg keeps its own
#            value afterwards (restored from a backup in the home if not).
#   cliapply xerces_cli plan apply (no --dry-launch) starts the game as Launch
#            modded does; then xerces_cli sig check and logs tail --role game.
#
# Without -Live nothing but Xerces.exe --dry-launch runs: it builds the plan
# file and the command line and starts nothing. -Live starts the real game:
# only with the user's go-ahead for that session (CLAUDE.md, Hard rules).
# The home folder must be outside AppData; it is a test home that this script
# writes freely (config, journal, logs).
param(
  [ValidateSet('vanilla', 'modded', 'crash', 'restart', 'cli', 'safeoff', 'reopen', 'dvars', 'cliapply')]
  [string]$Case = 'vanilla',
  [string]$HomeDir = 'C:\Users\Ginld\XercesHome-m1',
  [string[]]$Stack = @('1944930083', '3171632315', '3656779213'),
  [string]$Mode = 'zm',
  [int]$TimeoutSeconds = 240,
  [string]$DvarName = 'cg_fov_default',
  [string]$DvarValue = '',
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

function Write-Config([bool]$modded, [hashtable]$dvars = $null) {
  $config = Join-Path $HomeDir 'config'
  New-Item -ItemType Directory -Force $config | Out-Null
  $mods = ($Stack | ForEach-Object { '"ws.' + $_ + '"' }) -join ', '
  $flag = if ($modded) { 'true' } else { 'false' }
  $extra = ''
  if ($dvars) {
    $pairs = ($dvars.GetEnumerator() | ForEach-Object { "`"$($_.Key)`": `"$($_.Value)`"" }) -join ', '
    $extra = "  `"dvars`": { $pairs },`n"
  }
  $json = "{`n  `"schema`": `"xerces.config/1`",`n  `"first_run_done`": true,`n  `"mode`": `"$Mode`",`n" + $extra +
    "  `"profiles`": { `"$Mode`": { `"mods`": [$mods], `"modded`": $flag } }`n}`n"
  [System.IO.File]::WriteAllText((Join-Path $config 'launcher.json'), $json)
}

function Finish {
  $results | Format-Table -AutoSize | Out-String -Width 200
  if ($results | Where-Object { $_.Result -eq 'fail' }) { exit 1 }
  exit 0
}

# The log of one launcher process (several can run in the reopen case).
function Launcher-Log($process) {
  Get-ChildItem (Join-Path $HomeDir 'logs') -Filter "*-launcher-$($process.Id).jsonl" -ErrorAction SilentlyContinue |
    Select-Object -First 1
}

# The launcher's launch.started event, or a fail with its refusal.
function Wait-Started($process) {
  $started = Wait-For {
    $log = Launcher-Log $process
    if ($log) { Find-Event $log.FullName 'launcher.launch.started' }
  } 30
  if ($started) {
    Note 'launcher started the game' 'pass' "pid $($started.data.pid); $($started.msg)"
    return $started
  }
  $log = Launcher-Log $process
  $refused = if ($log) { Find-Event $log.FullName 'launcher.launch.refused' } else { $null }
  $why = if ($refused) { $refused.msg } else { 'no launcher.launch.started event' }
  Note 'launcher started the game' 'fail' $why
  return $null
}

# Where the game is: launcher.json's game_dir, else the newest launcher log
# that found it.
function Get-GameDir {
  $configFile = Join-Path $HomeDir 'config\launcher.json'
  $gamePath = if (Test-Path $configFile) { (Get-Content $configFile -Raw | ConvertFrom-Json).game_dir } else { $null }
  if (-not $gamePath) {
    $prefix = 'Black Ops III found at '
    foreach ($log in (Get-ChildItem (Join-Path $HomeDir 'logs') -Filter '*launcher*.jsonl' | Sort-Object LastWriteTime -Descending)) {
      $event = Find-Event $log.FullName 'launcher.game.detected'
      if ($event -and $event.msg.StartsWith($prefix)) {
        # Logs write the profile folder as %USERPROFILE%.
        $gamePath = $event.msg.Substring($prefix.Length).Replace('%USERPROFILE%', $env:USERPROFILE)
        break
      }
    }
  }
  if (-not $gamePath -or -not (Test-Path (Join-Path $gamePath 'BlackOps3.exe'))) {
    throw 'cannot tell where the game is (no game_dir in launcher.json, no launcher.game.detected event)'
  }
  return $gamePath
}

# Runs xerces_cli on this home; stderr notes do not stop the script.
function Run-Cli([string[]]$cliArgs) {
  $env:XERCES_HOME = $HomeDir
  $saved = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try { $all = & $cli @cliArgs 2>&1; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $saved }
  return [pscustomobject]@{
    Code = $code
    Out = (($all | Where-Object { $_ -is [string] }) | Out-String).Trim()
    Err = (($all | Where-Object { $_ -isnot [string] } | ForEach-Object { $_.ToString() }) | Out-String).Trim()
  }
}

function Game-Log([int]$gamePid) {
  Get-ChildItem (Join-Path $HomeDir 'logs') -Filter "*-game-$gamePid.jsonl" -ErrorAction SilentlyContinue |
    Select-Object -First 1
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
  # How it went, for the reopen case: exited, closed or killed.
  $script:lastClose = 'exited'
  if (-not $process -or $process.HasExited) { return }
  [void]$process.CloseMainWindow()
  $script:lastClose = 'closed'
  if (-not $process.WaitForExit(10000)) { Stop-Process -Id $process.Id -Force; $script:lastClose = 'killed' }
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
} elseif (@('vanilla', 'modded', 'crash') -notcontains $Case) {
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
  $gamePath = Get-GameDir
  $env:XERCES_HOME = $HomeDir
  $env:XERCES_DATA_PATH = Join-Path $repo 'data'
  Remove-Item Env:XERCES_LAUNCH_ID -ErrorAction SilentlyContinue
  $arguments = @('-launch', '-nointro', '+set', 'fs_game', $Stack[0])
  if ($Stack.Count -gt 1) { $arguments += @('+set', 'fs_extramods', (($Stack | Select-Object -Skip 1) -join ',')) }
  $game = Start-Process -FilePath $client -ArgumentList $arguments -WorkingDirectory $gamePath -PassThru
  Note 'game started without a launch id' 'info' "pid $($game.Id)"
  # Without a launch id the game says so (loader.journal.none) and writes no
  # phases; loader.journal.skipped is the case of an id that is not the
  # journal's.
  $skipped = Wait-For {
    $log = Get-ChildItem (Join-Path $HomeDir 'logs') -Filter "*-game-$($game.Id).jsonl" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($log) {
      Read-Events $log.FullName | Where-Object { $_.event -eq 'loader.journal.none' -or $_.event -eq 'loader.journal.skipped' } |
        Select-Object -First 1
    }
  } $TimeoutSeconds
  if ($skipped) { Note 'the game leaves the crashed journal alone' 'pass' "$($skipped.event): $($skipped.msg)" }
  else { Note 'the game leaves the crashed journal alone' 'fail' 'no loader.journal.none or loader.journal.skipped event' }
  # Give it time to reach the menu, where a game that owned the journal
  # would write frontend.
  Start-Sleep -Seconds 25
  $gameLog = Get-ChildItem (Join-Path $HomeDir 'logs') -Filter "*-game-$($game.Id).jsonl" -ErrorAction SilentlyContinue | Select-Object -First 1
  $phases = if ($gameLog) { @(Read-Events $gameLog.FullName | Where-Object { $_.event -eq 'loader.phase' }) } else { @() }
  if ($phases.Count -eq 0) { Note 'no phases written by the restarted game' 'pass' }
  else { Note 'no phases written by the restarted game' 'fail' (($phases | ForEach-Object { $_.msg }) -join ', ') }
  Start-Sleep -Seconds 5
  $after = Get-Content (Join-Path $HomeDir 'logs\journal.json') -Raw
  if ($after -eq $raw) { Note 'journal unchanged' 'pass' "still $($before.phase), plan '$($before.plan_id)'" }
  else { Note 'journal unchanged' 'fail' 'journal.json changed' }
  & (Join-Path $PSScriptRoot 'stop-game.ps1') -WaitSeconds 30 | ForEach-Object { Note 'quit' 'info' $_ }
  $after = Get-Content (Join-Path $HomeDir 'logs\journal.json') -Raw
  if ($after -eq $raw) { Note 'journal unchanged after the restarted game quit' 'pass' }
  else { Note 'journal unchanged after the restarted game quit' 'fail' 'the quit wrote the journal' }
  Finish
}

# --- safeoff: Home's "Launch with mods off" after a crash -------------------
if ($Case -eq 'safeoff') {
  $before = Read-Journal
  if (-not $before -or $before.phase -eq 'clean_exit' -or -not $before.stack) {
    throw 'no crashed modded launch to recover from: run -Case crash first'
  }
  Note 'crashed launch on record' 'info' "phase $($before.phase), $(@($before.stack).Count) mods"
  Write-Config $true
  $launcherProcess = Start-Launcher @('+3000 click safe-off') ($TimeoutSeconds + 180)
  $started = Wait-Started $launcherProcess
  if (-not $started) { Close-Launcher $launcherProcess; Finish }
  $gamePid = [int]$started.data.pid
  if ($started.msg -match 'fs_game|fs_extramods|xerces-plan') { Note 'no mods on the command line' 'fail' $started.msg }
  else { Note 'no mods on the command line' 'pass' }
  $journal = Wait-Frontend $gamePid
  if ($journal) {
    if ($journal.safe_mode) { Note 'journal marks the launch as safe mode' 'pass' }
    else { Note 'journal marks the launch as safe mode' 'fail' 'safe_mode is false' }
    Start-Sleep -Seconds 3
    Note 'fs_game' 'info' (Console-Value 'fs_game')
  }
  $log = Game-Log $gamePid
  $start = if ($log) { Find-Event $log.FullName 'loader.start' } else { $null }
  if ($start -and $start.msg.StartsWith('no plan')) { Note 'the game loads no plan' 'pass' $start.msg }
  else { Note 'the game loads no plan' 'fail' $(if ($start) { $start.msg } else { 'no loader.start event' }) }
  Quit-Game $null
  Close-Launcher $launcherProcess
  $shot = Join-Path $HomeDir 'm1-after-safe.png'
  $again = Start-Launcher @("+2500 shot $shot", '+1000 quit') 30
  [void]$again.WaitForExit(40000)
  if (Test-Path $shot) { Note 'Home after the safe launch (no safe-mode offer expected)' 'info' $shot }
  Finish
}

# --- reopen: close the launcher while the game runs, open it again ----------
if ($Case -eq 'reopen') {
  Write-Config $true
  Reset-Journal
  Warm-Catalog
  $first = Start-Launcher @('+3000 click launch-modded') ($TimeoutSeconds + 180)
  $started = Wait-Started $first
  if (-not $started) { Close-Launcher $first; Finish }
  $gamePid = [int]$started.data.pid
  $journal = Wait-Frontend $gamePid
  if (-not $journal) { Close-Launcher $first; Quit-Game $null; Finish }
  Close-Launcher $first
  Note 'launcher closed while the game runs' $(if ($lastClose -eq 'killed') { 'fail' } else { 'pass' }) $lastClose
  Start-Sleep -Seconds 3
  if (-not (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) {
    Note 'the game keeps running without the launcher' 'fail' "pid $gamePid is gone"
    Finish
  }
  Note 'the game keeps running without the launcher' 'pass' "pid $gamePid"
  $runEntries = @(Get-ChildItem (Join-Path $HomeDir 'run') -ErrorAction SilentlyContinue)
  Note 'run folder' 'info' (($runEntries | ForEach-Object { $_.Name }) -join ', ')
  $journalBefore = Get-Content (Join-Path $HomeDir 'logs\journal.json') -Raw
  $shot = Join-Path $HomeDir 'm1-reopen.png'
  $second = Start-Launcher @("+3000 shot $shot", '+1000 click launch-modded') ($TimeoutSeconds + 120)
  $refused = Wait-For {
    $log = Launcher-Log $second
    if ($log) { Read-Events $log.FullName | Where-Object { $_.event -eq 'launcher.launch.refused' -or $_.event -eq 'launcher.launch.started' } | Select-Object -First 1 }
  } 20
  if ($refused -and $refused.event -eq 'launcher.launch.refused') { Note 'a second launch is refused while the game runs' 'pass' $refused.msg }
  elseif ($refused) { Note 'a second launch is refused while the game runs' 'fail' "it started one: $($refused.msg)" }
  else { Note 'a second launch is refused while the game runs' 'fail' 'neither refused nor started' }
  if (Test-Path $shot) { Note 'Home of the reopened launcher (no safe-mode offer expected)' 'info' $shot }
  $journalNow = Get-Content (Join-Path $HomeDir 'logs\journal.json') -Raw
  if ($journalNow -eq $journalBefore -or (Read-Journal).phase -in @('frontend', 'in_game')) {
    Note 'the reopened launcher leaves the running launch alone' 'pass' "phase $((Read-Journal).phase)"
  } else { Note 'the reopened launcher leaves the running launch alone' 'fail' "phase $((Read-Journal).phase)" }
  Quit-Game $null
  $gone = Wait-For {
    $log = Launcher-Log $second
    if ($log) { Find-Event $log.FullName 'launcher.game.gone' }
  } 15
  if ($gone) { Note 'the reopened launcher saw the game exit' 'pass' $gone.msg }
  else { Note 'the reopened launcher saw the game exit' 'fail' 'no launcher.game.gone event' }
  Close-Launcher $second
  Finish
}

# --- dvars: a launcher setting wins for the session, config.cfg keeps its own
if ($Case -eq 'dvars') {
  $gameDir = Get-GameDir
  $cfg = Join-Path $gameDir 'boiii_players\user\config.cfg'
  $pattern = '^\s*seta?\s+' + [regex]::Escape($DvarName) + '\s'
  $savedLine = $null
  if (Test-Path $cfg) {
    $backup = Join-Path $HomeDir ('config.cfg.before-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    Copy-Item $cfg $backup
    Note 'config.cfg backed up' 'info' $backup
    $savedLine = Get-Content $cfg | Where-Object { $_ -match $pattern } | Select-Object -First 1
  }
  Note "config.cfg's $DvarName" 'info' $(if ($savedLine) { $savedLine } else { '(none)' })
  $value = $DvarValue
  if (-not $value) { $value = if ($savedLine -match '"90"') { '100' } else { '90' } }
  Write-Config $true @{ $DvarName = $value }
  Reset-Journal
  Warm-Catalog
  $launcherProcess = Start-Launcher @('+3000 click launch-modded') ($TimeoutSeconds + 180)
  $started = Wait-Started $launcherProcess
  if (-not $started) { Close-Launcher $launcherProcess; Write-Config $true; Finish }
  $gamePid = [int]$started.data.pid
  if ($started.msg.Contains("+set $DvarName $value")) { Note 'the setting is on the command line' 'pass' "+set $DvarName $value" }
  else { Note 'the setting is on the command line' 'fail' $started.msg }
  $journal = Wait-Frontend $gamePid
  $log = Game-Log $gamePid
  foreach ($name in @('xerces.dvars.session', 'xerces.dvars.reapplied')) {
    $event = if ($log) { Find-Event $log.FullName $name } else { $null }
    if ($event) { Note $name 'pass' $event.msg } else { Note $name 'fail' 'not in the game log' }
  }
  if ($journal) {
    Start-Sleep -Seconds 3
    $now = Console-Value $DvarName
    if ($now -match ('is: "' + [regex]::Escape($value) + '"')) { Note "$DvarName in the game" 'pass' $now }
    else { Note "$DvarName in the game" 'fail' $now }
  }
  Quit-Game $null
  Close-Launcher $launcherProcess
  $afterLine = if (Test-Path $cfg) { Get-Content $cfg | Where-Object { $_ -match $pattern } | Select-Object -First 1 } else { $null }
  if ($afterLine -eq $savedLine) { Note 'config.cfg keeps its own value' 'pass' $(if ($afterLine) { $afterLine } else { '(none)' }) }
  else {
    Note 'config.cfg keeps its own value' 'fail' "now '$afterLine', was '$savedLine'"
    if ($backup) { Copy-Item $backup $cfg -Force; Note 'config.cfg restored from the backup' 'info' $backup }
  }
  Write-Config $true # the home's config without the test setting
  Finish
}

# --- cliapply: xerces_cli plan apply starts the game -----------------------
if ($Case -eq 'cliapply') {
  Write-Config $true
  Reset-Journal
  Warm-Catalog
  $apply = Run-Cli @('plan', 'apply', '--mode', $Mode, '--json')
  $doc = $null
  try { $doc = $apply.Out | ConvertFrom-Json } catch { }
  if ($apply.Code -eq 0 -and $doc -and $doc.started) {
    Note 'xerces_cli plan apply started the game' 'pass' "pid $($doc.pid); $($doc.command_line)"
  } else {
    Note 'xerces_cli plan apply started the game' 'fail' "exit $($apply.Code): $($apply.Err) $($apply.Out)"
    Finish
  }
  if ($apply.Err) { Note 'plan apply notes' 'info' $apply.Err }
  $gamePid = [int]$doc.pid
  $journal = Wait-Frontend $gamePid
  Check-GameLog $gamePid $true
  if ($journal) {
    Start-Sleep -Seconds 3
    Note 'fs_game' 'info' (Console-Value 'fs_game')
    Note 'fs_extramods' 'info' (Console-Value 'fs_extramods')
  }
  $sig = Run-Cli @('sig', 'check')
  Note 'xerces_cli sig check' $(if ($sig.Code -eq 0) { 'pass' } else { 'fail' }) "exit $($sig.Code): $($sig.Out)"
  $tail = Run-Cli @('logs', 'tail', '--role', 'game', '--lines', '4')
  Note 'xerces_cli logs tail --role game' $(if ($tail.Code -eq 0 -and $tail.Out) { 'pass' } else { 'fail' }) $tail.Out
  Quit-Game $null
  Finish
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
  if ($results | Where-Object { $_.Result -eq 'fail' }) { exit 1 }
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
