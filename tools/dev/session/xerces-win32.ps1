# Win32 helpers shared by the session scripts: find boiii.exe's windows and
# read their text. Dot-source this file; it only defines the XercesWin type.
if (-not ("XercesWin" -as [type])) {
  Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class XercesWin {
  public delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lparam);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lparam);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd, StringBuilder sb, int max);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder sb, int max);
  [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageTimeoutW")]
  public static extern IntPtr SendMessageTimeoutText(IntPtr hwnd, uint msg, IntPtr wparam, StringBuilder lparam, uint flags, uint timeout, out IntPtr result);
  [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageTimeoutW")]
  public static extern IntPtr SendMessageTimeoutString(IntPtr hwnd, uint msg, IntPtr wparam, string lparam, uint flags, uint timeout, out IntPtr result);
  [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")]
  public static extern IntPtr SendMessageTimeoutPtr(IntPtr hwnd, uint msg, IntPtr wparam, IntPtr lparam, uint flags, uint timeout, out IntPtr result);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wparam, IntPtr lparam);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);

  public static string ClassOf(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
  public static string TitleOf(IntPtr h) { var sb = new StringBuilder(512); GetWindowText(h, sb, 512); return sb.ToString(); }

  public static List<IntPtr> TopWindows(uint pid) {
    var list = new List<IntPtr>();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) list.Add(h); return true; }, IntPtr.Zero);
    return list;
  }
  public static List<IntPtr> Children(IntPtr parent) {
    var list = new List<IntPtr>();
    EnumChildWindows(parent, (h, l) => { list.Add(h); return true; }, IntPtr.Zero);
    return list;
  }
  public static string ReadText(IntPtr h) {
    IntPtr len;
    // WM_GETTEXTLENGTH = 0x0E, WM_GETTEXT = 0x0D, SMTO_ABORTIFHUNG = 2
    SendMessageTimeoutPtr(h, 0x0E, IntPtr.Zero, IntPtr.Zero, 2, 5000, out len);
    int n = len.ToInt32();
    if (n <= 0) return "";
    var sb = new StringBuilder(n + 1);
    IntPtr got;
    SendMessageTimeoutText(h, 0x0D, new IntPtr(n + 1), sb, 2, 10000, out got);
    return sb.ToString();
  }
}
"@
}

# The running boiii.exe, or $null.
function Get-XercesGame {
  Get-Process boiii -ErrorAction SilentlyContinue | Select-Object -First 1
}

# The first visible top-level window of the game with this class
# ("CoDBlackOps" for the game, "BOIII WinConsole" for the console), or $null.
function Find-XercesWindow([System.Diagnostics.Process]$Game, [string]$Class) {
  foreach ($h in [XercesWin]::TopWindows([uint32]$Game.Id)) {
    if ([XercesWin]::ClassOf($h) -eq $Class) { return $h }
  }
  return $null
}

# The console's child controls: Buffer (RICHEDIT50W) and Input (edit, id 0x65).
function Get-XercesConsoleControls([IntPtr]$Console) {
  $controls = @{ Buffer = $null; Input = $null }
  foreach ($c in [XercesWin]::Children($Console)) {
    $cls = [XercesWin]::ClassOf($c)
    if ($cls -eq "RICHEDIT50W") { $controls.Buffer = $c }
    if ($cls -eq "Edit" -and [XercesWin]::GetDlgCtrlID($c) -eq 0x65) { $controls.Input = $c }
  }
  return $controls
}
