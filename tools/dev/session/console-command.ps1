# Runs one console command in the running game, the way typing it into the
# BOIII console does (WM_SETTEXT, then Enter, both to the console's input line
# only), and prints the console lines that appeared within -WaitMs. Meant for
# queries such as "fs_extramods" and for test steps such as "disconnect".
param(
  [Parameter(Mandatory = $true)][string]$Command,
  [int]$WaitMs = 800
)

. "$PSScriptRoot\xerces-win32.ps1"

$game = Get-XercesGame
if (-not $game) { "boiii is not running"; exit 1 }
$console = Find-XercesWindow $game "BOIII WinConsole"
if (-not $console) { "no console window"; exit 1 }
$controls = Get-XercesConsoleControls $console
if (-not $controls.Input -or -not $controls.Buffer) { "console input line or buffer not found"; exit 1 }

$before = ([XercesWin]::ReadText($controls.Buffer) -split "\r\n|\r|\n").Count
$result = [IntPtr]::Zero
[void][XercesWin]::SendMessageTimeoutString($controls.Input, 0x0C, [IntPtr]::Zero, $Command, 2, 5000, [ref]$result)  # WM_SETTEXT
[void][XercesWin]::PostMessage($controls.Input, 0x102, [IntPtr]13, [IntPtr]1)  # WM_CHAR Enter
Start-Sleep -Milliseconds $WaitMs
$lines = [XercesWin]::ReadText($controls.Buffer) -split "\r\n|\r|\n"
$lines | Select-Object -Skip ([Math]::Max(0, $before - 1))
