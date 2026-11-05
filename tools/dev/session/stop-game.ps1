# Quits the running game the way closing its console does: WM_CLOSE to the
# BOIII console window runs "quit" (console.cpp; the client forces the exit
# after 3 s). Kills the process only if it is still there after -WaitSeconds.
param([int]$WaitSeconds = 30)

. "$PSScriptRoot\xerces-win32.ps1"

$game = Get-XercesGame
if (-not $game) { "boiii is not running"; exit 0 }
$console = Find-XercesWindow $game "BOIII WinConsole"
if (-not $console) { "no console window"; exit 1 }
[void][XercesWin]::PostMessage($console, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
if ($game.WaitForExit($WaitSeconds * 1000)) {
  "boiii quit"
} else {
  Stop-Process -Id $game.Id -Force
  "boiii was still running after $WaitSeconds s; stopped it"
}
