# Play the game by hand without losing the evidence.
#
# The engine truncates Game.log on every start, and the launcher's own journal with it. So the
# run that crashed is erased by the run you start to check whether it crashes again - which is
# exactly what you do after a crash. An evening of play can leave nothing behind.
#
# This copies both logs aside before launching, named by when the PREVIOUS session ended, and
# then starts the game normally. Nothing is ever deleted.
#
#   .\play.ps1                      # campaign from the menu, memory high
#   .\play.ps1 -Level Downtown      # straight into a level
#   .\play.ps1 -Extra "-keepband:14-16"    # with the old workaround back on, to compare
#
# After a crash, say so and the journal is still there: registers, stack, module names.

param(
    [string]$Level = "",
    [string]$Extra = "",
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


$game = Find-GameFolder $Game
$bin  = Join-Path $game "Bin64"
$logs = Join-Path $game "launcher_logs"

if (-not (Test-Path $logs)) { New-Item -ItemType Directory -Path $logs | Out-Null }

Get-Process launcher64, Crysis2*, WerFault, BugTrapN -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# Keep what the last session left, stamped with when it finished rather than now - that is the
# time you will remember the crash by.
$kept = @()
foreach ($name in @("Game.log", "launcher_faults.txt", "error.log")) {
    $src = Join-Path $game $name
    if (-not (Test-Path $src)) { continue }
    $when = (Get-Item $src).LastWriteTime.ToString("yyyyMMdd_HHmmss")
    $dst  = Join-Path $logs ("kept_" + $when + "_" + $name)
    if (-not (Test-Path $dst)) {
        Copy-Item $src $dst -ErrorAction SilentlyContinue
        $kept += (Split-Path $dst -Leaf)
    }
}

if ($kept.Count -gt 0) { "kept from the last session: $($kept -join ', ')" }
else                   { "nothing to keep - no previous logs" }

$argList = @("-topdown")
if ($Extra.Trim().Length -gt 0) { $argList += $Extra.Trim().Split(" ") }
if ($Level.Trim().Length  -gt 0) { $argList += @("+map", $Level.Trim()) }

"starting: launcher64.exe $($argList -join ' ')"
Start-Process -FilePath (Join-Path $bin "launcher64.exe") -ArgumentList $argList -WorkingDirectory $bin
