# Plays the campaign without a human and says what happened.
#
# Catching crashes by hand costs an evening per crash: the game has to be played until it dies,
# and the interesting ones only show up after an hour. This runs the levels on its own, moves the
# player so the AI and the renderer are not idling at a spawn point, and collects the launcher's
# own counters per level into one table.
#
# It never deletes a log. An earlier script removed Game.log before each run "to keep it clean"
# and threw away the record of a full campaign playthrough - the one thing that could not be
# reproduced. Everything here reads from a remembered offset instead, and keeps a copy per level.
param(
    [string[]]$Levels = @(),                  # default: the campaign, in order
    [int]$Sec         = 120,                  # seconds of play per level, after it has loaded
    [int]$LoadTimeout = 240,                  # seconds to wait for a level to finish loading
    [string]$Extra    = "",                   # extra launcher flags
    [switch]$Move,                            # drive the player: walk, look, shoot
                                              # (off by default - the run must not steal the
                                              #  keyboard, mouse or focus while someone plays)
    [switch]$KeepGoing,                       # do not stop the whole soak on the first crash
    [string]$Game     = ""                    # Crysis 2 folder (auto-detected if omitted)
)
# Where the game is. These scripts live next to the launcher source, which sits inside a Crysis 2
# installation, so the folder is found by walking up until Bin64\CrySystem.dll appears.
# Pass -Game to point somewhere else.
function Find-GameFolder([string]$Explicit) {
    if ($Explicit) { return $Explicit }

    # Two markers, not one: Bin64\CrySystem.dll alone also matches the Mod SDK, which sits
    # next to the game in a typical modding setup and would be picked instead. GameCrysis2
    # is what makes it the game. Each level up is checked together with its subfolders,
    # because these scripts usually live in a sibling folder rather than inside the game.
    function Test-GameFolder([string]$p) {
        return (Test-Path (Join-Path $p "Bin64\CrySystem.dll")) -and
               (Test-Path (Join-Path $p "GameCrysis2"))
    }

    $d = Split-Path -Parent $PSCommandPath
    while ($d) {
        if (Test-GameFolder $d) { return $d }
        foreach ($s in Get-ChildItem -Path $d -Directory -ErrorAction SilentlyContinue) {
            if (Test-GameFolder $s.FullName) { return $s.FullName }
        }
        $d = Split-Path -Parent $d
    }
    throw "Crysis 2 folder not found. Pass -Game ""C:\path\to\Crysis 2""."
}


$ErrorActionPreference = "Stop"

$game = Find-GameFolder $Game
$exe  = Join-Path $game "Bin64\launcher64.exe"
$logs = Join-Path $game "launcher_logs"
if (-not (Test-Path $exe)) { throw "launcher64.exe not found at $exe" }
if (-not (Test-Path $logs)) { New-Item -ItemType Directory $logs | Out-Null }

if ($Levels.Count -eq 0) {
    # The campaign in the order the game plays it, taken from a real playthrough's logs.
    $Levels = @("Intro", "BatteryPark", "AlienVessel", "FDR", "Warehouse", "Downtown", "Spear",
                "Hive", "FloodedStreets", "CentralStation", "Terminal", "TimesSquare",
                "Roosevelt", "Prism", "Prism2", "Convoy", "CentralPark")
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class SoakInput {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, IntPtr extra);
}
"@

function Key-Down([byte]$vk) { [SoakInput]::keybd_event($vk, 0, 0, [IntPtr]::Zero) }
function Key-Up([byte]$vk)   { [SoakInput]::keybd_event($vk, 0, 2, [IntPtr]::Zero) }
function Key-Tap([byte]$vk)  { Key-Down $vk; Start-Sleep -Milliseconds 60; Key-Up $vk }
function Mouse-Move([int]$dx, [int]$dy) { [SoakInput]::mouse_event(0x0001, $dx, $dy, 0, [IntPtr]::Zero) }
function Mouse-Click { [SoakInput]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
                       Start-Sleep -Milliseconds 80
                       [SoakInput]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero) }

# Text a file has gained since a remembered length. Reading the whole of a 12 MB log per poll is
# what made the earlier script slow enough to want to delete it.
function Get-Tail([string]$path, [long]$from) {
    if (-not (Test-Path $path)) { return "" }
    $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
        if ($fs.Length -le $from) { return "" }
        $fs.Seek($from, 'Begin') | Out-Null
        $buf = New-Object byte[] ($fs.Length - $from)
        $read = $fs.Read($buf, 0, $buf.Length)
        return [System.Text.Encoding]::ASCII.GetString($buf, 0, $read)
    } finally { $fs.Close() }
}

function File-Length([string]$path) {
    if (-not (Test-Path $path)) { return 0 }
    try { return (Get-Item $path).Length } catch { return 0 }
}

$faults  = Join-Path $game "launcher_faults.txt"
$gamelog = Join-Path $game "Game.log"
$stamp   = Get-Date -Format "yyyyMMdd_HHmmss"
$report  = Join-Path $logs "soak_$stamp.txt"
$rows    = @()

"soak started $(Get-Date -Format 'HH:mm:ss'), $($Levels.Count) level(s), ${Sec}s each, flags: '$Extra'" |
    Tee-Object -FilePath $report

foreach ($level in $Levels) {
    # Kill leftovers from a previous iteration, including the crash reporters, which hold the
    # executable open and would block the next start.
    Get-Process launcher64,WerFault,BugTrapN -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500

    $faultsFrom = File-Length $faults
    $t0 = Get-Date

    $cmdArgs = "$Extra +map $level".Trim()
    $p = Start-Process $exe -ArgumentList $cmdArgs -PassThru
    $loaded  = $false
    $entered = $false
    $died    = $false
    $loadSec = 0

    # Wait for the level, dismissing the start screen once the window exists.
    while (((Get-Date) - $t0).TotalSeconds -lt $LoadTimeout) {
        Start-Sleep -Milliseconds 700
        if (@(Get-Process -Id $p.Id -ErrorAction SilentlyContinue).Count -eq 0) { $died = $true; break }

        if (-not $entered) {
            $p.Refresh()
            if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
                [void][SoakInput]::ShowWindow($p.MainWindowHandle, 5)
                if ([SoakInput]::SetForegroundWindow($p.MainWindowHandle)) {
                    [System.Windows.Forms.SendKeys]::SendWait("{ENTER}")
                    $entered = $true
                }
            }
        }

        # The engine writes this once the level is up and the UI has been told about it. The
        # file has to be this run's: the launcher moves the previous one aside at startup, but
        # if that ever fails the old log would answer for the new level.
        $fresh = (Test-Path $gamelog) -and ((Get-Item $gamelog).LastWriteTime -gt $t0)
        if ($fresh -and (Select-String -Path $gamelog -Pattern "\[UI\] OnLoadLevel" -Quiet)) {
            $loaded = $true
            $loadSec = [int]((Get-Date) - $t0).TotalSeconds
            break
        }
    }

    $playedSec = 0
    if ($loaded -and -not $died) {
        Start-Sleep -Seconds 3          # let the first frames settle before touching anything
        $p.Refresh()
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
            [void][SoakInput]::SetForegroundWindow($p.MainWindowHandle)
        }

        $tp = Get-Date
        $step = 0
        while (((Get-Date) - $tp).TotalSeconds -lt $Sec) {
            if (@(Get-Process -Id $p.Id -ErrorAction SilentlyContinue).Count -eq 0) { $died = $true; break }

            # If the game is no longer the front window, take it back - and send nothing this
            # round. Typing W and clicking the mouse into someone else's window is the kind of
            # automation mistake that is hard to undo.
            $p.Refresh()
            if ($Move -and [SoakInput]::GetForegroundWindow() -ne $p.MainWindowHandle) {
                [void][SoakInput]::SetForegroundWindow($p.MainWindowHandle)
                Start-Sleep -Milliseconds 300
                continue
            }

            if ($Move) {
                # Walk, look around, and shoot now and then. Standing at a spawn point exercises
                # almost nothing: the AI never acquires a target and the renderer draws one view.
                switch ($step % 8) {
                    0 { Key-Down 0x57; Start-Sleep -Milliseconds 900; Key-Up 0x57 }   # W
                    1 { Mouse-Move 220 0 }
                    2 { Key-Down 0x41; Start-Sleep -Milliseconds 500; Key-Up 0x41 }   # A
                    3 { Mouse-Move -160 40 }
                    4 { Key-Down 0x57; Start-Sleep -Milliseconds 900; Key-Up 0x57 }   # W
                    5 { Mouse-Click }
                    6 { Key-Down 0x44; Start-Sleep -Milliseconds 500; Key-Up 0x44 }   # D
                    7 { Key-Tap 0x20; Mouse-Move 0 -30 }                              # jump, look up
                }
                $step++
            }
            Start-Sleep -Milliseconds 400
        }
        $playedSec = [int]((Get-Date) - $tp).TotalSeconds
    }

    $alive = (@(Get-Process -Id $p.Id -ErrorAction SilentlyContinue).Count -gt 0)
    if ($alive) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 800

    # What the launcher recorded for this level only.
    $tail = Get-Tail $faults $faultsFrom
    $av   = ([regex]::Matches($tail, "=== access violation ===")).Count
    $fault1 = ""
    $m = [regex]::Match($tail, "faulting code : ([^\r\n]+)")
    if ($m.Success) { $fault1 = $m.Groups[1].Value.Trim() }
    $checks = ""
    foreach ($mm in [regex]::Matches($tail, "(aifix|sndfix): (\d+) checked, (\d+) turned away")) {
        $checks += "{0} {1}/{2}  " -f $mm.Groups[1].Value, $mm.Groups[3].Value, $mm.Groups[2].Value
    }
    # A short run never reaches the five-minute summary, so fall back to the "is live" lines.
    if ($checks -eq "") {
        foreach ($mm in [regex]::Matches($tail, "(aifix|sndfix): check is live, (\d+) call")) {
            $checks += "{0} 0/{1}+  " -f $mm.Groups[1].Value, $mm.Groups[2].Value
        }
    }
    $mt = [regex]::Match($tail, "topdown: (?:memory is landing high|steering), (\d+) (?:of|so far,) (\d+)")
    if ($mt.Success) { $checks += "high " + $mt.Groups[1].Value + "/" + $mt.Groups[2].Value + "  " }

    # The census line is the honest answer to "is this really using 64-bit memory".
    $mem = ""
    foreach ($mm in [regex]::Matches($tail, "memory: (\d+) MB low, (\d+) MB high \((\d+)%\)")) {
        $mem = "{0}/{1} MB ({2}%)" -f $mm.Groups[2].Value, ([int]$mm.Groups[1].Value + [int]$mm.Groups[2].Value), $mm.Groups[3].Value
    }
    $rejects = ([regex]::Matches($tail, "turned away\b")).Count

    # Keep this level's engine log; the next start would otherwise move it under a name that says
    # nothing about which level it was. A clean run only needs its tail - a full campaign of
    # 12 MB logs fills a folder fast - but anything that died keeps everything.
    if (Test-Path $gamelog) {
        $dst = Join-Path $logs ("soak_{0}_{1}.log" -f $stamp, $level)
        try {
            if ($died -or -not $loaded) { Copy-Item $gamelog $dst -Force }
            else { Get-Content $gamelog -Tail 3000 | Set-Content $dst -Encoding UTF8 }
        } catch {}
    }

    foreach ($ev in "error.log", "error.dmp", "error.bmp") {
        $src = Join-Path $game $ev
        if (Test-Path $src) {
            $dst = Join-Path $logs ("soak_{0}_{1}_{2}" -f $stamp, $level, $ev)
            try { Move-Item $src $dst -Force } catch {}
        }
    }

    $status = if ($died) { "CRASHED" } elseif (-not $loaded) { "NO LOAD" } else { "ok" }
    $row = [PSCustomObject]@{
        Level   = $level
        Status  = $status
        LoadSec = $loadSec
        PlaySec = $playedSec
        AVs     = $av
        Where   = $fault1
        Checks  = $checks.Trim()
        HighMem = $mem
    }
    $rows += $row

    $line = "{0,-16} {1,-8} load {2,4}s  play {3,4}s  AV {4}  high {5}  {6} {7}" -f `
            $level, $status, $loadSec, $playedSec, $av, $mem, $fault1, $checks.Trim()
    $line | Tee-Object -FilePath $report -Append

    if ($died -and -not $KeepGoing) {
        "stopping: $level crashed. Rerun with -KeepGoing to continue past crashes." |
            Tee-Object -FilePath $report -Append
        break
    }
}

""
"=== summary ==="
$rows | Format-Table -AutoSize | Out-String | Tee-Object -FilePath $report -Append
"report: $report"
