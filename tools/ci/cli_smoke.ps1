<#
.SYNOPSIS
  CLI smoke test for the commands that work on <home> as Xerces.exe does
  (docs/DESIGN.md 15.2): mods list, info, enable, disable and order, plan
  apply --dry-launch, packs apply, logs tail and sig check; and the offline
  benchmark (bench loads, launcher, analyze and compare) on small logs and
  frame CSVs written here.

.DESCRIPTION
  Runs xerces_cli against a fresh home and a copy of a fake game folder (two
  mods that declare a conflict, plus a legacy mod with escape sequences in its
  title written here). The fake game comes from -FakeGame, else
  XERCES_FAKE_GAME, else C:\Users\Ginld\XercesDev\fake-game when it exists;
  without one, or with -Generate, the same layout is written here (CI).
  Everything goes under -WorkDir (default build\cli-smoke), outside AppData.

  The copy's BlackOps3.exe is replaced by a fake executable that only this
  test's own compat\builds.json knows, next to a copy of xerces_cli in
  <WorkDir>\bin. That copy's pre-flight passes, as it does for the real game,
  so dry launches reach the command line; the build's own xerces_cli reads the
  repository's compat\builds.json, where the fake game is unknown, and is
  refused as Xerces.exe would be.

  It never starts the game: plan apply always runs dry (--dry-launch, and
  XERCES_DRY_LAUNCH=1 for every call), the copy of xerces_cli has no boiii.exe
  next to it, so even a real launch would stop there, and the build's own one
  is refused at the pre-flight. Every call passes --home; XERCES_HOME is set
  too and restored.
#>
param(
  [string]$Cli,
  [string]$FakeGame = $env:XERCES_FAKE_GAME,
  [string]$WorkDir,
  [switch]$Generate
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Cli) { $Cli = Join-Path $root 'build\bin\x64\Release\xerces_cli.exe' }
if (-not $WorkDir) { $WorkDir = Join-Path $root 'build\cli-smoke' }
if (-not (Test-Path $Cli)) { throw "$Cli does not exist; build xerces_cli first." }
if ($Generate) {
  $FakeGame = $null
} elseif (-not $FakeGame -and (Test-Path 'C:\Users\Ginld\XercesDev\fake-game')) {
  $FakeGame = 'C:\Users\Ginld\XercesDev\fake-game'
}

# ------------------------------------------------------------------ layout
if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
$game = Join-Path $WorkDir 'steamapps\common\Black Ops III'
$homeDir = Join-Path $WorkDir 'home'
New-Item -ItemType Directory -Force (Split-Path -Parent $game), $homeDir | Out-Null
if ($FakeGame) {
  Copy-Item -Recurse $FakeGame $game
} else {
  New-Item -ItemType Directory -Force "$game\mods\conflict_a\zone", "$game\mods\conflict_b\zone" | Out-Null
  Set-Content -NoNewline -Encoding ascii "$game\mods\conflict_a\xerces.mod.json" `
    '{"schema":1,"id":"test.conflict_a","name":"Conflict A","version":"1.0.0","license":"MIT","conflicts":[{"id":"test.conflict_b","reason":"Both rewrite the box"}]}'
  Set-Content -NoNewline -Encoding ascii "$game\mods\conflict_b\xerces.mod.json" `
    '{"schema":1,"id":"test.conflict_b","name":"Conflict B","version":"1.0.0","license":"MIT"}'
  New-Item -ItemType File "$game\mods\conflict_a\zone\core_mod.ff", "$game\mods\conflict_b\zone\core_mod.ff" | Out-Null
}
# A Windows executable as far as the pre-flight reads one (the DOS header,
# "PE" at 0x40, the optional header's CheckSum), and a build only this test's
# compat\builds.json lists. Written into the copy, never the source.
$pe = New-Object byte[] 184
$pe[0] = 0x4D; $pe[1] = 0x5A
[BitConverter]::GetBytes([uint32]0x40).CopyTo($pe, 0x3C)
[BitConverter]::GetBytes([uint32]0x4550).CopyTo($pe, 0x40)
[BitConverter]::GetBytes([uint32]0x7E570001).CopyTo($pe, 0x40 + 24 + 64)
[IO.File]::WriteAllBytes("$game\BlackOps3.exe", $pe)
# A legacy mod whose Workshop title carries escape sequences (JSON decodes
# them to real ESC and BEL characters).
New-Item -ItemType Directory -Force "$game\mods\esc_mod\zone" | Out-Null
Set-Content -NoNewline -Encoding ascii "$game\mods\esc_mod\zone\workshop.json" `
  '{"Title":"Esc\u001b[2J Mod\u001b]52;c;aGk=\u0007","PublisherID":"","Type":"mod"}'
New-Item -ItemType File "$game\mods\esc_mod\zone\core_mod.ff" | Out-Null

# The copy of xerces_cli this test runs: its compat\ is the one next to it,
# and no game client is.
$bin = Join-Path $WorkDir 'bin'
New-Item -ItemType Directory -Force "$bin\compat" | Out-Null
Copy-Item $Cli "$bin\xerces_cli.exe"
Set-Content -NoNewline -Encoding ascii "$bin\compat\builds.json" `
  '{"schema":"xerces.builds/1","builds":[{"name":"CLI smoke fake game","exe":"BlackOps3.exe","checksum":"0x7E570001","status":"supported"}]}'
if (Test-Path "$bin\boiii.exe") { throw "$bin\boiii.exe must not exist: this test never starts a game." }
$repoCli = $Cli
$script:exe = "$bin\xerces_cli.exe"

New-Item -ItemType Directory -Force "$homeDir\config" | Out-Null
$config = [ordered]@{ schema = 'xerces.config/1'; first_run_done = $true; mode = 'zm'; game_dir = $game }
[IO.File]::WriteAllText("$homeDir\config\launcher.json", ($config | ConvertTo-Json))

# ----------------------------------------------------------------- helpers
$script:failures = @()
# Invoke-Xcli <expected exit code> <arguments...>: stdout and stderr as one string.
# Plain $args, so "--up" and the like reach xerces_cli as they are.
function Invoke-Xcli {
  $expect = [int]$args[0]
  $rest = @($args | Select-Object -Skip 1)
  # Windows PowerShell turns a native program's stderr into errors, which
  # 'Stop' would throw.
  $ErrorActionPreference = 'Continue'
  $out = & $script:exe @rest --home $homeDir 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  if ($code -ne $expect) {
    $script:failures += "xerces_cli $($rest -join ' '): exit $code, expected $expect`n  $($out -join "`n  ")"
  }
  return ($out -join "`n")
}
# The same for --json: stdout only, parsed.
function Invoke-XcliJson {
  $expect = [int]$args[0]
  $rest = @($args | Select-Object -Skip 1)
  $ErrorActionPreference = 'Continue'
  $out = & $script:exe @rest --home $homeDir 2>$null
  $code = $LASTEXITCODE
  if ($code -ne $expect) {
    $script:failures += "xerces_cli $($rest -join ' '): exit $code, expected $expect"
  }
  return (($out -join "`n") | ConvertFrom-Json)
}
function Check([bool]$Ok, [string]$What) {
  if (-not $Ok) { $script:failures += $What }
}
function ZmProfile {
  $json = Get-Content -Raw "$homeDir\config\launcher.json" | ConvertFrom-Json
  return $json.profiles.zm
}
function Mods { return @((ZmProfile).mods) -join ',' }
function PlanFiles { return @(Get-ChildItem "$homeDir\config\plans" -Filter '*.json' -ErrorAction SilentlyContinue).Count }
$esc = [string][char]27
$bel = [string][char]7

$savedHome = $env:XERCES_HOME
$savedDry = $env:XERCES_DRY_LAUNCH
$env:XERCES_HOME = $homeDir
$env:XERCES_DRY_LAUNCH = '1'
try {
  # ------------------------------------------------------- mods: the catalog
  $list = Invoke-Xcli 0 mods list
  Check ($list -match 'test\.conflict_a' -and $list -match 'test\.conflict_b') "mods list shows both fake mods`n$list"
  # Text output never carries a title's control characters to the console.
  Check ($list -match 'Esc\?\[2J Mod\?\]52;c;aGk=\?' -and -not $list.Contains($esc) -and -not $list.Contains($bel)) "mods list filters control characters`n$list"
  $escInfo = Invoke-Xcli 0 mods info local.esc_mod
  Check ($escInfo -match 'Esc\?\[2J Mod' -and -not $escInfo.Contains($esc)) "mods info filters control characters`n$escInfo"
  # --json keeps the title, escaped.
  $escJson = Invoke-XcliJson 0 mods info local.esc_mod --json
  Check ($escJson.name -eq "Esc$esc[2J Mod$esc]52;c;aGk=$bel") "mods info --json keeps the title: $($escJson.name)"

  # ------------------------------------------- mods: the Mods screen's edits
  Invoke-Xcli 0 mods enable test.conflict_a | Out-Null
  Check ((Mods) -eq 'test.conflict_a') "enable: load order is $(Mods)"
  $both = Invoke-Xcli 0 mods enable test.conflict_b
  Check ((Mods) -eq 'test.conflict_a,test.conflict_b') "enable goes to the bottom: $(Mods)"
  Check ($both -match 'conflict') "enabling the second mod names the declared conflict`n$both"
  Invoke-Xcli 1 mods enable test.not_installed | Out-Null
  Invoke-Xcli 0 mods order test.conflict_b --up | Out-Null
  Check ((Mods) -eq 'test.conflict_b,test.conflict_a') "order --up: $(Mods)"
  Invoke-Xcli 0 mods order test.conflict_b --to 2 | Out-Null
  Check ((Mods) -eq 'test.conflict_a,test.conflict_b') "order --to 2: $(Mods)"
  Invoke-Xcli 1 mods order test.conflict_b | Out-Null # no direction
  # Anything a command does not take stops it before it changes anything.
  Invoke-Xcli 1 mods disable test.conflict_b extra | Out-Null
  Invoke-Xcli 1 mods order test.conflict_b --upp | Out-Null
  Check ((Mods) -eq 'test.conflict_a,test.conflict_b') "refused edits changed nothing: $(Mods)"

  # The declared conflict blocks the plan: exit 2, nothing launched.
  Invoke-Xcli 2 plan apply --dry-launch | Out-Null
  Check (-not (Test-Path "$homeDir\logs\journal.json")) 'a refused plan wrote no journal'

  $info = Invoke-XcliJson 0 mods info test.conflict_a --json
  Check ($info.position -eq 1 -and $info.conflicts -contains 'test.conflict_b' -and $info.problems.Count -gt 0) `
    "mods info: position $($info.position), conflicts $($info.conflicts), $($info.problems.Count) problems"

  Invoke-Xcli 0 mods disable test.conflict_b | Out-Null
  Check ((Mods) -eq 'test.conflict_a') "disable: $(Mods)"
  Invoke-Xcli 0 mods disable test.conflict_b | Out-Null # already off: nothing changes

  # ------------------------------------------------- plan apply, dry launch
  $dry = Invoke-Xcli 0 plan apply --dry-launch
  Check ($dry -match 'command: -launch -nointro -xerces-plan [0-9a-f]{64} \+set fs_game conflict_a(\r?\n|$)') "plan apply --dry-launch command`n$dry"
  Check ($dry -match 'a real launch would stop: boiii\.exe is missing') "the dry launch says what a real one would refuse`n$dry"
  Check ((PlanFiles) -eq 1) "one plan file, found $(PlanFiles)"
  Check (-not (Test-Path "$homeDir\logs\journal.json")) 'a dry launch writes no journal'
  $json = Invoke-XcliJson 0 plan apply --dry-launch --json
  Check ($json.dry_run -and -not $json.started -and $json.command_line -match 'fs_game conflict_a') "plan apply --json`n$($json | ConvertTo-Json)"
  $vanilla = Invoke-Xcli 0 plan apply --vanilla --dry-launch
  Check ($vanilla -match 'command: -launch -nointro(\r?\n|$)') "vanilla dry launch`n$vanilla"
  # XERCES_DRY_LAUNCH=1 makes it dry without the flag, as it does Xerces.exe.
  $byEnv = Invoke-Xcli 0 plan apply
  Check ($byEnv -match 'XERCES_DRY_LAUNCH=1, so this is a dry launch' -and $byEnv -match 'command: -launch') "XERCES_DRY_LAUNCH=1 plan apply`n$byEnv"
  Check (-not (Test-Path "$homeDir\logs\journal.json")) 'XERCES_DRY_LAUNCH=1 wrote no journal'

  # A misspelled or misplaced --dry-launch is refused, never dropped.
  $plans = PlanFiles
  foreach ($bad in @(@('--dry-run'), @('--dry-launch=yes'), @('-dry-launch'), @('dry-launch'),
                     @('--vanilla', '--home', '--dry-launch'), @('--json=1'))) {
    $refused = Invoke-Xcli 1 plan apply @bad
    Check ($refused -match 'Nothing was done') "plan apply $($bad -join ' ') is refused`n$refused"
  }
  Check ((PlanFiles) -eq $plans) 'refused plan applies wrote no plan file'
  $follow = Invoke-Xcli 1 logs tail --follow=yes
  Check ($follow -match '--follow takes no value') "logs tail --follow=yes is refused`n$follow"

  # Xerces.exe's pre-flight: the build's own xerces_cli reads the repository's
  # compat\builds.json, where the fake game is unknown, and a dry launch is
  # refused there as Home's is.
  $script:exe = $repoCli
  $unknown = Invoke-Xcli 1 plan apply --dry-launch
  $script:exe = "$bin\xerces_cli.exe"
  Check ($unknown -match 'not a build Xerces knows' -and $unknown -notmatch 'command:') "a dry launch on an unknown build is refused`n$unknown"
  Check ((PlanFiles) -eq $plans) 'the refused dry launch wrote no plan file'

  # ------------------------------------------------------------ packs apply
  $packFile = Join-Path $WorkDir 'smoke.xpack.json'
  Invoke-Xcli 0 packs export --mods-dir "$game\mods" --name 'Smoke pack' --mod test.conflict_b --out $packFile | Out-Null
  $applied = Invoke-Xcli 0 packs apply $packFile
  Check ($applied -match 'Ready to launch') "packs apply from a file`n$applied"
  $p = ZmProfile
  Check ((Mods) -eq 'test.conflict_b' -and $p.pack_name -eq 'Smoke pack' -and $p.pins.'test.conflict_b' -eq '1.0.0') `
    "packs apply: profile $($p | ConvertTo-Json -Compress)"
  $backups = @(Get-ChildItem "$homeDir\backups" -Recurse -Filter 'launcher.json')
  Check ($backups.Count -ge 1) 'packs apply backed the config up first'
  $dry = Invoke-Xcli 0 plan apply --dry-launch
  Check ($dry -match '\+set fs_game conflict_b') "the pack's plan launches`n$dry"
  # A hand edit detaches the profile from the pack.
  $edit = Invoke-Xcli 0 mods enable test.conflict_a
  Check ($edit -match 'Detached from the pack' -and -not (ZmProfile).pack_name) "a hand edit detaches the pack`n$edit"
  # A share code, and a pack in the library by the start of its id.
  $code = (Invoke-Xcli 0 packs export --mods-dir "$game\mods" --name 'Code pack' --mod test.conflict_a --code).Trim()
  Invoke-Xcli 0 packs apply $code | Out-Null
  Check ((ZmProfile).pack_name -eq 'Code pack') 'packs apply from a share code'
  $pack = Get-Content -Raw $packFile | ConvertFrom-Json
  New-Item -ItemType Directory -Force "$homeDir\config\packs" | Out-Null
  Copy-Item $packFile "$homeDir\config\packs\$($pack.id).json"
  Invoke-Xcli 0 packs apply $pack.id.Substring(0, 8) | Out-Null
  Check ((ZmProfile).pack_name -eq 'Smoke pack' -and (ZmProfile).pack_id -eq $pack.id) 'packs apply by library id'
  Invoke-Xcli 2 packs apply 'no-such-pack' | Out-Null
  # A file whose id belongs to a different pack in the library is applied
  # under a fresh id, as importing it would place it; the library is not
  # written. The very same pack keeps its id.
  $otherFile = Join-Path $WorkDir 'other.xpack.json'
  [IO.File]::WriteAllText($otherFile, (Get-Content -Raw $packFile).Replace('"Smoke pack"', '"Other pack"'))
  $other = Invoke-XcliJson 0 packs apply $otherFile --json
  Check ($other.renumbered_from -eq $pack.id -and $other.pack -ne $pack.id -and $other.pack -ne '' -and
         (ZmProfile).pack_id -eq $other.pack -and (ZmProfile).pack_name -eq 'Other pack') `
    "a file with a library pack's id is renumbered: $($other | ConvertTo-Json -Compress -Depth 2)"
  Check (@(Get-ChildItem "$homeDir\config\packs").Count -eq 1) 'packs apply does not write the library'
  $same = Invoke-XcliJson 0 packs apply $packFile --json
  Check ($same.pack -eq $pack.id -and -not $same.renumbered_from -and (ZmProfile).pack_id -eq $pack.id) `
    "the library's own pack keeps its id: $($same | ConvertTo-Json -Compress -Depth 2)"

  # ------------------------------------------ refused while Xerces.exe runs
  # The launcher's per-home mutex (xerces_core::launcher_instance_name).
  $full = [IO.Path]::GetFullPath($homeDir).TrimEnd('\')
  $hash = [uint64]2166136261
  foreach ($c in $full.ToCharArray()) {
    $hash = (($hash -bxor [uint64][int][char]::ToLowerInvariant($c)) * [uint64]16777619) % [uint64]4294967296
  }
  $mutex = [System.Threading.Mutex]::new($true, "Local\xerces_launcher_$hash")
  try {
    $refused = Invoke-Xcli 1 mods enable test.conflict_a
    Check ($refused -match 'Xerces\.exe is open') "edits refused while a launcher runs`n$refused"
    Invoke-Xcli 1 packs apply $packFile | Out-Null
  } finally {
    $mutex.ReleaseMutex()
    $mutex.Dispose()
  }

  # ------------------------------------------------------------ logs, sigs
  Invoke-Xcli 0 mods enable local.esc_mod | Out-Null
  $tail = Invoke-Xcli 0 logs tail --role cli --lines 100
  foreach ($event in 'cli.launch.dry', 'cli.launch.refused', 'cli.mods.edit', 'cli.packs.backup', 'cli.packs.applied') {
    Check ($tail -match [regex]::Escape($event)) "logs tail shows $event`n$tail"
  }
  Check ($tail -match 'Esc\?\[2J Mod' -and -not $tail.Contains($esc) -and -not $tail.Contains($bel)) "logs tail filters control characters`n$tail"
  Check (@(Get-ChildItem "$homeDir\logs" -Filter '*-cli-*.jsonl').Count -eq 1) 'the CLI keeps one log per day'
  Invoke-Xcli 1 logs tail --role game | Out-Null
  # A log that cannot be opened is an error, not an empty success.
  $locked = Join-Path $homeDir 'logs\20991231-235959-launcher-1.jsonl'
  [IO.File]::WriteAllText($locked, '{"event":"launcher.start","level":"info","msg":"late","ts":"2099-12-31T23:59:59.000Z"}' + "`n")
  $handle = [IO.File]::Open($locked, 'Open', 'Read', 'None')
  try {
    $unreadable = Invoke-Xcli 1 logs tail --role launcher
    Check ($unreadable -match 'cannot be read') "an unreadable log is an error`n$unreadable"
  } finally {
    $handle.Dispose()
  }
  $late = Invoke-Xcli 0 logs tail --role launcher
  Check ($late -match 'launcher\.start \| late') "logs tail reads it once it can`n$late"

  Invoke-Xcli 1 sig check | Out-Null # no report yet
  $report = '{"schema":"xerces.sigreport/1","game_build":"0x6531394","checked":"2026-11-05T03:00:00Z","entries":[{"id":"a","name":"A","resolved":true,"rva":"0x1000"},{"id":"b","name":"B","resolved":false,"error":"no match"}],"disabled":{"multi_mod":["b"]}}'
  New-Item -ItemType Directory -Force "$homeDir\cache" | Out-Null
  [IO.File]::WriteAllText("$homeDir\cache\signatures-report.json", $report)
  $sig = Invoke-Xcli 4 sig check
  Check ($sig -match 'MISS\s+b' -and $sig -match 'multi_mod') "sig check names the miss`n$sig"
  [IO.File]::WriteAllText("$homeDir\cache\signatures-report.json", $report.Replace('"resolved":false', '"resolved":true').Replace('"multi_mod":["b"]', ''))
  Invoke-Xcli 0 sig check | Out-Null
  Invoke-Xcli 1 sig check --registry compat/signatures.json | Out-Null # --registry goes with --dump

  # ------------------------------------------------- bench (offline only)
  # A launcher log, a map-check folder (game log, console, results.jsonl) and
  # frame CSVs written here; bench only reads them. No --home on these calls.
  function Invoke-Bench {
    $expect = [int]$args[0]
    $rest = @($args | Select-Object -Skip 1)
    $ErrorActionPreference = 'Continue'
    $out = & $script:exe @rest 2>$null
    $code = $LASTEXITCODE
    if ($code -ne $expect) {
      $script:failures += "xerces_cli $($rest -join ' '): exit $code, expected $expect"
    }
    return ($out -join "`n")
  }
  $bench = Join-Path $WorkDir 'bench'
  $benchLogs = Join-Path $bench 'logs'
  $checks = Join-Path $bench 'map-checks'
  $runDir = Join-Path $checks '20261105-120000-zm_test-vanilla'
  New-Item -ItemType Directory -Force $benchLogs, $runDir | Out-Null
  [IO.File]::WriteAllText("$benchLogs\20261105-120000-launcher-100.jsonl", (@(
        '{"event":"launcher.start","level":"info","msg":"Xerces","role":"launcher","session":"100","ts":"2026-11-05T12:00:00.000Z","data":{"dry_launch":false}}'
        '{"event":"launcher.startup.step","level":"info","msg":"window created at 15.0 ms","role":"launcher","session":"100","ts":"2026-11-05T12:00:00.015Z","data":{"ms":15.0}}'
        '{"event":"launcher.startup.step","level":"info","msg":"first frame presented at 220.0 ms","role":"launcher","session":"100","ts":"2026-11-05T12:00:00.220Z","data":{"ms":220.0}}'
        '{"event":"launcher.catalog.scanned","level":"info","msg":"17 items","role":"launcher","session":"100","ts":"2026-11-05T12:00:00.400Z","data":{"bytes_hashed":0,"cache_hits":686,"cache_misses":0,"items":17,"seconds":0.08}}'
        '{"event":"launcher.launch.started","level":"info","msg":"-launch","role":"launcher","session":"100","ts":"2026-11-05T12:00:05.000Z","data":{"pid":4242,"plan_id":""}}'
      ) -join "`n") + "`n")
  [IO.File]::WriteAllText("$runDir\20261105-120019-game-4242.jsonl", (@(
        '{"event":"loader.phase","level":"info","msg":"symbols","role":"game","session":"4242","ts":"2026-11-05T12:00:19.000Z","data":{"phase":"symbols","plan_id":""}}'
        '{"event":"loader.phase","level":"info","msg":"frontend","role":"game","session":"4242","ts":"2026-11-05T12:00:39.000Z","data":{"phase":"frontend","plan_id":""}}'
        '{"event":"loader.phase","level":"info","msg":"loading_map","role":"game","session":"4242","ts":"2026-11-05T12:00:50.000Z","data":{"phase":"loading_map","plan_id":""}}'
        '{"event":"loader.phase","level":"info","msg":"in_game","role":"game","session":"4242","ts":"2026-11-05T12:01:10.000Z","data":{"phase":"in_game","plan_id":""}}'
        '{"event":"loader.map.ready","level":"info","msg":"zm_test","role":"game","session":"4242","ts":"2026-11-05T12:01:10.000Z","data":{"map":"zm_test"}}'
      ) -join "`n") + "`n")
  [IO.File]::WriteAllText("$runDir\console.txt", (@(
        'Command line: -launch -nointro'
        'FastFileLoad: Setting core_frontend to XZONE_LOADED in 2000ms.'
        ']map zm_test'
        'ModLoad start: usermaps'
        'FastFileLoad: Setting core_frontend to XZONE_LOADED in 2300ms.'
        'ModLoad done: usermaps'
        'FastFileLoad: Setting zm_test to XZONE_LOADED in 900ms.'
        'FastFileLoad: Setting core_frontend to XZONE_LOADED in 2400ms.'
      ) -join "`r`n") + "`r`n")
  [IO.File]::WriteAllText("$checks\results.jsonl",
    '{"time":"2026-11-05T06:00:00","map":"zm_test","mode":"zm","stack":[],"result":"in_game","detail":"","menu_seconds":38.0,"load_seconds":26.0,"pid":4242,"console":"X:\\map-checks\\20261105-120000-zm_test-vanilla\\console.txt"}' + "`n")

  $loads = (Invoke-Bench 0 bench loads --logs $benchLogs --console $checks --json) | ConvertFrom-Json
  $launch = @($loads.launches)[0]
  $mapLoad = @($loads.map_loads)[0]
  Check ($launch.game_start_to_frontend_s -eq 34 -and $launch.first_log_to_frontend_s -eq 20 -and $launch.variant -eq 'vanilla') `
    "bench loads: launch $($launch | ConvertTo-Json -Compress -Depth 3)"
  Check ($mapLoad.journal -eq 'new' -and $mapLoad.map -eq 'zm_test' -and $mapLoad.map_command_to_ready_s -eq 26 -and
         $mapLoad.loading_map_to_ready_s -eq 20 -and $mapLoad.map_command_to_level_s -eq 6) `
    "bench loads: map load times $($mapLoad | ConvertTo-Json -Compress -Depth 4)"
  Check ($mapLoad.zones.total_ms -eq 3200 -and $mapLoad.frontend_reload_ms -eq 2300 -and $mapLoad.after_map_zone.ms -eq 2400) `
    "bench loads: zones up to the map's own zone $($mapLoad | ConvertTo-Json -Compress -Depth 4)"
  $loadsText = Invoke-Bench 0 bench loads --logs $benchLogs --console $checks
  Check ($loadsText -match 'zm_test' -and $loadsText -match 'Front-end zones reloaded') "bench loads text`n$loadsText"
  $launcher = (Invoke-Bench 0 bench launcher --logs $benchLogs --json) | ConvertFrom-Json
  $window = @($launcher.marks_ms | Where-Object { $_.mark -eq 'window created' })
  Check ($launcher.sessions -eq 1 -and $window.Count -eq 1 -and $window[0].median -eq 15 -and $launcher.scans.warm_s.n -eq 1) `
    "bench launcher $($launcher | ConvertTo-Json -Compress -Depth 4)"

  # Frame CSVs as the probe writes them: three runs a side, B 0.002 ms slower.
  $inv = [Globalization.CultureInfo]::InvariantCulture
  foreach ($side in 'a', 'b') {
    New-Item -ItemType Directory -Force "$bench\$side" | Out-Null
    for ($run = 0; $run -lt 3; $run++) {
      $sb = New-Object Text.StringBuilder
      [void]$sb.Append("i,t_ms,dt_ms,present_ms,overlay_us,cpu,flags`n")
      $t = 0.0
      for ($i = 0; $i -lt 200; $i++) {
        $dt = 4.0 + ($i % 5) * 0.01 + $run * 0.001
        if ($side -eq 'b') { $dt += 0.002 }
        if ($i -eq 100) { $dt = 30.0 }
        $t += $dt
        [void]$sb.Append([string]::Format($inv, "{0},{1:0.000},{2:0.0000},0.2,15,2,0`n", $i, $t, $dt))
      }
      [IO.File]::WriteAllText("$bench\$side\run$run.frames.csv", $sb.ToString())
    }
  }
  $analyzed = (Invoke-Bench 0 bench analyze "$bench\a\run0.frames.csv" --json) | ConvertFrom-Json
  $first = @($analyzed.runs)[0]
  Check ($first.frames -eq 200 -and $first.mean_ms -eq 4.15 -and $first.hitches.count -eq 1 -and $first.low_1.worst_frames -eq 2) `
    "bench analyze $($first | ConvertTo-Json -Compress -Depth 2)"
  $compared = (Invoke-Bench 0 bench compare --a "$bench\a" --b "$bench\b" --json) | ConvertFrom-Json
  Check ($compared.verdict.mean_ms -match '^equivalent' -and $compared.pooled_lows.descriptive_only -eq $true -and
         @($compared.metrics).Count -ge 10) "bench compare $($compared.verdict | ConvertTo-Json -Compress)"
  $comparedText = Invoke-Bench 0 bench compare --a "$bench\a" --b "$bench\b"
  Check ($comparedText -match 'mean_ms\s+equivalent within \+-1\.0%') "bench compare text`n$comparedText"

  Invoke-Bench 3 bench run | Out-Null # needs the game: not built
  Invoke-Bench 1 bench | Out-Null
  Invoke-Bench 1 bench analyze | Out-Null
  Invoke-Bench 1 bench analyze "$runDir\console.txt" | Out-Null # not a frames CSV
  Invoke-Bench 1 bench compare --a "$bench\a" | Out-Null
  Invoke-Bench 1 bench compare --a "$bench\a" --b "$bench\b" --margin-mean x | Out-Null
  Invoke-Bench 1 bench loads --logs $benchLogs --bogus 1 | Out-Null
  Invoke-Bench 1 bench launcher --logs (Join-Path $bench 'missing') | Out-Null
} finally {
  $env:XERCES_HOME = $savedHome
  $env:XERCES_DRY_LAUNCH = $savedDry
}

if ($script:failures.Count) {
  $script:failures | ForEach-Object { Write-Host "FAIL: $_" }
  throw "CLI smoke test (home commands): $($script:failures.Count) failed"
}
Write-Host 'CLI smoke test (home commands): ok'
