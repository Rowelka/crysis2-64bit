# crysis2-64bit

A 64-bit game launcher for **Crysis 2**.

Crysis 2 shipped with a 32-bit game executable only. The Crysis 2 Mod SDK, however, ships a
complete set of **64-bit engine DLLs** - they were built for the Sandbox editor, and no 64-bit
game client was ever released. This launcher boots those x64 DLLs as a playable game client.

The game is completable start to finish on this launcher (verified by an external tester).

> **Status:** working, but rough around the edges. See [Known issues](#known-issues).

---

## Installation

You need **Crysis 2** patched to 1.9 (the Maximum Edition already is) and the **Crysis 2 Mod SDK**.
The Mod SDK is what puts the 64-bit engine DLLs into the game's `Bin64` folder - without it there
is nothing here to run.

1. Install Crysis 2, then install the Crysis 2 Mod SDK into the same folder.
2. Download `launcher64.exe` from the [Releases](https://github.com/Rowelka/crysis2-64bit/releases) page.
3. Put it in `<game folder>\Bin64\`, next to `CrySystem.dll` and `Editor.exe`.
4. Run `launcher64.exe`.

Nothing is overwritten and no game file is modified - the original 32-bit `Crysis2.exe` keeps
working exactly as before. To uninstall, delete `launcher64.exe`.

### If something goes wrong

Every launch writes `launcher_diag.txt` into the game folder, listing your hardware, display mode,
and the versions of every engine module and pak. Attach that file to a bug report; it usually
identifies the problem immediately.

Two flags exist if a fix causes trouble on your setup:

| Flag | Effect |
|---|---|
| `-noborderless` | leave the window alone, use whatever mode the game picks |
| `-keepintro` | keep the intro videos and the debug overlay |

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

> Note: this is listed as an open item in [c2-launcher](https://github.com/ItsNiklas/c2-launcher)'s
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

### 3. The x64 heap truncation crash

Retail `CrySystem.dll` contains a bucket allocator that stores slab addresses **truncated to 32
bits**. In a 64-bit process whose heap lands above the 4 GB line, those pointers are silently
corrupted. The editor does not hit this because its primary CRT is `msvcr90`, which places the
heap low.

The launcher reproduces that condition on purpose: it is built with the **VC90 compiler from WDK
7.1** and links `msvcr90` as the primary CRT (`/MD`), so the process heap lands below 4 GB and the
truncation becomes harmless. This is why `build.ps1` uses the WDK toolchain rather than a modern
MSVC - it is load-bearing, not legacy.

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

### 6. The invisible cursor

The game loads its cursor with `LoadCursorA` against the running executable, which with this
project is the launcher rather than `Crysis2.exe`. The original executable carries those cursor
resources; a launcher built without them makes `LoadCursorA` return NULL, and the cursor
disappears. The mouse still works: menu buttons highlight on hover, clicks land where they should,
but nothing is drawn under the pointer.

The launcher links the same resources under the same ids the game asks for (103-107, the amber,
blue, green, red and white Crysis cursors). Those are Crytek assets, so they are not stored here.
`extract_cursors.py` pulls them out of the `Crysis2.exe` of the installed game at build time, and
`build.ps1` runs it automatically.

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
6. **Creates the engine** through `CreateSystemInterface`, in the same order the editor uses.
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

## Building

You need:

- **WDK 7.1** for the VC90 x64 compiler. This is not nostalgia: the launcher has to link against
  msvcr90 as its primary CRT so the process heap lands below the 4 GB line, or retail CrySystem's
  truncated slab pointers come back corrupt. See the heap section above.
- **Windows 10 SDK** for `rc.exe` and `mt.exe`, the resource compiler and manifest tool.
- **An installed copy of the game**, because the build extracts cursor resources from its
  `Crysis2.exe`.

Edit the paths at the top of `build.ps1` to match your install, then run it:

```powershell
.\build.ps1
```

The script generates `CrySystem.lib` from `CrySystem.def`, extracts the cursors, compiles the
resource script, builds `Main_min.cpp`, embeds the VC90 CRT manifest, and copies the result into
the game's `Bin64`.

Two files are deliberately absent from this repository and produced at build time instead:
`CrySystem.lib`, which is derived from the game's own DLL, and the cursor `.cur` files, which are
Crytek assets. Neither is ours to distribute.

## Known issues

| Issue | Notes |
|---|---|
| Intro videos do not decode | Skipped by default; the decoder itself is unfixed. |
| Co-op dialogue lines cut off and repeat | Not game-breaking. |

## Diagnostics

On every start the launcher writes `launcher_diag.txt` next to the game, before engine init, so
the file exists even if the engine dies on startup. It records the launcher build, the command
line, whether the timer fix and borderless mode actually applied, OS build, CPU thread count,
RAM, desktop mode and refresh rate, GPU name, DPI scale, the size and date of every engine module,
and the size of every game pak.

The pak list matters more than it looks: bug reports that appear to be launcher problems often
turn out to be differences between game copies (retail versus a repack with re-encoded or removed
files). Comparing pak sizes settles that in seconds instead of a debugging session.

Nothing user-identifying is collected: no user name, no profile paths, no serials, no network
information.

## Credits

- **[c1-launcher](https://github.com/ccomrade/c1-launcher)** by *ccomrade* - the original
  open-source Crysis launcher, and the reference that showed this approach is viable.
- **[c2-launcher](https://github.com/ItsNiklas/c2-launcher)** - the CryAction assert-patch
  addresses used here were taken from that project.

This launcher's source is written from scratch (STL-free, so it can be compiled by the VC90
toolchain); it is not a fork of either project. 

## License

MIT - see [LICENSE](LICENSE).
