# crysis2-64bit

A 64-bit game launcher for **Crysis 2**.

Crysis 2 shipped with a 32-bit game executable only. The Crysis 2 Mod SDK, however, ships a
complete set of **64-bit engine DLLs** - they were built for the Sandbox editor, and no 64-bit
game client was ever released. This launcher boots those x64 DLLs as a playable game client.

The game is completable start to finish on this launcher (verified by an external tester).

> **Status:** working, but rough around the edges. See [Known issues](#known-issues).

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
overlay whose status line ends in `DevMode`. A tester reported this as "64bit comes with full
debug stuff activated". It turned out not to need any reversing: one CVar switches it off.

The startup logos also render as white rectangles on black, because the x64 build fails to decode
the intro videos. The files themselves are present and `Videos.pak` is intact, so this is a
decoder problem rather than missing content. Rather than chase the decoder, the launcher skips
the intro.

Both are set from the launcher's command line at startup:

```
+g_skipIntro 1 +sys_rendersplashscreen 0 +sys_intromoviesduringinit 0 +r_DisplayInfo 0
```

Pass `-keepintro` to restore the original behaviour, or set the CVars from the console.

## Requirements

You need a legitimate copy of **Crysis 2** and the **Crysis 2 Mod SDK**. This repository contains
no game files, no engine DLLs and no Crytek headers - only launcher source.

## Building

```powershell
# Requires WDK 7.1 (for the VC90 x64 compiler) at C:\WinDDK\7600.16385.1
.\build.ps1
```

`build.ps1` compiles `Main_min.cpp`, embeds the VC90 CRT manifest and copies the result into the
game's `Bin64`. Adjust the paths at the top of the script to match your install.

## Running

Place `launcher64.exe` in the game's `Bin64` directory (next to the x64 engine DLLs from the Mod
SDK) and run it.

---

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
