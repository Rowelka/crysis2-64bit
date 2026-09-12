# Known open items

What is reported, what is understood about each, and what it would take. Kept honest: an item
sits here until it is fixed, not until it is explained.

## Closed

### Random crash in the 64-bit renderer, minutes into a level

Closed 11.09.2026. The renderer caches D3D11 constant buffers in arrays indexed by the buffer's
size in vectors, sized 512 / 128 / 128 by buffer type (the switch at `CryRenderD3D11` RVA
`0x05A93A`, jump table at `0x05A9FC`). Nothing checks that the requested size fits the array:
the entry point at `0x036E10` verifies only that `offset + count` fits the buffer itself.

A skinning buffer asks for 224 vectors - about seventy-four bones - so for buffer type 1 the
renderer reads entry 224 of a 128-entry array, takes whatever lies past its end for an
`ID3D11Buffer`, and hands it to `Map`. Depending on what is there, the process either faults
inside `d3d11.dll` at `+0x172FEB` reading `[rdx+0xC9]`, or faults on the read itself at
`CryRenderD3D11+0x036F0E`. The main thread is left waiting on the render thread, so the engine's
own watchdog reports `Runaway thread` and kills the process - which is what made this look like a
hang rather than a crash.

This explains the "crashes sometimes, usually after a few minutes" reports: it depends on which
characters are on screen. Measured over 105-second runs of TimesSquare, one run in two died.

The launcher now raises every array to 1024 entries before the table is built, which costs
240 KB and covers every size the engine can request. On by default; `-nocbfix` disables it.

## From testers

### Low framerate in exclusive fullscreen

Reported by ErBuSlayer. The 64 FPS lock is fixed (see README, section 1) and the launcher runs
borderless by default, where the fix shows fully. Exclusive fullscreen is a separate case and is
still slow.

What is known: the renderer has no refresh-rate control at all. Scanning `CryRenderD3D11.dll`
for anything refresh-related turns up exactly two console variables, `r_Fullscreen` and
`r_VSync`, and nothing else. So in exclusive fullscreen DXGI picks the mode itself, hands the
engine 60 Hz, and with VSync on the game is pinned to 60 with the input lag that comes with it.

What it would take: intercepting swapchain creation and supplying the display's actual mode,
rather than letting DXGI choose. That is a hook on the DXGI factory, not a CVar - real work, and
it has to be proven on a high-refresh display, which is not the machine this is developed on.

Until then `-noborderless` exists for anyone who wants the old behaviour, and borderless is the
better mode anyway: no tearing, no VSync lag, no Alt-Tab crash.

### Stuttering on the loading screen

Reported by ErBuSlayer, who notes the remaster fixed it. Not investigated yet. The likely area
is resource streaming during level load - the engine reports its own idle and busy times for the
render thread during loading, so there is a measurement to start from rather than a guess.

### Multiplayer crashes after `net_setonlinemode LAN`

Reported by ErBuSlayer. `net_setonlinemode` lives in the retail game DLL, and both `CryNetwork`
and `CryAction` are built against **GameSpy**, which was shut down in 2014. So multiplayer has
no working backend regardless of what the launcher does.

Worth doing anyway: not crashing. A crash tells the player nothing; a message saying the service
is gone tells them everything. Needs the crash located first.

### Resolution reads "undefined" in the 64-bit client

Reported by ErBuSlayer, who tested the same thing on a second machine and with the older
c2-launcher to separate it from this one. In the 64-bit client the graphics menu shows the
resolution as `undefined` and the game does not pick a mode by itself; with DX11 on, the older
launcher gives a small mis-sized window. The 32-bit client sets the resolution automatically and
does not. DXVK on 64-bit fixes it, which points at the D3D path rather than at the menu.

So this is a 64-bit renderer issue Crytek left behind, not something the launcher introduced:
our build does go fullscreen, but the menu still reads `undefined`. Worth pairing with the
refresh-rate item above, since both end in the same place - taking over mode selection instead
of leaving it to the renderer. c1-launcher has a patch for the sibling bug in Crysis 1
(ccomrade/c1-launcher, commit f205939, "fixes the low refresh rate bug in DX10 mode") and is
worth reading before writing our own.

## Open from our own testing

### Weapon effects stop appearing

Muzzle flashes and tracers show for the first couple of shots and then stop, both for the player
and for AI. Confirmed by the user in normal play, so it is not caused by any diagnostic flag.

Ruled out so far:

- **our cutscene workarounds** - counters were added to all seven places where the launcher's
  exception handler skips an element of a sequence. Six and a half minutes of the intro: zero
  hits. Nothing is being dropped there. `-moviestats` writes those counters to `movie_skips.txt`.
- **our destruction experiments** - `g_breakImpulseScale` and `g_breakage_particles_limit` were
  returned to stock values and the effects still did not come back.
- **missing effect assets** - the log names seven effects it cannot find across a whole level,
  none of them weapon-related.

So the effects are not failing, they are not being created, and nothing is logged when that
happens. A silent refusal. Next step is tracing the creation path rather than guessing again.

12.09: two things worth trying before that trace. Every level log carries the line

    Allocate render buffer for particles (16384 verts, 32768 tris)

which is room for about four thousand particles on screen at once - and the engine draws debris,
smoke and tracers from the same buffer. The pools behind it come from the console build too:
`e_ParticlesPoolSize` and `e_ParticlesEmitterPoolSize`. A pool with no free emitter left would
refuse to create an effect exactly like this: silently. `bigpools.cfg` raises both, so the test
is one run with `+exec bigpools.cfg` and a look at whether tracers come back.

### Does the pointer truncation actually matter

**Answered, 12.09.2026: yes, and there was far more of it than one site.**

The old criterion here was "the game runs for hours under `-forcehighheap` exactly as it does
without it". That flag could never settle it - it reserves all the low address space, which
breaks the renderer whether the pointers are correct or not.

`-topdown` settles it instead. It hooks `NtAllocateVirtualMemory` in ntdll and serves large
reservations from 8 GB up, so a lost upper half is fatal on demand. Hooking `VirtualAlloc`
through the import tables catches nothing, because the engine takes its memory through the CRT
and the CRT goes straight to ntdll.

What it found, in one evening:

* The bucket allocator is not one piece of code. **Every module carries its own compiled copy**,
  with its own globals. CrySoundSystem, CryRenderD3D11 and CryRenderD3D9 each have the same four
  globals written 32 bits wide and read 64, the same list walk reading the next block's address
  with half of it missing - eight places each.
* CryScriptSystem's Lua pool swaps its free-list heads with a **32-bit compare-and-exchange**
  while the code around it is already 64-bit. Eight places, in three shapes.
* `-modfix` widens all of them: one REX bit where the length allows, a trampoline where the
  64-bit form is longer. It refuses to touch an allocator that has already served a block.

Measured, playing:

| | below 4 GB | above 4 GB |
|---|---|---|
| without `-topdown` | 1831 MB | **0 MB** |
| with `-topdown -modfix` | 1645 MB | **622 MB (27%)** |

Before this, the 64-bit build never put a single byte above the 4 GB line - it was 64-bit in
file format only. With the corrections in, TimesSquare, Downtown and CentralStation each load in
their usual time and play with no exceptions at all.

**Still open:** the other 73 percent. Most of the game's memory comes from the process heap,
which reserved its first regions before the launcher could hook anything, and from file mappings
for the .pak archives. Raising that share is what remains, along with running the whole campaign
this way rather than three levels.

## Cosmetic

- The intro logos render as white rectangles. The decoder is not the problem: an in-level
  cutscene plays for its full 71 seconds with no CRI error at all, so the fault is in presenting
  a frame. Skipped by default; `-keepintro` restores them.
- The engine reports 0 MB of video memory. Real, but no consequence could be measured.
- Co-op dialogue lines cut off and repeat.
