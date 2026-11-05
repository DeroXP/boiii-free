# Starts Xerces.exe again and again in dry-launch mode and summarizes how long
# each start took, from the launcher's own log (docs/TESTING.md, Launcher
# speed). Starts nothing but Xerces.exe: --dry-launch is always passed.
#
#   tools\dev\bench\launcher-bench.ps1 -Runs 20 -HomeDir C:\Users\Ginld\XercesHome-bench
#   tools\dev\bench\launcher-bench.ps1 -Exe build\bin\x64\Release\Xerces-old.exe -Label before
#
# Each run is `Xerces.exe --home <home> --dry-launch --skip-boot --view home
# --seconds 2`; one more run without --skip-boot times the boot screen
# (-BootRuns 0 leaves it out). The summary has, per startup mark
# (launcher.startup.step) and per measured event, the median, p90, mean and a
# 95% interval of the mean, and is saved as JSON in -Out.
#
# The home must be outside AppData (CLAUDE.md) and should hold a hash cache,
# or the first run reads the whole Workshop folder: seed it by copying
# <another home>\cache\hashes.txt and config\launcher.json into it. The runs
# write logs and the cache in it.
#
# -FreshScan times one scan with no cache at all, in <HomeDir>-fresh (its
# config is copied from -HomeDir, its cache deleted first). It reads every
# Workshop file (about 60 GB on the test PC), so it never runs by default and
# never right before load benchmarks.
param(
  [int]$Runs = 20,
  [string]$HomeDir = 'C:\Users\Ginld\XercesHome-bench',
  [string]$Exe = '',
  [string]$Out = '',
  [string]$Label = '',
  [int]$BootRuns = 1,
  [double]$Seconds = 2,
  [switch]$FreshScan,
  [int]$FreshTimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Exe) { $Exe = Join-Path $repo 'build\bin\x64\Release\Xerces.exe' }
$Exe = (Resolve-Path $Exe).Path
if ($HomeDir -like "$env:LOCALAPPDATA*" -or $HomeDir -like "$env:APPDATA*") {
  throw "-HomeDir must be outside AppData (CLAUDE.md)"
}
if (-not $Out) { $Out = Join-Path $HomeDir 'bench' }
New-Item -ItemType Directory -Force $Out, $HomeDir | Out-Null
foreach ($name in 'BlackOps3', 'boiii') {
  if (Get-Process -Name $name -ErrorAction SilentlyContinue) {
    throw "$name.exe is running; the numbers would include the game's load"
  }
}
if (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($Exe)) -ErrorAction SilentlyContinue) {
  Write-Warning 'A launcher is already running; a launcher of the same home would take the runs over.'
}
if (-not (Test-Path (Join-Path $HomeDir 'cache\hashes.txt')) -and -not $FreshScan) {
  Write-Warning "$HomeDir has no hash cache: the first run reads the whole Workshop folder."
}

# t for a 95% two-sided interval, by degrees of freedom (1..30; 1.96 above).
$t95 = @(0, 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
  2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
  2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042)

function Summarize([double[]]$values) {
  $n = $values.Count
  if ($n -eq 0) { return $null }
  $sorted = $values | Sort-Object
  $mean = ($values | Measure-Object -Average).Average
  $sd = 0.0
  if ($n -gt 1) {
    $sum = 0.0
    foreach ($v in $values) { $sum += ($v - $mean) * ($v - $mean) }
    $sd = [math]::Sqrt($sum / ($n - 1))
  }
  $t = if ($n -le 1) { 0 } elseif ($n - 1 -le 30) { $t95[$n - 1] } else { 1.96 }
  $median = if ($n % 2) { $sorted[($n - 1) / 2] } else { ($sorted[$n / 2 - 1] + $sorted[$n / 2]) / 2 }
  # Nearest rank.
  $p90 = $sorted[[math]::Max(0, [math]::Ceiling(0.9 * $n) - 1)]
  [pscustomobject]@{
    n = $n; median = [math]::Round($median, 1); p90 = [math]::Round($p90, 1)
    mean = [math]::Round($mean, 1); ci95 = [math]::Round($t * $sd / [math]::Sqrt($n), 1)
    min = [math]::Round($sorted[0], 1); max = [math]::Round($sorted[-1], 1)
  }
}

# One launcher run; returns its events and the wall time.
function Start-Launcher([string]$homeDir, [string[]]$extra, [double]$seconds, [scriptblock]$until) {
  $arguments = @('--home', "`"$homeDir`"", '--dry-launch', '--view', 'home', '--seconds', [string]$seconds) + $extra
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $process = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
  $logs = Join-Path $homeDir 'logs'
  if ($until) {
    while (-not $process.HasExited -and $watch.Elapsed.TotalSeconds -lt $seconds) {
      Start-Sleep -Milliseconds 500
      $log = Get-ChildItem $logs -Filter "*-launcher-$($process.Id).jsonl" -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($log -and (& $until $log.FullName)) { [void]$process.CloseMainWindow(); break }
    }
  }
  if (-not $process.WaitForExit([int](($seconds + 30) * 1000))) {
    $process.Kill()
    throw "Xerces.exe (pid $($process.Id)) did not exit"
  }
  $wall = $watch.Elapsed.TotalMilliseconds
  $log = Get-ChildItem $logs -Filter "*-launcher-$($process.Id).jsonl" | Select-Object -First 1
  if (-not $log) { throw "no log for pid $($process.Id) in $logs" }
  $events = @(Get-Content $log.FullName | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
  [pscustomobject]@{ wall_ms = $wall; events = $events; log = $log.FullName }
}

function Ms-Between($from, $to) {
  ([DateTime]::Parse($to.ts).ToUniversalTime() - [DateTime]::Parse($from.ts).ToUniversalTime()).TotalMilliseconds
}

# Everything a run measured, as name -> value (ms unless the name says so).
function Measures($run) {
  $m = [ordered]@{}
  $start = $run.events | Where-Object { $_.event -eq 'launcher.start' } | Select-Object -First 1
  foreach ($e in $run.events) {
    switch ($e.event) {
      'launcher.startup.step' {
        $step = $e.msg -replace ' at [0-9.]+ ms$', ''
        if (-not $m.Contains("mark: $step")) { $m["mark: $step"] = [double]$e.data.ms }
      }
      'launcher.view.loaded' { $m["load_view: $($e.data.view)"] = [double]$e.data.ms }
      'launcher.view.shown' {
        if ($e.data -and $null -ne $e.data.ms) { $m["show: $($e.msg)"] = [double]$e.data.ms }
        if ($start -and -not $m.Contains("shown after start: $($e.msg)")) {
          $m["shown after start: $($e.msg)"] = Ms-Between $start $e
        }
      }
      'launcher.game.detected' {
        if ($start -and -not $m.Contains('game detected after start')) { $m['game detected after start'] = Ms-Between $start $e }
      }
      'launcher.refresh.timing' {
        foreach ($p in $e.data.PSObject.Properties) {
          if ($p.Value -isnot [bool]) { $m["refresh: $($p.Name)"] = [double]$p.Value }
        }
      }
      'launcher.activity' {
        # With --seconds the loop never waits, so these are upper bounds.
        foreach ($state in 'front', 'behind', 'minimized', 'game_running') {
          $s = $e.data.$state
          if ($s -and $s.seconds -gt 0) { $m["presents per minute: $state"] = [double]$s.per_minute }
        }
        if ($e.data.resolve) { $m['resolve total (ms)'] = [double]$e.data.resolve.total_ms }
      }
      'launcher.services.resolve' {
        if (-not $m.Contains('resolve (first)')) { $m['resolve (first)'] = [double]$e.data.ms }
      }
      'launcher.catalog.scanned' {
        if ($start -and -not $m.Contains('catalog scanned after start')) { $m['catalog scanned after start'] = Ms-Between $start $e }
        $m['scan seconds (x1000)'] = [double]$e.data.seconds * 1000
        $m['scan cache misses (count)'] = [double]$e.data.cache_misses
        $m['scan bytes hashed (MB)'] = [double]$e.data.bytes_hashed / 1e6
        if ($e.data.bytes_hashed -gt 0 -and $null -ne $e.data.per_file_mb_s) {
          $m['scan MB/s per file'] = [double]$e.data.per_file_mb_s
          $m['scan MB/s overall'] = [double]$e.data.overall_mb_s
          $m['scan slowest file (s)'] = [double]$e.data.slowest_seconds
          $m['scan threads (count)'] = [double]$e.data.threads
        }
      }
    }
  }
  $m['wall time incl. --seconds'] = $run.wall_ms
  $m
}

function Report([string]$title, $runs) {
  $names = New-Object System.Collections.ArrayList
  foreach ($r in $runs) { foreach ($k in $r.Keys) { if (-not $names.Contains($k)) { [void]$names.Add($k) } } }
  $rows = foreach ($name in $names) {
    $values = [double[]]@($runs | Where-Object { $_.Contains($name) } | ForEach-Object { $_[$name] })
    $s = Summarize $values
    if ($s) { [pscustomobject][ordered]@{ measure = $name; n = $s.n; median = $s.median; p90 = $s.p90; mean = $s.mean; ci95 = $s.ci95; min = $s.min; max = $s.max } }
  }
  Write-Host "== $title"
  Write-Host ($rows | Format-Table -AutoSize | Out-String -Width 200)
  $rows
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$summary = [ordered]@{ label = $Label; exe = $Exe; home = $HomeDir; runs = $Runs; started = $stamp }

if ($FreshScan) {
  $fresh = "$HomeDir-fresh"
  New-Item -ItemType Directory -Force (Join-Path $fresh 'config') | Out-Null
  Remove-Item -Force (Join-Path $fresh 'cache\hashes.txt') -ErrorAction SilentlyContinue
  Copy-Item (Join-Path $HomeDir 'config\launcher.json') (Join-Path $fresh 'config\launcher.json') -ErrorAction SilentlyContinue
  "Fresh scan in $fresh (reads every Workshop file)..."
  $run = Start-Launcher $fresh @('--skip-boot') $FreshTimeoutSeconds {
    param($path) (Select-String -Path $path -Pattern '"launcher.catalog.scanned"' -SimpleMatch -Quiet)
  }
  $m = Measures $run
  $m.GetEnumerator() | Where-Object { $_.Key -like 'scan*' -or $_.Key -like 'catalog*' -or $_.Key -like 'refresh*' } |
    ForEach-Object { '{0,-40} {1,12:N1}' -f $_.Key, $_.Value }
  $scanned = $run.events | Where-Object { $_.event -eq 'launcher.catalog.scanned' } | Select-Object -First 1
  if ($scanned) { 'launcher.catalog.scanned: ' + ($scanned.data | ConvertTo-Json -Compress -Depth 5) }
  $summary.fresh_scan = $m
}

if ($Runs -gt 0) {
  $measured = @()
  for ($i = 1; $i -le $Runs; $i++) {
    $run = Start-Launcher $HomeDir @('--skip-boot') $Seconds $null
    $measured += , (Measures $run)
    Write-Progress -Activity 'launcher-bench' -Status "run $i of $Runs" -PercentComplete (100 * $i / $Runs)
  }
  Write-Progress -Activity 'launcher-bench' -Completed
  $summary.skip_boot = Report "$Runs starts with --skip-boot" $measured
}

if ($BootRuns -gt 0) {
  $boot = @()
  for ($i = 1; $i -le $BootRuns; $i++) {
    $run = Start-Launcher $HomeDir @() ([math]::Max($Seconds, 3)) $null
    $boot += , (Measures $run)
  }
  $summary.boot = Report "$BootRuns start(s) through the boot screen" $boot
}

$file = Join-Path $Out "launcher-bench-$stamp$(if ($Label) { "-$Label" }).json"
[IO.File]::WriteAllText($file, ($summary | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding($false)))
"Saved $file"
