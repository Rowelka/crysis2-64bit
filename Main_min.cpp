// A 64-bit game launcher for Crysis 2, booting the x64 engine DLLs from the Mod SDK.
//
// It is deliberately STL-free and built with the VC90 compiler from WDK 7.1. That is not
// legacy baggage: retail CrySystem contains a bucket allocator that stores slab addresses
// truncated to 32 bits, so in a 64-bit process whose heap sits above the 4 GB line those
// pointers get corrupted. Building against msvcr90 as the primary CRT (/MD) puts the heap
// low, exactly where it lands in the editor, and the truncation becomes harmless.
// CrySystem is imported statically so it loads alongside msvcr90 in the right order.
//
// A few pieces can be compiled out, which is how the cause of a startup failure was narrowed
// down once: build variants that differ by exactly one thing, run each three times, and compare.
// Working forward from a build that starts, rather than backward from one that does not, is what
// makes that useful - see docs/FINDINGS.md.
//
//     /DNO_DIAG          skip the diagnostic report
//     /DNO_TIMER_LOAD    do not load WINMM or raise the timer resolution
//     /DNO_DETECTOR      do not install the access-violation handler
//     /DNO_SLABFIX       leave out the engine pointer-width patch entirely
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "cry_min.h"

// Declared locally rather than including psapi.h, to avoid adding a static import - see the
// note above LoadTimerApi for why the launcher's import list must stay minimal.
typedef struct _MODULEINFO_LOCAL {
	LPVOID lpBaseOfDll;
	DWORD  SizeOfImage;
	LPVOID EntryPoint;
} MODULEINFO;

// VC90 CRT side-by-side dependency. /MD adds it as well; stated explicitly to be safe.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.VC90.CRT' version='9.0.21022.8' processorArchitecture='amd64' publicKeyToken='1fc8b3b9a1e18e3b'\"")

static void SetCwdToGameRoot()
{
	char exePath[MAX_PATH];
	GetModuleFileNameA(NULL, exePath, MAX_PATH);
	char* p = strrchr(exePath, '\\'); if (p) *p = 0;   // .../Bin64/launcher64.exe -> .../Bin64
	p = strrchr(exePath, '\\'); if (p) *p = 0;          // .../Bin64 -> the game root
	SetCurrentDirectoryA(exePath);
}

// Patch a single byte in memory. Used to defuse the CryAction release asserts below.
static void PatchByte(unsigned char* addr, unsigned char val)
{
	DWORD oldProt = 0;
	if (VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
		*addr = val;
		VirtualProtect(addr, 1, oldProt, &oldProt);
	}
}

// Vectored exception handler covering the CryMovie fixes below.
//
// Unloading a layer while a cutscene is precaching leaves dangling entries in CryMovie's
// structures: the memory gets reused, so vtable pointers end up addressing the heap, freed
// shader data, or unmapped holes. The inline guards in section 1c reject obvious garbage
// cheaply, but a pointer whose high bits are zero and which happens to address an unmapped
// page passes them and faults on dereference.
//
// Instead of crashing, each case below resumes inside the original function at a point that
// skips the corrupt element and continues iterating. The worst outcome is a broken node
// losing its animation.
//
// Note that it catches any exception code while RIP is inside the code cave, not just
// 0xC0000005: a bad pointer landing in a guard page raises STATUS_GUARD_PAGE_VIOLATION
// instead, and nothing in the cave can fault except that dereference.
static unsigned char*      g_movieCave = 0;
static unsigned long long  g_movieRetSkip = 0;
// Reallocation-aware iteration of CMovieSystem::Update.
//
// The original iterates a std::vector by iterator while the loop body can grow that vector:
// Animate() starts nested sequences, which push_back into m_playingSequences and reallocate
// it, leaving the iterator pointing into the freed buffer. (Crytek's own source acknowledges
// this - Movie.cpp carries the comment that Animate can invalidate the iterator.)
//
// This holds the begin pointer seen on the previous iteration; when the trampoline notices
// that begin has moved, it rebases the iterator into the new buffer and carries on.
static unsigned long long  g_movieOldBegin = 0;
// Module base of CryMovie.dll, used by the handler above to locate its patched ranges.
static unsigned long long  g_cryMovieBase = 0;
// How often each of the workarounds below had to fire, indexed A..G in their own order.
//
// These are not a curiosity. Every hit is an element of a cutscene that was skipped, so a
// non-zero count means the scene on screen is missing something it was supposed to show - a
// node, a track, a key. A sequence that plays correctly leaves all seven at zero. -moviestats
// writes them to movie_skips.txt while the game runs.
static volatile LONG g_movieSkips[7] = { 0, 0, 0, 0, 0, 0, 0 };
static const char* const kMovieSkipNames[7] = {
	"A element unmapped in the update loop",
	"B virtual call from the update loop went astray",
	"C track key accessor read past its array",
	"D node hierarchy walk hit a reused object",
	"E node hierarchy virtual call landed on data",
	"F sequence node skipped (reused memory)",
	"G track reported as empty (freed under precache)"
};

static LONG CALLBACK MovieVEH(EXCEPTION_POINTERS* ep)
{
	if (ep && ep->ExceptionRecord && g_movieCave) {
		unsigned long long caveLo = (unsigned long long)g_movieCave;
		unsigned long long caveHi = caveLo + 128;
		unsigned long long rip = (unsigned long long)ep->ContextRecord->Rip;
		// A: faulted inside the cave itself, dereferencing an element pointer that was unmapped.
		if (rip >= caveLo && rip < caveHi) {
			InterlockedIncrement(&g_movieSkips[0]);
			ep->ContextRecord->Rip = g_movieRetSkip;   // skip this element, continue the loop
			return EXCEPTION_CONTINUE_EXECUTION;
		}
		// B: the virtual call jumped to a garbage or null target, so RIP is outside the cave -
		// but the call had already pushed its return address, which still points into it.
		// Recognise that, pop the return address and skip the element.
		if (ep->ExceptionRecord->ExceptionCode == 0xC0000005) {
			unsigned long long rsp = (unsigned long long)ep->ContextRecord->Rsp;
			unsigned long long ret = *(unsigned long long*)rsp;
			if (ret >= caveLo && ret < caveHi) {
				ep->ContextRecord->Rsp = rsp + 8;         // drop the failed call's return address
				InterlockedIncrement(&g_movieSkips[1]);
				ep->ContextRecord->Rip = g_movieRetSkip;  // skip this element
				return EXCEPTION_CONTINUE_EXECUTION;
			}
		}
		// C: the track key accessor read past its array because the index was corrupt. Return
		// 0.0f and continue. This only fires on the fault itself; valid calls are untouched.
		if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 && g_cryMovieBase) {
			unsigned long long accLo = g_cryMovieBase + 0x2B5F0;
			unsigned long long accHi = g_cryMovieBase + 0x2B604;
			if (rip >= accLo && rip < accHi) {
				ep->ContextRecord->Xmm0.Low = 0; ep->ContextRecord->Xmm0.High = 0;  // return 0.0f
				InterlockedIncrement(&g_movieSkips[2]);
				ep->ContextRecord->Rip = g_cryMovieBase + 0x2B604;                    // ret
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			// D: walking the node hierarchy reached a sub-object whose memory was reused, leaving a
			// garbage vtable. Leave through the function's own "not found" exit, which unwinds
			// the stack correctly.
			unsigned long long h_lo = g_cryMovieBase + 0x3A630;
			unsigned long long h_hi = g_cryMovieBase + 0x3A69B;
			if (rip >= h_lo && rip < h_hi) {
				InterlockedIncrement(&g_movieSkips[3]);
				ep->ContextRecord->Rip = g_cryMovieBase + 0x3A67A;   // xor eax,eax; add rsp,0x20; pop rbx; ret
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			// E: the same walk, but the virtual call landed on data rather than code, so RIP is
			// outside the function entirely. Recognise it by the return address the call pushed,
			// then leave the same way as D.
			if (!(rip >= h_lo && rip < h_hi)) {
				unsigned long long rsp = (unsigned long long)ep->ContextRecord->Rsp;
				unsigned long long ret = *(unsigned long long*)rsp;
				if (ret >= h_lo && ret < h_hi) {
					ep->ContextRecord->Rsp = rsp + 8;                    // drop the failed call's return
					InterlockedIncrement(&g_movieSkips[4]);
					ep->ContextRecord->Rip = g_cryMovieBase + 0x3A67A;   // leave as "not found"
					return EXCEPTION_CONTINUE_EXECUTION;
				}
			}
			// F: walking a sequence's node vector, one node's memory had been reused, so its vtable
			// points into the heap and the virtual call goes astray. Skip that node; the rest of the
			// sequence survives, since the fault happens before any state is modified.
			if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 && g_cryMovieBase) {
				unsigned long long fl = g_cryMovieBase + 0x2104;
				unsigned long long fh = g_cryMovieBase + 0x2148;
				if (rip >= fl && rip < fh) {                              // faulted inside the loop body
					InterlockedIncrement(&g_movieSkips[5]);
					ep->ContextRecord->Rip = g_cryMovieBase + 0x213E;    // skip this node, keep iterating
					return EXCEPTION_CONTINUE_EXECUTION;
				} else {                                                 // the call jumped to data
					unsigned long long rsp = (unsigned long long)ep->ContextRecord->Rsp;
					unsigned long long ret = *(unsigned long long*)rsp;
					if (ret >= fl && ret < fh) {
						ep->ContextRecord->Rsp = rsp + 8;                // drop the failed call's return
						InterlockedIncrement(&g_movieSkips[5]);
						ep->ContextRecord->Rip = g_cryMovieBase + 0x213E; // skip this node
						return EXCEPTION_CONTINUE_EXECUTION;
					}
				}
			}
			// G: a node's track was freed and its memory handed to the cutscene's shader precache,
			// so reading the track faults. Emulate "this track has no keys" and let the function
			// take its own path to the next one - counters and registers stay consistent.
			if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 && g_cryMovieBase) {
				unsigned long long gl = g_cryMovieBase + 0x5AE4F;
				unsigned long long gh = g_cryMovieBase + 0x5AE5C;
				if (rip >= gl && rip < gh) {                              // faulted reading the track
					ep->ContextRecord->Rax = 0;                          // report zero keys
					InterlockedIncrement(&g_movieSkips[6]);
					ep->ContextRecord->Rip = g_cryMovieBase + 0x5AE5C;   // → je 0x5bacf → inc r14 → next track
					return EXCEPTION_CONTINUE_EXECUTION;
				} else {                                                 // the call jumped to data
					unsigned long long rsp = (unsigned long long)ep->ContextRecord->Rsp;
					unsigned long long ret = *(unsigned long long*)rsp;
					if (ret >= gl && ret < gh) {
						ep->ContextRecord->Rsp = rsp + 8;                // drop the failed call's return
						ep->ContextRecord->Rax = 0;
						InterlockedIncrement(&g_movieSkips[6]);
						ep->ContextRecord->Rip = g_cryMovieBase + 0x5AE5C;
						return EXCEPTION_CONTINUE_EXECUTION;
					}
				}
			}
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// The ~64 FPS ceiling of the x64 build is the Windows timer quantum, not load.
//
// Nobody raises the timer resolution: Crysis2.exe, Editor.exe and CrySystem.dll all import
// only timeGetTime from WINMM, never timeBeginPeriod. The default resolution is 15.625 ms,
// and 1000 / 15.625 = 64.0. The main thread waits on the physics barrier (measured at about
// 15.5 ms per frame, with physics itself at 0.1 ms and the GPU at 5.6 ms - the hardware is
// idle), and Windows rounds that wait up to a full scheduler quantum, so one frame costs one
// quantum.
//
// It also explains the bimodal behaviour players reported: either pinned at 64 or spiking to
// 200-300, with nothing in between. Frames that skip the wait run free; frames that hit it
// pay a whole quantum.
//
// Since Windows 10 2004 the call is per-process, so this affects only the game.
// WINMM is loaded at runtime rather than imported statically, and this is load-bearing.
// A static import is resolved by the loader before any of our code runs, pulling WINMM and its
// dependencies in ahead of the CRT and shifting the process address space. That defeats the whole
// point of building against msvcr90 (see the note at the top of this file): the heap has to land
// below the 4 GB line, or retail CrySystem's truncated slab pointers come back corrupt and the
// engine dies with "Failed CMTSafeHeap::m_pBigPool allocation" before it finishes starting.
// Whether this matters depends on how a given system lays out the address space, so it can
// hold on one machine and fail on another.
typedef unsigned (__stdcall *TimePeriodFn)(unsigned);
static TimePeriodFn g_timeBeginPeriod = 0;
static TimePeriodFn g_timeEndPeriod   = 0;

static void LoadTimerApi()
{
	HMODULE winmm = LoadLibraryA("winmm.dll");
	if (!winmm) return;
	g_timeBeginPeriod = (TimePeriodFn)GetProcAddress(winmm, "timeBeginPeriod");
	g_timeEndPeriod   = (TimePeriodFn)GetProcAddress(winmm, "timeEndPeriod");
}

// Borderless windowed fullscreen.
//
// With the framerate unlocked, exclusive fullscreen tears. VSync is not a good answer: the
// engine has no refresh-rate CVar at all (CryRenderD3D11 exposes only r_Fullscreen and
// r_VSync), so DXGI hands it 60 Hz and VSync pins the game to 60 FPS with input lag, even on
// a 165 Hz display.
//
// Windowed mode behaves differently: frames go through the Windows compositor, which
// synchronises presentation itself. Tearing is impossible by design, the framerate stays
// unlocked, and there is no VSync input lag. So the launcher runs the game in a borderless
// window sized to the screen - it looks like fullscreen and feels like windowed.
//
// This also fixes Alt-Tab crashes: a windowed swapchain has no exclusive device to lose on a
// focus switch.
//
// If the game ends up in exclusive fullscreen anyway, its window is already WS_POPUP covering
// the screen, so the needFix check below finds nothing to do and nothing is touched.
// Disable with -noborderless.
struct SBorderlessSearch { DWORD pid; HWND found; };

static BOOL CALLBACK BorderlessEnumProc(HWND h, LPARAM lp)
{
	SBorderlessSearch* s = (SBorderlessSearch*)lp;
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid != s->pid) return TRUE;
	if (!IsWindowVisible(h)) return TRUE;
	if (GetWindow(h, GW_OWNER) != NULL) return TRUE;          // skip dialogs and splash windows
	RECT r;
	if (!GetWindowRect(h, &r)) return TRUE;
	if ((r.right - r.left) < 320 || (r.bottom - r.top) < 240) return TRUE;
	s->found = h;
	return FALSE;
}

static void ApplyBorderless(HWND h, int w, int hgt)
{
	LONG_PTR st = GetWindowLongPtrA(h, GWL_STYLE);
	st &= ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME);
	st |= WS_POPUP;
	SetWindowLongPtrA(h, GWL_STYLE, st);
	LONG_PTR ex = GetWindowLongPtrA(h, GWL_EXSTYLE);
	ex &= ~(LONG_PTR)(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
	SetWindowLongPtrA(h, GWL_EXSTYLE, ex);
	SetWindowPos(h, HWND_TOP, 0, 0, w, hgt, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
}

static DWORD WINAPI BorderlessThread(LPVOID)
{
	const int w   = GetSystemMetrics(SM_CXSCREEN);
	const int hgt = GetSystemMetrics(SM_CYSCREEN);
	bool logged = false;
	// A separate thread, never the frame loop. It keeps watching rather than applying once,
	// because the engine creates its window late and can recreate it later.
	for (int tick = 0; ; tick++)
	{
		// Poll often for the first ten seconds: the window is created during startup and half a
		// second of it having a frame is visible to the eye. After that this is only a safety net
		// for the engine recreating its window on a video mode change.
		Sleep(tick < 200 ? 50 : 500);
		SBorderlessSearch s;
		s.pid = GetCurrentProcessId();
		s.found = NULL;
		EnumWindows(BorderlessEnumProc, (LPARAM)&s);
		if (!s.found) continue;
		if (IsIconic(s.found)) continue;                       // minimised: don't fight Alt-Tab
		LONG_PTR st = GetWindowLongPtrA(s.found, GWL_STYLE);
		RECT r;
		if (!GetWindowRect(s.found, &r)) continue;
		const bool needFix = ((st & WS_CAPTION) != 0)
		                  || (r.left != 0) || (r.top != 0)
		                  || ((r.right - r.left) != w) || ((r.bottom - r.top) != hgt);
		if (!needFix) continue;
		ApplyBorderless(s.found, w, hgt);
		if (!logged) {
			FILE* f = fopen("launcher_diag.txt", "a");
			if (f) { fprintf(f, "borderless     : applied to window %p at %dx%d\n", (void*)s.found, w, hgt); fclose(f); }
			logged = true;
		}
	}
}

// Diagnostic report for testers.
//
// Startup behaviour varies between systems in ways that cannot be reproduced elsewhere: the
// same build can hit a framerate cap or an Alt-Tab crash on one machine and neither on another.
// Without a record of the hardware and of the mode the launcher started in, such reports cannot
// be acted on. This file makes them concrete.
//
// Only technical information is collected: no user name, no profile paths, no serials,
// nothing about the network.
static void DiagLine(FILE* f, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
}

// ---------------------------------------------------------------------------------------
// The pointer truncation that makes this engine build fail on some machines and not others.
//
// Retail CrySystem's bucket allocator keeps the head of its free-page list in a global.
// Every read of that global is 64-bit, but the one instruction that writes it is 32-bit:
//
//     RVA 0x0A16F1 / 0x0A1715 / 0x0A1A30   mov rXX, [rip+...]     64-bit reads
//     RVA 0x0A1A49                         mov [rip+...], ebp     32-bit write
//
// So the top half of the pointer is dropped on the way in and read back as zero. While the
// process heap happens to sit below the 4 GB line the dropped half is zero anyway and nothing
// notices. The moment one block lands above that line, the value read back addresses a low
// address that was never mapped. That is the "Failed CMTSafeHeap::m_pBigPool allocation" box
// at startup, and the access violations on unmapped low addresses once a level is loaded.
//
// Where the heap lands is decided by the layout of the address space, which is why the same
// files work on one machine and fail on the next, and why the same machine can differ between
// two runs: driver DLLs, overlays and ASLR all move the boundary.
//
// The instruction is exactly one REX.W prefix short of being correct. Immediately before it:
//
//     mov [rsp+40h], rax
//     mov rax, [rsp+40h]     <- reloads the register just stored, and the next instruction
//                               (lea rax, [rbp+80000h]) overwrites rax regardless
//
// That reload is dead. Replacing it with padding frees the byte the prefix needs, and since
// the store still ends at the same address its RIP-relative displacement stays correct.
//
// Bin64 comes from the Mod SDK and is byte-identical for everyone, so a fixed offset is safe
// here - but the patch still verifies the exact bytes and declines if they differ.
// ---------------------------------------------------------------------------------------
// Detector for truncated pointers.
//
// A pointer that lost its top half addresses somewhere in the low 4 GB, which in a 64-bit
// process is almost always unmapped. So an access violation on a low address is the signature
// of this bug, and the fault tells us exactly which instruction dereferenced it.
//
// The handler only observes: it writes a record and lets the exception continue to whoever
// would have handled it, so behaviour is unchanged. Used together with -forcehighheap, which
// makes the fault happen on demand rather than by luck, this turns "find the truncations" from
// guesswork into a loop: run, read the address, find who wrote that pointer, fix, run again.
//
// Deliberately avoids the CRT: at fault time the heap may be the very thing that is broken.
#define MAX_FAULT_RECORDS 32
static volatile long g_faultsLogged = 0;

static void AppendFaultLog(const char* text, unsigned long len)
{
	HANDLE h = CreateFileA("launcher_faults.txt", FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
	                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return;
	DWORD written = 0;
	SetFilePointer(h, 0, NULL, FILE_END);
	WriteFile(h, text, len, &written, NULL);
	CloseHandle(h);
}

// Names the module an address belongs to, without pulling in psapi: the allocation base of a
// mapped image is its module handle.
static const char* ModuleAt(ULONG_PTR addr, ULONG_PTR* rvaOut)
{
	static char path[MAX_PATH];
	MEMORY_BASIC_INFORMATION mbi;

	if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) return 0;
	if (!mbi.AllocationBase) return 0;
	if (!GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH)) return 0;

	*rvaOut = addr - (ULONG_PTR)mbi.AllocationBase;
	const char* slash = strrchr(path, 92);          // last backslash
	return slash ? slash + 1 : path;
}

static bool AddressIsMapped(ULONG_PTR addr)
{
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) return false;
	return mbi.State == MEM_COMMIT;
}

static LONG CALLBACK TruncationVEH(EXCEPTION_POINTERS* ep)
{
	if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
	if (ep->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
	if (g_faultsLogged >= MAX_FAULT_RECORDS) return EXCEPTION_CONTINUE_SEARCH;

	const ULONG_PTR addr = (ULONG_PTR)ep->ExceptionRecord->ExceptionInformation[1];
	const ULONG_PTR op   = (ULONG_PTR)ep->ExceptionRecord->ExceptionInformation[0];
	if (addr >= (ULONG_PTR)0x100000000) return EXCEPTION_CONTINUE_SEARCH;   // not this signature

	InterlockedIncrement(&g_faultsLogged);

	const CONTEXT* c = ep->ContextRecord;
	const ULONG_PTR regs[16] = {
		c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp, c->Rsi, c->Rdi,
		c->R8,  c->R9,  c->R10, c->R11, c->R12, c->R13, c->R14, c->R15
	};
	static const char* const names[16] = {
		"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
		"r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"
	};

	char buf[2048];
	int n = 0;
	ULONG_PTR rva = 0;
	const char* mod = ModuleAt((ULONG_PTR)c->Rip, &rva);

	n += sprintf(buf + n, "=== access violation on a low address ===%s", "\n");
	n += sprintf(buf + n, "  faulting code : %s+0x%08llX%s",
	             mod ? mod : "(unknown)", (unsigned long long)rva, "\n");
	n += sprintf(buf + n, "  operation     : %s%s",
	             op == 0 ? "read" : (op == 1 ? "write" : "execute"), "\n");
	n += sprintf(buf + n, "  address       : 0x%016llX (%s)%s", (unsigned long long)addr,
	             addr < 0x10000 ? "null-ish, probably not truncation" : "unmapped low address",
	             "\n");

	// The most useful line: a register whose low half equals the faulting address but whose top
	// half is still intact is the original pointer, and names what was truncated on the way in.
	for (int i = 0; i < 16; ++i) {
		if ((regs[i] & 0xFFFFFFFF) == (addr & 0xFFFFFFFF) && (regs[i] >> 32) != 0)
			n += sprintf(buf + n, "  intact copy   : %s = 0x%016llX  <- pointer before truncation%s",
			             names[i], (unsigned long long)regs[i], "\n");
	}
	for (int i = 0; i < 16; ++i) {
		if (regs[i] == addr)
			n += sprintf(buf + n, "  held in       : %s%s", names[i], "\n");
	}

	n += sprintf(buf + n, "  registers     :%s", "\n");
	for (int i = 0; i < 16; i += 4) {
		n += sprintf(buf + n, "    %s=%016llX %s=%016llX %s=%016llX %s=%016llX%s",
		             names[i],   (unsigned long long)regs[i],
		             names[i+1], (unsigned long long)regs[i+1],
		             names[i+2], (unsigned long long)regs[i+2],
		             names[i+3], (unsigned long long)regs[i+3], "\n");
	}
	n += sprintf(buf + n, "%s", "\n");

	AppendFaultLog(buf, (unsigned long)n);
	return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------------------
// A proxy over the engine's main allocation entry point.
//
// The constructor of the pak heap calls it through a pointer and gives up if the result is
// null (CrySystem RVA 0x0A2253 calls, 0x0A2280 tests, 0x0A2285 raises the fatal error). The
// store and the test are both 64-bit and correct, so the allocator really does return zero -
// the question is what it was asked for at that moment, and this answers it.
//
// The pointer lives in a table that CrySystem fills lazily from RVA 0x0369E0. That function
// takes no arguments (the call at 0x0A2246 sets none), so calling it ourselves first is safe
// and leaves the table populated, after which the entry can be swapped without racing anyone.
//
// This is also the shape of the eventual fix: the same swap, with an allocator of our own on
// the other side instead of a passthrough.
#define CRT_TABLE_INIT_RVA 0x0369E0
#define CRYMALLOC_PTR_RVA  0x6EF6A8
#define BIG_ALLOC_INTEREST (16 * 1024)

typedef void  (*CrtTableInitFn)(void);
typedef void* (*CryMallocFn)(size_t size, size_t* allocated);

static CryMallocFn   g_origCryMalloc = 0;
static volatile long g_allocLogged = 0;

static void* ProxyCryMalloc(size_t size, size_t* allocated)
{
	// Note the request before making it. A call that fails by never returning - because the
	// allocator raises a fatal error of its own - is otherwise invisible.
	if (size >= BIG_ALLOC_INTEREST && g_allocLogged < 200)
	{
		char ask[96];
		int m = sprintf(ask, "  ask   %9u bytes ...%s", (unsigned)size, "\n");
		AppendFaultLog(ask, (unsigned long)m);
	}

	void* p = g_origCryMalloc(size, allocated);

	// Only the interesting ones: every failure, and the large blocks the pools are made of.
	// Small allocations run into the millions and would drown the log.
	if ((!p || size >= BIG_ALLOC_INTEREST) && g_allocLogged < 200)
	{
		InterlockedIncrement(&g_allocLogged);
		char line[160];
		int n = sprintf(line, "  alloc %9u bytes -> 0x%016llX%s%s",
		                (unsigned)size, (unsigned long long)(ULONG_PTR)p,
		                p ? ((ULONG_PTR)p >= (ULONG_PTR)0x100000000 ? "   ABOVE 4GB" : "") : "   FAILED",
		                "\n");
		AppendFaultLog(line, (unsigned long)n);
	}
	return p;
}

// How much contiguous address space is actually left below the 4 GB line. The engine's pools
// need single blocks of up to 14 MB, so this is the number that decides whether startup
// succeeds, and it is invisible from anywhere else.
static size_t LargestFreeBlockBelow4GB(size_t* totalFreeOut)
{
	const ULONG_PTR limit = (ULONG_PTR)0x100000000;
	size_t largest = 0, total = 0;
	ULONG_PTR a = 0x10000;
	MEMORY_BASIC_INFORMATION mbi;

	while (a < limit && VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) == sizeof(mbi))
	{
		ULONG_PTR next = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
		if (mbi.State == MEM_FREE)
		{
			SIZE_T sz = mbi.RegionSize;
			if ((ULONG_PTR)mbi.BaseAddress + sz > limit)
				sz = (SIZE_T)(limit - (ULONG_PTR)mbi.BaseAddress);
			total += sz;
			if (sz > largest) largest = sz;
		}
		if (next <= a) break;
		a = next;
	}
	if (totalFreeOut) *totalFreeOut = total;
	return largest;
}

static const char* InstallAllocProxy(void)
{
	HMODULE cs = GetModuleHandleA("CrySystem.dll");
	if (!cs) return "CrySystem not loaded";
	unsigned char* base = (unsigned char*)cs;

	// Populate the table first, so the entry we read is the real one.
	((CrtTableInitFn)(base + CRT_TABLE_INIT_RVA))();

	CryMallocFn* slot = (CryMallocFn*)(base + CRYMALLOC_PTR_RVA);
	if (!*slot) return "allocator pointer still empty";
	if (*slot == ProxyCryMalloc) return "already installed";

	g_origCryMalloc = *slot;

	DWORD oldProt = 0;
	if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProt)) return "VirtualProtect failed";
	*slot = ProxyCryMalloc;
	VirtualProtect(slot, sizeof(*slot), oldProt, &oldProt);
	return "installed";
}

// Site 1 - the head of the allocator's free-page list, widened in place.
//
//     mov [rsp+40h], rax
//     mov rax, [rsp+40h]     <- reloads what was just stored, and the next instruction
//                               (lea rax, [rbp+80000h]) overwrites rax regardless
//     mov [rip+...], ebp     <- 32-bit store of a 64-bit pointer
//
// The reload is dead, so padding it out frees the byte the REX.W prefix needs. The store still
// ends at the same address, so its displacement stays correct.
#define SITE1_RVA 0x0A1A3F
static const unsigned char kSite1Expect[] = {
	0x48, 0x89, 0x44, 0x24, 0x40,
	0x48, 0x8B, 0x44, 0x24, 0x40
};
static const unsigned char kSite1Patched[] = {
	0x48, 0x89, 0x44, 0x24, 0x40,
	0x90, 0x90, 0x90, 0x90, 0x48
};

static bool WriteBytes(unsigned char* at, const unsigned char* src, unsigned long len)
{
	DWORD oldProt = 0;
	if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
	memcpy(at, src, len);
	VirtualProtect(at, len, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), at, len);
	return true;
}

// Applies the pointer-width correction described above. Only the one proven site is here.
//
// Other candidates were found by scanning for globals written 32 bits wide and read 64, and
// three of them were patched through a code cave at one point. That was a mistake: the scan
// also finds ordinary 32-bit fields the compiler reads in pairs, widening one of those corrupts
// a value that was never a pointer, and applying them together broke startup on a machine where
// it had been working. They are not kept behind a flag either - a switch that breaks the game is
// not a feature. If they are ever needed, the reasoning and the addresses are in docs/FINDINGS.md.
static const char* PatchSlabPointerWidth(void)
{
	HMODULE cs = GetModuleHandleA("CrySystem.dll");
	if (!cs) return "CrySystem not loaded";

	unsigned char* at = (unsigned char*)cs + SITE1_RVA;
	if (memcmp(at, kSite1Patched, sizeof(kSite1Patched)) == 0) return "already applied";
	if (memcmp(at, kSite1Expect, sizeof(kSite1Expect)) != 0) return "did not match this engine build";
	return WriteBytes(at, kSite1Patched, sizeof(kSite1Patched)) ? "applied" : "write failed";
}

// Reserves every free region below the 4 GB line, which forces the engine's heap above it.
//
// This makes the failure above reproducible on demand instead of waiting for a machine whose
// address space happens to be laid out badly. Without the patch this reliably reproduces the
// startup box; with it, startup should be unaffected. Diagnostic use only: -forcehighheap.
static size_t ReserveLowAddressSpace(void)
{
	const ULONG_PTR limit = (ULONG_PTR)0x100000000;
	size_t reserved = 0;
	ULONG_PTR a = 0x10000;
	MEMORY_BASIC_INFORMATION mbi;

	while (a < limit && VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) == sizeof(mbi))
	{
		ULONG_PTR next = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
		if (mbi.State == MEM_FREE)
		{
			SIZE_T sz = mbi.RegionSize;
			if ((ULONG_PTR)mbi.BaseAddress + sz > limit)
				sz = (SIZE_T)(limit - (ULONG_PTR)mbi.BaseAddress);
			if (VirtualAlloc(mbi.BaseAddress, sz, MEM_RESERVE, PAGE_NOACCESS))
				reserved += sz;
		}
		if (next <= a) break;
		a = next;
	}
	return reserved;
}

static void WriteDiagReport(const char* cmdLine, bool timerRaised, bool borderless,
                            const char* slabFix, size_t lowReserved, const char* allocTrace)
{
	FILE* f = fopen("launcher_diag.txt", "w");
	if (!f) return;

	DiagLine(f, "=== crysis2-64bit launcher diagnostics ===");
	DiagLine(f, "launcher build : %s %s", __DATE__, __TIME__);
	DiagLine(f, "command line   : %s", (cmdLine && *cmdLine) ? cmdLine : "(none)");
	DiagLine(f, "timer 1ms      : %s", timerRaised ? "raised OK" : "FAILED (expect ~64 fps cap)");
	DiagLine(f, "borderless     : %s", borderless ? "enabled" : "disabled (-noborderless)");
	DiagLine(f, "engine fix     : %s", slabFix);
	DiagLine(f, "alloc trace    : %s", allocTrace);
	{
		size_t totalFree = 0;
		const size_t largest = LargestFreeBlockBelow4GB(&totalFree);
		DiagLine(f, "low address sp : %u MB free, largest single block %u MB",
		         (unsigned)(totalFree / (1024 * 1024)), (unsigned)(largest / (1024 * 1024)));
	}
	if (lowReserved)
		DiagLine(f, "low space      : %u MB reserved (-forcehighheap, diagnostic)",
		         (unsigned)(lowReserved / (1024 * 1024)));
	DiagLine(f, "");

	// Install path, write access, locale and free space: environment differences that hardware
	// fields do not cover and that can cause startup failures on their own - a read-only
	// Program Files install, a full disk, or a locale whose case rules differ (Turkish being the
	// classic example, where 'i' does not uppercase to 'I').
	DiagLine(f, "--- install ---");
	{
		char cwd[MAX_PATH];
		if (GetCurrentDirectoryA(MAX_PATH, cwd))
			DiagLine(f, "game path      : %s", cwd);

		// Probe write access the only reliable way: actually try to create a file.
		HANDLE probe = CreateFileA("write_probe.tmp", GENERIC_WRITE, 0, NULL,
		                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
		if (probe != INVALID_HANDLE_VALUE) {
			DiagLine(f, "write access   : yes");
			CloseHandle(probe);
		} else {
			DiagLine(f, "write access   : NO (error %lu) - the engine cannot write its own files here", GetLastError());
		}

		ULARGE_INTEGER freeBytes;
		memset(&freeBytes, 0, sizeof(freeBytes));
		if (GetDiskFreeSpaceExA(NULL, &freeBytes, NULL, NULL))
			DiagLine(f, "free disk      : %llu MB", freeBytes.QuadPart / (1024ull * 1024ull));

		char lang[64] = {0};
		char ctry[64] = {0};
		GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SENGLANGUAGE, lang, sizeof(lang));
		GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SENGCOUNTRY, ctry, sizeof(ctry));
		DiagLine(f, "locale         : %s / %s", lang[0] ? lang : "?", ctry[0] ? ctry : "?");
		DiagLine(f, "ANSI codepage  : %u", GetACP());
	}
	DiagLine(f, "");

	DiagLine(f, "--- system ---");
	// RtlGetVersion reports the real version; GetVersionEx under-reports without a manifest.
	typedef LONG (WINAPI *RtlGetVersion_t)(void*);
	struct { ULONG dwOSVersionInfoSize; ULONG major, minor, build, platformId; WCHAR csd[128]; } osv;
	memset(&osv, 0, sizeof(osv));
	osv.dwOSVersionInfoSize = sizeof(osv);
	HMODULE ntdll = GetModuleHandleA("ntdll.dll");
	RtlGetVersion_t pRtlGetVersion = ntdll ? (RtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion") : 0;
	if (pRtlGetVersion && pRtlGetVersion(&osv) == 0)
		DiagLine(f, "Windows        : %lu.%lu build %lu", osv.major, osv.minor, osv.build);
	else
		DiagLine(f, "Windows        : (version query failed)");

	SYSTEM_INFO si;
	memset(&si, 0, sizeof(si));
	GetNativeSystemInfo(&si);
	DiagLine(f, "CPU threads    : %lu", si.dwNumberOfProcessors);

	MEMORYSTATUSEX ms;
	memset(&ms, 0, sizeof(ms));
	ms.dwLength = sizeof(ms);
	if (GlobalMemoryStatusEx(&ms))
		DiagLine(f, "RAM            : %llu MB total, %llu MB available",
		         ms.ullTotalPhys / (1024ull*1024ull), ms.ullAvailPhys / (1024ull*1024ull));
	DiagLine(f, "");

	DiagLine(f, "--- display ---");
	DEVMODEA dm;
	memset(&dm, 0, sizeof(dm));
	dm.dmSize = sizeof(dm);
	if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm))
		DiagLine(f, "desktop mode   : %lux%lu @ %lu Hz, %lu bpp",
		         dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, dm.dmBitsPerPel);
	DiagLine(f, "virtual screen : %dx%d, monitors: %d",
	         GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
	         GetSystemMetrics(SM_CMONITORS));

	// Adapter name via EnumDisplayDevices, so this needs no DXGI dependency.
	DISPLAY_DEVICEA dd;
	memset(&dd, 0, sizeof(dd));
	dd.cb = sizeof(dd);
	for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++) {
		if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
			DiagLine(f, "adapter %lu      : %s", i, dd.DeviceString);
		memset(&dd, 0, sizeof(dd));
		dd.cb = sizeof(dd);
	}

	// GetDpiForSystem is in USER32, which is already imported; using it avoids a GDI32
	// dependency, for the same load-order reason described above.
	typedef UINT (WINAPI *GetDpiForSystemFn)(void);
	HMODULE user32 = GetModuleHandleA("user32.dll");
	GetDpiForSystemFn pGetDpi = user32 ? (GetDpiForSystemFn)GetProcAddress(user32, "GetDpiForSystem") : 0;
	if (pGetDpi) {
		UINT dpi = pGetDpi();
		if (dpi) DiagLine(f, "DPI scale      : %u%% (%u dpi)", (dpi * 100) / 96, dpi);
	}
	DiagLine(f, "");

	// Allocator probe.
	//
	// Some systems abort startup with "Failed CMTSafeHeap::m_pBigPool allocation" while tens of
	// gigabytes are free. That message means new[] returned NULL for a pool of at most 14 MB, so
	// the engine's allocator is already in a bad state by the time it asks.
	//
	// CrySystem is imported statically, so it is loaded and its static initialisation has run
	// before any of this code executes. Its allocator state is therefore readable before the
	// engine can fail, and the same allocation can be repeated through the CRT the engine uses.
	//
	// Offsets come from reversing this exact build (CrySystem 1.1.1.217); they are checked
	// against the module size first so a different build simply skips the probe.
	DiagLine(f, "--- allocator ---");
	{
		HMODULE cs = GetModuleHandleA("CrySystem.dll");
		MODULEINFO mi;
		memset(&mi, 0, sizeof(mi));
		bool haveInfo = false;
		HMODULE psapi = LoadLibraryA("psapi.dll");
		if (psapi) {
			typedef BOOL (WINAPI *GetModuleInformationFn)(HANDLE, HMODULE, void*, DWORD);
			GetModuleInformationFn pGMI = (GetModuleInformationFn)GetProcAddress(psapi, "GetModuleInformation");
			if (pGMI && cs) haveInfo = (pGMI(GetCurrentProcess(), cs, &mi, sizeof(mi)) != 0);
		}
		if (cs && haveInfo && mi.SizeOfImage > 0x700000) {
			unsigned char* b = (unsigned char*)cs;
			unsigned long long slab = *(unsigned long long*)(b + 0x6F9338);
			unsigned long long mallocFn = *(unsigned long long*)(b + 0x6EF6C8);
			DiagLine(f, "bucket slab    : 0x%016llX %s", slab,
			         slab == 0 ? "(NULL - allocator never initialised)"
			                   : (slab < 0x100000000ull ? "(below 4 GB, as expected)"
			                                            : "(ABOVE 4 GB - pointer truncation applies)"));
			DiagLine(f, "crt malloc ptr : 0x%016llX", mallocFn);
		} else {
			DiagLine(f, "bucket slab    : (skipped - unexpected CrySystem build)");
		}

		// Repeat the engine's own allocation, through the engine's own CRT. If this fails here,
		// the problem is the runtime, not the engine.
		HMODULE m90 = GetModuleHandleA("msvcr90.dll");
		if (m90) {
			typedef void* (__cdecl *MallocFn)(size_t);
			typedef void  (__cdecl *FreeFn)(void*);
			MallocFn m90malloc = (MallocFn)GetProcAddress(m90, "malloc");
			FreeFn   m90free   = (FreeFn)GetProcAddress(m90, "free");
			if (m90malloc && m90free) {
				void* big = m90malloc(14 * 1024 * 1024);   // the largest pool the engine asks for
				DiagLine(f, "msvcr90 14MB   : %s (%p)", big ? "OK" : "FAILED - this is the failure itself", big);
				if (big) m90free(big);
			} else {
				DiagLine(f, "msvcr90 14MB   : (malloc/free not found)");
			}
		} else {
			DiagLine(f, "msvcr90        : NOT LOADED - the engine's CRT is missing");
		}
	}
	DiagLine(f, "");

	DiagLine(f, "--- Bin64 modules (size / modified) ---");
	static const char* mods[] = {
		"CrySystem.dll", "CryRenderD3D11.dll", "CryGameCrysis2.dll", "CryAction.dll",
		"CryPhysics.dll", "Cry3DEngine.dll", "CryAnimation.dll", 0
	};
	for (int i = 0; mods[i]; i++) {
		WIN32_FILE_ATTRIBUTE_DATA fa;
		char path[MAX_PATH];
		sprintf(path, "Bin64/%s", mods[i]);
		if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
			SYSTEMTIME st;
			FileTimeToSystemTime(&fa.ftLastWriteTime, &st);
			ULONGLONG sz = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
			DiagLine(f, "  %-22s %10llu  %04u-%02u-%02u", mods[i], sz, st.wYear, st.wMonth, st.wDay);
		} else {
			DiagLine(f, "  %-22s MISSING", mods[i]);
		}
	}
	DiagLine(f, "");

	// The pak list catches differences between game copies (retail versus a repack with
	// re-encoded or removed files), where a report that looks like a launcher bug is not one.
	DiagLine(f, "--- game paks ---");
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA("gamecrysis2/*.pak", &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			ULONGLONG sz = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
			DiagLine(f, "  %-26s %12llu", fd.cFileName, sz);
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	} else {
		DiagLine(f, "  (gamecrysis2 not found)");
	}

	fclose(f);
}

// Writes the cutscene-skip counters to movie_skips.txt while the game runs, enabled with
// -moviestats.
//
// A thread rather than a write on the way out: the interesting scene may be twenty minutes
// into a level, and the game can be closed in ways that never reach the end of WinMain. The
// file is rewritten only when a number actually changes, so watching a clean cutscene costs
// nothing.
static DWORD WINAPI MovieStatsThread(LPVOID)
{
	LONG last[7];
	int i;
	for (i = 0; i < 7; i++) last[i] = -1;

	for (;;)
	{
		Sleep(3000);

		bool changed = false;
		for (i = 0; i < 7; i++)
			if (g_movieSkips[i] != last[i]) changed = true;
		if (!changed) continue;

		FILE* f = fopen("movie_skips.txt", "w");
		if (!f) continue;

		fprintf(f, "Cutscene elements the launcher's workarounds had to skip.\n");
		fprintf(f, "All zero means no cutscene lost anything. Any other number means a node,\n");
		fprintf(f, "a track or a key was dropped, and the scene is missing part of itself.\n\n");

		LONG total = 0;
		for (i = 0; i < 7; i++)
		{
			last[i] = g_movieSkips[i];
			total += last[i];
			fprintf(f, "%9ld  %s\n", last[i], kMovieSkipNames[i]);
		}
		fprintf(f, "\n%9ld  total\n", total);
		fclose(f);
	}
}

// What a player sees when a piece of the 64-bit engine is not where it has to be.
//
// Left to Windows, a missing CrySystem.dll produces "the system cannot find CrySystem.dll, try
// reinstalling the program" - which points at the wrong thing entirely: reinstalling the
// launcher changes nothing, and the retail game does not contain these files at all. They come
// with the Mod SDK. So the launcher says that itself, and says where it looked.
static void ExplainMissing(const char* what)
{
	char dir[MAX_PATH];
	GetModuleFileNameA(NULL, dir, MAX_PATH);
	char* p = strrchr(dir, '\\');
	if (p) *p = 0;

	char msg[1200];
	sprintf(msg,
	        "%s was not found.\n\n"
	        "Looked in:\n    %s\n\n"
	        "launcher64.exe has to sit in the game's Bin64 folder, next to the 64-bit engine "
	        "files (CrySystem.dll and the other Cry*.dll).\n\n"
	        "Those files are not part of the retail game - the disc and the store versions ship "
	        "a 32-bit game only. They come with the free Crysis 2 Mod SDK.\n\n"
	        "Install the Mod SDK into your Crysis 2 folder, then copy launcher64.exe into\n"
	        "    <Crysis 2>\\Bin64\\\n"
	        "and start it from there.",
	        what, dir);
	MessageBoxA(NULL, msg, "Crysis 2 - 64-bit launcher", MB_OK | MB_ICONINFORMATION);
}

// The engine entry point, resolved by hand so the case above can be caught. Retail exports it
// undecorated, so no name mangling is involved.
static CreateSystemInterfaceFn LoadEngine(void)
{
	HMODULE engine = LoadLibraryA("CrySystem.dll");
	if (!engine)
	{
		ExplainMissing("The 64-bit engine (CrySystem.dll)");
		return 0;
	}

	CreateSystemInterfaceFn fn =
		(CreateSystemInterfaceFn)GetProcAddress(engine, "CreateSystemInterface");
	if (!fn)
	{
		MessageBoxA(NULL,
		            "CrySystem.dll was found, but it does not export CreateSystemInterface.\n\n"
		            "This usually means the file belongs to a different CryEngine game or a "
		            "different SDK version. The one needed here comes with the Crysis 2 Mod SDK.",
		            "Crysis 2 - 64-bit launcher", MB_OK | MB_ICONINFORMATION);
		return 0;
	}
	return fn;
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
	// Starting a second copy by accident is easy and confusing: both instances run, both eat
	// their ~650 MB, and both write to the same Game.log, which makes any later bug report
	// unreadable. The original launcher asked the same question. -allowmultiple skips it.
	{
		HANDLE once = CreateMutexA(NULL, FALSE, "crysis2-64bit-launcher");
		if (once && GetLastError() == ERROR_ALREADY_EXISTS &&
		    !(lpCmdLine && strstr(lpCmdLine, "-allowmultiple")))
		{
			if (MessageBoxA(NULL, "Crysis 2 is already running.\n\nStart another copy anyway?",
			                "crysis2-64bit", MB_YESNO | MB_ICONQUESTION) != IDYES)
				return 0;
		}
	}

	// Resolve the engine before anything else is set up. The static import used to bind here
	// too, at process start, so nothing about the load order changes - only the error message.
	CreateSystemInterfaceFn pCreateSystem = LoadEngine();
	if (!pCreateSystem) return 0;

	// Before engine init: 15.625 ms -> 1 ms (see the note above timeBeginPeriod).
#ifndef NO_TIMER_LOAD
	LoadTimerApi();
#endif
	const bool timerRaised = (g_timeBeginPeriod && g_timeBeginPeriod(1) == 0);

	// CrySystem stores one allocator pointer with a 32-bit write while every read of it is
	// 64-bit, so the top half is dropped (see PatchSlabPointerWidth). The bug is real, but it
	// only bites if an allocation lands above the 4 GB line, and in practice none does: the
	// process is built against msvcr90, whose heap stays low. Verified by forcing the heap up
	// with -forcehighheap, which startup survives either way. So the engine is left alone
	// unless asked: -enginefix applies the correction.
	const char* slabFix = (lpCmdLine && strstr(lpCmdLine, "-enginefix"))
	                    ? PatchSlabPointerWidth()
	                    : "off (engine untouched)";

	// Diagnostic: watch what the engine asks the allocator for, and what it gets back.
	const char* allocTrace = (lpCmdLine && strstr(lpCmdLine, "-traceallocs"))
	                       ? InstallAllocProxy() : "off";

	// Diagnostic: force the heap above the 4 GB line to reproduce the failure on demand.
	size_t lowReserved = 0;
	if (lpCmdLine && strstr(lpCmdLine, "-forcehighheap"))
		lowReserved = ReserveLowAddressSpace();

	SetCwdToGameRoot();

	// Diagnostic: report what the cutscene workarounds are dropping, if anything.
	if (lpCmdLine && strstr(lpCmdLine, "-moviestats"))
	{
		DWORD tid = 0;
		HANDLE th = CreateThread(NULL, 0, MovieStatsThread, NULL, 0, &tid);
		if (th) CloseHandle(th);
	}

#ifndef NO_DETECTOR
	// Watch for pointers that lost their top half. Observes only; see TruncationVEH.
	AddVectoredExceptionHandler(1, TruncationVEH);
#endif

	SSystemInitParams startupParams;
	startupParams.hInstance = GetModuleHandleA(NULL);
	startupParams.sLogFileName = "Game.log";
	// The engine applies the console commands on this line in order, so ours go first and the
	// player's own command line goes last. That way anything they pass wins over a default of
	// ours, and a "+map <level>" of theirs is not stranded behind our CVars - which is exactly
	// what happened when the order was the other way round.
	startupParams.szSystemCmdLine[0] = 0;

	// Borderless is the default; -noborderless restores the previous behaviour.
	const bool wantBorderless = !(lpCmdLine && strstr(lpCmdLine, "-noborderless"));
	if (wantBorderless)
	{
		// Ask the engine to start windowed at desktop resolution ('+' introduces a console
		// command). If the game's profile overrides this back to fullscreen nothing breaks:
		// an exclusive fullscreen window is already borderless, so the thread leaves it alone.
		char extra[256];
		sprintf(extra, " +r_Fullscreen 0 +r_Width %d +r_Height %d",
		        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
		strncat(startupParams.szSystemCmdLine, extra,
		        sizeof(startupParams.szSystemCmdLine) - strlen(startupParams.szSystemCmdLine) - 1);

		DWORD tid = 0;
		HANDLE th = CreateThread(NULL, 0, BorderlessThread, NULL, 0, &tid);
		if (th) CloseHandle(th);
	}

	// Intro and debug overlay off by default.
	//
	// The startup logos render as white rectangles because the x64 build fails to decode the
	// intro videos; the files themselves are present, so this is a decoder problem, not missing
	// content. Rather than chase the decoder, skip the intro.
	//
	// r_DisplayInfo draws a debug overlay whose status line ends in "DevMode", which makes the
	// build look like a debug build. It is enabled by the game's own system.cfg, so no reversing
	// is needed to remove it - only this CVar. Pass -keepintro to restore the original behaviour.
	if (!(lpCmdLine && strstr(lpCmdLine, "-keepintro")))
	{
		strncat(startupParams.szSystemCmdLine,
		        " +g_skipIntro 1 +sys_rendersplashscreen 0 +sys_intromoviesduringinit 0 +r_DisplayInfo 0",
		        sizeof(startupParams.szSystemCmdLine) - strlen(startupParams.szSystemCmdLine) - 1);
	}

	// The player's own arguments, last so they take precedence.
	if (lpCmdLine && *lpCmdLine)
	{
		strncat(startupParams.szSystemCmdLine, " ",
		        sizeof(startupParams.szSystemCmdLine) - strlen(startupParams.szSystemCmdLine) - 1);
		strncat(startupParams.szSystemCmdLine, lpCmdLine,
		        sizeof(startupParams.szSystemCmdLine) - strlen(startupParams.szSystemCmdLine) - 1);
	}

	// Write the report before engine init, so the file survives a crash during startup and the
	// tester still has something to send.
#ifndef NO_DIAG
	WriteDiagReport(lpCmdLine, timerRaised, wantBorderless, slabFix, lowReserved, allocTrace);
#endif

	// Bring up the engine's memory system first, in the same order the editor does.
	ISystem* pSystem = pCreateSystem(startupParams);
	if (!pSystem)
	{
		MessageBoxA(NULL,
		            "The engine failed to start up.\n\n"
		            "Game.log in the Crysis 2 folder holds the engine's own account of what "
		            "happened, and launcher_diag.txt next to it describes this machine. Both "
		            "are worth attaching to a bug report.",
		            "Crysis 2 - 64-bit launcher", MB_OK | MB_ICONINFORMATION);
		return 0;
	}

	// Hand the ready system to the game DLL, which reuses it instead of building a second one
	// (ISystem.h: pSystem is "reused if not NULL"). Without this the game brings up its own
	// CSystem on top of ours, and the second one fails while allocating the pak heap pools -
	// which is the "Failed CMTSafeHeap::m_pBigPool allocation" box.
	startupParams.pSystem = pSystem;
	// Defuse the CryAction release asserts on the CLevelSystem::LoadLevel path: in the Bin64
	// build they force a crash. Addresses come from the open-source c2-launcher (CryAction
	// 1.1.1.217). CryAction is loaded explicitly so the patch is in place before the game
	// initialises and picks up the already-patched copy.
	{
		HMODULE cryAction = LoadLibraryA("CryAction.dll");
		if (cryAction) {
			unsigned char* b = (unsigned char*)cryAction;
			PatchByte(b + 0xBC7C, 0xEB); // je -> jmp, stepping over the forced crash
			PatchByte(b + 0xBC8A, 0xEB);
			PatchByte(b + 0xBDE0, 0xEB);
			PatchByte(b + 0xAF21, 0xE9);
			PatchByte(b + 0xAF22, 0xB2);
			PatchByte(b + 0xAF23, 0x00);
			PatchByte(b + 0xAF26, 0x90);
			PatchByte(b + 0xB400, 0xEB);
			PatchByte(b + 0xBB4A, 0xEB);
			PatchByte(b + 0xBFD5, 0xEB);
		}
	}

	// Survive corrupt entries in the CryMovie update loop.
	//
	// The loop walks a list of descriptors and makes virtual calls on each. Unloading a layer
	// while a cutscene precaches leaves entries dangling: the memory is reused for XML data, so
	// an entry can be null, can hold a plausible-looking pointer whose vtable now addresses the
	// heap instead of a module, or can be garbage outright. Any of those crashes the original
	// loop on Battery Park.
	//
	// The loop body is redirected into a code cave that validates each entry before calling it:
	// the element pointer must be a plausible address, and its vtable must lie inside CryMovie's
	// module bounds, where the nodes are actually defined. Anything failing either check is
	// skipped entirely, past all three virtual calls; everything else runs normally. Entries
	// that pass the checks and still fault are caught by the exception handler above.
	//
	// The patch site is written as an absolute jump, since the cave is far away in memory.
	{
		HMODULE cryMovie = LoadLibraryA("CryMovie.dll");
		if (cryMovie) {
			unsigned char* mb = (unsigned char*)cryMovie;
			unsigned long long retNormal = (unsigned long long)(mb + 0xF25C);
			unsigned long long retSkip   = (unsigned long long)(mb + 0xF2EC);
			unsigned char* cave = (unsigned char*)VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (cave) {
				int i = 0;
				// If the vector was reallocated (Animate pushed into it and moved begin), rebase the
				// iterator into the new buffer. Self-initialising: on the first iteration the iterator
				// equals begin, so it syncs without correcting.
				unsigned long long obAddr = (unsigned long long)&g_movieOldBegin;
				cave[i++]=0x4D; cave[i++]=0x8B; cave[i++]=0x56; cave[i++]=0x50;                     // mov r10,[r14+0x50] (new_begin)
				cave[i++]=0x49; cave[i++]=0x3B; cave[i++]=0xDA;                                     // cmp rbx,r10
				int j_first = i; cave[i++]=0x74; cave[i++]=0x00;                                    // je L_sync (first iteration)
				cave[i++]=0x48; cave[i++]=0xB8; *(unsigned long long*)(cave+i)=obAddr; i+=8;        // mov rax,&g_movieOldBegin
				cave[i++]=0x48; cave[i++]=0x8B; cave[i++]=0x00;                                     // mov rax,[rax] (old_begin)
				cave[i++]=0x49; cave[i++]=0x3B; cave[i++]=0xC2;                                     // cmp rax,r10
				int j_noreal = i; cave[i++]=0x74; cave[i++]=0x00;                                   // je L_sync (begin unchanged)
				cave[i++]=0x48; cave[i++]=0x2B; cave[i++]=0xD8;                                     // sub rbx,rax (offset = rbx-old)
				cave[i++]=0x49; cave[i++]=0x03; cave[i++]=0xDA;                                     // add rbx,r10 (rbx = new_begin+offset)
				int L_sync = i;
				cave[i++]=0x48; cave[i++]=0xB8; *(unsigned long long*)(cave+i)=obAddr; i+=8;        // mov rax,&g_movieOldBegin
				cave[i++]=0x4C; cave[i++]=0x89; cave[i++]=0x10;                                     // mov [rax],r10 (old = new_begin)
				// Revalidate the iterator: it must stay within [begin, end).
				//
				// A nested movie update overwrites the non-reentrant global above, so the outer
				// loop's correction can produce a bogus iterator. Faulting on it sends the handler
				// to "skip element", which advances past the end and never meets the loop condition
				// again - an endless fault-and-resume hang. If the iterator is out of range, leave
				// the loop instead of continuing.
				cave[i++]=0x4D; cave[i++]=0x8B; cave[i++]=0x5E; cave[i++]=0x58;                     // mov r11,[r14+0x58] (end)
				cave[i++]=0x49; cave[i++]=0x3B; cave[i++]=0xDA;                                     // cmp rbx,r10 (begin)
				int j_exlo = i; cave[i++]=0x72; cave[i++]=0x00;                                     // jb EXIT (rbx<begin)
				cave[i++]=0x49; cave[i++]=0x3B; cave[i++]=0xDB;                                     // cmp rbx,r11 (end)
				int j_exhi = i; cave[i++]=0x73; cave[i++]=0x00;                                     // jae EXIT (rbx>=end)
				cave[j_first+1]  = (unsigned char)(L_sync - (j_first+2));                           // rel8 -> L_sync
				cave[j_noreal+1] = (unsigned char)(L_sync - (j_noreal+2));
				cave[i++]=0x48; cave[i++]=0x8B; cave[i++]=0x0B;                                     // mov rcx,[rbx]  (element pointer)
				// guard1: is rcx a plausible pointer? All process memory sits below 4 GB, so a value
				// with non-zero high bits is garbage, and anything below 0x10000 is null or nonsense.
				cave[i++]=0x49; cave[i++]=0x89; cave[i++]=0xCB;                                     // mov r11,rcx
				cave[i++]=0x49; cave[i++]=0xC1; cave[i++]=0xEB; cave[i++]=0x20;                      // shr r11,32 (high half)
				int j_hi = i; cave[i++]=0x75; cave[i++]=0x00;                                       // jnz SKIP (above 4 GB = garbage)
				cave[i++]=0x48; cave[i++]=0x81; cave[i++]=0xF9; cave[i++]=0x00; cave[i++]=0x00; cave[i++]=0x01; cave[i++]=0x00; // cmp rcx,0x10000
				int j_lo = i; cave[i++]=0x72; cave[i++]=0x00;                                       // jb SKIP (null or small)
				cave[i++]=0x48; cave[i++]=0x8B; cave[i++]=0x01;                                     // mov rax,[rcx] (vtable), now safe
				// guard2: the vtable must lie within CryMovie's exact module bounds, since the nodes
				// are defined there. Checking only the high byte let garbage through that happened to
				// share it while pointing past the end of the module.
				cave[i++]=0x48; cave[i++]=0x3D; *(unsigned int*)(cave+i)=0x34000000; i+=4;          // cmp rax,0x34000000
				int j_vlo = i; cave[i++]=0x72; cave[i++]=0x00;                                      // jb SKIP (vtable < CryMovie)
				cave[i++]=0x41; cave[i++]=0xBB; *(unsigned int*)(cave+i)=0x34082000; i+=4;          // mov r11d,0x34082000
				cave[i++]=0x4C; cave[i++]=0x39; cave[i++]=0xD8;                                     // cmp rax,r11
				int j_vhi = i; cave[i++]=0x73; cave[i++]=0x00;                                      // jae SKIP (past CryMovie's end)
				cave[i++]=0xFF; cave[i++]=0x90; cave[i++]=0x80; cave[i++]=0x00; cave[i++]=0x00; cave[i++]=0x00; // call [rax+0x80]
				cave[i++]=0x49; cave[i++]=0xBB; *(unsigned long long*)(cave+i)=retNormal; i+=8;     // mov r11, 0xF25C
				cave[i++]=0x41; cave[i++]=0xFF; cave[i++]=0xE3;                                     // jmp r11
				int skip = i;
				cave[i++]=0x49; cave[i++]=0xBB; *(unsigned long long*)(cave+i)=retSkip; i+=8;       // SKIP: mov r11, 0xF2EC
				cave[i++]=0x41; cave[i++]=0xFF; cave[i++]=0xE3;                                     // jmp r11
				int L_exit = i;                                                                     // EXIT: clean exit from the loop
				cave[i++]=0x49; cave[i++]=0xBA; *(unsigned long long*)(cave+i)=(unsigned long long)(mb+0xF2FA); i+=8; // mov r10, 0xF2FA
				cave[i++]=0x41; cave[i++]=0xFF; cave[i++]=0xE2;                                     // jmp r10 (leave the loop)
				cave[j_hi+1]  = (unsigned char)(skip - (j_hi+2));                                   // rel8 -> SKIP
				cave[j_lo+1]  = (unsigned char)(skip - (j_lo+2));
				cave[j_vlo+1] = (unsigned char)(skip - (j_vlo+2));
				cave[j_vhi+1] = (unsigned char)(skip - (j_vhi+2));
				cave[j_exlo+1] = (unsigned char)(L_exit - (j_exlo+2));                              // rel8 -> EXIT
				cave[j_exhi+1] = (unsigned char)(L_exit - (j_exhi+2));
				unsigned char* site = mb + 0xF250;
				DWORD oldp = 0;
				if (VirtualProtect(site, 12, PAGE_EXECUTE_READWRITE, &oldp)) {
					site[0]=0x48; site[1]=0xB8; *(unsigned long long*)(site+2)=(unsigned long long)cave; // mov rax, cave
					site[10]=0xFF; site[11]=0xE0;                                                        // jmp rax
					VirtualProtect(site, 12, oldp, &oldp);
				}
				// Backstop: the handler catches faults inside the cave and skips the element.
				g_movieCave = cave;
				g_movieRetSkip = retSkip;
				g_cryMovieBase = (unsigned long long)mb;   // used by the handler's range checks
				AddVectoredExceptionHandler(1, MovieVEH);
			}
		}
	}

	// Fill a gap in the editor build of CrySystem.
	//
	// A memory service in CrySystem has two vtable slots left as _purecall, unimplemented in
	// this build. Game code calls one of them while tearing down cutscene UI, which aborts with
	// "Pure function call". All DLLs are the same version, so this is not a mismatch - the
	// editor build simply never implements those methods.
	//
	// The fix is to point every _purecall in that vtable at the engine's own do-nothing stub,
	// the one it already used for many other slots of the same service. The method becomes a
	// no-op: at worst a small leak when a cutscene tears down, instead of a crash.
	{
		HMODULE cs = GetModuleHandleA("CrySystem.dll");
		if (cs) {
			unsigned char* b = (unsigned char*)cs;
			unsigned long long* vt = (unsigned long long*)(b + 0x458200);
			unsigned long long purecall = (unsigned long long)(b + 0x1AFB72);
			unsigned long long stub     = (unsigned long long)(b + 0x68340);
			DWORD oldp = 0;
			if (VirtualProtect(vt, 48 * 8, PAGE_READWRITE, &oldp)) {
				for (int s = 0; s < 48; s++) if (vt[s] == purecall) vt[s] = stub;   // only _purecall slots
				VirtualProtect(vt, 48 * 8, oldp, &oldp);
			}
		}
	}

	// Load the game DLL and take its entry point.
	HMODULE gameDll = LoadLibraryA("CryGameCrysis2.dll");
	if (!gameDll) { ExplainMissing("The game library (CryGameCrysis2.dll)"); return 0; }

	IGameStartup::TEntryFunction pCreate = (IGameStartup::TEntryFunction)GetProcAddress(gameDll, "CreateGameStartup2");
	if (!pCreate)
	{
		MessageBoxA(NULL,
		            "CryGameCrysis2.dll was found, but it does not export CreateGameStartup2.\n\n"
		            "The file is probably from a different game or SDK version than the engine "
		            "next to it. Reinstalling the Crysis 2 Mod SDK restores a matching set.",
		            "Crysis 2 - 64-bit launcher", MB_OK | MB_ICONINFORMATION);
		return 0;
	}

	IGameStartup* pGameStartup = pCreate();
	if (!pGameStartup)
	{
		MessageBoxA(NULL,
		            "The game library refused to start.\n\n"
		            "Game.log in the Crysis 2 folder holds the engine's own account of what "
		            "happened, and launcher_diag.txt next to it describes this machine. Both "
		            "are worth attaching to a bug report.",
		            "Crysis 2 - 64-bit launcher", MB_OK | MB_ICONINFORMATION);
		return 0;
	}

	// Initialise the game (reusing the system created above) and enter the main loop.
	if (pGameStartup->Init(startupParams))   // IGameRef converts to non-null on success
	{
		pGameStartup->Run(NULL);
	}
	pGameStartup->Shutdown();
	if (timerRaised && g_timeEndPeriod) g_timeEndPeriod(1);   // hand the quantum back to the system
	return 0;
}
