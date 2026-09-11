# Builds launcher64.exe.
#
# The compiler is the VC90 one from WDK 7.1, not a modern toolchain. That is deliberate: it
# makes msvcr90 the process's primary CRT, which puts the heap in the low part of the address
# space - where this engine build expects its allocations to be. Main_min.cpp is STL-free for
# the same reason.
#
# Paths are discovered rather than hardcoded, so the repository builds wherever it is cloned.
# Override any of them if the discovery guesses wrong.
param(
    [string]$GamePath = "",                            # the Crysis 2 install (the folder holding Bin64)
    [string]$Wdk      = "C:\WinDDK\7600.16385.1",      # WDK 7.1, for the VC90 compiler
    [string]$Kit      = "",                            # Windows SDK bin folder, for rc.exe and mt.exe
    [switch]$NoDeploy                                  # build only, do not copy into the game
)

$ErrorActionPreference = "Stop"
$dir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$out = Join-Path $dir "launcher64_vc90.exe"

# --- locate the game -----------------------------------------------------------------------
# Walk up from this script looking for a Bin64 with the engine in it. The repository normally
# lives inside the installation, but it does not have to.
function Test-CrysisInstall([string]$path) {
    # An engine SDK also carries Bin64\CrySystem.dll, so require the game content as well,
    # otherwise the search happily settles on a CryENGINE SDK folder sitting next door.
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
    throw "Crysis 2 installation not found. Pass -GamePath ""C:\path\to\Crysis 2"" (the folder that contains Bin64 and gamecrysis2)."
}

# --- locate the Windows SDK tools ----------------------------------------------------------
if ($Kit -eq "") {
    $kitRoot = "C:\Program Files (x86)\Windows Kits\10\bin"
    if (Test-Path $kitRoot) {
        $newest = Get-ChildItem $kitRoot -Directory -ErrorAction SilentlyContinue |
                  Where-Object { Test-Path (Join-Path $_.FullName "x64\rc.exe") } |
                  Sort-Object Name -Descending | Select-Object -First 1
        if ($newest) { $Kit = Join-Path $newest.FullName "x64" }
    }
}

$cl = Join-Path $Wdk "bin\x86\amd64\cl.exe"
if (-not (Test-Path $cl)) { throw "VC90 compiler not found at $cl. Install WDK 7.1 or pass -Wdk." }
$rc = if ($Kit) { Join-Path $Kit "rc.exe" } else { "" }
$mt = if ($Kit) { Join-Path $Kit "mt.exe" } else { "" }

$dest   = Join-Path $GamePath "Bin64\launcher64.exe"
$game32 = Join-Path $GamePath "bin32\Crysis2.exe"

# Release the executable so linking and deploying are not blocked by a running game.
Get-Process launcher64,Crysis2,Editor,WerFault,BugTrapN -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

# WDK environment, set by hand: setenv.bat is finicky and pulls in more than is wanted here.
$env:PATH    = (Join-Path $Wdk "bin\x86\amd64") + ";" + (Join-Path $Wdk "bin\x86") + ";" + $env:PATH
$env:INCLUDE = (Join-Path $Wdk "inc\crt") + ";" + (Join-Path $Wdk "inc\api")
$env:LIB     = (Join-Path $Wdk "lib\crt\amd64") + ";" + (Join-Path $Wdk "lib\win7\amd64")

Set-Location $dir
if (Test-Path $out) { Remove-Item $out -Force }

# Cursor resources: the game calls LoadCursorA against its own executable, which is this
# launcher. Without them the in-game cursor is invisible (the mouse still works). The .cur
# files are Crytek assets and are not stored here - they are extracted from the installation.
$resFile = Join-Path $dir "launcher.res"
$haveCursors = Test-Path (Join-Path $dir "res\cursor_103.cur")
$haveIcon    = Test-Path (Join-Path $dir "res\icon_101.ico")
if (-not ($haveCursors -and $haveIcon)) {
    if (Test-Path $game32) {
        & python (Join-Path $dir "extract_resources.py") $game32 (Join-Path $dir "res")
        $haveCursors = Test-Path (Join-Path $dir "res\cursor_103.cur")
        $haveIcon    = Test-Path (Join-Path $dir "res\icon_101.ico")
    } else {
        Write-Warning "bin32\Crysis2.exe not found - building without the game's cursors and icon"
    }
}
if ($haveCursors -and $haveIcon -and $rc -and (Test-Path $rc)) {
    & $rc /nologo /fo $resFile (Join-Path $dir "launcher.rc")
}

$clArgs = @("/nologo", "/EHsc", "/MD", "/DWIN64", "/D_WIN64", "/I.", "Main_min.cpp", "/Fe$out",
            "/link", "kernel32.lib", "user32.lib", "shell32.lib", "gdi32.lib")
if (Test-Path $resFile) { $clArgs += $resFile }
$clArgs += @("/MACHINE:X64", "/SUBSYSTEM:WINDOWS", "/MANIFEST")

& $cl $clArgs
if (-not (Test-Path $out)) { throw "build failed: $out was not created" }

# Embed the manifest that names the VC90 CRT dependency.
if ($mt -and (Test-Path $mt)) {
    & $mt -nologo -manifest "$out.manifest" -outputresource:"$out;1"
}

if (-not $NoDeploy) {
    Copy-Item $out $dest -Force
    "OK: built {0} bytes -> {1}" -f (Get-Item $out).Length, $dest
} else {
    "OK: built {0} bytes -> {1} (not deployed)" -f (Get-Item $out).Length, $out
}
