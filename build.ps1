# Build launcher64 (VC90 from WDK 7.1) - recipe from x64-launcher-wall.md.
# msvcr90 primary CRT (/MD) -> heap <4GB -> slab truncation harmless. STL-free Main_min.cpp.
$ErrorActionPreference = "Stop"
$dir  = "D:\GAMES\Crysis 2 mod\My fun\x64launcher"   # [run91] папка переехала из "Свой прикол"
$wdk  = "C:\WinDDK\7600.16385.1"
$cl   = "$wdk\bin\x86\amd64\cl.exe"
$mt   = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\mt.exe"
$out  = "$dir\launcher64_vc90.exe"
$dest = "D:\GAMES\Crysis 2 mod\Crysis 2\Bin64\launcher64.exe"

# Kill running launcher/game so copy/link is not blocked (file in use).
Get-Process launcher64,Crysis2,Editor,WerFault,BugTrapN -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

# WDK env (manual - setenv.bat is finicky). NO stl70/CryCommon.
$env:PATH    = "$wdk\bin\x86\amd64;$wdk\bin\x86;$env:PATH"
$env:INCLUDE = "$wdk\inc\crt;$wdk\inc\api"
$env:LIB     = "$wdk\lib\crt\amd64;$wdk\lib\win7\amd64"

Set-Location $dir
if (Test-Path $out) { Remove-Item $out -Force }

& $cl /nologo /EHsc /MD /DWIN64 /D_WIN64 /I. Main_min.cpp /Fe"$out" /Fo"$dir\Main_min.obj" /link CrySystem.lib kernel32.lib user32.lib shell32.lib winmm.lib /SUBSYSTEM:WINDOWS /MANIFEST
if (-not (Test-Path $out)) { throw "BUILD FAILED: $out not created" }

# Embed manifest (VC90.CRT dependency).
& $mt -nologo -manifest "$out.manifest" -outputresource:"$out;1"

# Deploy to game Bin64.
Copy-Item $out $dest -Force
$sz = (Get-Item $out).Length
"OK: built $sz bytes -> Bin64\launcher64.exe"
