# Saves a picture for the test record.
#   capture.ps1 -Path game.png            the game window, even when covered
#                                         (PrintWindow, PW_RENDERFULLCONTENT)
#   capture.ps1 -Path screen.png -Screen  the whole desktop; use this while a
#                                         video plays, which PrintWindow shows
#                                         as black
# Read-only: no input is sent.
param(
  [Parameter(Mandatory = $true)][string]$Path,
  [switch]$Screen,
  [double]$Scale = 0.5
)

. "$PSScriptRoot\xerces-win32.ps1"
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
if (-not ("XercesCapture" -as [type])) {
  Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class XercesCapture {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
}
"@
}

if ($Screen) {
  $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
  $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bitmap.Size)
} else {
  $game = Get-XercesGame
  if (-not $game) { "boiii is not running"; exit 1 }
  $window = Find-XercesWindow $game "CoDBlackOps"
  if (-not $window) { "no game window"; exit 1 }
  $rect = New-Object XercesCapture+RECT
  [void][XercesCapture]::GetWindowRect($window, [ref]$rect)
  $bitmap = New-Object System.Drawing.Bitmap ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $hdc = $graphics.GetHdc()
  [void][XercesCapture]::PrintWindow($window, $hdc, 2)
  $graphics.ReleaseHdc($hdc)
}
$small = New-Object System.Drawing.Bitmap $bitmap, ([int]($bitmap.Width * $Scale)), ([int]($bitmap.Height * $Scale))
$small.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Path ($($bitmap.Width)x$($bitmap.Height) x $Scale)"
$graphics.Dispose(); $bitmap.Dispose(); $small.Dispose()
