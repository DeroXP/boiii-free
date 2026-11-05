# Reads the BOIII console window's text from a running boiii.exe (WM_GETTEXT
# only; nothing is sent to the game). Saves the whole buffer to -Out and prints
# the last -Tail lines, or the lines matching -Pattern.
param(
  [string]$Out = "",
  [int]$Tail = 40,
  [string]$Pattern = ""
)

. "$PSScriptRoot\xerces-win32.ps1"

$game = Get-XercesGame
if (-not $game) { "boiii is not running"; exit 1 }
$console = Find-XercesWindow $game "BOIII WinConsole"
if (-not $console) { "no console window"; exit 1 }
$buffer = (Get-XercesConsoleControls $console).Buffer
if (-not $buffer) { "no console buffer"; exit 1 }

$lines = [XercesWin]::ReadText($buffer) -split "\r\n|\r|\n"
if ($Out) {
  [System.IO.File]::WriteAllLines($Out, $lines, (New-Object System.Text.UTF8Encoding $false))
  "saved $($lines.Count) lines to $Out"
}
if ($Pattern) {
  $lines | Select-String -Pattern $Pattern | ForEach-Object { $_.Line }
} elseif ($Tail -gt 0) {
  $lines | Select-Object -Last $Tail
}
