# Build launcher64 (VC90 from WDK 7.1) - recipe from x64-launcher-wall.md.
# msvcr90 primary CRT (/MD) -> heap <4GB -> slab truncation harmless. STL-free Main_min.cpp.
$ErrorActionPreference = "Stop"
$dir    = "D:\GAMES\Crysis 2 mod\My fun\x64launcher"
$wdk    = "C:\WinDDK\7600.16385.1"
$cl     = "$wdk\bin\x86\amd64\cl.exe"
$kit    = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"
$mt     = "$kit\mt.exe"
$rc     = "$kit\rc.exe"
$out    = "$dir\launcher64_vc90.exe"
$dest   = "D:\GAMES\Crysis 2 mod\Crysis 2\Bin64\launcher64.exe"
$game32 = "D:\GAMES\Crysis 2 mod\Crysis 2\bin32\Crysis2.exe"

# Kill running launcher/game so copy/link is not blocked (file in use).
Get-Process launcher64,Crysis2,Editor,WerFault,BugTrapN -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

# WDK env (manual - setenv.bat is finicky). NO stl70/CryCommon.
$env:PATH    = "$wdk\bin\x86\amd64;$wdk\bin\x86;$env:PATH"
$env:INCLUDE = "$wdk\inc\crt;$wdk\inc\api"
$env:LIB     = "$wdk\lib\crt\amd64;$wdk\lib\win7\amd64"

Set-Location $dir
if (Test-Path $out) { Remove-Item $out -Force }

# Import library for CrySystem, generated from the .def rather than shipped: it is derived from
# the game's own DLL, so it is not ours to distribute, and generating it keeps this repository to
# source only. link /lib does the job; the WDK has no separate lib.exe.
if (-not (Test-Path "$dir\CrySystem.lib")) {
    & "$wdk\bin\x86\amd64\link.exe" /lib /nologo /def:"$dir\CrySystem.def" /machine:x64 /out:"$dir\CrySystem.lib"
    if (-not (Test-Path "$dir\CrySystem.lib")) { throw "FAILED to generate CrySystem.lib from CrySystem.def" }
}

# Cursor resources: the game calls LoadCursorA against its own executable, which is this
# launcher. Without them LoadCursorA returns NULL and the in-game cursor is invisible
# (the mouse still works and menu buttons still highlight). The .cur files are Crytek
# assets and are not stored in the repo - they are extracted from the installed game.
if (-not (Test-Path "$dir\res\cursor_103.cur")) {
    & python "$dir\extract_cursors.py" "$game32" "$dir\res"
}
if (Test-Path "$dir\res\cursor_103.cur") {
    & $rc /nologo /fo "$dir\launcher.res" "$dir\launcher.rc"
} else {
    Write-Warning "cursors not extracted - building without them (in-game cursor will be invisible)"
}

$res = ""
if (Test-Path "$dir\launcher.res") { $res = "$dir\launcher.res" }
& $cl /nologo /EHsc /MD /DWIN64 /D_WIN64 /I. Main_min.cpp /Fe"$out" /Fo"$dir\Main_min.obj" /link CrySystem.lib kernel32.lib user32.lib shell32.lib  $res /MACHINE:X64 /SUBSYSTEM:WINDOWS /MANIFEST
if (-not (Test-Path $out)) { throw "BUILD FAILED: $out not created" }

# Embed manifest (VC90.CRT dependency).
& $mt -nologo -manifest "$out.manifest" -outputresource:"$out;1"

# Deploy to game Bin64.
Copy-Item $out $dest -Force
$sz = (Get-Item $out).Length
"OK: built $sz bytes -> Bin64\launcher64.exe"
