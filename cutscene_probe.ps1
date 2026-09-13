# Did the intro cutscene play? Answered without anyone watching the screen.
#
# BatteryPark opens with a cutscene, and +map runs it - the earlier conclusion that +map skips
# cutscenes was wrong, it was the level that had none. During the cutscene the engine moves the
# camera; when the arenas are high it does not, and the player is left standing at the spawn
# point forever. The difference is not subtle:
#
#   working : camera travelled 23.34, 23.91, 6.86, 4.74 - a wide band, because how far the
#             cutscene gets before the probe ends varies from run to run
#   broken  : camera travelled  0.004 - the camera does not move at all, ever
#
# So the threshold is 1.0, not something in the middle: broken is literally zero, and any real
# movement means the cutscene started. A threshold of 5.0 cut the working band in half and
# reported a good configuration as broken.
#
# So the total distance the camera covers is the symptom, and one number separates the two.
#
#   .\cutscene_probe.ps1 -Extra "-topdown -keepband:off"
#
# Two ways this probe lies, both paid for:
#
#   Too short a run.  FDR loads slowly and its cutscene starts past the two-minute mark; at 120 s
#   it reported 1.2 (broken), at 130 s it reported 381 (played). The default is now 140 s, and a
#   new level needs a baseline run with no flags before its number means anything.
#
#   A level with no opening cutscene. Downtown and TimesSquare sit at ~0.018 even untouched, so
#   everything looks broken there. Use BatteryPark (fast, steady) or FDR (needs 135 s+).
#
# Exit code 0 = cutscene played, 1 = broken, 2 = run produced no trace.

param(
    [string]$Extra = "",
    [string]$Level = "BatteryPark",
    [int]$Seconds = 140,
    [double]$Threshold = 1.0,
    [switch]$Quiet,
    [string]$Game = ""       # path to the Crysis 2 folder (auto-detected if omitted)
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


$game  = Find-GameFolder $Game
$bin   = Join-Path $game "Bin64"
$trace = Join-Path $game "camera_trace.txt"

Get-Process Crysis2*, launcher64 -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
Remove-Item $trace -Force -ErrorAction SilentlyContinue

$argList = @("-trace")
if ($Extra.Trim().Length -gt 0) { $argList += $Extra.Trim().Split(" ") }
$argList += @("+map", $Level)

Start-Process -FilePath "$bin\launcher64.exe" -ArgumentList $argList -WorkingDirectory $bin
Start-Sleep -Seconds $Seconds

$moved = 0.0
$count = 0
if (Test-Path $trace) {
    foreach ($ln in Get-Content $trace) {
        $p = $ln -split "\s+" | Where-Object { $_ -ne "" }
        if ($p.Count -eq 5 -and $p[0] -match "^\d+$") {
            $moved += [double]($p[4] -replace ",", ".")
            $count++
        }
    }
}

Get-Process Crysis2*, launcher64 -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

if ($count -eq 0) {
    if (-not $Quiet) { "NO TRACE   ($Extra)" }
    exit 2
}

$m = [math]::Round($moved, 3)
if ($moved -ge $Threshold) {
    if (-not $Quiet) { "PLAYED     camera moved $m over $count samples   ($Extra)" }
    exit 0
} else {
    if (-not $Quiet) { "BROKEN     camera moved $m over $count samples   ($Extra)" }
    exit 1
}
