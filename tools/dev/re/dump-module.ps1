# Saves the in-memory image of one module of the running boiii.exe (default
# BlackOps3.exe, whose code is only readable in memory: docs/DESIGN.md 2.3,
# row 8) as a flat file laid out by RVA, plus a JSON sidecar with the base, the
# size, the file hashes and every region that was readable.
#
# Read-only: ReadProcessMemory only, nothing is written to the game. A dump is
# game code: it stays on this PC, outside the repository, OneDrive and AppData
# (DESIGN.md 3.5), and is never committed or shared.
param(
  [string]$Module = "BlackOps3.exe",
  [string]$OutDir = (Join-Path $env:USERPROFILE "XercesDev\dumps")
)

$full = [IO.Path]::GetFullPath($OutDir)
if ($full -match "\\OneDrive" -or ($env:OneDrive -and $full.StartsWith($env:OneDrive, [StringComparison]::OrdinalIgnoreCase))) {
  throw "refusing to write a dump under OneDrive: $full"
}
if ($full.StartsWith($env:APPDATA, [StringComparison]::OrdinalIgnoreCase) -or
  $full.StartsWith($env:LOCALAPPDATA, [StringComparison]::OrdinalIgnoreCase)) {
  throw "refusing to write a dump under AppData: $full"
}
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
if ($full.StartsWith($repo, [StringComparison]::OrdinalIgnoreCase)) {
  throw "refusing to write a dump inside the repository: $full"
}

if (-not ("XercesDump" -as [type])) {
  Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class XercesDump {
  [StructLayout(LayoutKind.Sequential)]
  public struct MEMORY_BASIC_INFORMATION {
    public IntPtr BaseAddress; public IntPtr AllocationBase; public uint AllocationProtect;
    public ushort PartitionId; public IntPtr RegionSize; public uint State; public uint Protect; public uint Type;
  }
  [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr VirtualQueryEx(IntPtr h, IntPtr addr, out MEMORY_BASIC_INFORMATION info, IntPtr size);
  [DllImport("kernel32.dll", SetLastError = true)] public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buffer, IntPtr size, out IntPtr read);

  // Returns "rva,size,protect" for each region copied.
  public static List<string> Dump(int pid, long baseAddr, long size, string path) {
    const uint PROCESS_VM_READ = 0x10, PROCESS_QUERY_INFORMATION = 0x400;
    const uint MEM_COMMIT = 0x1000, PAGE_NOACCESS = 0x01, PAGE_GUARD = 0x100;
    var regions = new List<string>();
    IntPtr h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, false, pid);
    if (h == IntPtr.Zero) throw new Exception("OpenProcess failed: " + Marshal.GetLastWin32Error());
    try {
      using (var fs = new FileStream(path, FileMode.Create, FileAccess.Write)) {
        fs.SetLength(size);
        long addr = baseAddr, end = baseAddr + size;
        var buffer = new byte[1 << 20];
        while (addr < end) {
          MEMORY_BASIC_INFORMATION mbi;
          if (VirtualQueryEx(h, new IntPtr(addr), out mbi, new IntPtr(Marshal.SizeOf(typeof(MEMORY_BASIC_INFORMATION)))) == IntPtr.Zero) break;
          long regionStart = Math.Max(addr, mbi.BaseAddress.ToInt64());
          long regionEnd = Math.Min(end, mbi.BaseAddress.ToInt64() + mbi.RegionSize.ToInt64());
          bool readable = mbi.State == MEM_COMMIT && mbi.Protect != 0 && (mbi.Protect & PAGE_NOACCESS) == 0 && (mbi.Protect & PAGE_GUARD) == 0;
          if (readable) {
            for (long p = regionStart; p < regionEnd; p += buffer.Length) {
              int want = (int)Math.Min(buffer.Length, regionEnd - p);
              IntPtr got;
              if (ReadProcessMemory(h, new IntPtr(p), buffer, new IntPtr(want), out got) && got.ToInt64() > 0) {
                fs.Seek(p - baseAddr, SeekOrigin.Begin);
                fs.Write(buffer, 0, (int)got.ToInt64());
              }
            }
            regions.Add(string.Format("0x{0:X},0x{1:X},0x{2:X}", regionStart - baseAddr, regionEnd - regionStart, mbi.Protect));
          }
          addr = regionEnd;
        }
      }
    } finally { CloseHandle(h); }
    return regions;
  }
}
"@
}

$proc = Get-Process boiii -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { "boiii is not running"; exit 1 }
$mod = $proc.Modules | Where-Object { $_.ModuleName -eq $Module } | Select-Object -First 1
if (-not $mod) { "module $Module not found"; exit 1 }

New-Item -ItemType Directory -Force $full | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$name = [IO.Path]::GetFileNameWithoutExtension($Module)
$bin = Join-Path $full "$name-$stamp.bin"
$base = [int64]$mod.BaseAddress
$size = [int64]$mod.ModuleMemorySize
$regions = [XercesDump]::Dump($proc.Id, $base, $size, $bin)

$meta = [ordered]@{
  module      = $Module
  file        = $mod.FileName
  file_size   = (Get-Item $mod.FileName).Length
  file_sha256 = (Get-FileHash $mod.FileName -Algorithm SHA256).Hash.ToLower()
  base        = ('0x{0:X}' -f $base)
  size        = ('0x{0:X}' -f $size)
  dump        = (Split-Path $bin -Leaf)
  dump_sha256 = (Get-FileHash $bin -Algorithm SHA256).Hash.ToLower()
  taken       = (Get-Date -Format "yyyy-MM-ddTHH:mm:ssK")
  boiii       = $proc.Path
  regions     = $regions
}
$meta | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 ($bin -replace "\.bin$", ".json")
"saved $bin ($([math]::Round($size / 1MB, 1)) MB, $($regions.Count) readable regions)"
