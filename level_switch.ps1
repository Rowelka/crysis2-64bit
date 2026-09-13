# Does switching levels inside one process crash?
#
# A crash that only happens on a level TRANSITION cannot be reproduced by loading the level
# directly: `+map Downtown` was clean for 100 seconds, while the same level reached from the
# previous one died in the renderer. The difference is the unload - an object released while
# the old level tears down, still referenced when the new one comes up.
#
# So this loads one level, waits for it to settle, then sends "map <second>" to the game's own
# console and watches what happens. The launcher's -sayonce sends it exactly once: a command
# with a side effect must not be repeated, and the default -say repeats three times.
#
#   .\level_switch.ps1 -From FDR -To Downtown -Extra "-topdown"
#   .\level_switch.ps1 -From FDR -To Downtown            # same run with memory left low
#
# Exit code 0 = survived the switch, 1 = died.

param(
    [string]$From  = "FDR",
    [string]$To    = "Downtown",
    [string]$Extra = "",
    [int]$Settle   = 95,     # seconds before the switch is sent (the level must be up)
    [int]$Watch    = 120,    # seconds to watch after it
    [int]$Times    = 1,      # how many switches in one process
    [int]$Every    = 60,     # seconds between them (a level needs ~25s to load)
    [string]$Game  = ""      # path to the Crysis 2 folder (auto-detected if omitted)
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
$log   = Join-Path $game "launcher_faults.txt"

Get-Process launcher64, Crysis2*, WerFault, BugTrapN -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
Remove-Item $log -Force -ErrorAction SilentlyContinue

# Tilde stands in for the space: the launcher swaps it back, so the command survives the
# command line in one piece. The trailing pipe ends it - without one, -say: swallows every
# flag that follows and hands the console "map Downtown -topdown +map FDR".
$argList = @("-sayafter:$Settle", "-sayevery:$Every", "-say:map~$To|")
if ($Times -le 1) { $argList = @("-sayonce") + $argList }
if ($Extra.Trim().Length -gt 0) { $argList += $Extra.Trim().Split(" ") }
$argList += @("+map", $From)

"switch $From -> $To x$Times, flags '$Extra', settle ${Settle}s, every ${Every}s, watch ${Watch}s"
$proc = Start-Process -FilePath (Join-Path $bin "launcher64.exe") -ArgumentList $argList `
                      -WorkingDirectory $bin -PassThru

Start-Sleep -Seconds ($Settle + ($Every * [math]::Max($Times - 1, 0)) + $Watch)

# Alive, and did it actually take the command? A run that never reached the console proves
# nothing either way, so that case is reported rather than counted as a pass.
$alive = $null -ne (Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)
$sent  = $false
$avs   = 0
if (Test-Path $log) {
    $text = Get-Content $log -Raw
    $sent = $text -match "say: map $To -> handed to the console"
    $avs  = ([regex]::Matches($text, "=== access violation ===")).Count
}

# Handing the command to the console is not the same as the level actually changing, and a run
# where it did not change proves nothing. The engine echoes the name exactly as it was typed,
# so this match must ignore case - "Loading level Downtown" from the console against
# "loading level downtown" from +map.
$arrived = $false
$gameLog = Join-Path $game "Game.log"
if (Test-Path $gameLog) {
    $arrived = (Get-Content $gameLog -Raw) -match "(?i)$To is loaded in"
}

Get-Process launcher64, Crysis2* -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

if (-not $sent) {
    "INCONCLUSIVE  the switch was never handed to the console (alive=$alive, AV=$avs)"
    exit 2
}
if (-not $arrived -and $alive) {
    "INCONCLUSIVE  the console took the command but $To never loaded (AV=$avs)"
    exit 2
}
if ($alive) {
    "SURVIVED      $From -> $To loaded, $avs access violation(s)   ($Extra)"
    exit 0
} else {
    $where = if ($arrived) { "after $To loaded" } else { "during the switch" }
    "DIED          $From -> $To $where, $avs access violation(s)   ($Extra)"
    exit 1
}
