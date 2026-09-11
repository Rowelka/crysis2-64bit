# Known open items

What is reported, what is understood about each, and what it would take. Kept honest: an item
sits here until it is fixed, not until it is explained.

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

### Does the pointer truncation actually matter

The engine's allocator stores one pointer with a 32-bit write while every read of it is 64-bit.
The bug is real and the site is known. What is **not** established is whether it ever bites.

Measured so far: with `-forcehighheap`, which forces the heap above the 4 GB line where the
truncated value becomes garbage, the game loads, the level builds, and entities spawn - 2315
against 2311 in a normal run, which is noise. One run out of two died after four minutes with
`Runaway thread` and no access violation recorded.

So the honest position is that the site looks dangerous and has not been shown to do damage.
Fixing something whose harm is unproven is how `-enginefix` happened, and that flag now sits
switched off because it breaks more than it helps.

The criterion for calling this closed: **the game runs for hours under `-forcehighheap` exactly
as it does without it**. Until that holds, the 32-bit legacy is alive and the current stability
rests on the CRT placing the heap low, which is luck rather than a fix.

## Cosmetic

- The intro logos render as white rectangles. The decoder is not the problem: an in-level
  cutscene plays for its full 71 seconds with no CRI error at all, so the fault is in presenting
  a frame. Skipped by default; `-keepintro` restores them.
- The engine reports 0 MB of video memory. Real, but no consequence could be measured.
- Co-op dialogue lines cut off and repeat.
