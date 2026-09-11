# Runs the launcher a few times and reports whether the game actually reached its menu.
#
# The point of this script is the verdict it uses. "No error in the log" is not a verdict: a
# build can stop printing an error and still be broken, and it will look fixed for as long as
# you keep measuring that. A run counts as good only if all of these hold:
#
#   - the renderer came up            (a "D3D11 CryRender Stats" block in Game.log)
#   - the window is a real window     (not the 8x8 one a half-initialised renderer creates)
#   - no allocator failure            (no CMTSafeHeap message)
#   - the process actually loaded     (more than 500 MB resident)
#
# Startup is also not deterministic, so one pass proves nothing. Three is the minimum that
# distinguishes a fix from luck.
param(
    [string]$GamePath = "",
    [int]$Runs = 3,
    [string]$Arguments = "",
    [int]$WaitSeconds = 26
)

$dir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }

function Test-CrysisInstall([string]$path) {
    if (-not $path) { return $false }
    if (-not (Test-Path (Join-Path $path "Bin64\CrySystem.dll"))) { return $false }
    return (Test-Path (Join-Path $path "gamecrysis2")) -or (Test-Path (Join-Path $path "bin32\Crysis2.exe"))
}

if ($GamePath -eq "") {
    $probe = $dir
    for ($i = 0; $i -lt 6 -and $probe; $i++) {
        if (Test-CrysisInstall $probe) { $GamePath = $probe; break }
        foreach ($child in Get-ChildItem $probe -Directory -ErrorAction SilentlyContinue) {
            if (Test-CrysisInstall $child.FullName) { $GamePath = $child.FullName; break }
        }
        if ($GamePath -ne "") { break }
        $probe = Split-Path $probe -Parent
    }
}
if (-not (Test-CrysisInstall $GamePath)) {
    throw "Crysis 2 installation not found. Pass -GamePath ""C:\path\to\Crysis 2""."
}

$exe = Join-Path $GamePath "Bin64\launcher64.exe"
$log = Join-Path $GamePath "Game.log"
if (-not (Test-Path $exe)) { throw "launcher64.exe is not in $GamePath\Bin64 - build it first." }

function Stop-Launcher {
    for ($k = 0; $k -lt 12; $k++) {
        $procs = @(Get-Process launcher64 -ErrorAction SilentlyContinue)
        if ($procs.Count -eq 0) { Start-Sleep -Milliseconds 400; return }
        $procs | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
    }
}

$good = 0
for ($r = 1; $r -le $Runs; $r++) {
    Stop-Launcher
    if (Test-Path $log) { Remove-Item $log -Force }

    if ($Arguments -ne "") { Start-Process $exe -ArgumentList $Arguments } else { Start-Process $exe }
    Start-Sleep -Seconds $WaitSeconds

    $procs = @(Get-Process launcher64 -ErrorAction SilentlyContinue)
    $mem = 0
    if ($procs.Count) { $mem = [int]($procs[0].PrivateMemorySize64 -shr 20) }

    if (-not (Test-Path $log)) {
        "run {0}: FAIL - no Game.log, the engine never started" -f $r
        Stop-Launcher
        continue
    }

    $renderer = $null -ne (Select-String -Path $log -Pattern "D3D11 CryRender Stats" -ErrorAction SilentlyContinue)
    $heapErr  = $null -ne (Select-String -Path $log -Pattern "CMTSafeHeap" -ErrorAction SilentlyContinue)
    $window   = Select-String -Path $log -Pattern "Creating window called" -ErrorAction SilentlyContinue
    $size     = "none"
    if ($window) { $size = (($window | Select-Object -First 1).Line -replace ".*\(", "(") }

    if ($renderer -and (-not $heapErr) -and ($size -ne "(8x8)") -and ($mem -gt 500)) {
        "run {0}: ok    window={1} memory={2} MB" -f $r, $size, $mem
        $good = $good + 1
    } else {
        "run {0}: FAIL  window={1} renderer={2} heapError={3} memory={4} MB" -f $r, $size, $renderer, $heapErr, $mem
    }
    Stop-Launcher
}

""
"{0} of {1} runs reached the menu" -f $good, $Runs
if ($good -lt $Runs) { exit 1 }
