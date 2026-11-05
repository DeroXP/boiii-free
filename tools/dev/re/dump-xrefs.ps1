# Offline helpers for a dump made by dump-module.ps1 (laid out by RVA).
#   -Callers 0x1412D8830,...  every E8 (call) and E9 (jmp) rel32 in executable
#                             sections whose target is one of these addresses.
#   -Fix                      writes <dump>.pe: a copy whose section table
#                             points raw offsets at the RVAs and whose ImageBase
#                             is -PreferredBase, so llvm-objdump disassembles it
#                             by game address:
#     llvm-objdump -d --no-show-raw-insn --start-address=0x1412D7B40 --stop-address=0x1412D7B70 <dump>.pe
# Addresses are the ones upstream's symbol tables use (preferred base
# 0x140000000). A byte scan can report a false E8/E9 inside another
# instruction, so confirm each hit by disassembling it.
param(
  [Parameter(Mandatory = $true)][string]$Dump,
  [string[]]$Callers = @(),
  [switch]$Fix,
  [UInt64]$PreferredBase = 0x140000000
)

if (-not ("XercesRe" -as [type])) {
  Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
public static class XercesRe {
  public struct Section { public string Name; public uint VirtualSize, VirtualAddress, Characteristics; public int HeaderOffset; }

  public static List<Section> Sections(byte[] img) {
    var list = new List<Section>();
    if (img[0] != 'M' || img[1] != 'Z') throw new Exception("no MZ header at offset 0");
    int nt = BitConverter.ToInt32(img, 0x3C);
    if (BitConverter.ToUInt32(img, nt) != 0x00004550) throw new Exception("no PE signature");
    int count = BitConverter.ToUInt16(img, nt + 6);
    int optSize = BitConverter.ToUInt16(img, nt + 20);
    int table = nt + 24 + optSize;
    for (int i = 0; i < count; ++i) {
      int o = table + i * 40;
      var s = new Section();
      s.Name = System.Text.Encoding.ASCII.GetString(img, o, 8).TrimEnd('\0');
      s.VirtualSize = BitConverter.ToUInt32(img, o + 8);
      s.VirtualAddress = BitConverter.ToUInt32(img, o + 12);
      s.Characteristics = BitConverter.ToUInt32(img, o + 36);
      s.HeaderOffset = o;
      list.Add(s);
    }
    return list;
  }

  public static void Fix(byte[] img, ulong preferredBase) {
    int nt = BitConverter.ToInt32(img, 0x3C);
    Array.Copy(BitConverter.GetBytes(preferredBase), 0, img, nt + 24 + 24, 8);  // OptionalHeader64.ImageBase
    foreach (var s in Sections(img)) {
      uint size = Math.Min(s.VirtualSize, (uint)img.Length - Math.Min(s.VirtualAddress, (uint)img.Length));
      Array.Copy(BitConverter.GetBytes(size), 0, img, s.HeaderOffset + 16, 4);              // SizeOfRawData
      Array.Copy(BitConverter.GetBytes(s.VirtualAddress), 0, img, s.HeaderOffset + 20, 4);  // PointerToRawData
    }
  }

  public static List<string> FindCallers(byte[] img, ulong preferredBase, ulong[] targets) {
    var result = new List<string>();
    var wanted = new HashSet<long>();
    foreach (var t in targets) wanted.Add((long)(t - preferredBase));
    foreach (var s in Sections(img)) {
      if ((s.Characteristics & 0x20000000) == 0) continue;  // IMAGE_SCN_MEM_EXECUTE
      long start = s.VirtualAddress, end = Math.Min((long)img.Length - 5, (long)s.VirtualAddress + s.VirtualSize);
      for (long i = start; i < end; ++i) {
        byte op = img[i];
        if (op != 0xE8 && op != 0xE9) continue;
        long target = i + 5 + BitConverter.ToInt32(img, (int)i + 1);
        if (wanted.Contains(target)) {
          result.Add(string.Format("{0} 0x{1:X} -> 0x{2:X} ({3})", op == 0xE8 ? "call" : "jmp ", (ulong)i + preferredBase, (ulong)target + preferredBase, s.Name));
        }
      }
    }
    return result;
  }
}
"@
}

$bytes = [IO.File]::ReadAllBytes($Dump)
"sections:"
foreach ($s in [XercesRe]::Sections($bytes)) {
  "  {0,-8} rva 0x{1:X8} size 0x{2:X8} flags 0x{3:X8}" -f $s.Name, $s.VirtualAddress, $s.VirtualSize, $s.Characteristics
}

if ($Callers.Count -gt 0) {
  # PowerShell turns an unquoted 0x... argument into a decimal string; accept both.
  $targets = [UInt64[]]($Callers | ForEach-Object {
      if ($_ -match "^0x") { [Convert]::ToUInt64($_.Substring(2), 16) } else { [UInt64]::Parse($_) } })
  "callers of $(($targets | ForEach-Object { '0x{0:X}' -f $_ }) -join ', '):"
  [XercesRe]::FindCallers($bytes, $PreferredBase, $targets) | ForEach-Object { "  $_" }
}

if ($Fix) {
  [XercesRe]::Fix($bytes, $PreferredBase)
  [IO.File]::WriteAllBytes("$Dump.pe", $bytes)
  "wrote $Dump.pe"
}
