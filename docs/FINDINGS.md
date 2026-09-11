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
| Fullscreen / DWM composition | windowed vs fullscreen | 64 FPS in **both** - kills the DWM theory |
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
a quantum produces two states - frames that hit the wait pay a full quantum, frames that don't
run free. Any explanation that doesn't account for the *missing middle* is the wrong explanation.

### Side effect worth knowing

Once the lock is gone the framerate exceeds the refresh rate, so tearing appears. That is not a
regression from the fix - it is the normal consequence of an uncapped framerate, and re-enabling
VSync now holds the refresh rate solidly because the headroom is real.

## The x64 heap truncation

Retail `CrySystem.dll` keeps the head of its bucket allocator's free-page list in a global that
every read treats as 64 bits wide and exactly one write treats as 32:

```
RVA 0x0A16F1   read    64-bit
RVA 0x0A1715   read    64-bit
RVA 0x0A1A30   read    64-bit
RVA 0x0A1A49   WRITE   32-bit      89 2D E9 78 65 00    mov dword ptr [rip+0x6578E9], ebp
```

In a 64-bit process this corrupts any pointer above the 4 GB line.

Finding it is a scan rather than a read-through: walk `.text` for RIP-relative accesses, group
them by target address, and look for globals written 32 bits wide and read 64. That scan also
produces false positives - ordinary 32-bit fields that the compiler reads in pairs - so the
deciding evidence is where the stored value came from. If the source register was loaded by a
32-bit read, nothing is being truncated. If it was used as a base for addressing, it is a pointer.

The store is one REX.W prefix short of being correct, and the instruction before it is a dead
reload of a register the next instruction overwrites anyway:

```
0x0A1A3F   48 89 44 24 40   mov [rsp+40h], rax
0x0A1A44   48 8B 44 24 40   mov rax, [rsp+40h]    <- dead
0x0A1A49   89 2D ...        mov [rip+...], ebp    <- 32-bit store of a 64-bit pointer
```

Padding out the reload frees the byte the prefix needs, and because the store still ends at the
same address its displacement does not change. That is `-enginefix`, and it is off by default -
see below for why.

The editor survives it because its primary CRT is `msvcr90`, whose heap lands low. Building the
launcher with the VC90 compiler from WDK 7.1 and linking `msvcr90` as the primary CRT (`/MD`)
reproduces that layout, and the truncation becomes harmless.

This constrains the whole project: the launcher must stay STL-free, because the WDK's bundled STL
does not compile on its own. `cry_min.h` exists for that reason - it declares just enough of the
engine interfaces to boot the system, with the struct layouts matching what retail `CrySystem.dll`
expects.

## Runtime patches applied at startup

**CryAction release asserts** (addresses from c2-launcher, CryAction 1.1.1.217) - the Bin64 build
force-crashes on asserts along the `CLevelSystem::LoadLevel` path. Patched to jumps in memory
before the game DLL initialises.

**CMovieSystem use-after-free** - unloading a layer during a cutscene precache leaves dangling
descriptors in the movie update list. Cheap inline guards catch null vtables, but a pointer into
an unmapped hole passes them and faults on dereference. A vectored exception handler catches the
fault inside the guarded code range and advances the loop to the next element. Note it must catch
*any* exception code, not just `0xC0000005` - a bad pointer landing in a guard page raises
`STATUS_GUARD_PAGE_VIOLATION` instead.

## Debugging a crash that only happens on other people's machines

The startup failure `Failed CMTSafeHeap::m_pBigPool allocation` was reported by several testers
and could not be reproduced here. Three tools turned that into something workable, and they are
worth more than the bug they caught.

**Reproduce the condition, not the crash.** The failure mode depends on where memory lands, so
the launcher can put memory where it needs to be: `-forcehighheap` walks the address space with
`VirtualQuery` and reserves every free region below the 4 GB line before engine init. The heap is
then forced above it. What took a shipped build and a day of waiting now takes twenty seconds.

**Watch the allocator instead of guessing at it.** `CrySystem` calls its allocation entry point
through a pointer in a table it fills lazily from `CrySystem+0x369E0`. That function takes no
arguments, so calling it first populates the table, after which the entry at `CrySystem+0x6EF6A8`
can be swapped for a proxy without racing anyone:

```c
typedef void* (*CryMallocFn)(size_t size, size_t* allocated);
```

`-traceallocs` installs such a proxy and logs every large request and the address it got. That
log is what showed the pools were being allocated successfully, which ruled out the entire class
of "ran out of memory" explanations in one run.

**Catch the fault where it happens.** A truncated pointer addresses the low 4 GB, which in a
64-bit process is almost always unmapped. So a vectored exception handler that records access
violations on low addresses names the faulting instruction directly. It also reports any register
whose low half matches the faulting address but whose top half is intact - that register is the
original pointer, and it names what was truncated on the way in.

### Two things not to do

**Do not patch the pak heap constructor.** It writes fifteen pool sizes and then loops sixteen
times (`lea ebp,[r13+10h]` at `0x0A221D`, counted down at `0x0A229D`). The sixteenth pass reads a
size of zero, skips the allocation, and the check right after treats the null pointer as failure.
That looks like an obvious off-by-one, and it is not: supplying the missing size, writing it
through a different register, and shortening the loop all produce the same result - the renderer
comes up with an 8x8 window and dies. The sixteenth slot is not a pool pointer; it is the next
field of the object. Leave the constructor alone.

**Do not measure "the error message is gone".** A build can stop logging that message and still
be broken, and it will look like a fix for as long as you keep measuring the wrong thing. The
useful verdict is whether the game reached its menu: a window that is not 8x8, a
`D3D11 CryRender Stats` block in the log, and a process holding more than 500 MB. Runs are also
not deterministic, so a single pass proves nothing - three passes per variant is the minimum that
distinguishes a fix from luck.

### The method that actually worked

Bisect forward from something that works, not backward from something that does not.

Going backward, every removed piece produces a new theory, and none of them is ever tested
against a known-good baseline. Going forward - take the last build that reaches the menu, add one
thing, run it three times, repeat - each step has a verdict that means something. The cause here
turned out to be a single line missing from the launcher's own startup path, which no amount of
reverse engineering the engine would have found.

## Two things looked broken and are not

**"0 Mb of video memory is available".** The renderer reports zero video memory on a card with
12 GB, and follows it with "Disabling of textures streaming...", which reads alarming. It is not
the disable it looks like: that message is printed unconditionally as part of reinitialising the
texture manager, and "Finished initializing textures streaming..." follows a few lines later.

The zero is real, though. The figure comes from a field of the renderer object read at
`RVA 0x1CCE69` (`mov r8, [rax+0F328h]`), shifted right by 20 to get megabytes, and nothing in
`CryRenderD3D11` ever writes that field - so whatever fills it is either absent in this build or
lives elsewhere. There is a size check on it at `RVA 0x1CCEEC` against 256 MB which clamps some
atlas sizes, and forcing that branch the other way changes nothing observable: same atlas figures,
same log. So the zero is cosmetic as far as could be measured. `r_TexturesStreamPoolSize` is
already 1024 and `r_TexturesStreaming 1` on the command line changes nothing either.

**The intro videos.** The decoder is not missing. Retail x64 `CrySystem.dll` carries a complete
64-bit CRI Sofdec build:

```
CRI Movie/PCx64 Ver.2.68   Build:Jan 18 2011
CRI Heap/PCx64 Ver.1.21.02
E09031802M: Need to call CriMv::Initialize() before CriMvEasyPlayer::Create().
E08052300H: CRI Heap is not initialized.
Decode USM header timeout. Video file could be broken.
```

Those error strings are the thread to pull on: if the white rectangles came from a decoder that
was never initialised, one of them would appear in `Game.log` when a video is requested.

None of them does. Playing through to the first in-game cutscene on Battery Park produces a clean
run through the decoder:

```
PlayVideo: ... Video '/videos/fb1.usm' ...
Disable scene rendering for playback of FMV sequence...
... re-enable scene rendering after playback of FMV sequence.
```

Seventy-one seconds between those two lines, which is the length of the video, and not one CRI
error in between. So the decoder runs to completion and the engine believes the video played.
Whether anything was actually visible on screen is the part that still needs a pair of eyes -
that distinction matters, because "no picture" and "no playback" have completely different causes
and only the second one is what everybody assumed was happening.
