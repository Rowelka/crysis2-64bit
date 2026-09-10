# Findings

Reverse-engineering notes for the Crysis 2 x64 build. Written so the diagnostics can be
reproduced, not just the results repeated.

## Diagnosing the 64 FPS lock

The lock looks like a load problem and is not one. The decisive step is to stop guessing at
causes and measure where the frame time actually goes.

**1. Rule out the obvious suspects first.** Each of these was tested and made no difference:

| Suspect | Test | Result |
|---|---|---|
| VSync | `r_VSync 0` | no change |
| Fullscreen / DWM composition | windowed vs fullscreen | 64 FPS in **both** — kills the DWM theory |
| Engine frame cap | `sys_MaxFPS 0`, `sys_MaxFPS 300` | no change |
| Physics thread count | `p_num_threads 4` | no change |
| Physics timestep | `p_fixed_timestep 0`, `p_max_substeps 1` | no change |

**2. Read the frame breakdown.** With the engine profiler the picture is unambiguous:

```
WaitPhys   ~15.5 ms      <- main thread blocked here
Phys        ~0.1 ms      <- the physics work itself is trivial
GPU         ~5.6 ms      <- GPU is idle most of the frame
```

The main thread is not computing anything for 15.5 ms. It is *waiting*. That reframes the
question from "what is slow?" to "why does a wait cost exactly 15.5 ms?"

**3. Recognise the number.** 15.625 ms is the default Windows timer quantum, and
`1000 / 15.625 = 64.0`. A wait that would otherwise take microseconds gets rounded up to a full
quantum, so one frame costs one quantum.

**4. Confirm nobody raises the resolution.** Parse the PE import tables:

```
Crysis2.exe (retail 32-bit)  ->  WINMM.dll: [timeGetTime]
Editor.exe                   ->  WINMM.dll: [timeGetTime]
CrySystem.dll (x64)          ->  WINMM.dll: [timeGetTime]
launcher64.exe               ->  (no WINMM import at all)
```

`timeBeginPeriod` appears in none of them, and grepping the binaries for the string finds no
dynamic resolution either. The engine reads time but never asks for a finer timer.

**5. Fix and verify.** `timeBeginPeriod(1)` before engine init: a hard 64 FPS becomes 270-330
FPS on an RTX 3060 at 1080p.

### Why the bimodal 64-or-300 behaviour

Players reported the framerate either pinned at 64 or spiking to 200-300, with nothing in
between. That gap is the tell. Load-related limits produce a continuous spread of framerates;
a quantum produces two states — frames that hit the wait pay a full quantum, frames that don't
run free. Any explanation that doesn't account for the *missing middle* is the wrong explanation.

### Side effect worth knowing

Once the lock is gone the framerate exceeds the refresh rate, so tearing appears. That is not a
regression from the fix — it is the normal consequence of an uncapped framerate, and re-enabling
VSync now holds the refresh rate solidly because the headroom is real.

## The x64 heap truncation

Retail `CrySystem.dll` has a bucket allocator that stores slab addresses truncated to 32 bits. In
a 64-bit process this corrupts any pointer above the 4 GB line.

The editor survives it because its primary CRT is `msvcr90`, whose heap lands low. Building the
launcher with the VC90 compiler from WDK 7.1 and linking `msvcr90` as the primary CRT (`/MD`)
reproduces that layout, and the truncation becomes harmless.

This constrains the whole project: the launcher must stay STL-free, because the WDK's bundled STL
does not compile on its own. `cry_min.h` exists for that reason — it declares just enough of the
engine interfaces to boot the system, with the struct layouts matching what retail `CrySystem.dll`
expects.

## Runtime patches applied at startup

**CryAction release asserts** (addresses from c2-launcher, CryAction 1.1.1.217) — the Bin64 build
force-crashes on asserts along the `CLevelSystem::LoadLevel` path. Patched to jumps in memory
before the game DLL initialises.

**CMovieSystem use-after-free** — unloading a layer during a cutscene precache leaves dangling
descriptors in the movie update list. Cheap inline guards catch null vtables, but a pointer into
an unmapped hole passes them and faults on dereference. A vectored exception handler catches the
fault inside the guarded code range and advances the loop to the next element. Note it must catch
*any* exception code, not just `0xC0000005` — a bad pointer landing in a guard page raises
`STATUS_GUARD_PAGE_VIOLATION` instead.
