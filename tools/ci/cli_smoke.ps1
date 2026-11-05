<#
.SYNOPSIS
  CLI smoke test for the commands that work on <home> as Xerces.exe does
  (docs/DESIGN.md 15.2): mods list, info, enable, disable and order, plan
  apply --dry-launch, packs apply, logs tail and sig check.

.DESCRIPTION
  Runs xerces_cli against a fresh home and a copy of a fake game folder (a
  two-byte BlackOps3.exe and two mods that declare a conflict). The fake game
  comes from -FakeGame, else XERCES_FAKE_GAME, else
  C:\Users\Ginld\XercesDev\fake-game when it exists; without one, or with
  -Generate, the same layout is written here (CI). Everything goes under -WorkDir (default
  build\cli-smoke), outside AppData.

  It never starts the game: plan apply always runs with --dry-launch, and the
  fake BlackOps3.exe fails the pre-flight check, so even a real launch would
  be refused. Every call passes --home; XERCES_HOME is set too and restored.
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
  [IO.File]::WriteAllBytes("$game\BlackOps3.exe", [byte[]](0x4D, 0x5A))
  Set-Content -NoNewline -Encoding ascii "$game\mods\conflict_a\xerces.mod.json" `
    '{"schema":1,"id":"test.conflict_a","name":"Conflict A","version":"1.0.0","license":"MIT","conflicts":[{"id":"test.conflict_b","reason":"Both rewrite the box"}]}'
  Set-Content -NoNewline -Encoding ascii "$game\mods\conflict_b\xerces.mod.json" `
    '{"schema":1,"id":"test.conflict_b","name":"Conflict B","version":"1.0.0","license":"MIT"}'
  New-Item -ItemType File "$game\mods\conflict_a\zone\core_mod.ff", "$game\mods\conflict_b\zone\core_mod.ff" | Out-Null
}
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
  $out = & $Cli @rest --home $homeDir 2>&1 | ForEach-Object { "$_" }
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
  $out = & $Cli @rest --home $homeDir 2>$null
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

$savedHome = $env:XERCES_HOME
$env:XERCES_HOME = $homeDir
try {
  # ------------------------------------------------------- mods: the catalog
  $list = Invoke-Xcli 0 mods list
  Check ($list -match 'test\.conflict_a' -and $list -match 'test\.conflict_b') "mods list shows both fake mods`n$list"

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
  Check ($dry -match 'a real launch would stop') "the dry launch says what a real one would refuse`n$dry"
  $plans = @(Get-ChildItem "$homeDir\config\plans" -Filter '*.json')
  Check ($plans.Count -eq 1) "one plan file, found $($plans.Count)"
  Check (-not (Test-Path "$homeDir\logs\journal.json")) 'a dry launch writes no journal'
  $json = Invoke-XcliJson 0 plan apply --dry-launch --json
  Check ($json.dry_run -and -not $json.started -and $json.command_line -match 'fs_game conflict_a') "plan apply --json`n$($json | ConvertTo-Json)"
  $vanilla = Invoke-Xcli 0 plan apply --vanilla --dry-launch
  Check ($vanilla -match 'command: -launch -nointro(\r?\n|$)') "vanilla dry launch`n$vanilla"

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
  Check ((ZmProfile).pack_name -eq 'Smoke pack') 'packs apply by library id'
  Invoke-Xcli 2 packs apply 'no-such-pack' | Out-Null

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
  $tail = Invoke-Xcli 0 logs tail --role cli --lines 100
  foreach ($event in 'cli.launch.dry', 'cli.launch.refused', 'cli.mods.edit', 'cli.packs.backup', 'cli.packs.applied') {
    Check ($tail -match [regex]::Escape($event)) "logs tail shows $event`n$tail"
  }
  Check (@(Get-ChildItem "$homeDir\logs" -Filter '*-cli-*.jsonl').Count -eq 1) 'the CLI keeps one log per day'
  Invoke-Xcli 1 logs tail --role game | Out-Null

  Invoke-Xcli 1 sig check | Out-Null # no report yet
  $report = '{"schema":"xerces.sigreport/1","game_build":"0x6531394","checked":"2026-11-05T03:00:00Z","entries":[{"id":"a","name":"A","resolved":true,"rva":"0x1000"},{"id":"b","name":"B","resolved":false,"error":"no match"}],"disabled":{"multi_mod":["b"]}}'
  New-Item -ItemType Directory -Force "$homeDir\cache" | Out-Null
  [IO.File]::WriteAllText("$homeDir\cache\signatures-report.json", $report)
  $sig = Invoke-Xcli 4 sig check
  Check ($sig -match 'MISS\s+b' -and $sig -match 'multi_mod') "sig check names the miss`n$sig"
  [IO.File]::WriteAllText("$homeDir\cache\signatures-report.json", $report.Replace('"resolved":false', '"resolved":true').Replace('"multi_mod":["b"]', ''))
  Invoke-Xcli 0 sig check | Out-Null
} finally {
  $env:XERCES_HOME = $savedHome
}

if ($script:failures.Count) {
  $script:failures | ForEach-Object { Write-Host "FAIL: $_" }
  throw "CLI smoke test (home commands): $($script:failures.Count) failed"
}
Write-Host 'CLI smoke test (home commands): ok'
