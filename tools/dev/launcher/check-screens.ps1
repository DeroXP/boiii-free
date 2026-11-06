# Opens every launcher screen in a dry-launch run, captures it as a PNG and
# lists the RmlUi warnings and errors each run logged (docs/TESTING.md,
# Launcher). Starts nothing but Xerces.exe: --dry-launch is always passed.
# Every screen is one document in data\xerces\ui\views, so the default -Views
# is read from that folder and a new screen is checked without editing this
# file (boot is not in it; the runs pass --skip-boot).
#
#   tools\dev\launcher\check-screens.ps1 -Out C:\Users\Ginld\XercesDev\launcher-checks\all
#   tools\dev\launcher\check-screens.ps1 -Theme light -Language pseudo
#   tools\dev\launcher\check-screens.ps1 -Scale 1.5 -Exe build\bin\x64\Release\Xerces-old.exe
#
# -HomeDir is the XERCES_HOME for the runs (outside AppData on this PC; the
# Workshop hash cache in it makes the catalog scan fast after the first run).
# -Scale fixes the UI scale (1 is 100 percent; by default Windows' scale and the
# config's). -Exe runs another build, such as the previous one kept next to the
# new one for before-and-after captures.
param(
  [string]$Out = (Join-Path $env:TEMP 'xerces-screens'),
  [string]$HomeDir = 'C:\Users\Ginld\XercesHome',
  [string[]]$Views = @(),
  [string]$Theme = '',
  [string]$Language = '',
  [double]$Scale = 0,
  [string]$Exe = '',
  [int]$WaitMs = 4000
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Exe) { $Exe = Join-Path $repo 'build\bin\x64\Release\Xerces.exe' }
if (-not (Test-Path $Exe)) { throw "no launcher build at $Exe (tools\build.ps1 -Launcher)" }
$Exe = (Resolve-Path $Exe).Path
if (-not $Views) {
  $Views = @(Get-ChildItem (Join-Path $repo 'data\xerces\ui\views') -Filter '*.rml' |
    Sort-Object Name | ForEach-Object { $_.BaseName })
}
New-Item -ItemType Directory -Force $Out, $HomeDir | Out-Null
$env:XERCES_HOME = $HomeDir

$problems = 0
foreach ($view in $Views) {
  $shot = Join-Path $Out "$view.png"
  $script = Join-Path $Out "$view.steps.txt"
  [IO.File]::WriteAllText($script, "+$WaitMs shot $shot`n+300 quit`n")
  $arguments = @('--dry-launch', '--skip-boot', '--view', $view, '--script', $script, '--seconds', [string](($WaitMs / 1000) + 15))
  if ($Theme) { $arguments += @('--theme', $Theme) }
  if ($Language) { $arguments += @('--language', $Language) }
  if ($Scale -gt 0) { $arguments += @('--scale', $Scale.ToString([Globalization.CultureInfo]::InvariantCulture)) }
  $started = Get-Date
  $process = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(($WaitMs + 30000))) { $process.Kill(); throw "$view did not exit" }
  $log = Get-ChildItem (Join-Path $HomeDir 'logs') -Filter '*launcher*.jsonl' |
    Where-Object { $_.LastWriteTime -ge $started.AddSeconds(-1) } |
    Sort-Object LastWriteTime | Select-Object -Last 1
  $warnings = @()
  if ($log) {
    $warnings = Get-Content $log.FullName | ForEach-Object { $_ | ConvertFrom-Json } |
      Where-Object { $_.event -like 'launcher.rmlui.*' -or $_.level -eq 'error' }
  }
  $state = if (Test-Path $shot) { 'captured' } else { 'NO CAPTURE' }
  "{0,-12} {1}, {2} warnings or errors" -f $view, $state, $warnings.Count
  foreach ($w in $warnings) { "    {0} {1}" -f $w.event, $w.msg; $problems++ }
  if (-not (Test-Path $shot)) { $problems++ }
}
"Captures in $Out"
exit ([int]($problems -gt 0))
