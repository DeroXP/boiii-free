# Posts one key press (WM_KEYDOWN + WM_KEYUP, with the scan code in lParam the
# way the engine reads it) to the game window only. Whatever else has focus,
# system dialogs included, never receives the key.
#
#   send-key.ps1 Return      send-key.ps1 Down      send-key.ps1 Escape
param(
  [Parameter(Mandatory = $true, Position = 0)][string]$Key  # a System.Windows.Forms.Keys name
)

. "$PSScriptRoot\xerces-win32.ps1"
Add-Type -AssemblyName System.Windows.Forms

$game = Get-XercesGame
if (-not $game) { "boiii is not running"; exit 1 }
$window = Find-XercesWindow $game "CoDBlackOps"
if (-not $window) { "no game window"; exit 1 }

$vk = [uint32][System.Windows.Forms.Keys]::$Key
if (-not $vk) { "unknown key name $Key"; exit 1 }
$scan = [XercesWin]::MapVirtualKey($vk, 0)
# Arrow and navigation keys are extended keys (E0 prefix); without bit 24 the
# engine reads their scan codes as the numeric keypad, which menus ignore.
$extendedKeys = "Up", "Down", "Left", "Right", "Insert", "Delete", "Home", "End", "PageUp", "PageDown", "Next", "Prior"
$extended = if ($extendedKeys -contains $Key) { 1 -shl 24 } else { 0 }
$down = [IntPtr](1 -bor ($scan -shl 16) -bor $extended)
$up = [IntPtr]([int64](1 -bor ($scan -shl 16) -bor $extended -bor (1 -shl 30)) -bor 0x80000000L)
[void][XercesWin]::PostMessage($window, 0x100, [IntPtr]$vk, $down)  # WM_KEYDOWN
Start-Sleep -Milliseconds 80
[void][XercesWin]::PostMessage($window, 0x101, [IntPtr]$vk, $up)    # WM_KEYUP
"posted $Key (vk 0x{0:X2}, scan 0x{1:X2}) to the game window" -f $vk, $scan
