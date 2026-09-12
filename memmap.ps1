# How much of the running game's memory is above the 4 GB line.
#
# This is the number that says whether a build is really using 64-bit memory. The launcher's own
# counter of intercepted reservations does not answer it: most of the game's memory arrives by
# other routes - the heap growing inside a region it reserved earlier, file mappings for the .pak
# archives, allocations the graphics driver makes on the process's behalf.
#
# Usage:  .\memmap.ps1            (the running launcher64)
#         .\memmap.ps1 -Detail    (also the largest regions, to see what lives where)
param([switch]$Detail)

$src = @"
using System;
using System.Runtime.InteropServices;
public class VMQuery {
  [StructLayout(LayoutKind.Sequential)]
  public struct MBI {
    public IntPtr BaseAddress, AllocationBase;
    public uint AllocationProtect;
    public IntPtr RegionSize;
    public uint State, Protect, Type;
  }
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll")] public static extern int VirtualQueryEx(IntPtr h, IntPtr addr, out MBI m, int len);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@
if (-not ("VMQuery" -as [type])) { Add-Type -TypeDefinition $src }

$p = Get-Process launcher64 -ErrorAction SilentlyContinue
if (-not $p) { "launcher64 is not running"; return }

# PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
$h = [VMQuery]::OpenProcess(0x0410, $false, $p.Id)
if ($h -eq [IntPtr]::Zero) { "could not open the process (try running as the same user)"; return }

$MEM_COMMIT = 0x1000
$MEM_IMAGE  = 0x1000000
$MEM_MAPPED = 0x40000

$at = [long]0x10000
$low = [long]0; $high = [long]0; $image = [long]0; $mapped = [long]0
$regions = @()
$steps = 0

while ($at -lt 0x7FFFFFFF0000 -and $steps -lt 300000) {
    $m = New-Object VMQuery+MBI
    if ([VMQuery]::VirtualQueryEx($h, [IntPtr]$at, [ref]$m, 48) -eq 0) { break }
    $size = [long]$m.RegionSize
    if ($size -le 0) { break }

    if ($m.State -eq $MEM_COMMIT) {
        if     ($m.Type -eq $MEM_IMAGE)  { $image  += $size }
        elseif ($m.Type -eq $MEM_MAPPED) { $mapped += $size }
        elseif ($at -ge 0x100000000)     { $high   += $size }
        else                             { $low    += $size }

        if ($Detail -and $size -ge 16MB) {
            $regions += [PSCustomObject]@{
                Address = "0x{0:X}" -f $at
                MB      = [int]($size / 1MB)
                Where   = if ($at -ge 0x100000000) { "above 4 GB" } else { "below" }
                Type    = switch ($m.Type) { $MEM_IMAGE { "image" } $MEM_MAPPED { "mapped" } default { "private" } }
            }
        }
    }
    $at += $size
    $steps++
}
[void][VMQuery]::CloseHandle($h)

$total = $low + $high
"pid {0}, working set {1} MB" -f $p.Id, [int]($p.WorkingSet64 / 1MB)
"private below 4 GB : {0,6} MB" -f [int]($low / 1MB)
"private above 4 GB : {0,6} MB   <- the point of all this" -f [int]($high / 1MB)
if ($total -gt 0) { "                     {0,6}% of private memory is high" -f [int](100 * $high / $total) }
"module images      : {0,6} MB" -f [int]($image / 1MB)
"file mappings      : {0,6} MB" -f [int]($mapped / 1MB)

if ($Detail -and $regions.Count) {
    ""
    "regions of 16 MB or more:"
    $regions | Sort-Object MB -Descending | Select-Object -First 20 | Format-Table -AutoSize | Out-String
}
