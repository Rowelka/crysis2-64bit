# crysis2-64bit

A 64-bit game launcher for **Crysis 2**.

Crysis 2 shipped with a 32-bit game executable only. The Crysis 2 Mod SDK, however, ships a
complete set of **64-bit engine DLLs** - they were built for the Sandbox editor, and no 64-bit
game client was ever released. This launcher boots those x64 DLLs as a playable game client.

The game is completable start to finish on this launcher (verified by an external tester).

> **Status:** working, but rough around the edges. See [Known issues](#known-issues).

---

## Installation

### What you need

Three things, and two of them are downloads.

| | Where |
|---|---|
| **Crysis 2**, patched to **1.9** | Maximum Edition and the Steam release are already 1.9. A disc copy normally needs the patch. |
| **Crysis 2 Mod SDK** (free, by Crytek) | [archive.org](https://archive.org/details/Crysis2ModSDK1.0) - 1.4 GB, the file carries Crytek's own signature. Also on [ModDB](https://www.moddb.com/downloads/crysis-2-sdk). |
| **`launcher64.exe`** | The [Releases](https://github.com/Rowelka/crysis2-64bit/releases) page here. One file, about 50 KB. |

The Mod SDK is not optional and it is not a nice-to-have: **the 64-bit engine lives inside it**.
Retail Crysis 2 ships a 32-bit game only, so without the SDK there is nothing on your disk for
this launcher to start. The SDK installs those 64-bit files into the game's `Bin64` folder.

### Steps

1. Install Crysis 2 and make sure it is patched to 1.9.
2. Install the Mod SDK **into the same folder as the game**.
3. Copy `launcher64.exe` into `Bin64`, next to `CrySystem.dll`.
4. Run `launcher64.exe`.

When it is in the right place, the folder looks like this:

```
Crysis 2\
  Bin32\Crysis2.exe       the original 32-bit game, untouched
  Bin64\CrySystem.dll     came with the Mod SDK
  Bin64\launcher64.exe    what you downloaded
```

Nothing is overwritten and no game file is modified - the original 32-bit `Crysis2.exe` keeps
working exactly as before. To uninstall, delete `launcher64.exe`.

### If it does not start

The launcher checks the common mistakes itself and says what is wrong in plain words, including
the folder it looked in. If you get one of these boxes:

| What it says | What to do |
|---|---|
| The 64-bit engine (CrySystem.dll) was not found | `launcher64.exe` is not in `Bin64`, or the Mod SDK was never installed. Check the folder layout above. |
| CrySystem.dll does not export CreateSystemInterface | The engine files are from a different game or SDK. Reinstall the Crysis 2 Mod SDK. |
| The engine failed to start up | Send `Game.log` and `launcher_diag.txt` (both in the Crysis 2 folder) with a bug report. |
| "the side-by-side configuration is incorrect" | Windows cannot find the 2008 C runtime. Install the **Visual C++ 2008 x64 Redistributable**. This is rare: the launcher asks for `Microsoft.VC90.CRT 9.0.21022.8 (amd64)`, the same assembly the SDK's own `Editor.exe` and `CrySystem.dll` ask for, so a working Mod SDK install normally already has it. |

Every launch writes `launcher_diag.txt` into the game folder, listing your hardware, display mode,
and the versions of every engine module and pak. Attach that file to a bug report; it usually
identifies the problem immediately.

Two flags exist if a fix causes trouble on your setup:

| Flag | Effect |
|---|---|
| `-noborderless` | leave the window alone, use whatever mode the game picks |
| `-keepintro` | keep the intro videos and the debug overlay |
| `-allowmultiple` | start a second copy without asking |

There are also diagnostic flags. They are not needed for playing, but they turn "it crashes on
your machine and not on mine" into something answerable:

| Flag | Effect |
|---|---|
| `-traceallocs` | log every large engine allocation, its size and the address it got, to `launcher_faults.txt` |
| `-forcehighheap` | reserve all free memory below the 4 GB line before engine init, forcing the heap above it |
| `-enginefix` | apply the pointer-width correction described below (off by default) |

`-forcehighheap` exists because pointer truncation only shows up when memory lands high, which on
most machines it never does. The flag makes that condition happen on demand, so a fix can be
tested in twenty seconds instead of by shipping a build and waiting for reports.

---

## What this fixes

### 1. The 64 FPS lock - root cause found

The x64 build has a hard framerate ceiling at almost exactly 64 FPS. It is not a GPU or CPU
limit: with the lock active the GPU sits at ~5.6 ms and physics at ~0.1 ms, while the main
thread spends **~15.5 ms per frame waiting on the physics barrier**. The hardware is idle.

The cause is not the engine. It is the **Windows timer quantum**:

```
default timer resolution on Windows       = 15.625 ms
1000 ms / 15.625 ms                       = 64.0 FPS      <- the observed ceiling
measured WaitPhys                         = ~15.5 ms      <- exactly one quantum
```

When the main thread waits on the physics barrier, Windows rounds that wait **up** to the next
scheduler quantum. One frame becomes one quantum. Checking the import tables confirms nobody
ever raises the resolution - `Crysis2.exe`, `Editor.exe` and `CrySystem.dll` all import only
`timeGetTime` from WINMM, never `timeBeginPeriod`:

```
Crysis2.exe (retail 32-bit)  ->  WINMM.dll: [timeGetTime]
Editor.exe                   ->  WINMM.dll: [timeGetTime]
CrySystem.dll (x64)          ->  WINMM.dll: [timeGetTime]
```

This also explains the odd bimodal behaviour players reported - the framerate is either pinned
at 64 or spikes to 200-300, with nothing in between. Frames that don't hit the wait run at full
speed; frames that do hit it cost a whole quantum. That is a signature of a timer quantum, not
of load.

The fix is `timeBeginPeriod(1)` in the launcher, before engine init. Since Windows 10 2004 the
call is per-process, so it affects only the game.

**Result on an RTX 3060 @ 1920x1080:** a hard 64 FPS becomes **270-330 FPS**.

> Note: this is listed as an open item in [c2-launcher](https://github.com/mvoolt/c2-launcher)'s
> TODO (*"fix DX11 fps cap limit on fullscreen"*), where the working hypothesis was to force
> borderless windowed. The real cause turned out to be unrelated to the window mode - the lock is
> present in windowed mode too.

### 2. Tearing, input lag and Alt-Tab crashes - one fix

With the framerate unlocked, exclusive fullscreen tears: the game renders well above the display
refresh rate. Enabling VSync is not a good answer here, because the engine has no refresh-rate
CVar at all - `CryRenderD3D11` exposes only `r_Fullscreen` and `r_VSync`, so DXGI hands it 60 Hz
and VSync pins the game to 60 FPS with noticeable input lag, even on a 165 Hz display.

Windowed mode behaves completely differently: frames go through the Windows compositor, which
synchronises presentation itself. Tearing is impossible by design, the framerate stays unlocked,
and there is no VSync input lag.

So the launcher runs the game in a **borderless window sized to the screen**: it asks the engine
to start windowed at desktop resolution, then a background thread finds the game window and
strips its frame. The result looks like fullscreen and feels like windowed.

This also fixes Alt-Tab crashes for free - a windowed swapchain has no exclusive device to lose
on a focus switch. Confirmed in practice: Alt-Tab is now fast and returns straight to a
borderless fullscreen view.

The thread polls twice a second (never in the frame loop), keeps watching rather than applying
once (the engine can recreate its window on a video mode change), and leaves minimised windows
alone so it doesn't fight Alt-Tab. If the game ends up in exclusive fullscreen anyway, its window
is already borderless and full-screen, so the check finds nothing to do. Disable with
`-noborderless`.

### 3. The truncated pointer in the engine's allocator

Retail `CrySystem.dll` keeps the head of its bucket allocator's free-page list in a global that
every read treats as 64 bits wide and exactly one write treats as 32:

```
RVA 0x0A16F1   read    64-bit
RVA 0x0A1715   read    64-bit
RVA 0x0A1A30   read    64-bit
RVA 0x0A1A49   WRITE   32-bit      mov dword ptr [rip+0x6578E9], ebp
```

The top half of the pointer is dropped on the way in and reads back as zero. In the original
32-bit game this was invisible, because addresses were 32 bits anyway. In a 64-bit process it
stays invisible only while the allocation sits below the 4 GB line.

The launcher keeps it that way: it is built with the **VC90 compiler from WDK 7.1** and links
`msvcr90` as the primary CRT (`/MD`), so the process heap lands low - the same situation the
editor is in, which is why the editor never hit this. That is why `build.ps1` uses the WDK
toolchain rather than a modern MSVC; it is load-bearing, not nostalgia.

A patch for the instruction itself exists (`-enginefix`): the store is one REX.W prefix short of
being correct, and the dead register reload in front of it frees exactly the byte that prefix
needs, so the fix is byte-for-byte the same length and no displacement moves. It is **off by
default**, because it turns out not to be needed - startup survives `-forcehighheap`, which forces
every allocation above the line, with the patch and without it. The engine is left alone unless
there is a reason to touch it.

### 4. Level-load and cutscene crashes

- **CryAction release asserts** on the `CLevelSystem::LoadLevel` path force-crash in the Bin64
  build. The launcher patches them out in memory at startup (addresses from c2-launcher).
- **CMovieSystem use-after-free**: unloading a layer during a cutscene precache leaves dangling
  descriptors in the movie update list, which crash on Battery Park. The launcher installs a
  vectored exception handler that skips the corrupt entry instead of dying - worst case a broken
  node loses its animation.

---

### 5. Debug overlay and broken intro, off by default

Two things made the build look like a debug build rather than a game.

The x64 `CrySystem.dll` is an editor build and enables `r_DisplayInfo`, which draws a debug
overlay whose status line ends in `DevMode`, which makes the build look like a debug build. It is
enabled by the game's own `system.cfg` and needs no reversing to remove - one CVar switches it off.

The startup logos also render as white rectangles on black, because the x64 build fails to decode
the intro videos. The files themselves are present and `Videos.pak` is intact, so this is a
decoder problem rather than missing content. Rather than chase the decoder, the launcher skips
the intro.

Both are set from the launcher's command line at startup:

```
+g_skipIntro 1 +sys_rendersplashscreen 0 +sys_intromoviesduringinit 0 +r_DisplayInfo 0
```

Pass `-keepintro` to restore the original behaviour, or set the CVars from the console.

### 6. The invisible cursor, and the missing icon

The game loads its cursor with `LoadCursorA` against the running executable, which with this
project is the launcher rather than `Crysis2.exe`. The original executable carries those cursor
resources; a launcher built without them makes `LoadCursorA` return NULL, and the cursor
disappears. The mouse still works: menu buttons highlight on hover, clicks land where they should,
but nothing is drawn under the pointer.

The launcher links the same resources under the same ids the game asks for (103-107, the amber,
blue, green, red and white Crysis cursors), and the game's own icon under id 101 - without it
Windows draws the blank default, and the launcher looks like a stray tool next to the game rather
than a way to start it.

Both are Crytek assets, so they are not stored here. `extract_resources.py` pulls them out of the
`Crysis2.exe` of the installed game at build time, and `build.ps1` runs it automatically.

## How it works

Crysis 2 has no 64-bit game executable, so this one takes the place of `Crysis2.exe` and drives
the engine directly. At startup it:

1. **Raises the timer resolution** to 1 ms, before anything else, which is what lifts the 64 FPS
   ceiling described above.
2. **Sets the working directory** to the game root, so the engine resolves its own paths.
3. **Appends console commands** to the engine's command line: windowed at desktop resolution for
   borderless mode, plus skipping the intro and the debug overlay.
4. **Starts a background thread** that finds the game window and strips its frame, then keeps
   watching in case the engine recreates the window.
5. **Writes `launcher_diag.txt`**, before engine init so the file survives a startup crash.
6. **Creates the engine** through `CreateSystemInterface`, in the same order the editor uses,
   and hands the result to the game DLL through `SSystemInitParams::pSystem`. That hand-off is
   not optional: the field is documented as "reused if not NULL", and without it the game brings
   up a second `CSystem` on top of the first. The second one dies while allocating the pak heap
   pools, which surfaces as `Failed CMTSafeHeap::m_pBigPool allocation` on startup.
7. **Patches the loaded engine DLLs** in memory: the CryAction release asserts that force-crash
   on level load, the CryMovie update loop that walks freed entries after a layer unload, and two
   unimplemented vtable slots in the editor build of CrySystem.
8. **Loads the game DLL**, calls its entry point, and enters the main loop.

Steps 6 and 7 are ordered deliberately: the patches have to be applied after the modules are
loaded but before the game initialises and starts using them.

Command line flags:

| Flag | Effect |
|---|---|
| `-noborderless` | leave the window alone, use whatever mode the game picks |
| `-keepintro` | keep the intro videos and the debug overlay |
| `-allowmultiple` | start a second copy without asking |
| `-traceallocs` | log large engine allocations to `launcher_faults.txt` |
| `-forcehighheap` | force the heap above the 4 GB line, to reproduce truncation on demand |
| `-enginefix` | apply the pointer-width correction (off by default) |

## Building

You need:

- **WDK 7.1** for the VC90 x64 compiler. This is not nostalgia: the launcher has to link against
  msvcr90 as its primary CRT so the process heap lands below the 4 GB line, or retail CrySystem's
  truncated slab pointers come back corrupt. See the heap section above.

  Tested rather than assumed: the same source built with MSVC 14.51, both `/MD` and `/MT`, does
  not start at all. The process hangs before `WinMain` reaches its first line - no `Game.log`, no
  `launcher_diag.txt` - because `CrySystem` is imported statically and its static initialisers
  allocate through the engine allocator while the wrong CRT is the primary one.
- **Windows 10 SDK** for `rc.exe` and `mt.exe`, the resource compiler and manifest tool.
- **An installed copy of the game**, because the build extracts the cursors and the icon from its
  `Crysis2.exe`.

Run it:

```powershell
.\build.ps1
```

The script finds the game by walking up from its own folder, looking for a `Bin64\CrySystem.dll`
next to game content. If it guesses wrong, or the repository is cloned somewhere else entirely,
name the install yourself:

```powershell
.\build.ps1 -GamePath "C:\Games\Crysis 2" -NoDeploy
```

| Parameter | Effect |
|---|---|
| `-GamePath` | the Crysis 2 install, the folder holding `Bin64` and `gamecrysis2` |
| `-Wdk` | WDK 7.1 root, if it is not in `C:\WinDDK\7600.16385.1` |
| `-Kit` | Windows SDK `bin\<version>\x64`, if the newest installed one is not wanted |
| `-NoDeploy` | build only, do not copy the result into the game |

The script extracts the cursors and the icon, compiles the resource script, builds `Main_min.cpp`, embeds the
VC90 CRT manifest, and copies the result into the game's `Bin64`.

Nothing links against the engine. `CreateSystemInterface` is resolved with `LoadLibrary` at
startup, so no import library is generated and none is needed - and a missing or misplaced engine
produces an explanation from the launcher rather than Windows' "reinstall the program" box.

The cursor `.cur` files and the `.ico` are absent from this repository on purpose and extracted
from the installation at build time: they are Crytek assets, not ours to distribute.

### Testing a build

```powershell
.\test.ps1 -Runs 3
```

It launches the game a few times and reports whether each run actually reached its menu. The
verdict matters more than it sounds: a build can stop printing an error and still be broken, so
"no error in the log" is not a pass. A run counts only if the renderer came up, the window is a
real window rather than the 8x8 one a half-initialised renderer creates, no allocator failure was
logged, and the process actually loaded content. Startup is not deterministic either, which is
why the default is three runs rather than one.

## Known issues

| Issue | Notes |
|---|---|
| Intro videos render as white rectangles | Skipped by default. The decoder is present and is not the problem - see below. |
| Co-op dialogue lines cut off and repeat | Not game-breaking. |
| The engine reports 0 MB of video memory | Real, but no effect could be measured - see below. |

**On the videos.** It is tempting to assume the 64-bit build has no video decoder. It has one:
retail x64 `CrySystem.dll` carries a complete 64-bit CRI Sofdec build (`CRI Movie/PCx64 Ver.2.68`),
together with its error strings - `Need to call CriMv::Initialize()`, `CRI Heap is not
initialized`, `Decode USM header timeout`.

More than that, the decoder demonstrably runs. Playing the in-level cutscene on Battery Park
logs `PlayVideo`, then `Disable scene rendering for playback of FMV sequence`, then
`re-enable scene rendering` exactly 71 seconds later - the length of the file - and not one CRI
error in between. Reproduced three times. So whatever is wrong with the intro logos is in
getting a decoded frame onto the screen, not in decoding it, which is a different place to look
than the one this section used to point at.

**On the video memory.** The renderer reports zero on a card with 12 GB, and follows it with
"Disabling of textures streaming...". That message is printed unconditionally as part of
reinitialising the texture manager - "Finished initializing textures streaming..." follows a few
lines later - so it is not the disable it appears to be. The zero itself is real, but forcing the
size check that depends on it, and setting `r_TexturesStreaming 1`, both change nothing
observable.

## Diagnostics

On every start the launcher writes `launcher_diag.txt` next to the game, before engine init, so
the file exists even if the engine dies on startup. It records:

- the launcher build and the command line it was given
- whether each fix actually applied - the timer, borderless, the engine patch, the allocator trace
- the install path, whether the game folder is writable, free disk space, and the system locale
- OS build, CPU thread count, RAM, desktop mode and refresh rate, GPU name, DPI scale
- how much address space is free below the 4 GB line, and the largest single free block in it
- where the engine's allocator actually placed its first block, and whether a 14 MB request succeeds
- the size and date of every engine module, and the size of every game pak

The write-access and free-space lines exist because a read-only install or a full disk produces
a failure that looks like a code bug. The address-space lines exist because this engine only
works while its memory stays low, so knowing where it landed is the difference between a guess
and an answer.

The pak list matters more than it looks: bug reports that appear to be launcher problems often
turn out to be differences between game copies (retail versus a repack with re-encoded or removed
files). Comparing pak sizes settles that in seconds instead of a debugging session.

Nothing user-identifying is collected: no user name, no profile paths, no serials, no network
information.

## Credits

- **[c1-launcher](https://github.com/ccomrade/c1-launcher)** by *ccomrade* - the original
  open-source Crysis launcher, and the reference that showed this approach is viable.
- **[c2-launcher](https://github.com/mvoolt/c2-launcher)** - the CryAction assert-patch
  addresses used here were taken from that project.

This launcher's source is written from scratch (STL-free, so it can be compiled by the VC90
toolchain); it is not a fork of either project.

## License

MIT - see [LICENSE](LICENSE).
