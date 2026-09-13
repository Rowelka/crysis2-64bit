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
		// The whole cave, not the 128 bytes the body used to fit in: widening the guards to
		// full 64-bit comparisons pushed it past that, and a fault in the tail would have gone
		// unhandled - the crashes this workaround exists to absorb.
		unsigned long long caveHi = caveLo + 512;
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

static void AppendTextFile(const char* path, const char* text, unsigned long len)
{
	HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
	                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return;
	DWORD written = 0;
	SetFilePointer(h, 0, NULL, FILE_END);
	WriteFile(h, text, len, &written, NULL);
	CloseHandle(h);
}

static void AppendFaultLog(const char* text, unsigned long len)
{
	AppendTextFile("launcher_faults.txt", text, len);
}

// Marks a new session in the fault log, and keeps the engine's log from the previous one.
//
// launcher_faults.txt is appended to, so faults from several play sessions pile up in one file
// with nothing to separate them - and the engine rewrites Game.log on every start, which is the
// only place that says which level was loading when a fault happened. Both are needed together
// to make sense of a crash reported hours later.
static void StartFaultSession(const char* cmdLine)
{
	SYSTEMTIME st;
	GetLocalTime(&st);

	// Keep what the previous run left behind: the engine overwrites all of it on start, and
	// a crash report is worth nothing once the next launch has erased it. error.log carries the
	// engine's own stack trace, error.bmp the frame it died on.
	static const char* const kKeep[] = { "Game.log", "error.log", "error.dmp", "error.bmp" };
	for (int i = 0; i < 4; i++)
	{
		if (GetFileAttributesA(kKeep[i]) == INVALID_FILE_ATTRIBUTES) continue;
		CreateDirectoryA("launcher_logs", NULL);

		const char* dot = strrchr(kKeep[i], '.');
		char stem[32];
		const size_t n = dot ? (size_t)(dot - kKeep[i]) : strlen(kKeep[i]);
		memcpy(stem, kKeep[i], n);
		stem[n] = 0;

		char kept[MAX_PATH];
		sprintf(kept, "launcher_logs%c%s_%04u%02u%02u_%02u%02u%02u%s", 92, stem,
		        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
		        dot ? dot : "");
		MoveFileA(kKeep[i], kept);
	}

	char line[512];
	int n = sprintf(line,
	                "%s=== session %04u-%02u-%02u %02u:%02u:%02u  args: %s ===%s",
	                "\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
	                (cmdLine && *cmdLine) ? cmdLine : "(none)", "\n");
	AppendFaultLog(line, (unsigned long)n);
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

// Reads one pointer-sized value, returning false instead of faulting on an unmapped page.
static bool SafePeek(const void* at, ULONG_PTR* out)
{
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQuery(at, &mbi, sizeof(mbi))) return false;
	if (mbi.State != MEM_COMMIT) return false;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
	*out = *(const ULONG_PTR*)at;
	return true;
}

// Writes a dump that still has the memory in it.
//
// The engine writes its own error.dmp, but it is a thin one: the Downtown crash left a stack
// saying a virtual call went to address zero, and the object it was called on was not in the
// dump at all - so there was no way to see whose object it was or what had happened to it.
//
// MiniDumpWithIndirectlyReferencedMemory adds the memory pointed at by registers and by values
// on the stack, which is exactly the missing piece, without the cost of dumping three gigabytes.
// dbghelp.dll is already loaded by the engine; failing to find it just means no extra dump.
static volatile LONG g_dumpsWritten = 0;

typedef BOOL (WINAPI *PFN_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, DWORD,
                                             void*, void*, void*);

static void WriteRichDump(EXCEPTION_POINTERS* ep)
{
	if (InterlockedIncrement(&g_dumpsWritten) > 2) return;   // two is plenty

	HMODULE dbg = GetModuleHandleA("dbghelp.dll");
	if (!dbg) dbg = LoadLibraryA("dbghelp.dll");
	if (!dbg) return;

	PFN_MiniDumpWriteDump write = (PFN_MiniDumpWriteDump)GetProcAddress(dbg, "MiniDumpWriteDump");
	if (!write) return;

	SYSTEMTIME st;
	GetLocalTime(&st);
	CreateDirectoryA("launcher_logs", NULL);
	char path[MAX_PATH];
	sprintf(path, "launcher_logs%clauncher_%04u%02u%02u_%02u%02u%02u.dmp", 92,
	        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return;

	// MINIDUMP_EXCEPTION_INFORMATION, declared inline to avoid pulling in dbghelp.h.
	struct { DWORD ThreadId; EXCEPTION_POINTERS* Pointers; BOOL ClientPointers; } mei;
	mei.ThreadId = GetCurrentThreadId();
	mei.Pointers = ep;
	mei.ClientPointers = FALSE;

	// Deliberately NOT MiniDumpWithFullMemory (0x2): that writes the whole address space, a
	// three-gigabyte file a dying process rarely finishes. The first attempt used it by
	// mistake and left a zero-byte dump behind.
	const DWORD kType = 0x00000004      // WithHandleData
	                  | 0x00000040      // WithIndirectlyReferencedMemory - the piece that matters
	                  | 0x00000800      // WithFullMemoryInfo - the map of what is mapped
	                  | 0x00001000;     // WithThreadInfo
	// With no exception to describe, the structure must not be passed at all: handing
	// MiniDumpWriteDump an exception record whose pointers are null takes the call down
	// with it, which is how the self-test produced a zero-byte file twice.
	const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), h, kType,
	                      ep ? &mei : NULL, NULL, NULL);
	const DWORD err = ok ? 0 : GetLastError();
	const DWORD size = GetFileSize(h, NULL);
	CloseHandle(h);

	char line[MAX_PATH + 96];
	int n = sprintf(line, "  %s dump: %s (%u bytes, error %u)%s",
	                ok ? "wrote" : "FAILED to write", path,
	                (unsigned)size, (unsigned)err, "\n");
	AppendFaultLog(line, (unsigned long)n);
}
static LONG CALLBACK TruncationVEH(EXCEPTION_POINTERS* ep)
{
	if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
	if (g_faultsLogged >= MAX_FAULT_RECORDS) return EXCEPTION_CONTINUE_SEARCH;

	// Every way the process can die hard, not only the truncation signature.
	//
	// Narrowing this to access violations on low addresses was right while that was the one
	// failure being hunted, but it made every other death invisible: the engine went down with a
	// divide by zero inside the allocator and neither log said a word about where. A handler
	// that only sees what it already expects is not a detector.
	const DWORD code = ep->ExceptionRecord->ExceptionCode;
	const char* kind =
		code == EXCEPTION_ACCESS_VIOLATION      ? "access violation" :
		code == EXCEPTION_INT_DIVIDE_BY_ZERO    ? "integer divide by zero" :
		code == EXCEPTION_ILLEGAL_INSTRUCTION   ? "illegal instruction" :
		code == EXCEPTION_PRIV_INSTRUCTION      ? "privileged instruction" :
		code == EXCEPTION_STACK_OVERFLOW        ? "stack overflow" :
		code == 0xC0000374                      ? "heap corruption" :
		code == EXCEPTION_INT_OVERFLOW          ? "integer overflow" : 0;
	if (!kind) return EXCEPTION_CONTINUE_SEARCH;

	const bool isAV = (code == EXCEPTION_ACCESS_VIOLATION &&
	                   ep->ExceptionRecord->NumberParameters >= 2);
	const ULONG_PTR addr = isAV ? (ULONG_PTR)ep->ExceptionRecord->ExceptionInformation[1] : 0;
	const ULONG_PTR op   = isAV ? (ULONG_PTR)ep->ExceptionRecord->ExceptionInformation[0] : 0;

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

	char buf[6144];   // registers, ten stack frames, and a line per register pointer
	int n = 0;
	ULONG_PTR rva = 0;
	const char* mod = ModuleAt((ULONG_PTR)c->Rip, &rva);

	n += sprintf(buf + n, "=== %s ===%s", kind, "\n");
	n += sprintf(buf + n, "  faulting code : %s+0x%08llX%s",
	             mod ? mod : "(unknown)", (unsigned long long)rva, "\n");
	if (isAV)
	{
		n += sprintf(buf + n, "  operation     : %s%s",
		             op == 0 ? "read" : (op == 1 ? "write" : "execute"), "\n");
		n += sprintf(buf + n, "  address       : 0x%016llX (%s)%s", (unsigned long long)addr,
		             addr >= 0x100000000 ? "high address, mapped or not" :
		             (addr < 0x10000 ? "null-ish, probably not truncation" : "unmapped low address"),
		             "\n");
	}

	// The most useful line: a register whose low half equals the faulting address but whose top
	// half is still intact is the original pointer, and names what was truncated on the way in.
	for (int i = 0; i < 16 && addr; ++i) {
		if ((regs[i] & 0xFFFFFFFF) == (addr & 0xFFFFFFFF) && (regs[i] >> 32) != 0)
			n += sprintf(buf + n, "  intact copy   : %s = 0x%016llX  <- pointer before truncation%s",
			             names[i], (unsigned long long)regs[i], "\n");
	}
	for (int i = 0; i < 16 && addr; ++i) {
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
	// Who called in. The faulting instruction is often inside a system library that was
	// simply handed something wrong; the frames above it name the code that did the handing.
	n += sprintf(buf + n, "  callers       :%s", "\n");
	{
		const ULONG_PTR* sp = (const ULONG_PTR*)c->Rsp;
		int shown = 0;
		for (int i = 0; i < 256 && shown < 10; i++)
		{
			ULONG_PTR v = 0;
			if (!SafePeek(&sp[i], &v)) break;
			ULONG_PTR r = 0;
			const char* m = ModuleAt(v, &r);
			if (!m) continue;
			n += sprintf(buf + n, "    [rsp+%04X] %s+0x%08llX%s", (unsigned)(i * 8), m,
			             (unsigned long long)r, "\n");
			shown++;
		}
	}
	n += sprintf(buf + n, "%s", "\n");

	// What the registers point at. Two crashes in a row came down to an object whose table
	// of methods held something that was not a table, and neither the engine's dump nor a
	// dump written from inside the fault handler kept that memory - dbghelp faulted trying.
	// Sixteen bytes read here, with the same guarded read used everywhere else, answers the
	// question directly: an object still alive starts with a pointer into a module.
	n += sprintf(buf + n, "  what the registers point at:%s", "\n");
	for (int i = 0; i < 16; i++)
	{
		const ULONG_PTR v = regs[i];
		if (v < 0x10000 || v >= 0x0000800000000000ull) continue;
		ULONG_PTR first = 0;
		if (!SafePeek((const void*)v, &first)) continue;
		ULONG_PTR rva = 0;
		const char* mod = ModuleAt(first, &rva);
		n += sprintf(buf + n, "    [%s] 0x%016llX -> 0x%016llX %s%s", names[i],
		             (unsigned long long)v, (unsigned long long)first,
		             mod ? "" : "(not a module address)", "\n");
		if (mod)
			n += sprintf(buf + n, "        vtable of %s+0x%llX%s", mod,
			             (unsigned long long)rva, "\n");
	}

	AppendFaultLog(buf, (unsigned long)n);

	// The dbghelp dump is NOT written from here: on the CentralStation crash it faulted
	// inside dbghelp and produced a zero-byte file. It stays available under -dumptest.
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

// Low address space held back so that one allocation has to go above the 4 GB line.
//
// The blocks are remembered rather than simply reserved, because the whole point is to let go
// of them the moment the arena has been allocated - see the note in ProxyCryMalloc.
#define MAX_LOW_BLOCKS 128
static LPVOID   g_lowBlocks[MAX_LOW_BLOCKS];
static unsigned g_lowBlockCount = 0;
static bool     g_lowHeld = false;

static void ReleaseLowAddressSpace(void)
{
	if (!g_lowHeld) return;
	g_lowHeld = false;
	for (unsigned i = 0; i < g_lowBlockCount; i++)
	{
		if (g_lowBlocks[i]) VirtualFree(g_lowBlocks[i], 0, MEM_RELEASE);
		g_lowBlocks[i] = 0;
	}
	g_lowBlockCount = 0;
}

// Threshold for -highslab: allocations of at least this many bytes are served from above the
// 4 GB line instead of from the engine's own heap. Zero disables it.
static SIZE_T   g_highThreshold = 0;
static unsigned g_highTaken  = 0;      // large allocations moved above 4 GB
static unsigned g_highMissed = 0;      // large allocations that could not be moved

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

	// No squeeze here. This is the wrong door: the allocator takes its arena through
	// CrySystemCrtMalloc, not through CryMalloc - see ProxyCrtMalloc below. Pushing what
	// arrives here above the 4 GB line moved memory that had nothing to do with the bug.
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

static bool g_crtTableReady = false;

// Filling the table has to happen exactly once. Making it conditional on the slot being
// empty leaves the table unfilled and the game dies fifteen seconds in; calling it again
// after a proxy is installed puts the original pointer back and quietly removes the proxy.
static void EnsureCrtTable(unsigned char* base)
{
	if (g_crtTableReady) return;
	((CrtTableInitFn)(base + CRT_TABLE_INIT_RVA))();
	g_crtTableReady = true;
}

static const char* InstallAllocProxy(void)
{
	HMODULE cs = GetModuleHandleA("CrySystem.dll");
	if (!cs) return "CrySystem not loaded";
	unsigned char* base = (unsigned char*)cs;

	CryMallocFn* slot = (CryMallocFn*)(base + CRYMALLOC_PTR_RVA);
	if (*slot == ProxyCryMalloc) return "already installed";

	// Populate the table, so the entry read below is the real one.
	EnsureCrtTable(base);
	if (!*slot) return "allocator pointer still empty";

	g_origCryMalloc = *slot;

	DWORD oldProt = 0;
	if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProt)) return "VirtualProtect failed";
	*slot = ProxyCryMalloc;
	VirtualProtect(slot, sizeof(*slot), oldProt, &oldProt);
	return "installed";
}

// The arena the bucket allocator runs on never came from CryMalloc.
//
// CrySystem fills a small table of allocation entry points from its own exports (the function
// at RVA 0x0369E0): CryMalloc lands at 0x6EF6A8, CryRealloc at 0x6EF6B8, CryGetMemSize at
// 0x6EF6C0, and CrySystemCrtMalloc at 0x6EF6C8. That last one is a jump straight to
// MSVCR90!malloc, and it is what the bucket allocator calls to build its 0x80000-byte arena
// (the call at 0xA1922). Requests over 0x200 bytes take the same route (0xA12CE); smaller ones
// are served out of the arena, which is the whole point of the thing.
//
// So proxying CryMalloc - which is what -highslab did until now - never saw an arena being
// created at all. The 512 KB blocks it caught and pushed above the 4 GB line belonged to
// something else entirely, the allocator's own memory stayed low the whole time, and the
// pointer corrections had nothing to correct. That is the answer to why -enginefix made no
// difference: the bug it fixes was never given a chance to fire.
#define CRT_MALLOC_PTR_RVA 0x6EF6C8
#define BUCKET_ARENA_BYTES 0x80000

typedef void* (*CrtMallocFn)(size_t size);
static CrtMallocFn g_origCrtMalloc = 0;
static bool        g_arenaExact    = true;   // match the arena size exactly, not a threshold
static unsigned    g_arenaSeen     = 0;      // arena-sized requests observed

// Serving every arena from above the 4 GB line, instead of squeezing one of them up there.
//
// The squeeze only works once. It lets go of the low address space the moment the first arena is
// taken, so arena number two and the hundred after it come straight back down - watching the
// globals while the game runs shows exactly that: one arena high, then 0x4BD16BE0 for the next
// minute. A condition that holds for a fraction of a second proves nothing either way.
//
// So the arena is served directly instead. The allocator gets its 0x80000-byte block like any
// other, only from a region reserved above the line; CrySystemCrtFree and CrySystemCrtSize are
// hooked alongside it so the block behaves like a CRT one for its whole life. That was what went
// wrong the last time this was tried: memory handed over without the rest of its life accounted
// for. Nothing low is taken away, so the renderer is untouched - which is what made
// -forcehighheap useless as a test.
#define CRT_FREE_PTR_RVA 0x6EF6D0
#define CRY_FREE_PTR_RVA 0x6EF6C0
#define CRT_SIZE_PTR_RVA 0x6EF6D8
#define ARENA_POOL_SLOTS 2048

// Arenas are spaced two slots apart, so no arena ends exactly where the next one begins.
// The allocator decides which arena a pointer belongs to with `base <= p <= end`, end
// inclusive, and real malloc never hands out blocks that touch - a pool that does would be
// asking a question the engine was never written to answer.
#define ARENA_SLOT_STRIDE (BUCKET_ARENA_BYTES * 2)

// The one call site that builds an arena: CrySystem RVA 0xA1922, and the instruction is
// "call qword ptr [rip+disp32]" - six bytes, so the return address is 0xA1928. Requests
// of exactly the arena size also arrive from 0xA12CE, which is the ordinary path for any
// block over 0x200 bytes; serving those from the pool as well would move memory that has
// nothing to do with the experiment.
#define ARENA_CALL_SITE_RVA 0x0A1928

extern "C" void* _ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)

typedef void   (*CrtFreeFn)(void* p);
typedef size_t (*CrtSizeFn)(void* p);

static CrtFreeFn g_origCrtFree = 0;
static CrtFreeFn g_origCryFree = 0;
static unsigned  g_arenaFreedElsewhere = 0;   // arenas released through CryFree, not CrtFree
static CrtSizeFn g_origCrtSize = 0;
static bool      g_arenaHigh   = false;
static const char* g_highArenaMsg = "off";   // reported in the diagnostic file

static unsigned char* g_arenaPool = 0;                  // reserved, ARENA_POOL_SLOTS * 0x80000
static volatile LONG  g_arenaSlot[ARENA_POOL_SLOTS];    // 0 free, 1 in use
static volatile LONG  g_arenaLive = 0;                  // arenas served and not yet freed
static volatile LONG  g_arenaNext = 0;                  // next slot to hand out, never rewound
static unsigned       g_arenaPeak = 0;
static unsigned       g_arenaFull = 0;                  // times the pool had nothing left
static unsigned       g_arenaFreed = 0;
static unsigned       g_arenaSkipped = 0;      // arena-sized requests from anywhere else
static const void*    g_arenaCallSite = 0;

// Slots are handed out in order and never reused, even after the arena is freed.
//
// Reusing one deadlocked the engine: it keeps arenas on a chain it walks by address
// ([arena+0x2010]), and handing back an address it already has on that chain can close the
// chain into a ring. The walk then never ends - the thread stops answering, the engine's own
// watchdog reports "Runaway thread", and the process is killed sixty seconds in. Addresses are
// cheap here: the pool is reserved address space, and a session used 186 of 2048.
static void* ArenaAlloc(void)
{
	if (!g_arenaPool) return 0;

	const LONG slot = InterlockedIncrement(&g_arenaNext) - 1;
	if (slot < 0 || slot >= (LONG)ARENA_POOL_SLOTS)
	{
		g_arenaFull++;
		return 0;
	}

	void* p = VirtualAlloc(g_arenaPool + (size_t)slot * ARENA_SLOT_STRIDE,
	                       BUCKET_ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
	if (!p)
	{
		g_arenaFull++;
		return 0;
	}
	InterlockedExchange(&g_arenaSlot[slot], 1);
	const LONG live = InterlockedIncrement(&g_arenaLive);
	if ((unsigned)live > g_arenaPeak) g_arenaPeak = (unsigned)live;
	return p;
}

static bool ArenaOwns(void* p, unsigned* slotOut)
{
	if (!g_arenaPool || !p) return false;
	const ULONG_PTR a = (ULONG_PTR)p;
	const ULONG_PTR lo = (ULONG_PTR)g_arenaPool;
	const ULONG_PTR hi = lo + (ULONG_PTR)ARENA_POOL_SLOTS * ARENA_SLOT_STRIDE;
	if (a < lo || a >= hi) return false;
	if (((a - lo) % ARENA_SLOT_STRIDE) != 0) return false;   // inside an arena, not its start
	*slotOut = (unsigned)((a - lo) / ARENA_SLOT_STRIDE);
	return true;
}

static void ProxyCrtFree(void* p)
{
	unsigned slot = 0;
	if (ArenaOwns(p, &slot))
	{
		// The pages stay committed on purpose. Decommitting them turns a stale pointer into
		// an access violation, and the allocator is not the only one holding pointers into an
		// arena - the renderer reads through them too. Real malloc does not unmap freed blocks
		// either, so keeping them mapped is the behaviour being imitated, not a workaround.
		InterlockedExchange(&g_arenaSlot[slot], 2);   // retired, not reusable
		InterlockedDecrement(&g_arenaLive);
		g_arenaFreed++;
		return;
	}
	g_origCrtFree(p);
}

// CryFree, the entry point the rest of the engine uses. An arena should never arrive here,
// but if one does, it must not be handed to the real CRT - that block was never its.
static void ProxyCryFree(void* p)
{
	unsigned slot = 0;
	if (ArenaOwns(p, &slot))
	{
		g_arenaFreedElsewhere++;
		InterlockedExchange(&g_arenaSlot[slot], 2);   // retired, not reusable
		InterlockedDecrement(&g_arenaLive);
		return;
	}
	g_origCryFree(p);
}

static size_t ProxyCrtSize(void* p)
{
	unsigned slot = 0;
	if (ArenaOwns(p, &slot)) return BUCKET_ARENA_BYTES;
	return g_origCrtSize(p);
}

static void* ProxyCrtMalloc(size_t size)
{
	if (size == BUCKET_ARENA_BYTES)
	{
		g_arenaSeen++;
		if (g_arenaHigh && _ReturnAddress() != g_arenaCallSite)
		{
			// Report the first one. An off-by-one in the return address silently turns every
			// arena away, and the run then looks exactly like a run with the flag switched off.
			if (++g_arenaSkipped == 1)
			{
				char sk[160];
				int k = sprintf(sk, "  arena-sized request from 0x%016llX, expected 0x%016llX\n",
				                (unsigned long long)(ULONG_PTR)_ReturnAddress(),
				                (unsigned long long)(ULONG_PTR)g_arenaCallSite);
				AppendFaultLog(sk, (unsigned long)k);
			}
		}
		else if (g_arenaHigh)
		{
			void* h = ArenaAlloc();
			if (h)
			{
				if (g_arenaSeen <= 3 || (g_arenaSeen % 64) == 0)
				{
					char hl[160];
					int k = sprintf(hl, "  arena #%u (slot %d/%u) -> 0x%016llX  %s\n",
			                g_arenaSeen, (int)g_arenaNext, (unsigned)ARENA_POOL_SLOTS,
					                (unsigned long long)(ULONG_PTR)h,
					                ((ULONG_PTR)h >= (ULONG_PTR)0x100000000) ? "ABOVE 4GB" : "still low");
					AppendFaultLog(hl, (unsigned long)k);
				}
				return h;
			}
		}
	}

	// Squeeze for exactly one allocation - the arena - and let go immediately afterwards, so
	// the renderer finds the low address space back where it expects it.
	const bool squeeze = (g_lowHeld && (g_arenaExact ? (size == BUCKET_ARENA_BYTES)
	                                                 : (g_highThreshold && size >= g_highThreshold)));

	void* p = g_origCrtMalloc(size);

	if (squeeze)
	{
		ReleaseLowAddressSpace();
		if (p)
		{
			const bool high = ((ULONG_PTR)p >= (ULONG_PTR)0x100000000);
			if (high) g_highTaken++; else g_highMissed++;
			char hl[160];
			int k = sprintf(hl, "  arena   %9u bytes -> 0x%016llX  %s\n",
			                (unsigned)size, (unsigned long long)(ULONG_PTR)p,
			                high ? "ABOVE 4GB" : "still low");
			AppendFaultLog(hl, (unsigned long)k);
		}
	}
	return p;
}

static const char* InstallArenaProxy(void)
{
	HMODULE cs = GetModuleHandleA("CrySystem.dll");
	if (!cs) return "CrySystem not loaded";
	unsigned char* base = (unsigned char*)cs;

	CrtMallocFn* slot = (CrtMallocFn*)(base + CRT_MALLOC_PTR_RVA);
	if (*slot == ProxyCrtMalloc) return "already installed";

	EnsureCrtTable(base);
	if (!*slot) return "crt allocator pointer still empty";

	g_origCrtMalloc = *slot;

	DWORD oldProt = 0;
	if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProt)) return "VirtualProtect failed";
	*slot = ProxyCrtMalloc;
	VirtualProtect(slot, sizeof(*slot), oldProt, &oldProt);
	return "installed";
}

static const char* InstallHighArena(void)
{
	HMODULE cs = GetModuleHandleA("CrySystem.dll");
	if (!cs) return "CrySystem not loaded";
	unsigned char* base = (unsigned char*)cs;

	CrtMallocFn* mslot = (CrtMallocFn*)(base + CRT_MALLOC_PTR_RVA);
	CrtFreeFn*   fslot = (CrtFreeFn*)(base + CRT_FREE_PTR_RVA);
	CrtSizeFn*   sslot = (CrtSizeFn*)(base + CRT_SIZE_PTR_RVA);
	CrtFreeFn*   gslot = (CrtFreeFn*)(base + CRY_FREE_PTR_RVA);

	EnsureCrtTable(base);
	if (!*mslot || !*fslot || !*sslot) return "crt table incomplete";

	// A gigabyte of address space, not of memory: pages are committed one arena at a time and
	// given back on free, so the cost is what the allocator actually holds.
	for (ULONGLONG at = 0x200000000ULL; at < 0x1000000000ULL; at += 0x40000000ULL)
	{
		LPVOID p = VirtualAlloc((LPVOID)at, (SIZE_T)ARENA_POOL_SLOTS * ARENA_SLOT_STRIDE,
		                        MEM_RESERVE, PAGE_READWRITE);
		if (p) { g_arenaPool = (unsigned char*)p; break; }
	}
	if (!g_arenaPool) return "no address space above 4 GB";

	g_origCrtMalloc = *mslot;
	g_origCrtFree   = *fslot;
	g_origCrtSize   = *sslot;
	if (*gslot) { g_origCryFree = *gslot; }

	DWORD oldProt = 0;
	if (!VirtualProtect(mslot, 8 * 4, PAGE_READWRITE, &oldProt)) return "VirtualProtect failed";
	*mslot = ProxyCrtMalloc;
	*fslot = ProxyCrtFree;
	*sslot = ProxyCrtSize;
	if (g_origCryFree) *gslot = ProxyCryFree;
	VirtualProtect(mslot, 8 * 4, oldProt, &oldProt);

	g_arenaCallSite = (const void*)(base + ARENA_CALL_SITE_RVA);
	g_arenaHigh = true;

	static char msg[96];
	sprintf(msg, "pool at 0x%llX, %u arenas of %u KB, %u KB apart",
	        (unsigned long long)(ULONG_PTR)g_arenaPool, (unsigned)ARENA_POOL_SLOTS,
	        (unsigned)(BUCKET_ARENA_BYTES / 1024), (unsigned)(ARENA_SLOT_STRIDE / 1024));
	return msg;
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

// Somewhere to put instructions that do not fit where they belong.
//
// Two of the four truncations cannot be corrected in place: the right instruction is one byte
// longer than the wrong one, the function ends a few bytes later, and the byte after that is
// the target of a branch. So those bytes are replaced by a jump into a region allocated next to
// CrySystem, the corrected code sits there, and it jumps back.
//
// The region has to be within 2 GB of the module, because a relative jump only carries a signed
// 32-bit displacement. Hence the search outward from the module base instead of letting the
// system choose an address.
static unsigned char* g_cave     = 0;
static size_t         g_caveUsed = 0;
static const size_t   kCaveSize  = 0x1000;

static unsigned char* AllocCaveNear(void* anchor, size_t size = kCaveSize)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	const ULONG_PTR gran = (ULONG_PTR)si.dwAllocationGranularity;
	const ULONG_PTR base = (ULONG_PTR)anchor & ~(gran - 1);

	for (ULONG_PTR off = gran; off < 0x30000000; off += gran)
	{
		void* p = VirtualAlloc((LPVOID)(base + off), size,
		                       MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (p) return (unsigned char*)p;
		if (base > off)
		{
			p = VirtualAlloc((LPVOID)(base - off), size,
			                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (p) return (unsigned char*)p;
		}
	}
	return 0;
}

// Writes "jmp rel32" at 'from' to 'to', padding the rest of 'len' with nop.
static bool WriteJump(unsigned char* from, const unsigned char* to, size_t len)
{
	if (len < 5) return false;
	const LONGLONG delta = (LONGLONG)(to - (from + 5));
	if (delta > 0x7FFFFFF0 || delta < -0x7FFFFFF0) return false;

	unsigned char buf[16];
	buf[0] = 0xE9;
	*(LONG*)(buf + 1) = (LONG)delta;
	for (size_t i = 5; i < len; i++) buf[i] = 0x90;
	return WriteBytes(from, buf, (unsigned long)len);
}

// Sites 3 and 4, the two that need a trampoline.
//
// Site 3, taking a block off the free list (0x0A14A0):
//     mov eax, dword ptr [rsi]                   <- reads the "next" pointer, half of it
//     mov dword ptr [r13+rbp*8+0x6F91A0], eax    <- stores it back as the new head, half again
// The head itself is read correctly one instruction earlier, with a 64-bit load. Only the walk
// through the list is narrow, so the first block comes back intact and the second is garbage.
//
// Site 4, publishing the arena pointer (0x0A1A70):
//     mov dword ptr [rip+0x6578ba], ecx          <- 32-bit store into global 0x6F9330
// which the allocator then reads back 64 bits wide at 0x0A188D. The register holding the
// return value is live across this instruction, so the trampoline uses r11, which is not.
// Site 5, publishing the arena base (0x0A1A2A):
//     mov dword ptr [rip+0x657760], ebp        <- 32-bit store into global 0x6F9190
// read back fifteen instructions later, in the same function, as a full pointer:
//     mov r11, qword ptr [rip+0x657774]        <- at 0x0A1A15
// r10 is dead here: it was loaded at 0x0A19FC and last used by the call at 0x0A1A0E.
#define SITE3_RVA 0x0A14A0
#define SITE4_RVA 0x0A1A70
#define SITE5_RVA 0x0A1A2A
#define GLOBAL_ARENA_RVA 0x6F9330
#define GLOBAL_BASE_RVA  0x6F9190

static const unsigned char kSite3Expect[] = {
	0x8B, 0x06,                                       // mov eax, [rsi]
	0x41, 0x89, 0x84, 0xED, 0xA0, 0x91, 0x6F, 0x00    // mov [r13+rbp*8+0x6F91A0], eax
};
static const unsigned char kSite4Expect[] = {
	0x89, 0x0D, 0xBA, 0x78, 0x65, 0x00                // mov [rip+0x6578BA], ecx
};
static const unsigned char kSite5Expect[] = {
	0x89, 0x2D, 0x60, 0x77, 0x65, 0x00                // mov [rip+0x657760], ebp
};

// Whether the corrected code is ever reached.
//
// "The patch was applied" and "the patched instruction runs" are two different claims, and only
// the first one has been checked so far. Each trampoline body begins with a counter, so the
// second one can be read off at any time - see SiteWatchThread.
//
// The flags register is saved around the increment. The instructions being replaced are plain
// stores that leave flags alone, and whatever follows them may still depend on a comparison
// made further back.
#define CAVE_COUNTERS_OFF 0xF00
static unsigned long long* g_siteHits = 0;      // [0] list walk, [1] arena, [2] base

static size_t EmitHitCounter(unsigned char* buf, unsigned char* liveAddr, unsigned long long* counter)
{
	size_t n = 0;
	buf[n++] = 0x9C;                                        // pushfq
	buf[n++] = 0x48; buf[n++] = 0xFF; buf[n++] = 0x05;      // inc qword ptr [rip+rel32]
	const LONG rel = (LONG)((unsigned char*)counter - (liveAddr + n + 4));
	memcpy(buf + n, &rel, 4); n += 4;
	buf[n++] = 0x9D;                                        // popfq
	return n;
}

// Stops the AI from calling a method on an object that no longer exists.
//
// CAIActor::CanAcquireTarget (CryAISystem RVA 0x1606AC) is handed a candidate target, checks it
// against null, and then calls through its table of methods:
//
//   0x1606C8  cmp rdx, rdi          ; candidate == null?
//   0x1606CB  je  0x160878          ; yes - return "not a target"
//   0x1606D1  mov rax, [rdx]        ; table of methods
//   0x1606D4  mov rcx, rdx
//   0x1606D7  call [rax + 0x38]
//
// Twice in a row the game died here with a candidate that was not null but was destroyed: its
// first field held a heap address where a table of methods belongs, so the call went to an
// address that was never code. It happens with the launcher's own corrections turned off too,
// so this is the game's own bug - an entity removed while its pointer stayed on a list the AI
// still walks.
//
// The fix reuses the engine's own answer. A live object of this kind starts with a pointer into
// a loaded module; a destroyed one does not. When the check fails, jump to 0x160878 - the exact
// place the function goes when the candidate is null - and the AI simply treats it as no target.
// Nothing else changes, and a real object takes the original path untouched.
#define AI_TARGET_CALL_RVA 0x1606D1
#define AI_TARGET_BACK_RVA 0x1606DA      // the instruction after the call
#define AI_TARGET_FAIL_RVA 0x160878      // xor al, al; restore; ret

static const unsigned char kAiExpect[] = {
	0x48, 0x8B, 0x02,              // mov rax, qword ptr [rdx]
	0x48, 0x8B, 0xCA,              // mov rcx, rdx
	0xFF, 0x50, 0x38               // call qword ptr [rax + 0x38]
};

// A table of module ranges, and the two places that check a pointer against it.
//
// Both crashes that survived the memory work look the same from the inside: an object is
// destroyed, a pointer to it stays on a list, and the engine calls a method through it. The
// first field of a live object of that kind points into a loaded module; a destroyed one holds
// whatever the allocator left there. That is the whole test.
//
// The first attempt kept a single lowest..highest span instead, which measured 0x1C470000 to
// 0x7FFEEFD9A000 in a real run - a hundred and forty terabytes, with the whole heap inside it.
// It would have passed every destroyed object straight through. Modules land where the system
// puts them and they are not neighbours, so each one needs its own pair.
//
// Each patch site gets its own copy of the table, because the table has to sit within a 32-bit
// offset of the trampoline that reads it, and the modules being patched are far apart.
//
// Layout of a table: count, rejected, the ranges, then the trampoline's code.
#define MR_MAX_RANGES  1024
#define MR_CAVE_SIZE   0x10000
#define MR_MAX_TABLES  4

#define MR_COUNT(t)   ((unsigned long long*)((t) + 0))
#define MR_REJECT(t)  ((unsigned long long*)((t) + 8))
#define MR_ACCEPT(t)  ((unsigned long long*)((t) + 16))
#define MR_RANGES(t)  ((unsigned long long*)((t) + 24))
#define MR_CODE(t)    ((t) + 24 + MR_MAX_RANGES * 16)

static unsigned char* g_mrTable[MR_MAX_TABLES];
static const char*    g_mrName[MR_MAX_TABLES];
static int            g_mrTables = 0;

typedef BOOL (WINAPI *PFN_EnumProcessModules)(HANDLE, HMODULE*, DWORD, LPDWORD);
static PFN_EnumProcessModules g_enumModules = 0;

static bool FindEnumModules(void)
{
	if (g_enumModules) return true;
	HMODULE k = GetModuleHandleA("kernel32.dll");
	if (k) g_enumModules = (PFN_EnumProcessModules)GetProcAddress(k, "K32EnumProcessModules");
	if (!g_enumModules)
	{
		HMODULE ps = GetModuleHandleA("psapi.dll");
		if (!ps) ps = LoadLibraryA("psapi.dll");
		if (ps) g_enumModules = (PFN_EnumProcessModules)GetProcAddress(ps, "EnumProcessModules");
	}
	return g_enumModules != 0;
}

// Whether a module is one of the engine's own. Those hold the tables the checks are looking for
// nearly every time, so they are listed first and the walk usually ends within a few steps.
static bool IsEngineModule(HMODULE m)
{
	char path[MAX_PATH];
	if (!GetModuleFileNameA(m, path, MAX_PATH)) return false;

	const char* name = path;
	for (const char* s = path; *s; s++) if (*s == 92 || *s == '/') name = s + 1;

	if ((name[0] == 'C' || name[0] == 'c') && name[1] == 'r' && name[2] == 'y') return true;
	return (name[0] == 'f' || name[0] == 'F') && name[1] == 'm' && name[2] == 'o' && name[3] == 'd';
}

// One past the last byte a module occupies, from its own header.
static unsigned long long ModuleEnd(HMODULE m)
{
	const unsigned char* base = (const unsigned char*)m;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
	if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
	return (unsigned long long)(ULONG_PTR)base + nt->OptionalHeader.SizeOfImage;
}

// Adds the modules a table does not list yet.
//
// The list only grows and the count is raised last, so a trampoline reads it without locking:
// it either sees a new entry or does not see it yet, never half of one. A module that unloads
// leaves its range behind, which at worst lets one stale pointer through - exactly what the game
// does without these fixes.
static int SyncOneTable(unsigned char* table)
{
	if (!table || !FindEnumModules()) return 0;

	HMODULE mods[512];
	DWORD needed = 0;
	if (!g_enumModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return 0;

	unsigned n = (unsigned)(needed / sizeof(HMODULE));
	if (n > 512) n = 512;

	unsigned long long* ranges = MR_RANGES(table);
	int added = 0;

	for (int pass = 0; pass < 2; pass++)
	for (unsigned i = 0; i < n; i++)
	{
		if ((pass == 0) != IsEngineModule(mods[i])) continue;

		const unsigned long long lo = (unsigned long long)(ULONG_PTR)mods[i];
		const unsigned long long hi = ModuleEnd(mods[i]);
		if (!hi || hi <= lo) continue;

		const unsigned long long have = *MR_COUNT(table);
		unsigned long long k = 0;
		for (; k < have; k++) if (ranges[k * 2] == lo) break;
		if (k < have)
		{
			// Same base, bigger image: a module was replaced by a larger one.
			if (hi > ranges[k * 2 + 1]) ranges[k * 2 + 1] = hi;
			continue;
		}
		if (have >= MR_MAX_RANGES) return added;

		ranges[have * 2 + 0] = lo;
		ranges[have * 2 + 1] = hi;
		InterlockedExchange64((LONGLONG volatile*)MR_COUNT(table), (LONGLONG)(have + 1));
		added++;
	}
	return added;
}

static int SyncModuleRanges(void)
{
	int added = 0;
	for (int i = 0; i < g_mrTables; i++) added += SyncOneTable(g_mrTable[i]);
	return added;
}

// A cave near 'anchor', with the module list already in it.
static unsigned char* NewRangeTable(void* anchor, const char* name)
{
	if (g_mrTables >= MR_MAX_TABLES || !FindEnumModules()) return 0;

	unsigned char* cave = AllocCaveNear(anchor, MR_CAVE_SIZE);
	if (!cave) return 0;

	*MR_COUNT(cave)  = 0;
	*MR_REJECT(cave) = 0;
	*MR_ACCEPT(cave) = 0;
	if (SyncOneTable(cave) <= 0) return 0;

	g_mrName[g_mrTables] = name;
	g_mrTable[g_mrTables++] = cave;
	return cave;
}

// Emits the walk over a table. Falls through when the value is inside some module, and the two
// returned offsets are the jumps to fill in: the first goes to the "not in a module" path, the
// second to the "in a module" path.
//
// Two register layouts, because the two sites have different registers to spare. Everything used
// is scratch by the calling convention and the flags are dead at both sites, so nothing the
// engine is holding gets disturbed.
//   layout 0: value in rax, cursor r10, counter r11   (the AI site)
//   layout 1: value in r10, cursor rax, counter r11   (the sound site, where r10 is the call)
static int EmitRangeWalk(unsigned char* code, int n, const unsigned char* table,
                         int layout, int* fixEmpty, int* fixInside)
{
	if (layout == 0) { code[n++] = 0x4C; code[n++] = 0x8D; code[n++] = 0x15; } // lea r10, [rip+..]
	else             { code[n++] = 0x48; code[n++] = 0x8D; code[n++] = 0x05; } // lea rax, [rip+..]
	{
		const long rel = (long)((const unsigned char*)MR_RANGES(table) - (code + n + 4));
		memcpy(code + n, &rel, 4); n += 4;
	}
	code[n++] = 0x4C; code[n++] = 0x8B; code[n++] = 0x1D;            // mov r11, [rip + count]
	{
		const long rel = (long)((const unsigned char*)MR_COUNT(table) - (code + n + 4));
		memcpy(code + n, &rel, 4); n += 4;
	}

	const int top = n;
	code[n++] = 0x4D; code[n++] = 0x85; code[n++] = 0xDB;            // test r11, r11
	code[n++] = 0x74; *fixEmpty = n; code[n++] = 0x00;               // jz  not-in-a-module

	if (layout == 0)
	{
		code[n++] = 0x49; code[n++] = 0x3B; code[n++] = 0x02;        // cmp rax, [r10]
		code[n++] = 0x72; const int below = n; code[n++] = 0x00;     // jb  next
		code[n++] = 0x49; code[n++] = 0x3B; code[n++] = 0x42; code[n++] = 0x08; // cmp rax, [r10+8]
		code[n++] = 0x72; *fixInside = n; code[n++] = 0x00;          // jb  in-a-module
		code[below] = (unsigned char)(n - (below + 1));
		code[n++] = 0x49; code[n++] = 0x83; code[n++] = 0xC2; code[n++] = 0x10; // add r10, 16
	}
	else
	{
		code[n++] = 0x4C; code[n++] = 0x3B; code[n++] = 0x10;        // cmp r10, [rax]
		code[n++] = 0x72; const int below = n; code[n++] = 0x00;     // jb  next
		code[n++] = 0x4C; code[n++] = 0x3B; code[n++] = 0x50; code[n++] = 0x08; // cmp r10, [rax+8]
		code[n++] = 0x72; *fixInside = n; code[n++] = 0x00;          // jb  in-a-module
		code[below] = (unsigned char)(n - (below + 1));
		code[n++] = 0x48; code[n++] = 0x83; code[n++] = 0xC0; code[n++] = 0x10; // add rax, 16
	}

	code[n++] = 0x49; code[n++] = 0xFF; code[n++] = 0xCB;            // dec r11
	code[n++] = 0xEB;
	code[n] = (unsigned char)(top - (n + 1)); n++;                   // jmp top
	return n;
}

// inc qword ptr [rip + counter]
static int EmitInc(unsigned char* code, int n, const unsigned long long* counter)
{
	code[n++] = 0x48; code[n++] = 0xFF; code[n++] = 0x05;
	const long rel = (long)((const unsigned char*)counter - (code + n + 4));
	memcpy(code + n, &rel, 4); n += 4;
	return n;
}

static int EmitJump(unsigned char* code, int n, const unsigned char* target)
{
	code[n++] = 0xE9;
	const long rel = (long)(target - (code + n + 4));
	memcpy(code + n, &rel, 4); n += 4;
	return n;
}

static const char* PatchAiDeadTarget(void)
{
	unsigned char* ai = (unsigned char*)GetModuleHandleA("CryAISystem.dll");
	if (!ai) return "CryAISystem not loaded";

	unsigned char* at = ai + AI_TARGET_CALL_RVA;
	if (memcmp(at, kAiExpect, sizeof(kAiExpect)) != 0) return "no match";

	unsigned char* table = NewRangeTable(ai, "aifix");
	if (!table) return "no cave";

	unsigned char* code = MR_CODE(table);
	int n = 0, fixEmpty = 0, fixInside = 0;

	code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0x02;            // mov rax, [rdx]
	n = EmitRangeWalk(code, n, table, 0, &fixEmpty, &fixInside);

	// Not in a module: count it and take the engine's own "no target" exit.
	code[fixEmpty] = (unsigned char)(n - (fixEmpty + 1));
	n = EmitInc(code, n, MR_REJECT(table));
	n = EmitJump(code, n, ai + AI_TARGET_FAIL_RVA);

	// In a module: do exactly what the overwritten bytes did, then go back.
	code[fixInside] = (unsigned char)(n - (fixInside + 1));
	n = EmitInc(code, n, MR_ACCEPT(table));
	code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0xCA;            // mov rcx, rdx
	code[n++] = 0xFF; code[n++] = 0x50; code[n++] = 0x38;            // call [rax + 0x38]
	n = EmitJump(code, n, ai + AI_TARGET_BACK_RVA);

	if (!WriteJump(at, code, sizeof(kAiExpect))) return "jump failed";

	static char result[160];
	sprintf(result, "applied (%llu modules listed)", *MR_COUNT(table));
	return result;
}

// Stops the sound engine from calling a method on an object that no longer exists.
//
// The finale cutscene killed the game here, with the level finished and the video about to play:
//
//   0x05AEFE  mov r10, [rcx]           ; the object's table of methods
//   0x05AF0A  mov [rsp+0x20], rax      ; fifth argument
//   0x05AF0F  call [r10 + 0x50]        ; died here
//
// The object was at 0x0B654518 and its first field held 0x0B654500 - a heap address eighteen
// bytes below itself, with text where the addresses of functions belong ("_flap" in the bytes).
// The call went to a non-canonical address, which is why the report says 0xFFFFFFFFFFFFFFFF.
// Two lines earlier the log says why: the cutscene's soundbank was "still queued for preload"
// and "Create sound ... failed! Invalid platform sound" - the sound was never made, and
// something kept using it anyway.
//
// When the check fails, return 0x38 - the value this same function returns two branches up when
// it does not like the object it was given. The caller already handles sound errors; the log is
// full of them. So the cutscene's audio goes quiet instead of taking the game down.
#define SND_CALL_RVA 0x05AF0A
#define SND_BACK_RVA 0x05AF13      // the instruction after the call

static const unsigned char kSndExpect[] = {
	0x48, 0x89, 0x44, 0x24, 0x20,  // mov qword ptr [rsp + 0x20], rax
	0x41, 0xFF, 0x52, 0x50         // call qword ptr [r10 + 0x50]
};

static const char* PatchSoundDeadObject(void)
{
	unsigned char* fm = (unsigned char*)GetModuleHandleA("fmodex64.dll");
	if (!fm) return "fmodex64 not loaded";

	unsigned char* at = fm + SND_CALL_RVA;
	if (memcmp(at, kSndExpect, sizeof(kSndExpect)) != 0) return "no match";

	unsigned char* table = NewRangeTable(fm, "sndfix");
	if (!table) return "no cave";

	unsigned char* code = MR_CODE(table);
	int n = 0, fixEmpty = 0, fixInside = 0;

	code[n++] = 0x48; code[n++] = 0x89; code[n++] = 0x44;            // mov [rsp+0x20], rax
	code[n++] = 0x24; code[n++] = 0x20;                              //   (frees rax for the walk)
	n = EmitRangeWalk(code, n, table, 1, &fixEmpty, &fixInside);

	// Not in a module: count it and return the error this function returns on its own.
	code[fixEmpty] = (unsigned char)(n - (fixEmpty + 1));
	n = EmitInc(code, n, MR_REJECT(table));
	code[n++] = 0xB8; code[n++] = 0x38; code[n++] = 0x00;            // mov eax, 0x38
	code[n++] = 0x00; code[n++] = 0x00;
	n = EmitJump(code, n, fm + SND_BACK_RVA);

	// In a module: make the call the overwritten bytes made, then go back.
	code[fixInside] = (unsigned char)(n - (fixInside + 1));
	n = EmitInc(code, n, MR_ACCEPT(table));
	code[n++] = 0x41; code[n++] = 0xFF; code[n++] = 0x52; code[n++] = 0x50;  // call [r10 + 0x50]
	n = EmitJump(code, n, fm + SND_BACK_RVA);

	if (!WriteJump(at, code, sizeof(kSndExpect))) return "jump failed";

	static char result[160];
	sprintf(result, "applied (%llu modules listed)", *MR_COUNT(table));
	return result;
}

// Makes the process ask the system for memory from the top of the address space.
//
// The game only survives as a 64-bit build because its memory happens to land below the 4 GB
// line, where an address with its upper half lost still points at the right place. That is luck,
// not a fix: every place that stores half a pointer is still there, waiting for a machine, a
// driver or a level that pushes memory higher. MEM_TOP_DOWN flips the condition on purpose - the
// system hands out the highest free address instead of the lowest - so those places fault here,
// on demand, in a two-minute run.
//
// Two earlier attempts at the same exam failed for reasons that had nothing to do with
// truncation. -forcehighheap reserved everything below 4 GB, and the renderer died because D3D
// needs address space of its own. -highslab swapped what CryMalloc returned, and the allocator
// died because it was handed bare memory with none of its own bookkeeping in it. This one changes
// neither: the engine calls VirtualAlloc itself, gets its own memory and lays out its own
// structures in it. Only the address is high.
//
// The hook goes in the import table of every module rather than into kernel32's code: one pointer
// per module, nothing to disassemble, and a module that maps later is picked up on the next pass.
typedef LPVOID (WINAPI *PFN_VA)(LPVOID, SIZE_T, DWORD, DWORD);

static PFN_VA g_origVA = 0;
static SIZE_T           g_topDownMin   = 64 * 1024;   // below this it is not worth the search
static volatile LONG    g_topDownCalls = 0;
static volatile LONG    g_topDownHigh  = 0;           // how many landed above 4 GB
static unsigned long long g_topDownHighest = 0;
static volatile LONGLONG  g_highBytes     = 0;   // how much actually went high

// Why a request was left alone. The census says a quarter of memory went high and nothing about
// what the other three quarters are. These four counters name them: a commit inside a region
// that was reserved before the hook existed, a request for one particular address, something
// below the threshold, or a file view, which is not this call at all.
static volatile LONG     g_skipCommitOnly = 0;
static volatile LONG     g_skipFixedAddr  = 0;
static volatile LONG     g_skipTooSmall   = 0;
static volatile LONGLONG g_skipCommitKb   = 0;
static volatile LONGLONG g_skipFixedKb    = 0;
static volatile LONGLONG g_skipSmallKb    = 0;

// File views - the .pak archives among them - do not come through NtAllocateVirtualMemory at all.
// They are mapped, and a hook that only watches allocation is blind to every byte of them.
typedef LONG (__stdcall *PFN_NtMapView)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T,
                                        PLARGE_INTEGER, PSIZE_T, DWORD, ULONG, ULONG);
static PFN_NtMapView     g_origNtMapView = 0;
static volatile LONG     g_mapCalls      = 0;
static volatile LONG     g_mapHigh       = 0;
static volatile LONGLONG g_mapKb         = 0;
static volatile LONGLONG g_mapHighKb     = 0;
static bool              g_topMap        = false;
static bool              g_memDebug      = false;   // -memdebug: the noisy part of the above

// lowguard - the reason all of the above exists.
//
// This engine crashes when it runs out of address space below 4 GB, and that is the crash
// testers see on the heavy levels: not a bug in any one place, just the 32-bit ceiling arriving.
// Steering everything high all the time would be the blunt answer, and it changes the memory
// layout for every player including the ones who never come near the ceiling.
//
// So the hooks go in always and do nothing, and a watcher measures the free space below the
// line. When it drops under the threshold, steering switches on and new allocations go high
// from then on. Players who never reach the ceiling run exactly as before; a level or a mod
// that would have hit the wall goes past it instead.
static SIZE_T g_lowGuardMb = 512;      // -lowguard:MB
static bool   g_lowGuardOn = false;    // OFF by default - see below
static bool   g_guardFired = false;

// Free address space below 4 GB, in MB. A few hundred VirtualQuery calls; cheap enough for
// every couple of seconds.
static unsigned long long FreeBelow4GB(void)
{
	MEMORY_BASIC_INFORMATION mbi;
	unsigned long long at = 0x10000, free = 0;

	for (int guard = 0; guard < 200000; guard++)
	{
		if (at >= 0x100000000ull) break;
		if (!VirtualQuery((LPCVOID)(ULONG_PTR)at, &mbi, sizeof(mbi))) break;

		unsigned long long size = (unsigned long long)mbi.RegionSize;
		if (at + size > 0x100000000ull) size = 0x100000000ull - at;
		if (mbi.State == MEM_FREE) free += size;

		const unsigned long long next = at + (unsigned long long)mbi.RegionSize;
		if (next <= at) break;
		at = next;
	}
	return free / (1024 * 1024);
}

// Who is holding the memory down. Counting calls says how much was left alone and nothing about
// who asked for it, and the answer decides where the next hook goes: a module with its own page
// allocator is one problem, the process heap growing is another. Attribution is by the first
// stack frame outside ntdll - RtlAllocateHeap and VirtualAlloc are pass-throughs, the module
// above them is the caller that matters. A frame count that never leaves ntdll is the heap
// itself, which has no caller worth naming.
#define CALLER_SLOTS 20
typedef struct { volatile LONGLONG site; volatile LONGLONG kb; volatile LONGLONG kbLow;
                 volatile LONG calls; volatile LONG low; } CallerSite;
static CallerSite g_callers[CALLER_SLOTS];

// ntdll, kernelbase, kernel32 and this launcher itself are all pass-throughs on the way down:
// stopping at any of them names the plumbing instead of the caller. The first frame outside all
// four is the module that actually wanted the memory.
#define PASS_SLOTS 8
static unsigned long long g_passLo[PASS_SLOTS], g_passHi[PASS_SLOTS];
static int g_passCount = 0;
typedef USHORT (WINAPI *PFN_CapStack)(ULONG, ULONG, PVOID*, PULONG);
static PFN_CapStack g_capStack = 0;

static void ReportAllocatorHeads(void);
static int  CheckAllocatorIntegrity(void);

// Which build of the game the corrections were measured against.
//
// Every patch in here names exact bytes at exact offsets inside these five modules. On another
// build they simply do not match, nothing is written, and the game runs as if this launcher were
// not here - which is safe but silent, and silence is how a player ends up with the old crashes
// and no idea why. So the sizes are checked at startup and the answer is said out loud.
//
// File size rather than a hash: it separates builds just as well, costs nothing, and does not
// need eleven megabytes read before the game starts. Crysis 2 has not been patched since 2012,
// so this list is a fixed target rather than a maintenance burden.
typedef struct { const char* name; unsigned long bytes; } KnownModule;

static const KnownModule kSupportedBuild[] = {
	{ "CrySystem.dll",      5225768 },
	{ "CrySoundSystem.dll",  827688 },
	{ "CryRenderD3D11.dll", 3493160 },
	{ "CryScriptSystem.dll", 652584 },
	{ "CryGameReal.dll",   11156264 },
};

static bool g_buildRecognised = true;

static void CheckGameBuild(void)
{
	char line[260];
	int mismatched = 0;

	for (int i = 0; i < (int)(sizeof(kSupportedBuild) / sizeof(kSupportedBuild[0])); i++)
	{
		char path[MAX_PATH];
		sprintf(path, "Bin64\\%s", kSupportedBuild[i].name);

		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
		{
			int n = sprintf(line, "  build: %s is missing%s", kSupportedBuild[i].name, "\n");
			AppendFaultLog(line, (unsigned long)n);
			mismatched++;
			continue;
		}

		const unsigned long got = fad.nFileSizeLow;
		if (got != kSupportedBuild[i].bytes)
		{
			int n = sprintf(line, "  build: %s is %lu bytes, the corrections were measured "
			                "against %lu%s", kSupportedBuild[i].name, got,
			                kSupportedBuild[i].bytes, "\n");
			AppendFaultLog(line, (unsigned long)n);
			mismatched++;
		}
	}

	g_buildRecognised = (mismatched == 0);

	int n;
	if (g_buildRecognised)
		n = sprintf(line, "  build: recognised - all five modules match the supported build "
		            "(1.1.1.217)%s", "\n");
	else
		n = sprintf(line, "  build: NOT the build these corrections were made for (%d module(s) "
		            "differ). The game will run, but the pointer corrections will not apply and "
		            "memory stays below 4 GB%s", mismatched, "\n");
	AppendFaultLog(line, (unsigned long)n);
}
static const char* WhyNotSafeForHighMemory(void);
static bool g_engineFixFailed = false;

// State of the self-check, declared here because the watching thread is defined above the
// module tables the check reads.
static volatile LONG g_integrityBad  = 0;
static volatile LONG g_integrityRuns = 0;
static bool          g_integrityTripped = false;

static volatile LONG g_sampleCount = 0;

// Everything the hook sees, before any filter at all. The counters that follow all narrow the
// field, and if the total here does not account for the memory on the census, then the memory is
// not coming through this call and no amount of work on the filters will find it.
static volatile LONG     g_sawCalls    = 0;
static volatile LONGLONG g_sawLowKb    = 0;
static volatile LONGLONG g_sawHighKb   = 0;
static volatile LONG     g_sawOtherPid = 0;
static volatile LONG g_lowLandCount = 0;

// Something large that ended up below 4 GB anyway, named on the spot. The counters say how much
// is down there and the census says it arrives in 32 MB pieces; this says who asked for one.
static void LowLandingSample(unsigned long long addr, SIZE_T bytes, ULONG type)
{
	if (!g_memDebug) return;
	if (addr == 0 || addr > 0xFFFFFFFFull || bytes < 4 * 1024 * 1024) return;
	if (InterlockedIncrement(&g_lowLandCount) > 8) return;
	if (!g_capStack) return;

	void* frames[14];
	const USHORT n = g_capStack(1, 14, frames, NULL);
	unsigned long long site = 0;
	for (USHORT i = 0; i < n; i++)
	{
		const unsigned long long a = (unsigned long long)(ULONG_PTR)frames[i];
		bool through = false;
		for (int k = 0; k < g_passCount; k++)
			if (a >= g_passLo[k] && a < g_passHi[k]) { through = true; break; }
		if (!through) { site = a; break; }
	}

	char who[80];
	if (!site) strcpy(who, "(never left ntdll)");
	else
	{
		HMODULE mod = 0;
		if (GetModuleHandleExA(0x00000004 | 0x00000002, (LPCSTR)(ULONG_PTR)site, &mod) && mod)
		{
			char full[MAX_PATH];
			if (GetModuleFileNameA(mod, full, MAX_PATH))
			{
				const char* b = strrchr(full, 0x5C);
				sprintf(who, "%s+0x%llX", b ? b + 1 : full,
				        site - (unsigned long long)(ULONG_PTR)mod);
			}
			else sprintf(who, "0x%llX", site);
		}
		else sprintf(who, "0x%llX (no module)", site);
	}

	char line[220];
	int m = sprintf(line, "  landed low: %llu MB at 0x%llX, type 0x%lX, asked by %s%s",
	                (unsigned long long)(bytes / (1024 * 1024)), addr, type, who, "\n");
	AppendFaultLog(line, (unsigned long)m);
}

static void SampleCall(PVOID* base, SIZE_T bytes, ULONG type)
{
	if (!g_memDebug || bytes < 1024 * 1024) return;
	if (InterlockedIncrement(&g_sampleCount) > 10) return;

	char line[200];
	int n = sprintf(line, "  sample: %llu KB, base 0x%llX, type 0x%lX%s%s%s%s",
	                (unsigned long long)(bytes / 1024),
	                (unsigned long long)(ULONG_PTR)*base, type,
	                (type & MEM_RESERVE) ? " reserve" : "",
	                (type & MEM_COMMIT) ? " commit" : "",
	                (type & MEM_TOP_DOWN) ? " topdown" : "", "\n");
	AppendFaultLog(line, (unsigned long)n);
}

static void AttributeCall(SIZE_T bytes, bool high)
{
	if (!g_capStack || bytes < 256 * 1024) return;

	void* frames[12];
	const USHORT n = g_capStack(1, 12, frames, NULL);
	LONGLONG site = 1;                      // 1 = never left ntdll: the process heap growing
	for (USHORT i = 0; i < n; i++)
	{
		const unsigned long long a = (unsigned long long)(ULONG_PTR)frames[i];
		bool through = false;
		for (int k = 0; k < g_passCount; k++)
			if (a >= g_passLo[k] && a < g_passHi[k]) { through = true; break; }
		if (!through) { site = (LONGLONG)a; break; }
	}

	for (int i = 0; i < CALLER_SLOTS; i++)
	{
		if (g_callers[i].site != site)
		{
			if (g_callers[i].site != 0) continue;
			if (InterlockedCompareExchange64((volatile LONGLONG*)&g_callers[i].site,
			                                 site, 0) != 0)
			{
				if (g_callers[i].site != site) continue;
			}
		}
		InterlockedExchangeAdd64((volatile LONGLONG*)&g_callers[i].kb,
		                         (LONGLONG)(bytes / 1024));
		InterlockedIncrement(&g_callers[i].calls);
		if (!high)
		{
			InterlockedExchangeAdd64((volatile LONGLONG*)&g_callers[i].kbLow,
			                         (LONGLONG)(bytes / 1024));
			InterlockedIncrement(&g_callers[i].low);
		}
		return;
	}
}

// Memory the display driver asks for is left exactly where the system would have put it.
//
// The corrections in this launcher cover the engine's allocator, module by module, byte by byte.
// The driver is somebody else's code entirely: nothing here has been checked against it, and it
// is free to pack pointers however it likes. Steering its allocations high is a bet with no
// evidence behind it - and the cutscene failure is what losing that bet looks like, since a
// video sequence is exactly the path that runs through the driver.
#define DRIVER_SLOTS 24   // the driver halves, plus whatever -keeplow adds
static unsigned long long g_drvLo[DRIVER_SLOTS], g_drvHi[DRIVER_SLOTS];
static int  g_drvCount = 0;
static volatile LONG g_drvLeftAlone = 0;

static void NoteDriverModule(HMODULE m)
{
	if (!m || g_drvCount >= DRIVER_SLOTS) return;
	for (int i = 0; i < g_drvCount; i++)
		if (g_drvLo[i] == (unsigned long long)(ULONG_PTR)m) return;

	__try
	{
		const unsigned long long lo = (unsigned long long)(ULONG_PTR)m;
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)m;
		const IMAGE_NT_HEADERS64* pe =
			(const IMAGE_NT_HEADERS64*)((const unsigned char*)m + dos->e_lfanew);
		g_drvLo[g_drvCount] = lo;
		g_drvHi[g_drvCount] = lo + pe->OptionalHeader.SizeOfImage;
		g_drvCount++;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { }
}

// -keeplow:Name, repeatable - memory this module asks for stays where the system would have
// put it. Written for finding out which module a failure belongs to: high memory breaks the
// cutscene, so exclude a module, see whether it comes back, and half the suspects are gone.
// It doubles as the fix if some module turns out to be one we must not touch.
#define KEEPLOW_SLOTS 8
static char g_keepLowNames[KEEPLOW_SLOTS][40];
static int  g_keepLowCount = 0;
static bool g_highAction = false;   // -highaction: steer CryAction's memory too
static bool g_gameLow    = false;   // -gamelow: leave the entire game layer low
// The arena band goes high like everything else. Off by default since 12.09.2026.
//
// It used to be pinned low, and the bisection that put it there was sound as far as it went:
// with the 14-16 MB band high the cutscenes broke, with it low they played. What the bisection
// could not see is that the band was never the cause. The cause was our own guard in the
// CryMovie update loop, which threw away every pointer whose high half was non-zero - see the
// workaround near the bottom of this file. Keeping the arenas low simply kept the sequence
// pointers below 4 GB, where that guard let them through.
//
// With the guard fixed, the band goes high and the cutscenes play: BatteryPark 4.816 and
// FDR 5.875 on the camera-path probe, against 0.004 before. Pinning it now would cost address
// space for nothing, so the default is off; -keepband:LO-HI still works for experiments.
static unsigned g_bandLoKb = 0, g_bandHiKb = 0;   // -keepband:LO-HI, in MB (0 = off)
static volatile LONG g_bandKept = 0;

static void ParseKeepLow(const char* cmd)
{
	const char* at = cmd;
	while (at && (at = strstr(at, "-keeplow:")) != 0 && g_keepLowCount < KEEPLOW_SLOTS)
	{
		at += 9;
		int n = 0;
		while (at[n] && at[n] != ' ' && n < 39) n++;
		memcpy(g_keepLowNames[g_keepLowCount], at, n);
		g_keepLowNames[g_keepLowCount][n] = 0;
		g_keepLowCount++;
		at += n;
	}
}

// The user-mode halves of the display driver, by the names they load under.
static void FindDriverModules(void)
{
	static const char* const kDriverNames[] = {
		"nvwgf2umx.dll", "nvldumdx.dll", "nvoglv64.dll",
		"amdxx64.dll", "atidxx64.dll", "igd10iumd64.dll",
	};
	for (int i = 0; i < (int)(sizeof(kDriverNames) / sizeof(kDriverNames[0])); i++)
		NoteDriverModule(GetModuleHandleA(kDriverNames[i]));

	// -gamelow: the whole game layer stays where it is.
	//
	// The corrections cover the allocator, the sound system, the renderer and the Lua pool -
	// those were found, patched and verified byte for byte. The game layer was never checked
	// against high memory at all, and it is where the cutscene damage shows. Until the exact
	// site is known, the honest split is "steer what was verified, leave the rest".
	if (g_gameLow)
	{
		static const char* const kGameLayer[] = {
			"CryAction.dll", "CryGameReal.dll", "CryGameCrysis2.dll", "CryMovie.dll",
			"CryEntitySystem.dll", "CryAnimation.dll", "CryAISystem.dll", "CryNetwork.dll",
		};
		for (int i = 0; i < (int)(sizeof(kGameLayer) / sizeof(kGameLayer[0])); i++)
			NoteDriverModule(GetModuleHandleA(kGameLayer[i]));
	}

	// CryAction, unless the command line insists otherwise.
	//
	// Steering its memory high stops cutscenes from taking over the view: the sequence does
	// start - the player loses control and their model is hidden - but the camera never
	// switches to the one the sequence drives, so the player stands in their own eyes able
	// only to look around. Bisection put it in this module; what exactly breaks inside is not
	// found yet, and until it is, the honest thing is to leave the module alone rather than
	// bet that it is fine. -highaction opts back in, for looking into it.
	if (!g_highAction)
	{
		HMODULE ca = GetModuleHandleA("CryAction.dll");
		if (ca) NoteDriverModule(ca);
	}

	// Whatever the command line asked to leave alone, treated the same way.
	for (int i = 0; i < g_keepLowCount; i++)
	{
		char withExt[48];
		sprintf(withExt, "%s.dll", g_keepLowNames[i]);
		HMODULE m = GetModuleHandleA(g_keepLowNames[i]);
		if (!m) m = GetModuleHandleA(withExt);
		NoteDriverModule(m);
	}
}

static bool AskedByDriver(void)
{
	if (!g_drvCount || !g_capStack) return false;

	void* frames[14];
	const USHORT n = g_capStack(1, 14, frames, NULL);
	for (USHORT i = 0; i < n; i++)
	{
		const unsigned long long a = (unsigned long long)(ULONG_PTR)frames[i];
		for (int k = 0; k < g_drvCount; k++)
			if (a >= g_drvLo[k] && a < g_drvHi[k]) return true;
	}
	return false;
}

static void AddPassThrough(HMODULE m)
{
	if (!m || g_passCount >= PASS_SLOTS) return;
	const unsigned long long lo = (unsigned long long)(ULONG_PTR)m;

	__try
	{
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)m;
		const IMAGE_NT_HEADERS64* pe =
			(const IMAGE_NT_HEADERS64*)((const unsigned char*)m + dos->e_lfanew);
		g_passLo[g_passCount] = lo;
		g_passHi[g_passCount] = lo + pe->OptionalHeader.SizeOfImage;
		g_passCount++;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}

static void PrepareAttribution(void)
{
	HMODULE nt = GetModuleHandleA("ntdll.dll");
	if (!nt) return;
	g_capStack = (PFN_CapStack)GetProcAddress(nt, "RtlCaptureStackBackTrace");

	AddPassThrough(nt);
	AddPassThrough(GetModuleHandleA("kernelbase.dll"));
	AddPassThrough(GetModuleHandleA("kernel32.dll"));
	AddPassThrough(GetModuleHandleA(NULL));      // the launcher's own hook is a pass-through too

	// The CRT is plumbing as well: every large allocation the engine makes arrives through
	// malloc, and stopping at msvcr90 names the pipe instead of whoever poured into it.
	AddPassThrough(GetModuleHandleA("msvcr90.dll"));

	// And so is the engine's own allocator. CryMalloc lives in CrySystem around 0xA1000 and
	// every module in the game calls it, so a frame in there names the allocator rather than
	// whoever wanted the memory. Only that stretch is skipped, not the whole module: plenty of
	// real callers live elsewhere in CrySystem.
	{
		HMODULE cs = GetModuleHandleA("CrySystem.dll");
		if (cs && g_passCount < PASS_SLOTS)
		{
			const unsigned long long base = (unsigned long long)(ULONG_PTR)cs;
			g_passLo[g_passCount] = base + 0xA0E00;   // CryMalloc's outer wrapper
			g_passHi[g_passCount] = base + 0xA2000;   // through the bucket allocator
			g_passCount++;
		}
	}
}
static unsigned long long g_topDownLowest  = ~0ull;

static LPVOID WINAPI TopDownVirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect)
{
	const bool steer = (addr == NULL) && ((type & MEM_RESERVE) != 0) && (size >= g_topDownMin);
	if (steer) type |= MEM_TOP_DOWN;

	LPVOID p = g_origVA ? g_origVA(addr, size, type, protect)
	                              : VirtualAlloc(addr, size, type, protect);
	if (steer && p)
	{
		const unsigned long long a = (unsigned long long)(ULONG_PTR)p;
		InterlockedIncrement(&g_topDownCalls);
		if (a > 0xFFFFFFFFull) InterlockedIncrement(&g_topDownHigh);
		if (a > g_topDownHighest) g_topDownHighest = a;
		if (a < g_topDownLowest)  g_topDownLowest  = a;
	}
	return p;
}

// -heaphigh: large blocks out of the process heap and into memory of our own, which -topdown
// then places above 4 GB.
//
// The measurement that led here: the hook on NtAllocateVirtualMemory sees 853 MB a level, and
// the census finds 1597 MB sitting below 4 GB in 32 MB pieces. The heap does not reserve those
// through the stub we patched, so no filter on that call was ever going to catch them. What the
// heap does do is hand out blocks, and every module asks for them through an import - HeapAlloc
// in kernel32, which msvcr90's malloc calls directly.
//
// A redirected block is our own VirtualAlloc with a 64-byte header in front of it. Recognising
// one on the way back is two tests: VirtualAlloc is granular to 64 KB, so a pointer of ours is
// always 64 bytes past a 64 KB boundary, and the header carries a magic word. A block from the
// real heap fails the first test almost always and the second one always.
typedef LPVOID (WINAPI *PFN_HeapAlloc)(HANDLE, DWORD, SIZE_T);
typedef LPVOID (WINAPI *PFN_HeapReAlloc)(HANDLE, DWORD, LPVOID, SIZE_T);
typedef BOOL   (WINAPI *PFN_HeapFree)(HANDLE, DWORD, LPVOID);
typedef SIZE_T (WINAPI *PFN_HeapSize)(HANDLE, DWORD, LPCVOID);

static PFN_HeapAlloc   g_origHeapAlloc   = 0;
static PFN_HeapReAlloc g_origHeapReAlloc = 0;
static PFN_HeapFree    g_origHeapFree    = 0;
static PFN_HeapSize    g_origHeapSize    = 0;

static SIZE_T g_heapHighMin = 1024 * 1024;      // -heaphigh:KB
static bool   g_heapHigh    = false;

static volatile LONG     g_hhBlocks = 0;        // redirected and still out there
static volatile LONG     g_hhTotal  = 0;
static volatile LONGLONG g_hhKb     = 0;
static volatile LONGLONG g_hhLiveKb = 0;
static volatile LONG     g_hhFailed = 0;

#define HH_HEADER 64
#define HH_MAGIC  0x48494748504D454DULL          /* MEMPHGIH */

static bool HighBlock(LPVOID p)
{
	if (!p) return false;
	if ((((ULONG_PTR)p) & 0xFFFF) != HH_HEADER) return false;
	return *(volatile unsigned long long*)((unsigned char*)p - HH_HEADER) == HH_MAGIC;
}

static SIZE_T HighBlockSize(LPVOID p)
{
	return (SIZE_T)*(volatile unsigned long long*)((unsigned char*)p - HH_HEADER + 8);
}

static LPVOID HighAlloc(SIZE_T size)
{
	unsigned char* raw = (unsigned char*)VirtualAlloc(NULL, size + HH_HEADER,
	                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!raw)
	{
		InterlockedIncrement(&g_hhFailed);
		return NULL;
	}
	*(unsigned long long*)raw = HH_MAGIC;
	*(unsigned long long*)(raw + 8) = (unsigned long long)size;

	InterlockedIncrement(&g_hhBlocks);
	InterlockedIncrement(&g_hhTotal);
	InterlockedExchangeAdd64(&g_hhKb, (LONGLONG)(size / 1024));
	InterlockedExchangeAdd64(&g_hhLiveKb, (LONGLONG)(size / 1024));
	return raw + HH_HEADER;
}

static LPVOID WINAPI HighHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size)
{
	if (g_heapHigh && size >= g_heapHighMin)
	{
		LPVOID p = HighAlloc(size);              // VirtualAlloc hands back zeroed pages already
		if (p) return p;
	}
	return g_origHeapAlloc(heap, flags, size);
}

static BOOL WINAPI HighHeapFree(HANDLE heap, DWORD flags, LPVOID p)
{
	if (HighBlock(p))
	{
		const SIZE_T was = HighBlockSize(p);
		unsigned char* raw = (unsigned char*)p - HH_HEADER;
		*(unsigned long long*)raw = 0;           // so a double free is not mistaken for a block
		InterlockedDecrement(&g_hhBlocks);
		InterlockedExchangeAdd64(&g_hhLiveKb, -(LONGLONG)(was / 1024));
		return VirtualFree(raw, 0, MEM_RELEASE);
	}
	return g_origHeapFree(heap, flags, p);
}

static SIZE_T WINAPI HighHeapSize(HANDLE heap, DWORD flags, LPCVOID p)
{
	if (HighBlock((LPVOID)p)) return HighBlockSize((LPVOID)p);
	return g_origHeapSize(heap, flags, p);
}

static LPVOID WINAPI HighHeapReAlloc(HANDLE heap, DWORD flags, LPVOID p, SIZE_T size)
{
	const bool mine = HighBlock(p);

	if (mine)
	{
		const SIZE_T was = HighBlockSize(p);
		if (size <= was) return p;               // shrinking in place, the slack is ours to keep
		if (flags & HEAP_REALLOC_IN_PLACE_ONLY) return NULL;

		LPVOID fresh = HighAlloc(size);
		if (!fresh) return NULL;
		memcpy(fresh, p, was);
		HighHeapFree(heap, flags, p);
		return fresh;
	}

	// Growing a heap block past the threshold: move it out of the heap while it is small enough
	// to copy cheaply.
	if (g_heapHigh && p && size >= g_heapHighMin && !(flags & HEAP_REALLOC_IN_PLACE_ONLY))
	{
		const SIZE_T was = g_origHeapSize(heap, flags, p);
		if (was != (SIZE_T)-1 && was < size)
		{
			LPVOID fresh = HighAlloc(size);
			if (fresh)
			{
				memcpy(fresh, p, was);
				g_origHeapFree(heap, flags, p);
				return fresh;
			}
		}
	}

	return g_origHeapReAlloc(heap, flags, p, size);
}

// Replaces every import that currently points at 'from'. Matching by address rather than by name
// catches the api-ms-win-core-memory forwarders as well, which is what most of these DLLs import.
static int RedirectImportsByAddressUnsafe(HMODULE mod, void* from, void* to)
{
	unsigned char* base = (unsigned char*)mod;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
	if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;

	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

	const IMAGE_DATA_DIRECTORY* dir =
		&nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir->VirtualAddress || !dir->Size) return 0;

	int done = 0;
	const IMAGE_IMPORT_DESCRIPTOR* imp = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
	for (; imp->Name; imp++)
	{
		ULONG_PTR* thunk = (ULONG_PTR*)(base + imp->FirstThunk);
		for (; *thunk; thunk++)
		{
			if ((void*)(ULONG_PTR)*thunk != from) continue;

			DWORD old = 0;
			if (!VirtualProtect(thunk, sizeof(*thunk), PAGE_READWRITE, &old)) continue;
			*thunk = (ULONG_PTR)to;
			VirtualProtect(thunk, sizeof(*thunk), old, &old);
			done++;
		}
	}
	return done;
}

// A module can be unloaded between being listed and having its header read, and then reading it
// faults on a page that was mapped a moment ago. Downtown died exactly there: the shader warm-up
// loads D3DCompiler_42.dll and drops it again, and the crash landed in this function with the
// module's base address in both registers. Nothing here is worth a crash - a module that went
// away has no imports left to redirect.
static int RedirectImportsByAddress(HMODULE mod, void* from, void* to)
{
	__try
	{
		return RedirectImportsByAddressUnsafe(mod, from, to);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
}

// Every module mapped right now. Called again as modules appear, since the engine loads most of
// its own long after the launcher starts.
static int HookTopDownEverywhere(void)
{
	if (!g_origVA)
	{
		HMODULE k = GetModuleHandleA("kernel32.dll");
		g_origVA = k ? (PFN_VA)GetProcAddress(k, "VirtualAlloc") : 0;
		if (!g_origVA) return 0;
	}
	if (!FindEnumModules()) return 0;

	HMODULE mods[512];
	DWORD needed = 0;
	if (!g_enumModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return 0;

	unsigned n = (unsigned)(needed / sizeof(HMODULE));
	if (n > 512) n = 512;

	int done = 0;
	for (unsigned i = 0; i < n; i++)
		done += RedirectImportsByAddress(mods[i], (void*)g_origVA, (void*)TopDownVirtualAlloc);

	if (g_heapHigh && g_origHeapAlloc)
		for (unsigned i = 0; i < n; i++)
		{
			// The launcher's own imports are left alone: the redirected functions call the
			// originals through these pointers, and redirecting them would be a loop.
			if (mods[i] == GetModuleHandleA(NULL)) continue;
			done += RedirectImportsByAddress(mods[i], (void*)g_origHeapAlloc,
			                                 (void*)HighHeapAlloc);
			done += RedirectImportsByAddress(mods[i], (void*)g_origHeapFree,
			                                 (void*)HighHeapFree);
			done += RedirectImportsByAddress(mods[i], (void*)g_origHeapReAlloc,
			                                 (void*)HighHeapReAlloc);
			done += RedirectImportsByAddress(mods[i], (void*)g_origHeapSize,
			                                 (void*)HighHeapSize);
		}
	return done;
}

static bool g_topDown = false;

// Walks the whole address space and adds up what is committed, on each side of the 4 GB line.
// This is the number that says whether the engine is really using 64-bit memory: the count of
// intercepted reservations does not, because most of the game's memory arrives another way -
// file mappings for the .pak archives, and driver allocations for textures.
// File views counted apart from private memory: they are a different call, a different hook and
// a different question, and lumping them together hid a gigabyte of .pak under "low".
static unsigned long long g_censusMapLow = 0, g_censusMapHigh = 0;

// The shape of what is left below 4 GB. A thousand scattered 64 KB blocks and five 300 MB slabs
// add up the same and need opposite fixes, so the census keeps the five largest reservations and
// a count of how many there are.
#define CENSUS_TOP 5
static unsigned long long g_topBase[CENSUS_TOP], g_topSize[CENSUS_TOP];
static unsigned long      g_topProt[CENSUS_TOP];
static unsigned long      g_topParts[CENSUS_TOP];
static unsigned long      g_lowSlabs = 0;
static unsigned long long g_lowBySize[4];     // under 1 MB, 1-8, 8-32, 32 and over
static unsigned long      g_lowCount[4];
static unsigned long long g_lowReserved = 0;  // reserved below 4 GB, committed or not
static unsigned long      g_lowResCount = 0;

static void NoteLowRegion(unsigned long long base, unsigned long long bytes,
                          unsigned long prot, unsigned long parts)
{
	if (!bytes) return;
	g_lowSlabs++;

	{
		const int band = (bytes < 1024 * 1024)      ? 0 :
		                 (bytes < 8 * 1024 * 1024)  ? 1 :
		                 (bytes < 32 * 1024 * 1024) ? 2 : 3;
		g_lowBySize[band] += bytes;
		g_lowCount[band]++;
	}
	for (int i = 0; i < CENSUS_TOP; i++)
	{
		if (bytes <= g_topSize[i]) continue;
		for (int k = CENSUS_TOP - 1; k > i; k--)
		{
			g_topSize[k]  = g_topSize[k - 1];
			g_topBase[k]  = g_topBase[k - 1];
			g_topProt[k]  = g_topProt[k - 1];
			g_topParts[k] = g_topParts[k - 1];
		}
		g_topSize[i] = bytes;
		g_topBase[i] = base;
		g_topProt[i] = prot;
		g_topParts[i] = parts;
		return;
	}
}

static void MemoryCensus(unsigned long long* lowMb, unsigned long long* highMb,
                         unsigned long long* imageMb);

static void CensusLine(const char* when)
{
	unsigned long long lo = 0, hi = 0, img = 0;
	MemoryCensus(&lo, &hi, &img);
	char line[200];
	int n = sprintf(line, "  census (%s): %llu MB low in %lu reservation(s), %llu MB high, "
	                "%llu MB modules, %llu MB reserved-not-committed low (%lu)%s",
	                when, lo, g_lowSlabs, hi, img,
	                g_lowReserved / (1024 * 1024), g_lowResCount, "\n");
	AppendFaultLog(line, (unsigned long)n);
}

static void MemoryCensus(unsigned long long* lowMb, unsigned long long* highMb,
                         unsigned long long* imageMb)
{
	unsigned long long low = 0, high = 0, image = 0;
	g_censusMapLow = g_censusMapHigh = 0;
	g_lowSlabs = 0;
	for (int i = 0; i < CENSUS_TOP; i++) { g_topSize[i] = 0; g_topBase[i] = 0; }
	for (int i = 0; i < 4; i++) { g_lowBySize[i] = 0; g_lowCount[i] = 0; }
	g_lowReserved = 0;
	g_lowResCount = 0;
	unsigned long long runBase = 0, runBytes = 0;
	unsigned long runProt = 0, runParts = 0;
	MEMORY_BASIC_INFORMATION mbi;
	unsigned long long at = 0x10000;

	for (int guard = 0; guard < 500000; guard++)
	{
		if (!VirtualQuery((LPCVOID)(ULONG_PTR)at, &mbi, sizeof(mbi))) break;

		const unsigned long long size = (unsigned long long)mbi.RegionSize;

		if (mbi.State == MEM_RESERVE && at < 0x100000000ull && mbi.Type == MEM_PRIVATE)
		{
			g_lowReserved += size;
			g_lowResCount++;
		}

		if (mbi.State == MEM_COMMIT)
		{
			if (mbi.Type == MEM_IMAGE) image += size;
			else if (at >= 0x100000000ull) high += size;
			else low += size;

			if (mbi.Type == MEM_MAPPED)
			{
				if (at >= 0x100000000ull) g_censusMapHigh += size;
				else                      g_censusMapLow  += size;
			}

			// Group the committed parts of one reservation together: a heap segment shows up as
			// a dozen regions with the same allocation base and is one thing, not a dozen.
			if (mbi.Type == MEM_PRIVATE && at < 0x100000000ull)
			{
				const unsigned long long ab = (unsigned long long)(ULONG_PTR)mbi.AllocationBase;
				if (ab != runBase)
				{
					NoteLowRegion(runBase, runBytes, runProt, runParts);
					runBase = ab;
					runBytes = 0;
					runParts = 0;
				}
				runBytes += size;
				runProt = mbi.Protect;
				runParts++;
			}
		}

		const unsigned long long next = at + size;
		if (next <= at) break;
		at = next;
		if (at >= 0x7FFFFFFF0000ull) break;
	}

	NoteLowRegion(runBase, runBytes, runProt, runParts);
	*lowMb = low / (1024 * 1024);
	*highMb = high / (1024 * 1024);
	*imageMb = image / (1024 * 1024);
}

// The same steering, one level down.
//
// Hooking VirtualAlloc through the import tables turned out to catch almost nothing: the engine
// takes its memory from the CRT, and the CRT goes to ntdll without passing through kernel32. A
// run with only that hook reported not a single large reservation. NtAllocateVirtualMemory is
// where they all end up - VirtualAlloc, HeapAlloc and the CRT alike - so the flag means something
// only from here.
//
// The stub is five bytes of "mov r10, rcx" plus "mov eax, <number>", which is enough room for a
// jump, and the copy of those bytes in the cave becomes the way to call the original.
typedef LONG (__stdcall *PFN_NtAllocVM)(HANDLE, PVOID*, ULONG_PTR, SIZE_T*, ULONG, ULONG);

static PFN_NtAllocVM      g_origNtAlloc = 0;
static bool               g_topDownMax  = false;              // the very edge of the space
// Where high memory starts. 8 GB by default.
//
// -highbase:GB moves it, and the reason is in Crytek's own allocator: it indexes its arenas by
// address, shifting the pointer right by 40 bits and using the result as a slot in a fixed-size
// table sized for 2 GB. Below a terabyte that shift yields the same slot for every address, so
// arenas placed high land in the same bucket as the ones down low, and the lookup that asks
// "which arena owns this pointer" starts answering with the wrong one. Placing them past a
// terabyte gives them a slot of their own.
static volatile LONGLONG  g_highCursor  = 0x200000000LL;      // 8 GB, and climbing
static LONGLONG           g_highBase    = 0x200000000LL;
static LONGLONG           g_highCeiling = 0x4000000000LL;     // 256 GB

// Memory high enough to expose a lost upper half, low enough that the rest of Windows still
// works. MEM_TOP_DOWN hands out 0x00007FF4........, and at that height dsound.dll dies on its
// own: it packs a pointer into 43 bits (the mask 0x000007FFFFFFFFF8 was sitting in rdi when it
// faulted). Eight gigabytes up is plenty - the upper half of the address is non-zero, which is
// all the exam needs - and leaves every packing scheme in the system intact.
// Modules that still keep pointers in 32 bits somewhere, and so must be served from below the
// 4 GB line until those places are found and widened.
//
//   CryScriptSystem - the Lua allocator swaps list heads with a 32-bit compare-and-exchange
//                     (lock cmpxchg dword ptr [rcx], edx at 0x1700): half the pointer is never
//                     written, so the head becomes garbage the moment the block is high.
//   dsound          - packs a pointer into 43 bits; not the game's code, and not ours to fix.
//
// Each name removed from this list is one subsystem that has become genuinely 64-bit.
//
// Both entries are gone now: CryScriptSystem's compare-and-exchange has been widened, and
// dsound only broke at the very top of the address space, which is no longer where memory goes.
// The list stays because the next subsystem to be found will go in it while it is being fixed.
static const char* const kNotReadyYet[] = { 0, 0, 0 };

// Whether this allocation is being made on behalf of a module that is not ready. Reading the
// stack costs something, but only large reservations get here - a few hundred over a whole run.
static bool CalledByUnreadyModule(void)
{
	if (!kNotReadyYet[0] && !kNotReadyYet[1] && !kNotReadyYet[2]) return false;

	void* frames[12];
	const USHORT n = CaptureStackBackTrace(1, 12, frames, NULL);

	for (USHORT i = 0; i < n; i++)
	{
		HMODULE m = 0;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        (LPCSTR)frames[i], &m) || !m)
			continue;

		char path[MAX_PATH];
		if (!GetModuleFileNameA(m, path, MAX_PATH)) continue;

		const char* name = path;
		for (const char* s = path; *s; s++) if (*s == 92 || *s == '/') name = s + 1;

		for (int k = 0; k < 3; k++)
			if (kNotReadyYet[k] && _stricmp(name, kNotReadyYet[k]) == 0) return true;
	}
	return false;
}

// -shadow - the trap that turns the silent breakage into a loud one.
//
// The arena band breaks the game without an exception and without a log line, which is the one
// shape of failure none of our tools can see. Four auditors read the code looking for a
// truncated pointer and found nothing, so this stops reading and starts catching.
//
// If a pointer to an arena loses its upper half somewhere, the access that follows goes to the
// lower 32 bits of that address. So: reserve a strip of low address space and give it no
// access, then place the arenas high at addresses whose lower 32 bits land inside that strip.
// A lost upper half now touches a page that cannot be touched, and the fault names the exact
// instruction that did it.
//
// The strip is kept below 0x80000000 on purpose: down there the sign-extended form of the
// truncation (movsxd, which this engine does use) has the same value as the plain one, so a
// single trap catches both shapes.
//
// A fault is healed rather than fatal - the page is committed and execution continues - so one
// run lists every place that truncates instead of stopping at the first.
static bool      g_shadowOn     = false;
static SIZE_T    g_shadowMb     = 512;
static ULONG_PTR g_shadowBase   = 0;
static ULONG_PTR g_shadowEnd    = 0;
static volatile LONGLONG g_shadowCursor = 0;
static volatile LONG g_shadowFloor  = 2;    // 2 -> arenas start 8 GB up
static volatile LONG g_shadowPlaced = 0;

// The bottom 32 bits of every arena placed under the trap. A truncated copy of an arena pointer
// equals one of these, or lands just inside one - so the hunt looks for exactly that instead of
// for any value in the strip, which at 512 MB wide catches ordinary numbers by the hundred.
#define SHADOW_ARENAS 512
static unsigned long g_arenaLow[SHADOW_ARENAS];
static volatile LONG g_arenaCount = 0;
static SIZE_T        g_huntWindow = 64;     // -hunt:BYTES from the start of an arena

// -arenamax:N - send only the first N arenas high, keep the rest low.
//
// Now that a run can be judged automatically, the question "which arena breaks it" can be
// answered by halving. Arenas are handed out in the same order every load, so N is a stable
// coordinate: find the smallest N that breaks the cutscene and the arena at that position is
// the one, with its caller and its size already recorded.
static long g_arenaMax  = -1;             // -1 = flag absent; 0 = every arena stays low
static long g_arenaMin  = 1;              // -arenamin:N - the first arena allowed up
static long g_arenaWatch = 0;             // -arenawatch:N - print the whole stack for this one

// -arenaat:MB - put the watched arena at a chosen height, nothing else moved.
//
// The trap proved the pointer is not truncated, so the next candidate is a 32-bit OFFSET: code
// that stores "how far is this from my base" in an int leaves no truncated address anywhere in
// memory, yet stops finding the object once the distance passes two gigabytes. That predicts a
// threshold, and a threshold can be measured: walk the arena up through 3, 4, 5, 6 GB and see
// where the cutscene stops playing.
static unsigned long long g_arenaAtMb = 0;
static volatile LONG g_arenaNth = 0;

static void NameCode(ULONG_PTR pc, char* out);   // defined with the shadow trap below
static LONG TryShadowHigh(PFN_NtAllocVM orig, HANDLE proc, PVOID* base, SIZE_T* size,
                          ULONG type, ULONG protect);

// Everything about one arena: who asked, from where, how big, where it went.
//
// Bisection named arena #37 out of forty. Two things are still open and this answers the first:
// which code asks for that particular one. The second - whether #37 is special or whether
// thirty-seven is simply one too many - is what -arenamin is for: shifting the window keeps the
// count the same while changing which arenas are in it.
static void ReportArena(long nth, SIZE_T bytes, ULONG_PTR where, const char* what)
{
	char line[600];
	int n = sprintf(line, "  arena #%ld: %llu bytes -> 0x%llX (%s)%s", nth,
	                (unsigned long long)bytes, (unsigned long long)where, what, "\n");
	AppendFaultLog(line, (unsigned long)n);

	void* frames[24];
	const USHORT got = CaptureStackBackTrace(1, 24, frames, NULL);

	for (USHORT f = 0; f < got; f++)
	{
		char who[220];
		NameCode((ULONG_PTR)frames[f], who);

		// Frames inside ntdll and the CRT are plumbing; the first name outside them is the
		// caller that matters, but print them all - the shape of the stack is the evidence.
		n = sprintf(line, "      [%2d] %s%s", (int)f, who, "\n");
		AppendFaultLog(line, (unsigned long)n);
	}
}

// -shadowdry: the control run. The strip is reserved and the arena addresses are computed and
// recorded exactly as they would be, but the arenas themselves stay low, where the game is known
// to work. Any value the hunt finds in this run is background - a constant, a size, a hash that
// happens to look like an arena address. Only what shows up in the real run and NOT here can be
// a truncated pointer. Without this the hunt cannot tell a finding from a coincidence.
static bool g_shadowDry = false;
static volatile LONG g_shadowHits   = 0;
static volatile LONG g_shadowHealed = 0;

// Instruction addresses already reported, so a fault inside a loop does not fill the file.
#define SHADOW_SEEN 96
static volatile LONGLONG g_shadowSeen[SHADOW_SEEN];
static volatile LONG     g_shadowSeenCount = 0;

static void NameCode(ULONG_PTR pc, char* out)
{
	HMODULE m = 0;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &m) && m)
	{
		char path[MAX_PATH];
		if (GetModuleFileNameA(m, path, MAX_PATH))
		{
			const char* nm = path;
			for (const char* s = path; *s; s++) if (*s == 92 || *s == 47) nm = s + 1;
			sprintf(out, "%s+0x%llX", nm, (unsigned long long)(pc - (ULONG_PTR)m));
			return;
		}
	}
	sprintf(out, "0x%llX", (unsigned long long)pc);
}

static LONG CALLBACK ShadowVeh(EXCEPTION_POINTERS* ep)
{
	if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
	if (ep->ExceptionRecord->ExceptionCode != (DWORD)EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;
	if (ep->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;

	const ULONG_PTR op   = (ULONG_PTR)ep->ExceptionRecord->ExceptionInformation[0];
	const ULONG_PTR what = (ULONG_PTR)ep->ExceptionRecord->ExceptionInformation[1];

	if (what < g_shadowBase || what >= g_shadowEnd) return EXCEPTION_CONTINUE_SEARCH;

	InterlockedIncrement(&g_shadowHits);

	const ULONG_PTR pc = (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;

	// Report each instruction once.
	bool fresh = true;
	const LONG have = g_shadowSeenCount;
	for (LONG i = 0; i < have && i < SHADOW_SEEN; i++)
		if ((ULONG_PTR)g_shadowSeen[i] == pc) { fresh = false; break; }

	if (fresh && have < SHADOW_SEEN)
	{
		const LONG slot = InterlockedIncrement(&g_shadowSeenCount) - 1;
		if (slot < SHADOW_SEEN)
		{
			g_shadowSeen[slot] = (LONGLONG)pc;

			char who[200], line[460];
			NameCode(pc, who);

			// The caller above it: the truncating instruction often sits inside a shared
			// allocator template, and the module that called it is the useful half.
			char up[200];
			up[0] = 0;
			void* frames[8];
			const USHORT n = CaptureStackBackTrace(0, 8, frames, NULL);
			for (USHORT f = 0; f < n; f++)
			{
				if ((ULONG_PTR)frames[f] == pc) continue;
				HMODULE fm = 0;
				if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				                        (LPCSTR)frames[f], &fm) || !fm) continue;
				NameCode((ULONG_PTR)frames[f], up);
				break;
			}

			int ln = sprintf(line, "  TRUNCATION: %s %s 0x%llX  (called from %s)%s",
			                 who,
			                 op == 1 ? "wrote" : (op == 8 ? "executed" : "read"),
			                 (unsigned long long)what,
			                 up[0] ? up : "?", "\n");
			AppendFaultLog(line, (unsigned long)ln);
		}
	}

	// Heal it: give the page real memory so the run carries on and the remaining truncations
	// show themselves too. The data is zeroes, so the game behaves as it does today.
	if (g_shadowHealed < 8192)
	{
		void* page = (void*)(what & ~(ULONG_PTR)0xFFF);
		if (VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE))
		{
			InterlockedIncrement(&g_shadowHealed);
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// One contiguous strip of low address space, reserved and untouchable. Taken early, while the
// engine has not claimed anything down there yet.
static void ReserveShadowStrip(void)
{
	const SIZE_T want = g_shadowMb * 1024 * 1024;

	// Deliberately not a round address. A strip starting at 0x10000000 makes every arena
	// address look like an ordinary constant - 0x10000000 itself appears in engine data by the
	// hundred - and the hunt drowns in them. Starting at an odd offset, with an odd stride,
	// gives the arenas addresses that nothing else in the process happens to hold.
	for (ULONG_PTR at = 0x11A30000; at + want < 0x78000000; at += 0x1730000)
	{
		void* got = VirtualAlloc((LPVOID)at, want, MEM_RESERVE, PAGE_NOACCESS);
		if (got)
		{
			g_shadowBase   = (ULONG_PTR)got;
			g_shadowEnd    = g_shadowBase + want;
			g_shadowCursor = (LONGLONG)(g_shadowBase + 0x2F0000);
			AddVectoredExceptionHandler(1, ShadowVeh);
			return;
		}
	}
}

// An arena placed high, with its lower 32 bits inside the strip.
static LONG TryShadowHigh(PFN_NtAllocVM orig, HANDLE proc, PVOID* base, SIZE_T* size,
                          ULONG type, ULONG protect)
{
	if (!g_shadowBase) return -1;

	const SIZE_T want = *size;
	LONGLONG step = (LONGLONG)((want + 0x1FFFF) & ~(SIZE_T)0xFFFF);
	if (step < 0x100000LL) step = 0x100000LL;

	type |= MEM_RESERVE;

	for (int attempt = 0; attempt < 48; attempt++)
	{
		LONGLONG low = InterlockedExchangeAdd64(&g_shadowCursor, step);

		// Ran off the end of the strip: start again at its base one floor higher, so the lower
		// 32 bits repeat while the full addresses stay distinct.
		if (low + step > (LONGLONG)g_shadowEnd)
		{
			InterlockedExchange64(&g_shadowCursor, (LONGLONG)g_shadowBase);
			if (InterlockedIncrement(&g_shadowFloor) > 4000) return -1;
			continue;
		}

		PVOID p = (PVOID)(ULONG_PTR)(((LONGLONG)g_shadowFloor << 32) + low);

		// Control run: record the address, place nothing.
		if (g_shadowDry)
		{
			const LONG slot = InterlockedIncrement(&g_shadowPlaced) - 1;
			if (slot < SHADOW_ARENAS)
			{
				g_arenaLow[slot] = (unsigned long)((ULONG_PTR)p & 0xFFFFFFFFul);
				InterlockedIncrement(&g_arenaCount);
			}
			return -1;
		}

		SIZE_T sz = want;
		const LONG st = orig(proc, &p, 0, &sz, type, protect);
		if (st >= 0)
		{
			*base = p;
			*size = sz;
			const LONG slot = InterlockedIncrement(&g_shadowPlaced) - 1;
			if (slot < SHADOW_ARENAS)
			{
				g_arenaLow[slot] = (unsigned long)((ULONG_PTR)p & 0xFFFFFFFFul);
				InterlockedIncrement(&g_arenaCount);
			}
			return st;
		}
	}
	return -1;
}

// The second trap: hunting for the truncated value itself.
//
// The first trap catches a lost upper half only when something dereferences it. A pointer that
// is merely compared - "is this the object I cached?" - answers no and the function quietly does
// nothing, with no fault to catch. That is exactly the shape of the arena failure: no exception,
// no log line, a cutscene that never starts.
//
// But the truncated value is still sitting in memory somewhere. And the shadow strip makes it
// unmistakable: nothing is ever committed inside it, so any 64-bit word whose upper half is zero
// and whose lower half points into the strip cannot be a real pointer. It can only be the
// bottom of one of our arenas with its top half lost.
//
// So walk every committed page and look for exactly that. A hit names the address the truncated
// copy lives at, which names the structure, which names the code that wrote it.
static volatile LONG g_huntRuns  = 0;
static volatile LONG g_huntFound = 0;

// Where the modules live, so a hit can be reported as module+RVA rather than a bare address.
static bool AddressInAModule(ULONG_PTR a, char* out)
{
	HMODULE m = 0;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m)
	{
		NameCode(a, out);
		return true;
	}
	return false;
}

static void ScanOneRegion(ULONG_PTR from, SIZE_T bytes)
{
	__try
	{
		const ULONG_PTR lo = g_shadowBase, hi = g_shadowEnd;
		const unsigned long long* w = (const unsigned long long*)from;
		const SIZE_T words = bytes / 8;

		for (SIZE_T i = 0; i < words; i++)
		{
			const unsigned long long v = w[i];

			// Upper half gone, lower half inside the strip - and not just anywhere in it, but
			// inside the first few kilobytes of an arena we actually placed. The strip alone is
			// half a gigabyte wide and ordinary numbers fall into it constantly; an arena
			// header is a target narrow enough that a hit means something.
			if (v < (unsigned long long)lo || v >= (unsigned long long)hi) continue;

			bool isArena = false;
			const LONG have = g_arenaCount;
			for (LONG a = 0; a < have && a < SHADOW_ARENAS; a++)
			{
				const unsigned long long ab = (unsigned long long)g_arenaLow[a];
				if (v >= ab && v < ab + (unsigned long long)g_huntWindow) { isArena = true; break; }
			}

			if (isArena)
			{
				if (g_huntFound >= 40) return;
				const LONG n = InterlockedIncrement(&g_huntFound);
				if (n > 40) return;

				const ULONG_PTR where = (ULONG_PTR)&w[i];

				char site[200];
				if (!AddressInAModule(where, site))
					sprintf(site, "heap 0x%llX", (unsigned long long)where);

				// What owns the structure this word sits in: the nearest word in a 128-byte
				// window that points into a loaded module is usually its vtable.
				char owner[200];
				owner[0] = 0;
				const unsigned long long* around = (const unsigned long long*)
				                                 (where & ~(ULONG_PTR)7);
				for (int k = -8; k <= 8 && !owner[0]; k++)
				{
					ULONG_PTR cand = 0;
					if (!SafePeek(&around[k], &cand) || cand < 0x10000) continue;
					if (cand == (ULONG_PTR)v) continue;
					char nm[200];
					if (AddressInAModule(cand, nm))
						sprintf(owner, "%s at %+d", nm, k * 8);
				}

				char line[480];
				int ln = sprintf(line, "  TRUNCATED PTR: %s holds 0x%llX (arena bottom), "
				                 "owner %s%s",
				                 site, (unsigned long long)v,
				                 owner[0] ? owner : "unknown", "\n");
				AppendFaultLog(line, (unsigned long)ln);
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}

static void HuntForTruncatedPointers(void)
{
	if (!g_shadowBase) return;
	InterlockedIncrement(&g_huntRuns);

	MEMORY_BASIC_INFORMATION mbi;
	ULONG_PTR at = 0x10000;

	for (int guard = 0; guard < 400000; guard++)
	{
		if (!VirtualQuery((LPCVOID)at, &mbi, sizeof(mbi))) break;

		const ULONG_PTR base = (ULONG_PTR)mbi.BaseAddress;
		const SIZE_T    size = mbi.RegionSize;

		const bool readable = (mbi.State == MEM_COMMIT) &&
		                      ((mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
		                                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
		                                       PAGE_EXECUTE_WRITECOPY)) != 0) &&
		                      ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0);

		// Skip the strip itself and anything the trap healed inside it.
		const bool isStrip = (base >= g_shadowBase && base < g_shadowEnd);

		// Our own module holds the list being searched for.
		HMODULE self = GetModuleHandleA(0);
		const bool isSelf = (self && base >= (ULONG_PTR)self &&
		                     base < (ULONG_PTR)self + 0x200000);

		if (readable && !isStrip && !isSelf && size && size < 0x40000000)
			ScanOneRegion(base, size);

		const ULONG_PTR next = base + size;
		if (next <= at) break;
		at = next;
		if (g_huntFound >= 40) break;
	}
}

// -say:COMMAND - ask the running game something, in its own console.
//
// The arena failure has no automatic symptom: no exception, no missing log line, and the one
// place a human sees it (a cutscene that never starts) does not happen under +map at all. But
// the engine will answer questions if asked - the console is reachable from gEnv, and a Lua
// line through it can print the number of entities the level actually created, which is the
// other half of what the failure looks like: NPCs that never appear.
//
// The command is handed over with deferred execution, so the engine runs it on its own thread
// at its own time rather than having it called into from ours.
// Same global the camera trace uses, needed here first.
#define GENV_RVA_IN_GAMEREAL 0xA0E8C0
#define PCONSOLE_OFF         0xA0
#define EXECUTESTRING_SLOT   33

// -pakinfo: where the file-reading methods actually live.
//
// The read that quietly does nothing above 4 GB is called through a vtable - CrySystem+0xBEA84
// is call [rdi+0x108], slot 33 of the file interface. Statically that is a dead end without
// RTTI; live it is three reads: gEnv+0x50 is pCryPak, its first word is the vtable, and the
// slots are function pointers into CrySystem. Printing them as RVAs makes the next step a
// disassembly of a known address instead of a search.
#define PCRYPAK_OFF 0x50
static bool g_pakInfo = false;
static bool g_saidPak = false;

static char  g_sayWhat[400] = { 0 };
static DWORD g_sayAfter     = 75;      // seconds; a level needs to be up first
// How many times to repeat. Three samples tell a stuck sequence from a running one, but a
// command with a side effect - "map downtown" - must be sent once and only once.
static int   g_sayMax       = 3;       // -sayonce
static DWORD g_sayEvery     = 10;      // -sayevery:N seconds between repeats
static int   g_saidTimes   = 0;      // the same question three times, ten seconds apart

typedef void (__fastcall *PFN_ExecuteString)(void*, const char*, bool, bool);

static bool RunConsoleCommand(const char* cmd)
{
	HMODULE gr = GetModuleHandleA("CryGameReal.dll");
	if (!gr) return false;

	ULONG_PTR env = 0;
	if (!SafePeek((unsigned char*)gr + GENV_RVA_IN_GAMEREAL, &env) || env < 0x10000)
		return false;

	ULONG_PTR con = 0;
	if (!SafePeek((unsigned char*)env + PCONSOLE_OFF, &con) || con < 0x10000)
		return false;

	ULONG_PTR vt = 0;
	if (!SafePeek((const void*)con, &vt) || vt < 0x10000) return false;

	ULONG_PTR fn = 0;
	if (!SafePeek((const void*)(vt + EXECUTESTRING_SLOT * 8), &fn) || fn < 0x10000)
		return false;

	__try
	{
		// silent = false so the answer reaches Game.log, deferred = true so the engine runs it
		// where it runs everything else.
		((PFN_ExecuteString)fn)((void*)con, cmd, false, true);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
	return true;
}

// Watching the XML buffer fill up - or not.
//
// The allocation that breaks the game turned out to be the XML reader's read buffer: CrySystem
// asks for a block the size of the file, hands it to CryPak::FReadRaw, and parses what lands in
// it. Everything up to that point is 64-bit clean - the pointer goes into r13 whole and reaches
// the read call whole. So the question is whether the read actually fills it when it sits above
// the 4 GB line, and that can be answered by looking: the first bytes of the level's object XML
// are recognisable text.
//
// Reading someone else's buffer while they use it is safe here in the one way that matters - it
// only reads, never writes - and the answer decides where to look next: an empty buffer means
// the file read is the failure, a full one means the parser is.
// -peekfind:WORD - is this word anywhere in the watched buffer?
//
// The sequences never get added, and the code that would add them gives up silently when the
// XML has no "SequenceData" node. So the question is whether that part of the file ever reached
// the buffer: if the word is there, the read was complete and the parser is at fault; if it is
// missing, the file arrived truncated.
static char g_peekFind[64] = { 0 };

static volatile ULONG_PTR g_peekAt    = 0;
static volatile SIZE_T    g_peekBytes = 0;

static void DescribeBytes(const char* what, const unsigned char* at, char* out)
{
	// 24 bytes as hex, then the same as text with unprintables dotted - enough to tell XML from
	// zeroes without dumping the file into the log.
	int n = sprintf(out, "  peek %s: ", what);
	for (int i = 0; i < 24; i++) n += sprintf(out + n, "%02X ", at[i]);
	n += sprintf(out + n, " |");
	for (int i = 0; i < 24; i++)
		n += sprintf(out + n, "%c", (at[i] >= 32 && at[i] < 127) ? at[i] : '.');
	sprintf(out + n, "|%s", "\n");
}

static DWORD WINAPI PeekThread(LPVOID)
{
	unsigned char first[24], last[24];
	bool everFilled = false;
	int  reads = 0, fails = 0;
	bool saidGone = false;

	for (int pass = 0; pass < 2000; pass++)
	{
		Sleep(50);

		const ULONG_PTR at = g_peekAt;
		const SIZE_T    sz = g_peekBytes;
		if (!at || !sz) continue;

		// Head and tail in separate guards: a reserved-but-not-yet-committed tail must not hide
		// what the head says.
		bool ok = true;
		__try { memcpy(first, (const void*)at, 24); }
		__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }

		__try { memcpy(last, (const void*)(at + sz - 32), 24); }
		__except (EXCEPTION_EXECUTE_HANDLER) { memset(last, 0xEE, 24); }

		// Say where we are every ten seconds, so silence is never the answer.
		if ((pass % 200) == 0)
		{
			char st[200];
			int sn = sprintf(st, "  peek: pass %d, at 0x%llX, %d read(s), %d fail(s)%s",
			                 pass, (unsigned long long)at, reads, fails, "\n");
			AppendFaultLog(st, (unsigned long)sn);
		}

		if (!ok)
		{
			// The buffer is gone - freed after parsing, or never really there. Say so once,
			// because "no output at all" reads the same as "the thread never ran".
			fails++;
			if (!saidGone && reads > 0)
			{
				saidGone = true;
				char line[200];
				int n = sprintf(line, "  peek: buffer unreadable after %d read(s), %s%s",
				                reads, everFilled ? "it had been filled" : "IT WAS NEVER FILLED",
				                "\n");
				AppendFaultLog(line, (unsigned long)n);
			}
			continue;
		}

		reads++;

		// Look for the word that decides the question, over the whole block.
		if (g_peekFind[0] && (pass % 40) == 0)
		{
			const int want = (int)strlen(g_peekFind);
			SIZE_T found = 0, hits = 0;
			__try
			{
				const char* b = (const char*)at;
				for (SIZE_T i = 0; i + (SIZE_T)want < sz; i++)
				{
					if (b[i] != g_peekFind[0]) continue;
					int k = 1;
					while (k < want && b[i + k] == g_peekFind[k]) k++;
					if (k == want) { if (!hits) found = i; hits++; }
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}

			char line[220];
			int fn = sprintf(line, "  peek find '%s': %llu hit(s), first at +0x%llX%s",
			                 g_peekFind, (unsigned long long)hits,
			                 (unsigned long long)found, "\n");
			AppendFaultLog(line, (unsigned long)fn);
		}

		// The first 24 bytes are the CRT's own header, the same whether the block is high or
		// low. What matters is whether the file's text ever lands in the block, so look for it:
		// scan the first 64 KB for a run of printable characters.
		if (!everFilled)
		{
			char found[80];
			found[0] = 0;

			__try
			{
				const unsigned char* b = (const unsigned char*)at;
				const SIZE_T span = (sz < 65536) ? sz : 65536;

				for (SIZE_T i = 0; i + 40 < span; i++)
				{
					int run = 0;
					while (run < 40 && b[i + run] >= 32 && b[i + run] < 127) run++;
					if (run >= 32)
					{
						for (int k = 0; k < 40; k++) found[k] = (char)b[i + k];
						found[40] = 0;
						break;
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}

			if (found[0])
			{
				everFilled = true;
				char line[220];
				int fn = sprintf(line, "  peek TEXT IN BUFFER: %s%s", found, "\n");
				AppendFaultLog(line, (unsigned long)fn);
			}
		}

		bool any = false;
		for (int i = 0; i < 24; i++) if (first[i]) { any = true; break; }

		// The first successful look, whatever it holds - zeroes are the interesting answer here.
		if (reads == 1)
		{
			char line[400];
			DescribeBytes(any ? "first look" : "first look (EMPTY)", first, line);
			AppendFaultLog(line, (unsigned long)strlen(line));
		}

		(void)any;

		// Every four seconds, how much of the whole block is non-zero. Sampled every 64th byte
		// so 16 MB costs nothing, and it answers the question the first 64 KB cannot: whether
		// the data landed somewhere else in the buffer or never landed at all.
		if ((pass % 80) == 0)
		{
			SIZE_T nz = 0, seen = 0, readable = 0;
			__try
			{
				const unsigned char* b = (const unsigned char*)at;
				for (SIZE_T i = 0; i < sz; i += 64)
				{
					seen++;
					if (b[i]) nz++;
					readable = i;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}

			char line[240];
			int sn = sprintf(line, "  peek fill: %llu of %llu sampled bytes non-zero"
			                 " (%llu%%), readable up to +0x%llX of 0x%llX%s",
			                 (unsigned long long)nz, (unsigned long long)seen,
			                 seen ? (unsigned long long)(nz * 100 / seen) : 0,
			                 (unsigned long long)readable, (unsigned long long)sz, "\n");
			AppendFaultLog(line, (unsigned long)sn);
		}
	}
	return 0;
}

// -readtest - does a file read into high memory work at all?
//
// The engine's XML buffer above 4 GB comes back empty while the read reports success. The path
// is CCryPak::FReadRaw -> CZipPseudoFile::FRead -> CIOWrapper::Fread, which is the CRT's fread.
// So before reverse-engineering any further: reproduce it standalone. Same CRT, same kind of
// buffer, same size, one from below the line and one from above, and read a real file into each.
//
// If both come back full, the fault is in the engine and the search continues there. If the high
// one comes back empty, the fault is under the engine entirely - and that changes what the fix
// has to be.
static void RunReadTest(void)
{
	// Something big enough to be worth reading and certain to exist.
	const char* candidates[3];
	candidates[0] = "GameCrysis2/GameData.pak";
	candidates[1] = "Bin64/CryGameReal.dll";
	candidates[2] = "Bin64/CrySystem.dll";

	const char* path = 0;
	for (int i = 0; i < 3 && !path; i++)
	{
		FILE* probe = fopen(candidates[i], "rb");
		if (probe) { fclose(probe); path = candidates[i]; }
	}

	char line[400];
	if (!path)
	{
		int n = sprintf(line, "  readtest: no test file found%s", "\n");
		AppendFaultLog(line, (unsigned long)n);
		return;
	}

	const SIZE_T want = 16580608;     // the same size the engine asked for

	// Two buffers: one wherever the system puts it, one deliberately above 4 GB.
	void* low  = VirtualAlloc(NULL, want, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	void* high = 0;
	for (ULONG_PTR at = 0x300000000ull; at < 0x380000000ull && !high; at += 0x2000000)
		high = VirtualAlloc((LPVOID)at, want, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

	int n = sprintf(line, "  readtest: file %s, low 0x%llX, high 0x%llX%s", path,
	                (unsigned long long)(ULONG_PTR)low,
	                (unsigned long long)(ULONG_PTR)high, "\n");
	AppendFaultLog(line, (unsigned long)n);

	if (!low || !high) return;

	for (int which = 0; which < 2; which++)
	{
		unsigned char* buf = (unsigned char*)(which ? high : low);
		memset(buf, 0, 4096);

		FILE* f = fopen(path, "rb");
		if (!f) continue;

		const size_t got = fread(buf, 1, want, f);
		fclose(f);

		// How much of the first 4 KB is actually non-zero - a read that did nothing leaves the
		// memset behind.
		int nonzero = 0;
		for (int i = 0; i < 4096; i++) if (buf[i]) nonzero++;

		n = sprintf(line, "  readtest %s: fread returned %llu, first 4 KB has %d non-zero byte(s)"
		            " -> %s%s",
		            which ? "HIGH" : "low ", (unsigned long long)got, nonzero,
		            nonzero ? "data arrived" : "NOTHING WAS WRITTEN", "\n");
		AppendFaultLog(line, (unsigned long)n);

		// And the same through ReadFile, in case the CRT is the difference.
		memset(buf, 0, 4096);
		HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		                       FILE_ATTRIBUTE_NORMAL, NULL);
		if (h != INVALID_HANDLE_VALUE)
		{
			DWORD read = 0;
			const BOOL ok = ReadFile(h, buf, (DWORD)want, &read, NULL);
			CloseHandle(h);

			nonzero = 0;
			for (int i = 0; i < 4096; i++) if (buf[i]) nonzero++;

			n = sprintf(line, "  readtest %s: ReadFile ok=%d read=%lu, %d non-zero -> %s%s",
			            which ? "HIGH" : "low ", (int)ok, (unsigned long)read, nonzero,
			            nonzero ? "data arrived" : "NOTHING WAS WRITTEN", "\n");
			AppendFaultLog(line, (unsigned long)n);
		}
	}

	VirtualFree(low, 0, MEM_RELEASE);
	VirtualFree(high, 0, MEM_RELEASE);
}

// -movdump - the state of the cutscene system, read out of the running game.
//
// Everything outside CryMovie checked out: the entities are there, the objects are there, the
// clock runs, the lookups are 64-bit clean. What is left is the sequence itself, which is
// reachable: gEnv+0xD8 is pMovieSystem, and the leaked CryMovie source gives the shape.
//
//   class CMovieSystem : public IMovieSystem
//       vtable, ISystem*, IMovieUser*, IMovieCallback*, CTimeValue m_lastUpdateTime,
//       int m_lastGenId, vector<_smart_ptr<IAnimSequence>> m_sequences,
//       list<PlayingSequence> m_playingSequences, ...
//
//   struct PlayingSequence { IAnimSequence* sequence; float start, end, current; bool, bool; }
//
// Retail offsets need not match the source exactly, so this prints the raw head of the object
// rather than trusting a layout: three dumps ten seconds apart say both what differs between a
// working run and a broken one, and what stands still while the game plays on.
#define PMOVIE_OFF 0xD8

static bool g_movDump   = false;
static int  g_movDumped = 0;

static void DumpMovieSystem(int pass)
{
	HMODULE gr = GetModuleHandleA("CryGameReal.dll");
	if (!gr) return;

	ULONG_PTR env = 0;
	if (!SafePeek((unsigned char*)gr + GENV_RVA_IN_GAMEREAL, &env) || env < 0x10000) return;

	ULONG_PTR mov = 0;
	if (!SafePeek((unsigned char*)env + PMOVIE_OFF, &mov) || mov < 0x10000) return;

	char line[400];
	int n = sprintf(line, "  movdump[%d]: CMovieSystem at 0x%llX%s", pass,
	                (unsigned long long)mov, "\n");
	AppendFaultLog(line, (unsigned long)n);

	// The sequences themselves. m_sequences sits at +0x30 as a vector of three pointers; the
	// first element is a CAnimSequence, whose own head carries its node list - and a sequence
	// with no nodes animates nothing, which is exactly what a cutscene that plays but does not
	// move would look like.
	ULONG_PTR vecBegin = 0, vecEnd = 0;
	if (SafePeek((const void*)(mov + 0x30), &vecBegin) &&
	    SafePeek((const void*)(mov + 0x38), &vecEnd) && vecBegin && vecEnd > vecBegin)
	{
		char sl[220];
		int sn = sprintf(sl, "  movdump[%d]: %llu sequence(s), vector at 0x%llX%s", pass,
		                 (unsigned long long)((vecEnd - vecBegin) / 8),
		                 (unsigned long long)vecBegin, "\n");
		AppendFaultLog(sl, (unsigned long)sn);

		ULONG_PTR seq = 0;
		if (SafePeek((const void*)vecBegin, &seq) && seq > 0x10000)
		{
			sn = sprintf(sl, "      sequence[0] at 0x%llX%s", (unsigned long long)seq, "\n");
			AppendFaultLog(sl, (unsigned long)sn);

			for (int so = 0; so < 0x60; so += 8)
			{
				ULONG_PTR sv = 0;
				if (!SafePeek((const void*)(seq + (ULONG_PTR)so), &sv)) break;
				const float* sf = (const float*)&sv;
				sn = sprintf(sl, "        seq+0x%02X = 0x%016llX  f(%.3f, %.3f)%s",
				             so, (unsigned long long)sv, sf[0], sf[1], "\n");
				AppendFaultLog(sl, (unsigned long)sn);
			}
		}
	}

	// The playing sequences. Not at +0x48 and not a std::list - the retail layout puts them in a
	// VECTOR at +0xB30, which the Update code gives away: it does (end-begin) and divides by 24,
	// the size of a PlayingSequence (sequence pointer, then start/end/current time).
	//
	// currentTime is the whole question: a cutscene that plays has one that grows, a cutscene
	// that is stuck has one that stands still. The flowgraph says the sequence was Started and
	// never reported Done, so this number decides whether it is running at all.
	// The gate inside the update loop. For a sequence whose flags have bit 8 set (a cutscene),
	// Update checks one global and skips the sequence when it is non-zero:
	//
	//   CryMovie+0xF256  call [rax+0x80]                 GetFlags()
	//   CryMovie+0xF25C  test al, 8
	//   CryMovie+0xF25E  je   <update the clock>
	//   CryMovie+0xF260  cmp  dword ptr [rip+0x63f95], r12d
	//   CryMovie+0xF267  jne  <skip this sequence>
	//
	// rip after that cmp is 0xF267, so the global is at CryMovie+0x731FC. If it is non-zero
	// while a cutscene is playing, the clock never advances and nothing is logged.
	{
		HMODULE cm = GetModuleHandleA("CryMovie.dll");
		ULONG_PTR gv = 0;
		if (cm && SafePeek((const unsigned char*)cm + 0x731FC, &gv))
		{
			char gl[220];
			int gn = sprintf(gl, "      cutscene gate: CryMovie+0x731FC = %u (0x%llX)%s",
			                 (unsigned)(gv & 0xFFFFFFFF), (unsigned long long)gv, "\n");
			AppendFaultLog(gl, (unsigned long)gn);
		}
	}

	// The first thing CMovieSystem::Update does (CryMovie+0xF120) is
	//     cmp byte ptr [rcx+0x99], 0
	//     jne <return>
	// - a flag that makes the whole update a no-op. If it is set while a cutscene is in the
	// playing list, the sequence sits there with its clock frozen, which is exactly what the
	// dump shows. So read it.
	{
		ULONG_PTR w = 0;
		unsigned char f99 = 0xFF, f9a = 0xFF, f98 = 0xFF;
		if (SafePeek((const void*)(mov + 0x98), &w))
		{
			const unsigned char* b = (const unsigned char*)&w;
			f98 = b[0]; f99 = b[1]; f9a = b[2];
		}
		char fl[200];
		int fn = sprintf(fl, "      flags: +0x98=%u  +0x99=%u (update gate)  +0x9A=%u%s",
		                 f98, f99, f9a, "\n");
		AppendFaultLog(fl, (unsigned long)fn);
	}

	// The real place, read off the engine's own code: CMovieSystem::IsPlaying (CryMovie+0xCDE0)
	// walks [this+0x50] to [this+0x58] with a stride of 0x20 and compares the sequence pointer
	// with a full 64-bit cmp. So the playing list is a vector at +0x50 of 32-byte entries - not
	// +0x48, not a std::list, not 24 bytes. Guessing the layout from the leaked headers was
	// wrong twice; the retail code says it plainly.
	ULONG_PTR pbegin = 0, pend = 0;
	if (SafePeek((const void*)(mov + 0x50), &pbegin) &&
	    SafePeek((const void*)(mov + 0x58), &pend) && pbegin && pend >= pbegin)
	{
		const unsigned long long count = (unsigned long long)(pend - pbegin) / 32;
		char pl[300];
		int pn = sprintf(pl, "      playing: %llu sequence(s)%s", count, "\n");
		AppendFaultLog(pl, (unsigned long)pn);

		for (unsigned long long k = 0; k < count && k < 4; k++)
		{
			const ULONG_PTR at2 = pbegin + (ULONG_PTR)(k * 32);
			// Update does: current += dt * [entry+0x14]. That multiplier is the sequence's
			// speed, and a zero there freezes the clock without any error anywhere - which is
			// precisely the symptom. Read it alongside the times.
			ULONG_PTR sp = 0;
			float tm[4] = { 0, 0, 0, 0 };
			SafePeek((const void*)at2, &sp);
			__try { memcpy(tm, (const void*)(at2 + 8), sizeof(tm)); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}

			pn = sprintf(pl, "        [%llu] seq=0x%llX start=%.3f end=%.3f CURRENT=%.3f "
			             "SPEED=%.4f%s",
			             k, (unsigned long long)sp, tm[0], tm[1], tm[2], tm[3], "\n");
			AppendFaultLog(pl, (unsigned long)pn);
		}
	}
	else
	{
		char pl[120];
		int pn = sprintf(pl, "      playing: none%s", "\n");
		AppendFaultLog(pl, (unsigned long)pn);
	}

	// The head of the object, eight bytes at a time, with a reading of what each word could be.
	for (int off = 0; off < 0x90; off += 8)
	{
		ULONG_PTR v = 0;
		if (!SafePeek((const void*)(mov + (ULONG_PTR)off), &v)) break;

		// A word can be a pointer, or a pair of floats, or a pair of ints - print all three and
		// let the comparison between runs pick the one that matters.
		const float* f = (const float*)&v;
		const unsigned* u = (const unsigned*)&v;

		n = sprintf(line, "      +0x%02X = 0x%016llX   %s  f(%.3f, %.3f)  i(%u, %u)%s",
		            off, (unsigned long long)v,
		            (v > 0x10000 && v < 0x7FFFFFFFFFFFull) ? "ptr" : "   ",
		            f[0], f[1], u[0], u[1], "\n");
		AppendFaultLog(line, (unsigned long)n);
	}
}

// -bp:RVA[,RVA...] - where does the sequence loading actually stop.
//
// CryMovie+0xBE00 is CMovieSystem::Serialize, and the sequences are loaded there:
//
//   +0xBE29  call [rax+0x130]     xmlNode->findChild("SequenceData") -> [rsp+0x20]
//   +0xBE2F  cmp qword [rsp+0x20], 0
//   +0xBE35  je  ...              node missing: give up silently
//   +0xBE51  call [rax+0x120]     seqNode->getChildCount() -> eax
//   +0xBE57  test eax, eax
//   +0xBE59  jle ...              no children: give up silently
//
// Both exits are silent, and the vector ends up untouched, so the dump cannot say which one was
// taken. A breakpoint can: one 0xCC byte, an exception handler that reports the registers and
// the stack slot, then the original byte is restored and execution continues. Nothing is
// detoured and nothing stays patched - the code runs as itself after the first hit.
#define BP_SLOTS 6
static ULONG_PTR g_bpAt[BP_SLOTS];
static unsigned char g_bpOrig[BP_SLOTS];
static int g_bpHits[BP_SLOTS];      // a call site fires many times; the first few are the story
static int  g_bpCount = 0;
static bool g_bpArmed = false;
static char g_bpModule[40] = "CryMovie.dll";

static LONG CALLBACK BreakpointVeh(EXCEPTION_POINTERS* ep)
{
	if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
	if (ep->ExceptionRecord->ExceptionCode != (DWORD)EXCEPTION_BREAKPOINT)
		return EXCEPTION_CONTINUE_SEARCH;

	const ULONG_PTR pc = (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;

	for (int i = 0; i < g_bpCount; i++)
	{
		if (g_bpAt[i] != pc) continue;

		// What the code has in hand at this point. rax/rcx/rdx cover the return value and the
		// object; the stack slot is where findChild puts the node it found.
		ULONG_PTR slot20 = 0;
		SafePeek((const void*)(ep->ContextRecord->Rsp + 0x20), &slot20);

		// CMovieSystem::Update keeps `this` in r14 from its third instruction on, and the list of
		// playing sequences is the pair at +0x50 / +0x58. The disassembly leaves exactly one
		// branch that can skip the clock update - `je` on those two being equal, an empty list -
		// so print the pair next to the object that owns it. Read through gEnv+0xD8 that list has
		// two entries; if r14 names a different object, that is the whole answer.
		ULONG_PTR play0 = 0, play1 = 0;
		SafePeek((const void*)(ep->ContextRecord->R14 + 0x50), &play0);
		SafePeek((const void*)(ep->ContextRecord->R14 + 0x58), &play1);

		// The other half of the branch at 0xF267: `cmp dword [CryMovie+0x731FC], r12d` with r12d
		// zeroed at the top of the function. Non-zero here, with flag 8 set on the sequence, and
		// the cutscene is taken off the playing list instead of having its clock advanced.
		unsigned glob = 0xFFFFFFFF;
		{
			HMODULE mv = GetModuleHandleA(g_bpModule);
			if (mv)
			{
				ULONG_PTR raw = 0;
				if (SafePeek((const unsigned char*)mv + 0x731FC, &raw))
					glob = (unsigned)(raw & 0xFFFFFFFFu);
			}
		}

		// Who called. At the first instruction of a function [rsp] is the return address, so a
		// breakpoint on the entry names the caller - which is the whole question when a function
		// is never reached: the fault is in whoever should have called it.
		// Walk a little of the interrupted stack and name everything on it that points into a
		// module's code. The exact return address is not always the first word - the breakpoint
		// may sit past a push - so print the first few callers and let the shape speak.
		char who[900];
		who[0] = 0;
		int wn = 0;
		int named = 0;
		for (int s = 0; s < 64 && named < 7; s++)
		{
			ULONG_PTR v = 0;
			if (!SafePeek((const void*)(ep->ContextRecord->Rsp + (ULONG_PTR)s * 8), &v)) break;
			if (v < 0x10000) continue;

			HMODULE fm = 0;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                        (LPCSTR)v, &fm) || !fm) continue;

			// A module address is not enough: vtables and string tables live in modules too, and
			// they are what turns up first on a stack. Only executable pages can be return
			// addresses.
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery((LPCVOID)v, &mbi, sizeof(mbi))) continue;
			if ((mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
			                    PAGE_EXECUTE_WRITECOPY)) == 0) continue;

			char nm[200];
			NameCode(v, nm);
			wn += sprintf(who + wn, "%s%s", named ? " <- " : "", nm);
			named++;
		}

		char line[1100];
		int n = sprintf(line, "  bp hit %s+0x%llX: from %s | r14=0x%llX play +0x50=0x%llX "
		                "+0x58=0x%llX %s | rbx=0x%llX rax=0x%llX rcx=0x%llX%s",
		                g_bpModule,
		                (unsigned long long)(pc - (ULONG_PTR)GetModuleHandleA(g_bpModule)),
		                who[0] ? who : "?",
		                (unsigned long long)ep->ContextRecord->R14,
		                (unsigned long long)play0,
		                (unsigned long long)play1,
		                (play0 == play1) ? "EMPTY" : "has entries",
		                (unsigned long long)ep->ContextRecord->Rbx,
		                (unsigned long long)ep->ContextRecord->Rax,
		                (unsigned long long)ep->ContextRecord->Rcx, "\n");
		AppendFaultLog(line, (unsigned long)n);

		// Put the code back and step over it. Re-arming after a few hits would need single-step;
		// instead the byte is restored for good once enough calls have been seen, which is all
		// this question needs.
		g_bpHits[i]++;

		DWORD old = 0;
		if (VirtualProtect((LPVOID)pc, 1, PAGE_EXECUTE_READWRITE, &old))
		{
			*(unsigned char*)pc = g_bpOrig[i];
			VirtualProtect((LPVOID)pc, 1, old, &old);
			FlushInstructionCache(GetCurrentProcess(), (LPCVOID)pc, 1);
		}
		ep->ContextRecord->Rip = (DWORD64)pc;
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

static void ArmBreakpoints(void)
{
	HMODULE m = GetModuleHandleA(g_bpModule);
	if (!m) return;

	// Which file is actually loaded, and what the code around the points really looks like in
	// memory. A breakpoint is only as good as the address it sits on: bytes read out of the file
	// on disk mean nothing if the process mapped a different build, or if something patched the
	// page first. Print both before arming anything.
	{
		char path[MAX_PATH];
		path[0] = 0;
		GetModuleFileNameA(m, path, MAX_PATH);

		char line[400];
		int n = sprintf(line, "  bp module: %s at 0x%llX%s", path,
		                (unsigned long long)(ULONG_PTR)m, "\n");
		AppendFaultLog(line, (unsigned long)n);

		for (int j = 0; j < g_bpCount; j++)
		{
			const unsigned char* q = (const unsigned char*)m + g_bpAt[j];
			n = sprintf(line, "  bp bytes at +0x%llX:", (unsigned long long)g_bpAt[j]);
			for (int k = 0; k < 12; k++)
			{
				ULONG_PTR one = 0;
				if (SafePeek(q + k, &one)) n += sprintf(line + n, " %02X", (unsigned)(one & 0xFF));
				else                       n += sprintf(line + n, " ??");
			}
			n += sprintf(line + n, "%s", "\n");
			AppendFaultLog(line, (unsigned long)n);
		}
	}

	for (int i = 0; i < g_bpCount; i++)
	{
		const ULONG_PTR at = (ULONG_PTR)m + g_bpAt[i];
		unsigned char* p = (unsigned char*)at;

		DWORD old = 0;
		if (!VirtualProtect((LPVOID)at, 1, PAGE_EXECUTE_READWRITE, &old)) continue;
		g_bpOrig[i] = *p;
		*p = 0xCC;
		VirtualProtect((LPVOID)at, 1, old, &old);
		FlushInstructionCache(GetCurrentProcess(), (LPCVOID)at, 1);

		g_bpAt[i] = at;     // from here on the table holds absolute addresses

		char line[200];
		int n = sprintf(line, "  bp armed at %s+0x%llX (was 0x%02X)%s", g_bpModule,
		                (unsigned long long)(at - (ULONG_PTR)m), g_bpOrig[i], "\n");
		AppendFaultLog(line, (unsigned long)n);
	}
	g_bpArmed = true;
}

// Survive a smart pointer copied from an object that is already gone.
//
// The renderer copies a small struct of reference-counted pointers - fields at +0x00, +0x18,
// +0x08, each with its own AddRef. Across a long campaign one of them can name an object that
// was released when a level unloaded, and the memory has since been handed out again:
//
//   CryRenderD3D11+0x57E53  mov [rbx], rcx        the destination field, written first
//   CryRenderD3D11+0x57E5B  mov rax, [rcx]        what should be the method table
//   CryRenderD3D11+0x57E5E  call [rax+8]          AddRef - dies here
//
// Seen twice, both times on a level transition, both times at this instruction. Once rax held
// 0x00001D1800001C9E - two numbers side by side where a method table should be, which is what
// reused memory looks like. Once the call itself jumped into nothing.
//
// The repair is not a guard in front of the call. A guard has to decide, on every single copy,
// whether a pointer is alive - and a guard that decides wrong is exactly what froze the
// cutscenes for a week. This waits for the fault to actually happen, then clears the field that
// was already written and resumes at the next line, leaving the pointer empty rather than dead.
// Every later use tests it against null first, which the code right below does three times.
// Nothing is patched, nothing is checked ahead of time, and a run where the object is alive
// never reaches this code at all.
static ULONG_PTR g_rndCopyCall = 0;      // CryRenderD3D11+0x57E5E, the failing call
static ULONG_PTR g_rndCopyNext = 0;      // +0x57E61, the line after it
static volatile LONG g_rndCopySkipped = 0;
static bool g_rndFixOff = false;         // -norndfix

static bool WritableQword(void* at)
{
	MEMORY_BASIC_INFORMATION mbi;
	if (!at || !VirtualQuery(at, &mbi, sizeof(mbi))) return false;
	if (mbi.State != MEM_COMMIT) return false;
	const DWORD w = PAGE_READWRITE | PAGE_WRITECOPY |
	                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	return (mbi.Protect & w) != 0;
}

static LONG CALLBACK RenderCopyVEH(EXCEPTION_POINTERS* ep)
{
	if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
	if (ep->ExceptionRecord->ExceptionCode != (DWORD)EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;
	if (!g_rndCopyCall) return EXCEPTION_CONTINUE_SEARCH;

	CONTEXT* c = ep->ContextRecord;
	bool mine = false;

	// Two ways the same object kills the same line: the method table is unreadable, so the call
	// faults where it stands; or it reads but holds garbage, so the call lands on nothing and
	// the fault happens elsewhere with our return address still on the stack.
	if ((ULONG_PTR)c->Rip == g_rndCopyCall)
	{
		mine = true;
	}
	else
	{
		ULONG_PTR ret = 0;
		if (SafePeek((const void*)c->Rsp, &ret) && ret == g_rndCopyNext)
		{
			c->Rsp += 8;          // drop the failed call's return address
			mine = true;
		}
	}
	if (!mine) return EXCEPTION_CONTINUE_SEARCH;

	// rbx is the destination struct, and its first field already holds the dead pointer.
	if (WritableQword((void*)c->Rbx)) *(ULONG_PTR*)c->Rbx = 0;

	c->Rip = (DWORD64)g_rndCopyNext;
	InterlockedIncrement(&g_rndCopySkipped);
	return EXCEPTION_CONTINUE_EXECUTION;
}

// Arm it once the renderer is in, and only if the bytes there are the ones this was written
// against. Reading the instruction out of the file on disk is not enough - a module can be
// patched in memory, by us or by anything else, and acting on a stale disassembly cost a week.
static void ArmRenderCopyFix(void)
{
	HMODULE m = GetModuleHandleA("CryRenderD3D11.dll");
	if (!m) return;

	const unsigned char* at = (const unsigned char*)m + 0x57E5E;
	unsigned char got[3];
	for (int i = 0; i < 3; i++)
	{
		ULONG_PTR one = 0;
		if (!SafePeek(at + i, &one)) return;
		got[i] = (unsigned char)(one & 0xFF);
	}

	char line[220];
	int n;

	// FF 50 08 = call qword ptr [rax+8]
	if (got[0] != 0xFF || got[1] != 0x50 || got[2] != 0x08)
	{
		n = sprintf(line, "  rndfix: NOT armed, bytes at +0x57E5E are %02X %02X %02X, expected "
		            "FF 50 08%s", got[0], got[1], got[2], "\n");
		AppendFaultLog(line, (unsigned long)n);
		g_rndCopyCall = 1;          // non-zero and never matched: do not look again
		return;
	}

	g_rndCopyCall = (ULONG_PTR)m + 0x57E5E;
	g_rndCopyNext = (ULONG_PTR)m + 0x57E61;
	AddVectoredExceptionHandler(1, RenderCopyVEH);

	n = sprintf(line, "  rndfix: armed at CryRenderD3D11+0x57E5E (0x%llX)%s",
	            (unsigned long long)g_rndCopyCall, "\n");
	AppendFaultLog(line, (unsigned long)n);
}

static volatile LONG g_highSkipped = 0;   // allocations left low on purpose

static LONG TryHighAt(PFN_NtAllocVM orig, HANDLE proc, PVOID* base, SIZE_T* size,
                      ULONG type, ULONG protect)
{
	const SIZE_T want = *size;

	// Asking for a particular address turns "commit" into "reserve and commit": the region does
	// not exist yet, and a bare commit would fail on it.
	type |= MEM_RESERVE;

	// One step past the end of what was asked for, rounded up to the 64 KB granularity, plus a
	// gap. A flat 64 MB was fine for ninety reservations a level and would run through the whole
	// 256 GB in one level now that every large malloc comes this way.
	LONGLONG step = (LONGLONG)((want + 0x1FFFF) & ~(SIZE_T)0xFFFF);
	if (step < 0x100000LL) step = 0x100000LL;

	for (int attempt = 0; attempt < 24; attempt++)
	{
		LONGLONG at = InterlockedExchangeAdd64(&g_highCursor, step);
		if (at > g_highCeiling)
		{
			// Wrap rather than give up: what was freed along the way has left gaps, and the
			// next pass over the range finds them.
			InterlockedExchange64(&g_highCursor, g_highBase);
			at = InterlockedExchangeAdd64(&g_highCursor, step);
			if (at > g_highCeiling) return -1;
		}

		PVOID p = (PVOID)(ULONG_PTR)at;
		SIZE_T sz = want;
		const LONG st = orig(proc, &p, 0, &sz, type, protect);
		if (st >= 0)
		{
			*base = p;
			*size = sz;
			return st;
		}
	}
	return -1;
}

static volatile LONG g_bigSeen = 0;

static LONG __stdcall SteeredNtAlloc(HANDLE proc, PVOID* base, ULONG_PTR zeroBits,
                                     SIZE_T* size, ULONG type, ULONG protect)
{
	// Every large request as it arrives, before any filter. The census keeps finding 32 MB
	// pieces below 4 GB that no counter here accounts for; this settles whether they come
	// through this call at all.
	// 4 MB and up, because that is the band the cutscene failure was narrowed to: high memory
	// at a 4 MB threshold breaks it, at 16 MB it plays. Whoever asks for a block in between is
	// the one to look at.
	const bool bigOne = g_memDebug && size && *size >= 4 * 1024 * 1024 &&
	                    InterlockedIncrement(&g_bigSeen) <= 80;

	if (bigOne && g_capStack)
	{
		void* frames[12];
		const USHORT n = g_capStack(1, 12, frames, NULL);
		unsigned long long site = 0;
		for (USHORT f = 0; f < n; f++)
		{
			const unsigned long long a = (unsigned long long)(ULONG_PTR)frames[f];
			bool through = false;
			for (int k = 0; k < g_passCount; k++)
				if (a >= g_passLo[k] && a < g_passHi[k]) { through = true; break; }
			if (!through) { site = a; break; }
		}

		char who[96];
		strcpy(who, "(plumbing only)");
		if (site)
		{
			HMODULE mod = 0;
			if (GetModuleHandleExA(0x00000004 | 0x00000002, (LPCSTR)(ULONG_PTR)site, &mod) && mod)
			{
				char full[MAX_PATH];
				if (GetModuleFileNameA(mod, full, MAX_PATH))
				{
					const char* b = strrchr(full, 0x5C);
					sprintf(who, "%s+0x%llX", b ? b + 1 : full,
					        site - (unsigned long long)(ULONG_PTR)mod);
				}
				else sprintf(who, "0x%llX", site);
			}
			else sprintf(who, "0x%llX (no module)", site);
		}

		char al[220];
		int an = sprintf(al, "  asks: %llu bytes (0x%llX) wanted by %s%s",
		                 (unsigned long long)*size, (unsigned long long)*size, who, "\n");
		AppendFaultLog(al, (unsigned long)an);
	}

	// g_topDown is the switch: off until -topdown says so, or until lowguard sees the low
	// address space running out. The hook itself is always installed, because it cannot move
	// what was placed before it existed - but with the switch off it only counts.
	const bool steer = g_topDown && base && (*base == NULL) && size &&
	                   (*size >= g_topDownMin) &&
	                   ((type & (MEM_RESERVE | MEM_COMMIT)) != 0) &&
	                   (proc == (HANDLE)(LONG_PTR)-1);

	if (!steer && g_topDown && base && size && proc == (HANDLE)(LONG_PTR)-1)
	{
		const LONGLONG kb = (LONGLONG)(*size / 1024);
		AttributeCall(*size, (unsigned long long)(ULONG_PTR)*base > 0xFFFFFFFFull);
		SampleCall(base, *size, type);
		if (*base != NULL)
		{
			InterlockedIncrement(&g_skipFixedAddr);
			InterlockedExchangeAdd64(&g_skipFixedKb, kb);
		}
		else if ((type & MEM_RESERVE) == 0)
		{
			InterlockedIncrement(&g_skipCommitOnly);
			InterlockedExchangeAdd64(&g_skipCommitKb, kb);
		}
		else
		{
			InterlockedIncrement(&g_skipTooSmall);
			InterlockedExchangeAdd64(&g_skipSmallKb, kb);
		}
	}

	// -arenamax:N applies to the band whether or not -keepband is on: it is the bisection
	// coordinate, and it has to be able to override both directions.
	if (steer && g_arenaMax >= 0 && *size >= 14u * 1024 * 1024 && *size <= 16u * 1024 * 1024)
	{
		const LONG nth = InterlockedIncrement(&g_arenaNth);
		const bool sendUp = (nth >= g_arenaMin) && (nth <= g_arenaMax);

		if (g_arenaWatch && nth == g_arenaWatch)
		{
			// ZeroBits is the caller saying how many top bits of the address must be zero - a
			// way of asking for memory below a line. We pass 0 when we place the block
			// ourselves, so if this is non-zero we have been overriding an explicit request.
			char zl[220];
			int zn = sprintf(zl, "  arena #%ld: zeroBits=%llu type=0x%lX protect=0x%lX%s",
			                 nth, (unsigned long long)zeroBits, (unsigned long)type,
			                 (unsigned long)protect, "\n");
			AppendFaultLog(zl, (unsigned long)zn);

			ReportArena(nth, *size, 0, sendUp ? "going high" : "staying low");
		}

		if (!sendUp)
		{
			InterlockedIncrement(&g_bandKept);
			const LONG stLow = g_origNtAlloc(proc, base, zeroBits, size, type, protect);

			// Watch it down here too - the same buffer in the configuration that works is the
			// only thing that says what the one up high should have looked like.
			if (g_arenaWatch && nth == g_arenaWatch && stLow >= 0 && base && *base)
			{
				ReportArena(nth, *size, (ULONG_PTR)*base, "landed low");
				g_peekBytes = *size;
				g_peekAt    = (ULONG_PTR)*base;
			}
			return stLow;
		}

		// With the trap armed, the arena goes to an address whose lower 32 bits sit in the
		// no-access strip. Aimed at one arena rather than all forty, the trap has no background
		// at all: anything it catches belongs to this allocation.
		LONG st2;
		if (g_arenaAtMb)
		{
			// A fixed address, so the only variable in the run is height.
			PVOID at = (PVOID)(ULONG_PTR)(g_arenaAtMb * 1024ull * 1024ull);
			SIZE_T sz = *size;
			st2 = g_origNtAlloc(proc, &at, 0, &sz, type | MEM_RESERVE, protect);
			if (st2 >= 0) { *base = at; *size = sz; }
		}
		else
		{
			st2 = g_shadowOn
			    ? TryShadowHigh(g_origNtAlloc, proc, base, size, type, protect)
			    : TryHighAt(g_origNtAlloc, proc, base, size, type, protect);
		}
		if (st2 >= 0)
		{
			InterlockedIncrement(&g_topDownCalls);
			if (g_arenaWatch && nth == g_arenaWatch)
			{
				ReportArena(nth, *size, (ULONG_PTR)*base, "landed");
				// Only publish the address - the watching thread is already running. Starting
				// one here would be a CreateThread from inside the allocation hook, with the
				// loader lock held.
				g_peekBytes = *size;
				g_peekAt    = (ULONG_PTR)*base;
			}
			return st2;
		}
		return g_origNtAlloc(proc, base, zeroBits, size, type, protect);
	}

	// -keepband:LO-HI (in MB): allocations in this size band stay low, whoever asked.
	//
	// Bisection landed here: high memory at a 12 MB threshold breaks the game, at 16 MB it is
	// fine, and every allocation in between is exactly 15 MB - asked for by the renderer, the
	// physics, the animation system, 3DEngine and CrySystem alike. One size from every
	// subsystem is not a coincidence; that is the allocator's arena, the slab it carves small
	// blocks out of. So the band can be pinned and everything else can still go high.
	// -shadow implies the band goes up: keeping it low would leave the trap with nothing to
	// watch. Stated here rather than at parse time so -keepband keeps its normal meaning.
	if (steer && g_shadowOn && g_bandLoKb == 0 && *size >= 14u * 1024 * 1024 &&
	    *size <= 16u * 1024 * 1024)
	{
		const LONG sst = TryShadowHigh(g_origNtAlloc, proc, base, size, type, protect);
		if (sst >= 0)
		{
			InterlockedIncrement(&g_topDownCalls);
			return sst;
		}
	}

	if (steer && g_bandLoKb && *size >= (SIZE_T)g_bandLoKb * 1024 &&
	    *size <= (SIZE_T)g_bandHiKb * 1024)
	{
		// With -shadow the band goes up instead, under the trap: the whole point is to have the
		// arenas high and watched. Everything else is steered exactly as it is today, so the
		// only difference from the working configuration is the thing being investigated.
		if (g_shadowOn)
		{
			const LONG sst = TryShadowHigh(g_origNtAlloc, proc, base, size, type, protect);
			if (sst >= 0)
			{
				InterlockedIncrement(&g_topDownCalls);
				return sst;
			}
		}

		InterlockedIncrement(&g_bandKept);
		return g_origNtAlloc(proc, base, zeroBits, size, type, protect);
	}

	// Anything sizeable that the driver asked for stays where it would have been.
	if (steer && *size >= 1024 * 1024 && AskedByDriver())
	{
		InterlockedIncrement(&g_drvLeftAlone);
		return g_origNtAlloc(proc, base, zeroBits, size, type, protect);
	}

	if (steer && CalledByUnreadyModule())
	{
		InterlockedIncrement(&g_highSkipped);
		return g_origNtAlloc(proc, base, zeroBits, size, type, protect);
	}

	LONG st;
	if (steer && !g_topDownMax)
	{
		st = TryHighAt(g_origNtAlloc, proc, base, size, type, protect);
		if (st >= 0)
		{
			const unsigned long long a = (unsigned long long)(ULONG_PTR)*base;

			if (bigOne)
			{
				char bl[190];
				int bn = sprintf(bl, "  big got: %llu MB at 0x%llX, steered%s",
				                 (unsigned long long)(*size / (1024 * 1024)), a, "\n");
				AppendFaultLog(bl, (unsigned long)bn);
			}
			InterlockedIncrement(&g_topDownCalls);
			if (a > 0xFFFFFFFFull)
			{
				InterlockedIncrement(&g_topDownHigh);
				InterlockedExchangeAdd64(&g_highBytes, (LONGLONG)*size);
			}
			if (a > g_topDownHighest) g_topDownHighest = a;
			if (a < g_topDownLowest)  g_topDownLowest  = a;
			return st;
		}
		*base = NULL;          // the attempts left it set; hand the original a clean request
	}
	else if (steer)
	{
		type |= MEM_TOP_DOWN;
	}

	st = g_origNtAlloc(proc, base, zeroBits, size, type, protect);

	if (st >= 0 && base && *base && size)
	{
		const unsigned long long a = (unsigned long long)(ULONG_PTR)*base;

		if (bigOne)
		{
			char bl[190];
			int bn = sprintf(bl, "  big got: %llu MB at 0x%llX, type 0x%lX%s",
			                 (unsigned long long)(*size / (1024 * 1024)), a, type, "\n");
			AppendFaultLog(bl, (unsigned long)bn);
		}

		InterlockedIncrement(&g_sawCalls);
		if (proc != (HANDLE)(LONG_PTR)-1) InterlockedIncrement(&g_sawOtherPid);
		if (a > 0xFFFFFFFFull)
			InterlockedExchangeAdd64(&g_sawHighKb, (LONGLONG)(*size / 1024));
		else
			InterlockedExchangeAdd64(&g_sawLowKb, (LONGLONG)(*size / 1024));

		LowLandingSample(a, *size, type);
	}

	if (steer && st >= 0 && base && *base)
	{
		const unsigned long long a = (unsigned long long)(ULONG_PTR)*base;
		InterlockedIncrement(&g_topDownCalls);
		if (a > 0xFFFFFFFFull) InterlockedIncrement(&g_topDownHigh);
		if (a > g_topDownHighest) g_topDownHighest = a;
		if (a < g_topDownLowest)  g_topDownLowest  = a;
	}
	return st;
}

// Counts every file view, and with -topmap steers them high as well. Counting is the point:
// if the .pak archives are a gigabyte of the address space, no amount of work on the allocator
// moves the number, and the next thing to hook is this call, not that one.
static LONG __stdcall CountedNtMapView(HANDLE section, HANDLE proc, PVOID* base,
                                       ULONG_PTR zeroBits, SIZE_T commit,
                                       PLARGE_INTEGER offset, PSIZE_T viewSize,
                                       DWORD inherit, ULONG type, ULONG protect)
{
	const bool ours = (proc == (HANDLE)(LONG_PTR)-1) && base && viewSize;

	if (ours && g_topMap && *base == NULL && *viewSize >= g_topDownMin)
		type |= MEM_TOP_DOWN;

	LONG st = g_origNtMapView(section, proc, base, zeroBits, commit, offset,
	                          viewSize, inherit, type, protect);

	if (ours && st >= 0 && *base && *viewSize)
	{
		const unsigned long long a = (unsigned long long)(ULONG_PTR)*base;

		// Large views as they land. The census keeps finding 8-32 MB pieces below 4 GB that the
		// allocation hook never sees; if they are mapped rather than allocated, they show here.
		static volatile LONG seen = 0;
		if (g_memDebug && *viewSize >= 8 * 1024 * 1024 && InterlockedIncrement(&seen) <= 12)
		{
			char ml[190];
			int mn = sprintf(ml, "  big view: %llu MB at 0x%llX, type 0x%lX, protect 0x%lX%s",
			                 (unsigned long long)(*viewSize / (1024 * 1024)), a, type, protect,
			                 "\n");
			AppendFaultLog(ml, (unsigned long)mn);
		}
		const LONGLONG kb = (LONGLONG)(*viewSize / 1024);
		InterlockedIncrement(&g_mapCalls);
		InterlockedExchangeAdd64(&g_mapKb, kb);
		if (a > 0xFFFFFFFFull)
		{
			InterlockedIncrement(&g_mapHigh);
			InterlockedExchangeAdd64(&g_mapHighKb, kb);
		}
	}
	return st;
}

static const char* HookNtMapView(void)
{
	if (g_origNtMapView) return "already";

	HMODULE nt = GetModuleHandleA("ntdll.dll");
	if (!nt) return "no ntdll";

	unsigned char* at = (unsigned char*)GetProcAddress(nt, "NtMapViewOfSection");
	if (!at) return "no NtMapViewOfSection";

	if (!(at[0] == 0x4C && at[1] == 0x8B && at[2] == 0xD1 && at[3] == 0xB8))
		return "unfamiliar stub";

	unsigned char* cave = AllocCaveNear(at);
	if (!cave) return "no cave";

	memcpy(cave, at, 8);
	cave[8] = 0xE9;
	{
		const long rel = (long)((at + 8) - (cave + 13));
		memcpy(cave + 9, &rel, 4);
	}
	g_origNtMapView = (PFN_NtMapView)cave;

	unsigned char* pad = cave + 32;
	pad[0] = 0xFF; pad[1] = 0x25; pad[2] = 0x00; pad[3] = 0x00; pad[4] = 0x00; pad[5] = 0x00;
	{
		void* target = (void*)CountedNtMapView;
		memcpy(pad + 6, &target, 8);
	}

	if (!WriteJump(at, pad, 8)) { g_origNtMapView = 0; return "jump failed"; }
	return "applied";
}

// The newer call, and on Windows 11 the one that matters. The segment heap - which is what a
// process gets by default there - reserves through NtAllocateVirtualMemoryEx, not through the
// call every tutorial hooks. It takes extended parameters instead of ZeroBits, so nothing about
// it can be shared with the older hook, and a build that only hooks the old one sees a hundred
// and seventy 32 MB reservations appear below 4 GB with no caller to blame.
typedef LONG (__stdcall *PFN_NtAllocVMEx)(HANDLE, PVOID*, SIZE_T*, ULONG, ULONG, PVOID, ULONG);
static PFN_NtAllocVMEx   g_origNtAllocEx = 0;
static volatile LONG     g_exCalls  = 0;
static volatile LONG     g_exHigh   = 0;
static volatile LONGLONG g_exHighKb = 0;
static volatile LONGLONG g_exLowKb  = 0;

static LONG TryHighAtEx(HANDLE proc, PVOID* base, SIZE_T* size, ULONG type, ULONG protect,
                        PVOID ext, ULONG extCount)
{
	const SIZE_T want = *size;
	type |= MEM_RESERVE;

	LONGLONG step = (LONGLONG)((want + 0x1FFFF) & ~(SIZE_T)0xFFFF);
	if (step < 0x100000LL) step = 0x100000LL;

	for (int attempt = 0; attempt < 24; attempt++)
	{
		LONGLONG at = InterlockedExchangeAdd64(&g_highCursor, step);
		if (at > g_highCeiling)
		{
			InterlockedExchange64(&g_highCursor, g_highBase);
			at = InterlockedExchangeAdd64(&g_highCursor, step);
			if (at > g_highCeiling) return -1;
		}

		PVOID p = (PVOID)(ULONG_PTR)at;
		SIZE_T sz = want;
		const LONG st = g_origNtAllocEx(proc, &p, &sz, type, protect, ext, extCount);
		if (st >= 0) { *base = p; *size = sz; return st; }
	}
	return -1;
}

static LONG __stdcall SteeredNtAllocEx(HANDLE proc, PVOID* base, SIZE_T* size, ULONG type,
                                       ULONG protect, PVOID ext, ULONG extCount)
{
	const bool steer = g_topDown && base && (*base == NULL) && size &&
	                   (*size >= g_topDownMin) &&
	                   ((type & (MEM_RESERVE | MEM_COMMIT)) != 0) &&
	                   (proc == (HANDLE)(LONG_PTR)-1) && !CalledByUnreadyModule();

	LONG st;
	if (steer && !g_topDownMax)
	{
		st = TryHighAtEx(proc, base, size, type, protect, ext, extCount);
		if (st >= 0)
		{
			InterlockedIncrement(&g_exCalls);
			InterlockedIncrement(&g_exHigh);
			InterlockedExchangeAdd64(&g_exHighKb, (LONGLONG)(*size / 1024));
			return st;
		}
		*base = NULL;
	}
	else if (steer)
	{
		type |= MEM_TOP_DOWN;
	}

	st = g_origNtAllocEx(proc, base, size, type, protect, ext, extCount);

	if (st >= 0 && base && *base && size)
	{
		const unsigned long long a = (unsigned long long)(ULONG_PTR)*base;
		InterlockedIncrement(&g_exCalls);
		if (a > 0xFFFFFFFFull)
		{
			InterlockedIncrement(&g_exHigh);
			InterlockedExchangeAdd64(&g_exHighKb, (LONGLONG)(*size / 1024));
		}
		else InterlockedExchangeAdd64(&g_exLowKb, (LONGLONG)(*size / 1024));
	}
	return st;
}

static const char* HookNtAllocEx(void)
{
	if (g_origNtAllocEx) return "already";

	HMODULE nt = GetModuleHandleA("ntdll.dll");
	if (!nt) return "no ntdll";

	unsigned char* at = (unsigned char*)GetProcAddress(nt, "NtAllocateVirtualMemoryEx");
	if (!at) return "not on this Windows";

	if (!(at[0] == 0x4C && at[1] == 0x8B && at[2] == 0xD1 && at[3] == 0xB8))
		return "unfamiliar stub";

	unsigned char* cave = AllocCaveNear(at);
	if (!cave) return "no cave";

	memcpy(cave, at, 8);
	cave[8] = 0xE9;
	{
		const long rel = (long)((at + 8) - (cave + 13));
		memcpy(cave + 9, &rel, 4);
	}
	g_origNtAllocEx = (PFN_NtAllocVMEx)cave;

	unsigned char* pad = cave + 32;
	pad[0] = 0xFF; pad[1] = 0x25; pad[2] = 0x00; pad[3] = 0x00; pad[4] = 0x00; pad[5] = 0x00;
	{
		void* target = (void*)SteeredNtAllocEx;
		memcpy(pad + 6, &target, 8);
	}

	if (!WriteJump(at, pad, 8)) { g_origNtAllocEx = 0; return "jump failed"; }
	return "applied";
}

static const char* HookNtAlloc(void)
{
	if (g_origNtAlloc) return "already";

	HMODULE nt = GetModuleHandleA("ntdll.dll");
	if (!nt) return "no ntdll";

	unsigned char* at = (unsigned char*)GetProcAddress(nt, "NtAllocateVirtualMemory");
	if (!at) return "no NtAllocateVirtualMemory";

	// mov r10, rcx ; mov eax, imm32   - the shape of every syscall stub on this Windows.
	if (!(at[0] == 0x4C && at[1] == 0x8B && at[2] == 0xD1 && at[3] == 0xB8))
		return "unfamiliar stub";

	unsigned char* cave = AllocCaveNear(at);
	if (!cave) return "no cave";

	// The original, callable: the eight bytes we are about to overwrite, then back to the rest.
	memcpy(cave, at, 8);
	cave[8] = 0xE9;
	{
		const long rel = (long)((at + 8) - (cave + 13));
		memcpy(cave + 9, &rel, 4);
	}
	g_origNtAlloc = (PFN_NtAllocVM)cave;

	// A 32-bit jump cannot reach across the address space, so the detour goes through an
	// absolute jump parked in the same cave: jmp [rip+0] followed by the address itself.
	// ('far' is still a macro in this compiler's headers, hence the name.)
	unsigned char* pad = cave + 32;
	pad[0] = 0xFF; pad[1] = 0x25; pad[2] = 0x00; pad[3] = 0x00; pad[4] = 0x00; pad[5] = 0x00;
	{
		void* target = (void*)SteeredNtAlloc;
		memcpy(pad + 6, &target, 8);
	}

	if (!WriteJump(at, pad, 8)) { g_origNtAlloc = 0; return "jump failed"; }
	return "applied";
}

// -trace: where the engine's camera is, four times a second, written to a file.
//
// The cutscene failure was invisible to everything we had: no exception, no log line, nothing
// in Game.log. The only thing that differs between a cutscene that plays and one that does not
// is that the camera moves on its own while the player stands still. So that is what gets
// recorded, and a run can be judged afterwards without anyone watching the screen.
//
// The path to it is the one the mechanics work already uses: gEnv is a global in CryGameReal,
// pSystem sits at +0xB0, and the view matrix at +0x4B0 - a Matrix34, so the position is the
// last column: elements 3, 7 and 11.
#define GENV_RVA_IN_GAMEREAL 0xA0E8C0
#define PSYSTEM_OFF          0xB0
#define VIEWMATRIX_OFF       0x4B0

static bool g_trace = false;

static bool ReadCameraPos(float* out3)
{
	HMODULE gr = GetModuleHandleA("CryGameReal.dll");
	if (!gr) return false;

	ULONG_PTR env = 0;
	if (!SafePeek((unsigned char*)gr + GENV_RVA_IN_GAMEREAL, &env) || env < 0x10000)
		return false;

	ULONG_PTR sys = 0;
	if (!SafePeek((unsigned char*)env + PSYSTEM_OFF, &sys) || sys < 0x10000)
		return false;

	const float* m = (const float*)((unsigned char*)sys + VIEWMATRIX_OFF);
	ULONG_PTR probe = 0;
	if (!SafePeek(m, &probe)) return false;

	__try
	{
		out3[0] = m[3];
		out3[1] = m[7];
		out3[2] = m[11];
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}

	// A camera at the origin means the engine has not placed it yet.
	return (out3[0] != 0.0f) || (out3[1] != 0.0f) || (out3[2] != 0.0f);
}

static DWORD WINAPI CameraTraceThread(LPVOID)
{
	const DWORD started = GetTickCount();
	char line[160];

	int n = sprintf(line, "=== camera trace, one sample every 250 ms%s", "\n");
	AppendTextFile("camera_trace.txt", line, (unsigned long)n);

	float last[3] = { 0, 0, 0 };
	bool  haveLast = false;

	for (;;)
	{
		Sleep(250);

		float pos[3];
		if (!ReadCameraPos(pos)) continue;

		// Distance since the previous sample, so a reader can tell movement from stillness
		// without doing the arithmetic itself.
		float moved = 0.0f;
		if (haveLast)
		{
			const float dx = pos[0] - last[0], dy = pos[1] - last[1], dz = pos[2] - last[2];
			// Squared distance: no math header needed, and a reader comparing runs cares
			// about "moved or not", not about metres.
			moved = dx * dx + dy * dy + dz * dz;
		}
		last[0] = pos[0]; last[1] = pos[1]; last[2] = pos[2];
		haveLast = true;

		n = sprintf(line, "%u %.2f %.2f %.2f %.3f%s", (unsigned)(GetTickCount() - started),
		            pos[0], pos[1], pos[2], moved, "\n");
		AppendTextFile("camera_trace.txt", line, (unsigned long)n);
	}
}

// -memstress:GB - the examination this whole effort is for.
//
// Everything until now measured where memory landed. This asks the question the mod work
// actually depends on: can the engine's own allocator hand out gigabytes, above the 4 GB line,
// and give back what was written into them. A truncated pointer cannot survive this - the block
// would be handed out at one address and read back at another, and the pattern would not match.
//
// The blocks come from CryMalloc, the retail bucket allocator, the one with eight truncating
// stores in every module. Every page is written and every page is checked, so this is real
// memory in use, not reserved address space.
typedef void* (__cdecl *PFN_CryMalloc)(size_t, size_t*, size_t);
typedef void  (__cdecl *PFN_CryFree)(void*, size_t);

static unsigned g_stressGb = 0;

#define STRESS_BLOCK (4 * 1024 * 1024)
#define STRESS_MAX   4096

static unsigned long long StressPattern(unsigned long long addr, unsigned long long off)
{
	return (addr ^ (off * 0x9E3779B97F4A7C15ULL)) + 0x5851F42D4C957F2DULL;
}

static void RunMemoryStress(void)
{
	HMODULE sys = GetModuleHandleA("CrySystem.dll");
	if (!sys) { AppendFaultLog("  memstress: CrySystem not loaded\n", 36); return; }

	PFN_CryMalloc cryMalloc = (PFN_CryMalloc)GetProcAddress(sys, "CryMalloc");
	PFN_CryFree   cryFree   = (PFN_CryFree)GetProcAddress(sys, "CryFree");
	if (!cryMalloc || !cryFree)
	{
		AppendFaultLog("  memstress: CryMalloc/CryFree not exported\n", 44);
		return;
	}

	void** blocks = (void**)VirtualAlloc(NULL, STRESS_MAX * sizeof(void*),
	                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!blocks) return;

	const unsigned want = (g_stressGb * 1024) / (STRESS_BLOCK / (1024 * 1024));
	unsigned got = 0, high = 0;
	unsigned long long bytes = 0;
	const DWORD started = GetTickCount();

	char line[224];
	int n = sprintf(line, "  memstress: asking the engine's allocator for %u GB in %u MB "
	                "blocks%s", g_stressGb, (unsigned)(STRESS_BLOCK / (1024 * 1024)), "\n");
	AppendFaultLog(line, (unsigned long)n);

	for (unsigned i = 0; i < want && i < STRESS_MAX; i++)
	{
		size_t allocated = 0;
		void* p = cryMalloc(STRESS_BLOCK, &allocated, 16);
		if (!p) break;

		blocks[got++] = p;
		bytes += STRESS_BLOCK;
		if ((unsigned long long)(ULONG_PTR)p > 0xFFFFFFFFull) high++;

		// Write every page, so this is memory the machine really has to find.
		unsigned long long* q = (unsigned long long*)p;
		for (unsigned long long off = 0; off < STRESS_BLOCK; off += 4096)
			q[off / 8] = StressPattern((unsigned long long)(ULONG_PTR)p, off);
	}

	const DWORD wrote = GetTickCount();

	// Read it all back. A pointer that lost its upper half would have written somewhere else.
	unsigned bad = 0;
	for (unsigned i = 0; i < got; i++)
	{
		const unsigned long long* q = (const unsigned long long*)blocks[i];
		for (unsigned long long off = 0; off < STRESS_BLOCK; off += 4096)
			if (q[off / 8] != StressPattern((unsigned long long)(ULONG_PTR)blocks[i], off))
			{
				bad++;
				break;
			}
	}

	unsigned long long lo = 0, hi = 0, img = 0;
	MemoryCensus(&lo, &hi, &img);

	n = sprintf(line, "  memstress: %u of %u blocks (%llu MB), %u above 4 GB, %u corrupted; "
	            "census now %llu MB low / %llu MB high%s",
	            got, want, bytes / (1024 * 1024), high, bad, lo, hi, "\n");
	AppendFaultLog(line, (unsigned long)n);

	for (unsigned i = 0; i < got; i++) cryFree(blocks[i], 16);
	VirtualFree(blocks, 0, MEM_RELEASE);

	n = sprintf(line, "  memstress: done, %u ms to fill, %u ms in total, and the game is still "
	            "running%s", (unsigned)(wrote - started), (unsigned)(GetTickCount() - started),
	            "\n");
	AppendFaultLog(line, (unsigned long)n);
}

// Large private regions below 4 GB, noticed the second they appear. Every hook we have says
// these are not being allocated through it, so the remaining question is when they show up: the
// launcher's clock and the engine's log share a wall clock, and whatever the engine was doing at
// that second is the thing that made them.
#define SLAB_WATCH 192
static unsigned long long g_knownSlab[SLAB_WATCH];
static int                g_knownSlabs = 0;
static volatile LONG      g_slabsLogged = 0;

static void WatchNewSlabs(unsigned elapsed)
{
	if (!g_memDebug) return;

	MEMORY_BASIC_INFORMATION mbi;
	unsigned long long at = 0x10000;

	for (int guard = 0; guard < 200000; guard++)
	{
		if (at >= 0x100000000ull) break;
		if (!VirtualQuery((LPCVOID)(ULONG_PTR)at, &mbi, sizeof(mbi))) break;

		const unsigned long long size = (unsigned long long)mbi.RegionSize;
		if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && size >= 8 * 1024 * 1024)
		{
			const unsigned long long ab = (unsigned long long)(ULONG_PTR)mbi.AllocationBase;
			bool known = false;
			for (int i = 0; i < g_knownSlabs; i++)
				if (g_knownSlab[i] == ab) { known = true; break; }

			if (!known)
			{
				if (g_knownSlabs < SLAB_WATCH) g_knownSlab[g_knownSlabs++] = ab;
				if (InterlockedIncrement(&g_slabsLogged) <= 60)
				{
					// What is in it. Pixels look like noise, a pool that has not been used yet
					// is zeros, and engine structures are pointers - recognisable on sight
					// because this game's MODULES load below 4 GB. Note the word: the modules,
					// not the memory. Data can and does live above the line once the arenas are
					// steered up, and a guard that forgot that distinction froze every cutscene
					// in the game for a week.
					unsigned long long a0 = 0, a1 = 0, a2 = 0, a3 = 0;
					unsigned nonzero = 0;
					{
						const unsigned long long mid = ab + size / 2;
						SafePeek((void*)(ULONG_PTR)ab, (ULONG_PTR*)&a0);
						SafePeek((void*)(ULONG_PTR)(ab + 0x1000), (ULONG_PTR*)&a1);
						SafePeek((void*)(ULONG_PTR)mid, (ULONG_PTR*)&a2);
						SafePeek((void*)(ULONG_PTR)(mid + 0x800), (ULONG_PTR*)&a3);

						for (unsigned s = 0; s < 256; s++)
						{
							ULONG_PTR v = 0;
							if (!SafePeek((void*)(ULONG_PTR)(ab + (unsigned long long)s * 0x2000),
							              &v)) break;
							if (v) nonzero++;
						}
					}

					char sl[240];
					int sn = sprintf(sl, "  new slab: %llu MB at 0x%llX, %u s in, prot 0x%lX, "
					                 "%u/256 used, %llX %llX %llX %llX%s",
					                 size / (1024 * 1024), ab, elapsed,
					                 (unsigned long)mbi.Protect, nonzero, a0, a1, a2, a3,
					                 "\n");
					AppendFaultLog(sl, (unsigned long)sn);
				}
			}
		}

		const unsigned long long next = at + size;
		if (next <= at) break;
		at = next;
	}
}

// Keeps every table current, and says what the checks turned away.
//
// Modules map long after the first patch goes in - CryGameReal among them - and a module missing
// from a table means live objects judged dead, which would leave the AI blind or the game silent.
// Appending is two stores the trampolines pick up on their next call.
static DWORD WINAPI RangeKeeperThread(LPVOID)
{
	unsigned long long seen[MR_MAX_TABLES];
	bool               live[MR_MAX_TABLES];
	for (int i = 0; i < MR_MAX_TABLES; i++) { seen[i] = 0; live[i] = false; }

	char line[224];
	unsigned elapsed = 0, nextReport = 60;   // first summary after a minute, then every five
	for (int i = 0; ; i++)
	{
		const unsigned step = (i < 60) ? 1 : 5;
		Sleep(step * 1000);
		elapsed += step;

		// Watched whatever the flags say: the question is whether these regions are something
		// this launcher causes or something the game does anyway.
		WatchNewSlabs(elapsed);

		if (g_topDown && (i % 4) == 0) FindDriverModules();

		// The correction, watched rather than assumed. Only meaningful once memory is actually
		// being placed high - below 4 GB a lost upper half is harmless and proves nothing.
		if (g_topDown && !g_integrityTripped && (i % 2) == 1)
		{
			if (CheckAllocatorIntegrity() > 0)
			{
				g_integrityTripped = true;
				g_topDown = false;          // stop placing memory high, right now

				int m = sprintf(line, "  INTEGRITY: a pointer lost its upper half - high memory "
				                "switched off after %ld check(s). The game keeps running on what "
				                "it already has%s", g_integrityRuns, "\n");
				AppendFaultLog(line, (unsigned long)m);
			}
		}

		// The ceiling, watched for rather than waited for.
		if (g_lowGuardOn && !g_topDown && !g_integrityTripped && (i % 2) == 0)
		{
			const unsigned long long freeMb = FreeBelow4GB();
			if (freeMb < (unsigned long long)g_lowGuardMb)
			{
				const char* notReady = WhyNotSafeForHighMemory();
				if (notReady)
				{
					// Room is running out and the one thing that makes high memory survivable
					// is not in place. Going high here would trade a possible crash later for
					// a certain one now, so the guard stays out of it and says why - once.
					static bool said = false;
					if (!said)
					{
						said = true;
						int m = sprintf(line, "  lowguard: %llu MB free below 4 GB, but %s - "
						                "staying low, this build is not corrected%s",
						                freeMb, notReady, "\n");
						AppendFaultLog(line, (unsigned long)m);
					}
				}
				else
				{
					g_topDown = true;
					g_guardFired = true;
					HookTopDownEverywhere();

					int m = sprintf(line, "  lowguard: %llu MB free below 4 GB, under the %u MB "
					                "mark - corrections verified, new allocations go high%s",
					                freeMb, (unsigned)g_lowGuardMb, "\n");
					AppendFaultLog(line, (unsigned long)m);
				}
			}
			else if (elapsed % 300 == 0)
			{
				int m = sprintf(line, "  lowguard: %llu MB still free below 4 GB%s",
				                freeMb, "\n");
				AppendFaultLog(line, (unsigned long)m);
			}
		}

		// Late enough that a level is loaded and the engine is doing its normal work.
		if (g_stressGb && elapsed >= 40)
		{
			const unsigned gb = g_stressGb;
			g_stressGb = 0;
			const unsigned keep = gb;
			g_stressGb = keep;
			RunMemoryStress();
			g_stressGb = 0;
		}

		// The hunt is not cheap - a few gigabytes read once - so it runs only with the trap
		// armed, and only after a level has had time to load and do its work.
		if (g_shadowOn && elapsed >= 50 && g_huntRuns < 6 && g_huntFound < 40)
			HuntForTruncatedPointers();

		if (g_bpCount && !g_bpArmed) ArmBreakpoints();

		if (!g_rndFixOff && !g_rndCopyCall) ArmRenderCopyFix();

		// Say when it actually fired. A repair nobody can count is indistinguishable from a
		// repair that never ran, and "the crash did not happen" is not proof either way - the
		// crash only showed up once every fourteen levels.
		{
			static LONG saidSkips = 0;
			const LONG now = g_rndCopySkipped;
			if (now != saidSkips)
			{
				saidSkips = now;
				char rl[200];
				int rn = sprintf(rl, "  rndfix: caught a dead smart-pointer copy, %ld time(s) "
				                 "so far%s", (long)now, "\n");
				AppendFaultLog(rl, (unsigned long)rn);
			}
		}

		// Re-arm the points that have not told their story yet: the call happens many times and
		// the interesting one is not always the first.
		if (g_bpArmed)
		{
			for (int i = 0; i < g_bpCount; i++)
			{
				if (g_bpHits[i] == 0 || g_bpHits[i] >= 40) continue;
				unsigned char* at = (unsigned char*)g_bpAt[i];
				if (*at == 0xCC) continue;
				DWORD old = 0;
				if (VirtualProtect((LPVOID)at, 1, PAGE_EXECUTE_READWRITE, &old))
				{
					*at = 0xCC;
					VirtualProtect((LPVOID)at, 1, old, &old);
					FlushInstructionCache(GetCurrentProcess(), (LPCVOID)at, 1);
				}
			}
		}

		if (g_movDump && g_movDumped < 10 && elapsed >= 45 + (DWORD)(g_movDumped * 4))
		{
			g_movDumped++;
			DumpMovieSystem(g_movDumped);
		}

		if (g_pakInfo && !g_saidPak && elapsed >= 45)
		{
			HMODULE gr = GetModuleHandleA("CryGameReal.dll");
			HMODULE cs = GetModuleHandleA("CrySystem.dll");
			ULONG_PTR env = 0, pak = 0, vt = 0;

			if (gr && cs &&
			    SafePeek((unsigned char*)gr + GENV_RVA_IN_GAMEREAL, &env) && env > 0x10000 &&
			    SafePeek((unsigned char*)env + PCRYPAK_OFF, &pak) && pak > 0x10000 &&
			    SafePeek((const void*)pak, &vt) && vt > 0x10000)
			{
				g_saidPak = true;
				int m = sprintf(line, "  pak: object 0x%llX, vtable 0x%llX (CrySystem+0x%llX)%s",
				                (unsigned long long)pak, (unsigned long long)vt,
				                (unsigned long long)(vt - (ULONG_PTR)cs), "\n");
				AppendFaultLog(line, (unsigned long)m);

				for (int s = 28; s <= 40; s++)
				{
					ULONG_PTR fn = 0;
					if (!SafePeek((const void*)(vt + (ULONG_PTR)s * 8), &fn) || fn < 0x10000)
						continue;
					m = sprintf(line, "      slot %2d (+0x%03X) = CrySystem+0x%llX%s%s",
					            s, s * 8, (unsigned long long)(fn - (ULONG_PTR)cs),
					            s == 33 ? "   <- FReadRaw" : "", "\n");
					AppendFaultLog(line, (unsigned long)m);
				}
			}
		}

		// Asking once tells you a value; asking three times tells you whether it is moving.
		// A sequence that is stuck and a sequence that is playing look identical in a single
		// sample of the engine's clock.
		if (g_sayWhat[0] && g_saidTimes < g_sayMax &&
		    elapsed >= g_sayAfter + (DWORD)(g_saidTimes * g_sayEvery))
		{
			g_saidTimes++;
			const bool ok = RunConsoleCommand(g_sayWhat);
			int m = sprintf(line, "  say: %s -> %s%s", g_sayWhat,
			                ok ? "handed to the console" : "console not reachable", "\n");
			AppendFaultLog(line, (unsigned long)m);
		}

		if (g_topDown)
		{
			HookTopDownEverywhere();

			// Proof the interception happens at all. Without this line a silent log cannot
			// tell "nothing went high" from "the hook is never called".
			static bool saidLive = false;
			if (!saidLive && g_topDownCalls > 0)
			{
				int m = sprintf(line, "  topdown: steering, %ld so far, %ld above 4 GB, "
				                "%ld left low, top 0x%llX%s", g_topDownCalls, g_topDownHigh,
				                g_highSkipped, g_topDownHighest, "\n");
				AppendFaultLog(line, (unsigned long)m);
				saidLive = true;
			}

			static bool saidHigh = false;
			if (!saidHigh && g_topDownHigh > 0)
			{
				int m = sprintf(line, "  topdown: memory is landing high, %ld of %ld above 4 GB,"
				                " %lld MB so far, top 0x%llX%s", g_topDownHigh, g_topDownCalls,
				                g_highBytes / (1024 * 1024), g_topDownHighest, "\n");
				AppendFaultLog(line, (unsigned long)m);
				saidHigh = true;
			}
		}

		const int added = SyncModuleRanges();
		if (added > 0 && g_mrTables > 0)
		{
			int m = sprintf(line, "  ranges: %d module(s) more, %llu listed%s",
			                added, *MR_COUNT(g_mrTable[0]), "\n");
			AppendFaultLog(line, (unsigned long)m);
		}

		// Say when a check actually turned something away. Nothing here means the fix is sitting
		// idle; a steady climb would mean it is rejecting live objects instead.
		for (int k = 0; k < g_mrTables; k++)
		{
			// Once: proof the patched instruction is being executed at all. A quiet log means
			// nothing until this line appears.
			if (!live[k] && *MR_ACCEPT(g_mrTable[k]) > 0)
			{
				int m = sprintf(line, "  %s: check is live, %llu call(s) so far%s",
				                g_mrName[k], *MR_ACCEPT(g_mrTable[k]), "\n");
				AppendFaultLog(line, (unsigned long)m);
				live[k] = true;
			}

			const unsigned long long rej = *MR_REJECT(g_mrTable[k]);
			if (rej == seen[k]) continue;
			int m = sprintf(line, "  %s: %llu dead object(s) turned away%s",
			                g_mrName[k], rej, "\n");
			AppendFaultLog(line, (unsigned long)m);
			seen[k] = rej;
		}

		// Every five minutes, what each check has actually seen. A play session that ends in a
		// crash still leaves the last of these in the log.
		if (elapsed < nextReport) continue;
		nextReport += 300;

		{
			unsigned long long lowMb = 0, highMb = 0, imageMb = 0;
			MemoryCensus(&lowMb, &highMb, &imageMb);
			const unsigned long long totalMb = lowMb + highMb;
			int c = sprintf(line, "  memory: %llu MB low, %llu MB high (%llu%%), %llu MB modules%s",
			                lowMb, highMb, totalMb ? (100 * highMb / totalMb) : 0, imageMb, "\n");
			AppendFaultLog(line, (unsigned long)c);

			{
				const unsigned long long mapLo = g_censusMapLow / (1024 * 1024);
				const unsigned long long mapHi = g_censusMapHigh / (1024 * 1024);
				const unsigned long long prLo = (lowMb > mapLo) ? lowMb - mapLo : 0;
				const unsigned long long prHi = (highMb > mapHi) ? highMb - mapHi : 0;
				const unsigned long long prTot = prLo + prHi;
				c = sprintf(line, "  of that: private %llu low / %llu high (%llu%%), "
				            "views %llu low / %llu high%s",
				            prLo, prHi, prTot ? (100 * prHi / prTot) : 0, mapLo, mapHi, "\n");
				AppendFaultLog(line, (unsigned long)c);

				c = sprintf(line, "  low reserved: %llu MB in %lu region(s) not committed%s",
				            g_lowReserved / (1024 * 1024), g_lowResCount, "\n");
				AppendFaultLog(line, (unsigned long)c);

				c = sprintf(line, "  low by size: under 1MB %lu = %lluMB, 1-8MB %lu = %lluMB, "
				            "8-32MB %lu = %lluMB, 32MB+ %lu = %lluMB%s",
				            g_lowCount[0], g_lowBySize[0] / (1024 * 1024),
				            g_lowCount[1], g_lowBySize[1] / (1024 * 1024),
				            g_lowCount[2], g_lowBySize[2] / (1024 * 1024),
				            g_lowCount[3], g_lowBySize[3] / (1024 * 1024), "\n");
				AppendFaultLog(line, (unsigned long)c);

				for (int q = 0; g_memDebug && q < CENSUS_TOP; q++)
				{
					if (!g_topSize[q]) continue;
					unsigned long long peek0 = 0, peek1 = 0;
					SafePeek((void*)(ULONG_PTR)g_topBase[q], (ULONG_PTR*)&peek0);
					SafePeek((void*)(ULONG_PTR)(g_topBase[q] + 8), (ULONG_PTR*)&peek1);
					c = sprintf(line, "  slab: 0x%llX %lluMB, prot 0x%lX, %lu part(s), starts 0x%llX 0x%llX%s",
					            g_topBase[q], g_topSize[q] / (1024 * 1024), g_topProt[q],
					            g_topParts[q], peek0, peek1, "\n");
					AppendFaultLog(line, (unsigned long)c);
				}

				c = sprintf(line, "  low private: %lu reservation(s), largest "
				            "0x%llX %lluMB, 0x%llX %lluMB, 0x%llX %lluMB, 0x%llX %lluMB, "
				            "0x%llX %lluMB%s", g_lowSlabs,
				            g_topBase[0], g_topSize[0] / (1024 * 1024),
				            g_topBase[1], g_topSize[1] / (1024 * 1024),
				            g_topBase[2], g_topSize[2] / (1024 * 1024),
				            g_topBase[3], g_topSize[3] / (1024 * 1024),
				            g_topBase[4], g_topSize[4] / (1024 * 1024), "\n");
				AppendFaultLog(line, (unsigned long)c);
			}
		}

		if (g_topDown)
		{
			int s = sprintf(line, "  left low: %ld commit-only (%lld MB), %ld fixed address "
			                "(%lld MB), %ld under threshold (%lld MB)%s",
			                g_skipCommitOnly, g_skipCommitKb / 1024,
			                g_skipFixedAddr,  g_skipFixedKb / 1024,
			                g_skipTooSmall,   g_skipSmallKb / 1024, "\n");
			AppendFaultLog(line, (unsigned long)s);

			if (g_memDebug)
			{
				HMODULE ntm = GetModuleHandleA("ntdll.dll");
				const unsigned char* stub = ntm ?
					(const unsigned char*)GetProcAddress(ntm, "NtAllocateVirtualMemory") : 0;
				if (stub)
				{
					s = sprintf(line, "  stub now: %02X %02X %02X %02X %02X %02X %02X %02X%s",
					            stub[0], stub[1], stub[2], stub[3],
					            stub[4], stub[5], stub[6], stub[7], "\n");
					AppendFaultLog(line, (unsigned long)s);
				}
			}

			ReportAllocatorHeads();   // declared above, defined with the module tables

			if (g_heapHigh)
			{
				s = sprintf(line, "  heaphigh: %ld block(s) moved out of the heap (%lld MB in total), "
				            "%ld live (%lld MB), %ld refused%s",
				            g_hhTotal, g_hhKb / 1024, g_hhBlocks, g_hhLiveKb / 1024,
				            g_hhFailed, "\n");
				AppendFaultLog(line, (unsigned long)s);
			}

			s = sprintf(line, "  hook saw: %ld call(s) reach the original, %lld MB landed low, "
			            "%lld MB landed high, %ld for another process%s",
			            g_sawCalls, g_sawLowKb / 1024, g_sawHighKb / 1024,
			            g_sawOtherPid, "\n");
			AppendFaultLog(line, (unsigned long)s);

			s = sprintf(line, "  alloc-ex: %ld call(s), %ld above 4 GB (%lld MB), %lld MB low; "
			            "%ld left low on purpose%s",
			            g_exCalls, g_exHigh, g_exHighKb / 1024, g_exLowKb / 1024,
			            g_highSkipped, "\n");
			AppendFaultLog(line, (unsigned long)s);

			s = sprintf(line, "  file views: %ld mapped (%lld MB), %ld above 4 GB (%lld MB)%s",
			            g_mapCalls, g_mapKb / 1024, g_mapHigh, g_mapHighKb / 1024, "\n");
			AppendFaultLog(line, (unsigned long)s);

			// Who asked, biggest first. Anything under 16 MB is noise next to the gigabyte
			// this is meant to explain.
			for (int q = 0; g_memDebug && q < CALLER_SLOTS; q++)
			{
				const LONGLONG site = g_callers[q].site;
				if (!site || g_callers[q].kb < 16 * 1024) continue;

				char who[96];
				if (site == 1)
					strcpy(who, "(process heap, never left ntdll)");
				else
				{
					HMODULE mod = 0;
					if (GetModuleHandleExA(0x00000004 | 0x00000002,
					                       (LPCSTR)(ULONG_PTR)site, &mod) && mod)
					{
						char full[MAX_PATH];
						if (GetModuleFileNameA(mod, full, MAX_PATH))
						{
							const char* b = strrchr(full, '\\');
							sprintf(who, "%s+0x%llX", b ? b + 1 : full,
							        (unsigned long long)site -
							        (unsigned long long)(ULONG_PTR)mod);
						}
						else sprintf(who, "0x%llX", (unsigned long long)site);
					}
					else sprintf(who, "0x%llX (no module)", (unsigned long long)site);
				}

				s = sprintf(line, "  holding: %s  %lld MB (%lld MB low) in %ld call(s)%s",
				            who, g_callers[q].kb / 1024, g_callers[q].kbLow / 1024,
				            g_callers[q].calls, "\n");
				AppendFaultLog(line, (unsigned long)s);
			}

			int m = sprintf(line, "  topdown: %ld steered, %ld above 4 GB (%lld MB), "
			                "0x%llX..0x%llX%s",
			                g_topDownCalls, g_topDownHigh, g_highBytes / (1024 * 1024),
			                (g_topDownLowest == ~0ull) ? 0 : g_topDownLowest,
			                g_topDownHighest, "\n");
			AppendFaultLog(line, (unsigned long)m);

			// A trap that caught nothing is a result too: it says the arenas break for some
			// reason other than a pointer losing its upper half.
			if (g_shadowOn)
			{
				m = sprintf(line, "  shadow: %ld arena(s) placed under the trap, %ld fault(s), "
				            "%ld distinct site(s), %ld page(s) healed; hunt %ld pass(es), "
				            "%ld truncated copy(ies)%s",
				            g_shadowPlaced, g_shadowHits, g_shadowSeenCount,
				            g_shadowHealed, g_huntRuns, g_huntFound, "\n");
				AppendFaultLog(line, (unsigned long)m);
			}
		}
		for (int k = 0; k < g_mrTables; k++)
		{
			const unsigned long long ok = *MR_ACCEPT(g_mrTable[k]);
			if (!ok) continue;
			int m = sprintf(line, "  %s: %llu checked, %llu turned away, %u min in%s",
			                g_mrName[k], ok, *MR_REJECT(g_mrTable[k]), elapsed / 60, "\n");
			AppendFaultLog(line, (unsigned long)m);
		}
	}
}

static volatile LONG g_keeperStarted = 0;

static void StartRangeKeeper(void)
{
	if (InterlockedCompareExchange(&g_keeperStarted, 1, 0) != 0) return;
	DWORD tid = 0;
	HANDLE th = CreateThread(NULL, 0, RangeKeeperThread, NULL, 0, &tid);
	if (th) CloseHandle(th);
}

// Waits for a module to be mapped, which happens well after the launcher starts.
static bool WaitForModule(const char* name)
{
	for (int i = 0; i < 12000; i++)
	{
		if (GetModuleHandleA(name)) { Sleep(50); return true; }  // let the loader finish with it
		Sleep(5);
	}
	return false;
}

static DWORD WINAPI AiFixThread(LPVOID)
{
	WaitForModule("CryAISystem.dll");

	const char* r = PatchAiDeadTarget();
	char line[224];
	int n = sprintf(line, "  aifix: dead-target check %s%s", r, "\n");
	AppendFaultLog(line, (unsigned long)n);

	if (g_mrTables > 0) StartRangeKeeper();
	return 0;
}

static DWORD WINAPI SoundFixThread(LPVOID)
{
	WaitForModule("fmodex64.dll");

	const char* r = PatchSoundDeadObject();
	char line[224];
	int n = sprintf(line, "  sndfix: dead-object check %s%s", r, "\n");
	AppendFaultLog(line, (unsigned long)n);

	if (g_mrTables > 0) StartRangeKeeper();
	return 0;
}

// The same truncation, in the other modules.
//
// The engine's bucket allocator is not one piece of code in CrySystem - every module carries its
// own compiled copy, with its own globals. The corrections made so far cover CrySystem's copy
// only, which was enough while everything lived below the 4 GB line.
//
// A run with -topdown proved it is not enough. The game died writing to 0x00000000FD333518 with
// rax = 0x00000000FD333510 while rdx held 0x00007FF4FD333528 - the same pointer, upper half
// gone. The faulting instruction was in CrySoundSystem, reading a global that another
// instruction had written with a 32-bit store.
//
// Auditing every module for "written 32, read 64" then found the same four globals, in the same
// order, in CrySoundSystem, CryRenderD3D11 and CryRenderD3D9. Eight stores each.
//
// Two shapes, two repairs:
//   REX form (7 or 8 bytes) - one bit turns the store into a 64-bit one, length unchanged.
//   rip-relative form (6 bytes) - the 64-bit version needs a seventh byte, so it goes through a
//   trampoline, exactly like sites 3 to 5 in CrySystem.
struct WidenSite
{
	unsigned             rva;
	const unsigned char* bytes;
	unsigned             len;
};

// CrySoundSystem.dll - this is the copy that actually crashed.
static const unsigned char kSnd0[] = { 0x89, 0x2D, 0xF8, 0x7C, 0x0B, 0x00 };
static const unsigned char kSnd1[] = { 0x89, 0x2D, 0xD1, 0x7C, 0x0B, 0x00 };
static const unsigned char kSnd2[] = { 0x89, 0x05, 0x8E, 0x7E, 0x0B, 0x00 };
static const unsigned char kSnd3[] = { 0x44, 0x89, 0x15, 0x36, 0x7E, 0x0B, 0x00 };
static const unsigned char kSnd4[] = { 0x89, 0x0D, 0xCA, 0x7C, 0x0B, 0x00 };
static const unsigned char kSnd5[] = { 0x47, 0x89, 0x94, 0xC8, 0xD0, 0xAD, 0x0C, 0x00 };
static const unsigned char kSnd6[] = { 0x8B, 0x03,                       // mov eax, [rbx]
                                       0x41, 0x89, 0x84, 0xED, 0xD0, 0xAD, 0x0C, 0x00 };
static const unsigned char kSnd7[] = { 0x45, 0x89, 0xA4, 0xDB, 0xD0, 0xAD, 0x0C, 0x00 };

static const WidenSite kSndSites[] = {
	{ 0x0130AA, kSnd0, sizeof(kSnd0) },
	{ 0x0130C9, kSnd1, sizeof(kSnd1) },
	{ 0x012F2C, kSnd2, sizeof(kSnd2) },
	{ 0x012F83, kSnd3, sizeof(kSnd3) },
	{ 0x0130F0, kSnd4, sizeof(kSnd4) },
	{ 0x01354D, kSnd5, sizeof(kSnd5) },
	{ 0x013B41, kSnd6, sizeof(kSnd6) },
	{ 0x013C60, kSnd7, sizeof(kSnd7) },
};

// CryRenderD3D11.dll - same allocator, same four globals.
static const unsigned char kR11_0[] = { 0x45, 0x89, 0xA4, 0xDB, 0x40, 0x3F, 0x3A, 0x00 };
static const unsigned char kR11_1[] = { 0x47, 0x89, 0x94, 0xC8, 0x40, 0x3F, 0x3A, 0x00 };
static const unsigned char kR11_2[] = { 0x8B, 0x03,
                                        0x41, 0x89, 0x84, 0xED, 0x40, 0x3F, 0x3A, 0x00 };
static const unsigned char kR11_3[] = { 0x89, 0x05, 0xAE, 0x75, 0x16, 0x00 };
static const unsigned char kR11_4[] = { 0x44, 0x89, 0x15, 0x56, 0x75, 0x16, 0x00 };
static const unsigned char kR11_5[] = { 0x89, 0x0D, 0xAF, 0x73, 0x16, 0x00 };
static const unsigned char kR11_6[] = { 0x89, 0x35, 0x5E, 0x3D, 0x1B, 0x00 };
static const unsigned char kR11_7[] = { 0x89, 0x35, 0x85, 0x3D, 0x1B, 0x00 };

static const WidenSite kR11Sites[] = {
	{ 0x23EEEF, kR11_0, sizeof(kR11_0) },
	{ 0x23F12F, kR11_1, sizeof(kR11_1) },
	{ 0x240C1F, kR11_2, sizeof(kR11_2) },
	{ 0x23E7CC, kR11_3, sizeof(kR11_3) },
	{ 0x23E823, kR11_4, sizeof(kR11_4) },
	{ 0x23E9CB, kR11_5, sizeof(kR11_5) },
	{ 0x23E9A4, kR11_6, sizeof(kR11_6) },
	{ 0x23E985, kR11_7, sizeof(kR11_7) },
};

// CryRenderD3D9.dll - not loaded in DX11 mode, but the same mine is in it.
static const unsigned char kR9_0[] = { 0x45, 0x89, 0xA4, 0xDB, 0x30, 0x92, 0x3B, 0x00 };
static const unsigned char kR9_1[] = { 0x47, 0x89, 0x94, 0xC8, 0x30, 0x92, 0x3B, 0x00 };
static const unsigned char kR9_2[] = { 0x8B, 0x03,
                                       0x41, 0x89, 0x84, 0xED, 0x30, 0x92, 0x3B, 0x00 };
static const unsigned char kR9_3[] = { 0x89, 0x05, 0x8E, 0x1E, 0x18, 0x00 };
static const unsigned char kR9_4[] = { 0x44, 0x89, 0x15, 0x36, 0x1E, 0x18, 0x00 };
static const unsigned char kR9_5[] = { 0x89, 0x0D, 0x8F, 0x1C, 0x18, 0x00 };
static const unsigned char kR9_6[] = { 0x89, 0x35, 0x2E, 0xD4, 0x1C, 0x00 };
static const unsigned char kR9_7[] = { 0x89, 0x35, 0x55, 0xD4, 0x1C, 0x00 };

static const WidenSite kR9Sites[] = {
	{ 0x23998F, kR9_0, sizeof(kR9_0) },
	{ 0x239BCF, kR9_1, sizeof(kR9_1) },
	{ 0x23B74F, kR9_2, sizeof(kR9_2) },
	{ 0x2391DC, kR9_3, sizeof(kR9_3) },
	{ 0x239233, kR9_4, sizeof(kR9_4) },
	{ 0x2393DB, kR9_5, sizeof(kR9_5) },
	{ 0x2393B4, kR9_6, sizeof(kR9_6) },
	{ 0x239395, kR9_7, sizeof(kR9_7) },
};

// Turns one 32-bit store into a 64-bit one. Returns 0 on success, or a reason.
static const char* WidenStore(unsigned char* base, const WidenSite* s, unsigned char** cave,
                              size_t* caveUsed)
{
	unsigned char* at = base + s->rva;
	if (memcmp(at, s->bytes, s->len) != 0)
	{
		// Already done? The REX form is the only one that can be recognised after the fact.
		if (s->bytes[0] >= 0x40 && s->bytes[0] <= 0x4F && at[0] == (s->bytes[0] | 0x08))
			return 0;
		return "no match";
	}

	// REX form: set W and the same instruction stores eight bytes.
	if (s->bytes[0] >= 0x40 && s->bytes[0] <= 0x4F)
	{
		const unsigned char rex = (unsigned char)(s->bytes[0] | 0x08);
		if (!WriteBytes(at, &rex, 1)) return "write failed";

		// Read it back. WriteBytes can report success while something else - another patcher,
		// a protection layer, a copy-on-write page that went somewhere else - leaves the
		// original byte in place, and a correction that is not there is worse than none:
		// we would go on to place memory high on the strength of it.
		if (*at != rex) return "write did not stick";
		return 0;
	}

	// The list walk: "mov eax, [reg]" reads the next block's address with half of it missing,
	// and the store puts that half back as the new head. Both halves of the bug are two
	// instructions apart, so one trampoline replaces the pair.
	//
	//   8B 03                     mov eax, dword ptr [rbx]
	//   41 89 84 ED <disp32>      mov dword ptr [r13 + rbp*8 + heads], eax
	//
	// becomes the same thing 64 bits wide. The displacement is relative to r13, which the
	// function loads with the module's own base, so it survives the move into the cave.
	const bool isPair = (s->bytes[0] == 0x8B && s->len == 10);

	if (!isPair && (s->bytes[0] != 0x89 || (s->bytes[1] & 0xC7) != 0x05)) return "unknown shape";

	if (!*cave)
	{
		*cave = AllocCaveNear(base);
		*caveUsed = 0;
		if (!*cave) return "no cave";
	}
	if (*caveUsed + 24 > kCaveSize) return "cave full";

	unsigned char* code = *cave + *caveUsed;
	int n = 0;

	if (isPair)
	{
		code[n++] = 0x48; code[n++] = 0x8B; code[n++] = s->bytes[1];       // mov rax, [reg]
		code[n++] = (unsigned char)(s->bytes[2] | 0x08);                   // REX.W on the store
		code[n++] = s->bytes[3]; code[n++] = s->bytes[4]; code[n++] = s->bytes[5];
		memcpy(code + n, s->bytes + 6, 4); n += 4;                         // displacement, as is
	}
	else
	{
		long disp = 0;
		memcpy(&disp, s->bytes + 2, 4);
		unsigned char* target = at + s->len + disp;      // the global being written

		code[n++] = 0x48; code[n++] = 0x89; code[n++] = s->bytes[1];
		const long rel = (long)(target - (code + n + 4));
		memcpy(code + n, &rel, 4); n += 4;
	}

	code[n++] = 0xE9;
	{
		const long rel = (long)((at + s->len) - (code + n + 4));
		memcpy(code + n, &rel, 4); n += 4;
	}
	*caveUsed += (size_t)n;

	return WriteJump(at, code, s->len) ? 0 : "jump failed";
}

// Where each copy keeps its 32 free-list heads. All zero means the allocator has not served a
// single block yet, which is the only moment it is safe to change how it stores pointers.
static unsigned HeadsRvaFor(const char* name)
{
	if (_stricmp(name, "CrySoundSystem.dll") == 0) return 0x0CADD0;
	if (_stricmp(name, "CryRenderD3D11.dll") == 0) return 0x3A3F40;
	if (_stricmp(name, "CryRenderD3D9.dll")  == 0) return 0x3B9230;
	return 0;
}

static bool AllocatorUntouched(unsigned char* base, const char* name)
{
	const unsigned rva = HeadsRvaFor(name);
	if (!rva) return true;

	const ULONG_PTR* heads = (const ULONG_PTR*)(base + rva);
	for (unsigned i = 0; i < 32; i++)
	{
		ULONG_PTR v = 0;
		if (SafePeek(&heads[i], &v) && v) return false;
	}
	return true;
}

static const char* WidenModule(const char* name, const WidenSite* sites, unsigned count)
{
	static char result[192];

	unsigned char* base = (unsigned char*)GetModuleHandleA(name);
	if (!base) { sprintf(result, "%s: not loaded", name); return result; }

	// Too late is worse than not at all: a half-truncated free list plus correct stores is a mix
	// the allocator cannot walk. A run that patched CrySoundSystem after it had started left the
	// game stuck at 69 MB, never finishing the load.
	if (!AllocatorUntouched(base, name))
	{
		sprintf(result, "%s: ALREADY RUNNING, left alone", name);
		return result;
	}

	unsigned char* cave = 0;
	size_t used = 0;
	unsigned done = 0, skipped = 0;
	const char* firstReason = 0;

	for (unsigned i = 0; i < count; i++)
	{
		const char* why = WidenStore(base, &sites[i], &cave, &used);
		if (!why) done++;
		else { skipped++; if (!firstReason) firstReason = why; }
	}

	if (skipped) sprintf(result, "%s: %u of %u widened (%s)", name, done, count, firstReason);
	else         sprintf(result, "%s: all %u widened", name, count);
	return result;
}

// The Lua pool in CryScriptSystem, which swaps list heads with a 32-bit compare-and-exchange.
//
// The allocator keeps three lock-free stacks of free blocks. Pushing and popping looks like this:
//
//   mov rbx, qword ptr [head]      ; the head, read in full
//   lea rcx, [head]
//   mov edx, edi                   ; the new head - HALF a pointer
//   mov r8d, ebx                   ; the expected head - HALF a pointer
//   mov qword ptr [rdi+0x10], rbx  ; item->next, written in full
//   call <lock cmpxchg dword ptr [rcx], edx>
//
// Half of the code is already 64-bit and half is not, which is what makes it invisible while
// everything sits low. With memory above the 4 GB line the head becomes a pointer with nothing
// in its upper half, and the first read through it dies - the game never gets past Lua.
//
// Each of the five places is replaced by a trampoline that does the same thing in full width:
// the addresses go into rdx and r8 whole, and the exchange becomes "lock cmpxchg qword".
//
// The blocks are 22 to 31 bytes, so a 5-byte jump fits with room to spare, and the retry branch
// keeps pointing at the instruction that re-reads the head.
#define LUA_SITES 8

struct LuaCasSite
{
	unsigned             rva;      // start of the block being replaced
	unsigned             len;      // how much of it
	unsigned             retry;    // where the CAS failure path goes
	unsigned             done;     // where control continues on success
	unsigned             head;     // the global holding the list head
	const unsigned char* bytes;
};

static const unsigned char kLua0[] = {          // pop: next comes from the block itself
	0x8B, 0x53, 0x10,                           // mov edx, [rbx+0x10]
	0x48, 0x8D, 0x0D, 0xFE, 0xFB, 0x09, 0x00,   // lea rcx, [head]
	0x44, 0x8B, 0xC3,                           // mov r8d, ebx
	0xE8, 0x7E, 0x67, 0xFF, 0xFF,               // call cas32
	0x3B, 0xC3,                                 // cmp eax, ebx
	0x75, 0xDA                                  // jne retry
};
static const unsigned char kLua1[] = {          // push, item in rdi
	0x48, 0x8D, 0x0D, 0x8A, 0xFB, 0x09, 0x00,
	0x8B, 0xD7,                                 // mov edx, edi
	0x44, 0x8B, 0xC3,
	0x48, 0x89, 0x5F, 0x10,                     // mov [rdi+0x10], rbx
	0xE8, 0x04, 0x67, 0xFF, 0xFF,
	0x3B, 0xC3,
	0x75, 0xE0
};
static const unsigned char kLua2[] = {          // push, item in rdi, different head
	0x48, 0x8D, 0x0D, 0x1A, 0xFA, 0x09, 0x00,
	0x8B, 0xD7,
	0x44, 0x8B, 0xC3,
	0x48, 0x89, 0x5F, 0x10,
	0xE8, 0xA4, 0x65, 0xFF, 0xFF,
	0x3B, 0xC3,
	0x75, 0xE0
};
static const unsigned char kLua3[] = {          // push, the head goes through the stack
	0x48, 0x8D, 0x0D, 0xF2, 0xF9, 0x09, 0x00,
	0x8B, 0xD7,
	0x48, 0x89, 0x44, 0x24, 0x50,               // mov [rsp+0x50], rax
	0x48, 0x8B, 0x5C, 0x24, 0x50,               // mov rbx, [rsp+0x50]
	0x44, 0x8B, 0xC3,
	0xE8, 0x7E, 0x65, 0xFF, 0xFF,
	0x3B, 0xC3,
	0x75, 0xDA
};
static const unsigned char kLua4[] = {          // push, item in rsi
	0x48, 0x8D, 0x0D, 0xCA, 0xF9, 0x09, 0x00,
	0x8B, 0xD6,                                 // mov edx, esi
	0x44, 0x8B, 0xC3,
	0x48, 0x89, 0x5E, 0x10,                     // mov [rsi+0x10], rbx
	0xE8, 0x44, 0x65, 0xFF, 0xFF,
	0x3B, 0xC3,
	0x75, 0xE0
};

// The same bug where the list head is not a global but an address already in a register:
// a per-size-class array of heads. Nothing to recompute, so 'head' is zero for these.
static const unsigned char kLua5[] = {          // push, head at [rsi], item in rdi
	0x8B, 0xD7,                                 // mov edx, edi
	0x48, 0x8B, 0xCE,                           // mov rcx, rsi
	0x44, 0x8B, 0xC3,                           // mov r8d, ebx
	0x48, 0x89, 0x1F,                           // mov [rdi], rbx
	0xE8, 0x7D, 0x6B, 0xFF, 0xFF,
	0x3B, 0xC3,
	0x75, 0xE9
};
static const unsigned char kLua6[] = {          // push, head at [rbp], item in r13
	0x41, 0x8B, 0xD5,                           // mov edx, r13d
	0x48, 0x8B, 0xCD,                           // mov rcx, rbp
	0x44, 0x8B, 0xC0,                           // mov r8d, eax
	0x48, 0x89, 0x07,                           // mov [rdi], rax
	0x8B, 0xD8,                                 // mov ebx, eax
	0xE8, 0x89, 0x5D, 0xFF, 0xFF,
	0x3B, 0xC3,
	0x75, 0xE5
};
static const unsigned char kLua7[] = {          // pop, head at [rdi]
	0x8B, 0x13,                                 // mov edx, [rbx]
	0x44, 0x8B, 0xC3,                           // mov r8d, ebx
	0x48, 0x8B, 0xCF,                           // mov rcx, rdi
	0xE8, 0xB4, 0x54, 0xFF, 0xFF,
	0x3B, 0xC3,
	0x75, 0xE7
};

static const LuaCasSite kLuaSites[LUA_SITES] = {
	{ 0x00AF70, sizeof(kLua0), 0x00AF60, 0x00AF86, 0x0AAB78, kLua0 },
	{ 0x00AFE7, sizeof(kLua1), 0x00AFE0, 0x00B000, 0x0AAB78, kLua1 },
	{ 0x00B147, sizeof(kLua2), 0x00B140, 0x00B160, 0x0AAB68, kLua2 },
	{ 0x00B167, sizeof(kLua3), 0x00B160, 0x00B186, 0x0AAB60, kLua3 },
	{ 0x00B1A7, sizeof(kLua4), 0x00B1A0, 0x00B1C0, 0x0AAB78, kLua4 },
	{ 0x00AB73, sizeof(kLua5), 0x00AB70, 0x00AB87, 0,        kLua5 },
	{ 0x00B964, sizeof(kLua6), 0x00B960, 0x00B97B, 0,        kLua6 },
	{ 0x00C23F, sizeof(kLua7), 0x00C237, 0x00C250, 0,        kLua7 },
};

static const char* PatchLuaPool(void)
{
	static char result[160];

	unsigned char* base = (unsigned char*)GetModuleHandleA("CryScriptSystem.dll");
	if (!base) return "not loaded";

	unsigned char* cave = 0;
	size_t used = 0;
	unsigned done = 0;
	const char* why = 0;

	for (int i = 0; i < LUA_SITES; i++)
	{
		const LuaCasSite* s = &kLuaSites[i];
		unsigned char* at = base + s->rva;
		if (memcmp(at, s->bytes, s->len) != 0) { if (!why) why = "no match"; continue; }

		if (!cave)
		{
			cave = AllocCaveNear(base);
			if (!cave) return "no cave";
		}
		if (used + 72 > kCaveSize) { why = "cave full"; break; }

		unsigned char* code = cave + used;
		int n = 0;

		if (s->head)
		{
			// The head's address, whole, so no displacement has to be recomputed.
			code[n++] = 0x48; code[n++] = 0xB9;                   // mov rcx, imm64
			const unsigned long long g = (unsigned long long)(ULONG_PTR)(base + s->head);
			memcpy(code + n, &g, 8); n += 8;
		}

		// The parts that differ: where the new head comes from, and where the old one lives.
		if (i == 0)
		{
			code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0x53; code[n++] = 0x10; // mov rdx,[rbx+0x10]
		}
		else if (i == 4)
		{
			code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0xD6;                  // mov rdx, rsi
		}
		else if (i == 5)
		{
			code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0xD7;                  // mov rdx, rdi
			code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0xCE;                  // mov rcx, rsi
		}
		else if (i == 6)
		{
			code[n++] = 0x49; code[n++] = 0x8B; code[n++] = 0xD5;                  // mov rdx, r13
			code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0xCD;                  // mov rcx, rbp
		}
		else if (i == 7)
		{
			code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0x13;                  // mov rdx, [rbx]
			code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0xCF;                  // mov rcx, rdi
		}
		else
		{
			code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0xD7;                  // mov rdx, rdi
		}

		if (i == 3)
		{
			code[n++] = 0x48; code[n++] = 0x89; code[n++] = 0x44; code[n++] = 0x24; code[n++] = 0x50;
			code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0x5C; code[n++] = 0x24; code[n++] = 0x50;
		}

		if (i == 6)
		{
			code[n++] = 0x4C; code[n++] = 0x8B; code[n++] = 0xC0;                  // mov r8, rax
			code[n++] = 0x48; code[n++] = 0x89; code[n++] = 0x07;                  // mov [rdi], rax
			code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0xD8;                  // mov rbx, rax
		}
		else
		{
			code[n++] = 0x4C; code[n++] = 0x8B; code[n++] = 0xC3;                  // mov r8, rbx
		}

		if (i == 1 || i == 2)
		{
			code[n++] = 0x48; code[n++] = 0x89; code[n++] = 0x5F; code[n++] = 0x10; // mov [rdi+0x10], rbx
		}
		else if (i == 4)
		{
			code[n++] = 0x48; code[n++] = 0x89; code[n++] = 0x5E; code[n++] = 0x10; // mov [rsi+0x10], rbx
		}
		else if (i == 5)
		{
			code[n++] = 0x48; code[n++] = 0x89; code[n++] = 0x1F;                   // mov [rdi], rbx
		}

		// The exchange itself, now eight bytes wide.
		code[n++] = 0x49; code[n++] = 0x8B; code[n++] = 0xC0;                      // mov rax, r8
		code[n++] = 0xF0; code[n++] = 0x48; code[n++] = 0x0F; code[n++] = 0xB1;
		code[n++] = 0x11;                                                          // lock cmpxchg [rcx], rdx
		code[n++] = 0x48; code[n++] = 0x3B; code[n++] = 0xC3;                      // cmp rax, rbx

		code[n++] = 0x0F; code[n++] = 0x85;                                        // jne retry
		{
			const long rel = (long)((base + s->retry) - (code + n + 4));
			memcpy(code + n, &rel, 4); n += 4;
		}
		code[n++] = 0xE9;                                                          // jmp done
		{
			const long rel = (long)((base + s->done) - (code + n + 4));
			memcpy(code + n, &rel, 4); n += 4;
		}

		if (!WriteJump(at, code, s->len)) { why = "jump failed"; continue; }
		used += (size_t)n;
		done++;
	}

	if (done == LUA_SITES) sprintf(result, "all %d widened", LUA_SITES);
	else sprintf(result, "%u of %d widened (%s)", done, LUA_SITES, why ? why : "?");
	return result;
}

// Which modules carry a copy, and where.
static const struct { const char* name; const WidenSite* sites; unsigned count; } kWidenWork[] = {
	{ "CrySoundSystem.dll", kSndSites, 8 },
	{ "CryRenderD3D11.dll", kR11Sites, 8 },
	{ "CryRenderD3D9.dll",  kR9Sites,  8 },
};

static volatile LONG g_widened[3] = { 0, 0, 0 };   // an attempt was made
static volatile LONG g_widenOk[3]  = { 0, 0, 0 };   // every site in the module took
static volatile LONG g_luaOk       = 0;

// Is the copy inside this module actually in use, and is the correction holding?
//
// Two questions in one reading. A head that is not null means this module's own copy of the
// allocator is serving allocations - it is not routing everything through CrySystem's CryMalloc.
// A head above 4 GB means the widened store kept the upper half: that is the correction working,
// observed in the engine's own data rather than inferred from a counter of ours.
// Does the correction still hold, judged from the engine's own data?
//
// Every non-empty head in these lists is a pointer to a free block inside a page the allocator
// owns, so it must be readable. A head that cannot be read is a pointer that lost its upper
// half - the exact failure this whole effort exists to prevent - and it means memory is being
// placed high while something up the chain is still storing 32 bits of it.
//
// Finding one is not a reason to log and carry on. It is a reason to stop placing memory high
// at once: what is already allocated keeps working, and the damage stops growing.
// May memory be placed high yet?
//
// The question is not "did we try" but "is every module that is loaded right now actually
// corrected". A module whose allocator still stores 32 bits of a pointer dies within seconds of
// the first high allocation - measured: with -nomodfix and high memory, CrySoundSystem faults
// at +0x12E68 before the level finishes loading, which is far too fast for any watchdog to
// catch. So the decision has to be made before the first high byte, not after.
//
// Returns 0 when it is safe, or the name of what is not ready.
static const char* WhyNotSafeForHighMemory(void)
{
	if (g_engineFixFailed) return "CrySystem allocator not corrected";

	for (int i = 0; i < 3; i++)
		if (GetModuleHandleA(kWidenWork[i].name) && !g_widenOk[i])
			return kWidenWork[i].name;

	if (GetModuleHandleA("CryScriptSystem.dll") && !g_luaOk)
		return "CryScriptSystem.dll";

	return 0;
}

static int CheckAllocatorIntegrity(void)
{
	int bad = 0;

	for (int i = 0; i < 3; i++)
	{
		HMODULE m = GetModuleHandleA(kWidenWork[i].name);
		if (!m) continue;

		const unsigned rva = HeadsRvaFor(kWidenWork[i].name);
		if (!rva) continue;

		const ULONG_PTR* heads = (const ULONG_PTR*)((unsigned char*)m + rva);
		for (unsigned k = 0; k < 32; k++)
		{
			ULONG_PTR head = 0;
			if (!SafePeek(&heads[k], &head)) break;
			if (!head) continue;

			// A truncated pointer is a small number that happens to be left over from the
			// lower half, so it is both unaligned-looking and unreadable more often than not.
			ULONG_PTR first = 0;
			if (!SafePeek((const void*)head, &first))
			{
				bad++;
				if (bad <= 3)
				{
					char line[200];
					int n = sprintf(line, "  INTEGRITY: %s list %u points at 0x%llX, "
					                "which cannot be read%s",
					                kWidenWork[i].name, k,
					                (unsigned long long)head, "\n");
					AppendFaultLog(line, (unsigned long)n);
				}
			}
		}
	}

	InterlockedIncrement(&g_integrityRuns);
	if (bad) InterlockedExchangeAdd(&g_integrityBad, bad);
	return bad;
}

static void ReportAllocatorHeads(void)
{
	for (int i = 0; i < 3; i++)
	{
		HMODULE m = GetModuleHandleA(kWidenWork[i].name);
		if (!m) continue;

		const unsigned rva = HeadsRvaFor(kWidenWork[i].name);
		if (!rva) continue;

		const ULONG_PTR* heads = (const ULONG_PTR*)((unsigned char*)m + rva);
		unsigned used = 0, high = 0;
		for (unsigned k = 0; k < 32; k++)
		{
			ULONG_PTR v = 0;
			if (!SafePeek(&heads[k], &v)) break;
			if (!v) continue;
			used++;
			if ((unsigned long long)v > 0xFFFFFFFFull) high++;
		}

		char line[200];
		int n = sprintf(line, "  heads: %s %u of 32 lists in use, %u of them above 4 GB%s",
		                kWidenWork[i].name, used, high, "\n");
		AppendFaultLog(line, (unsigned long)n);
	}
}
static volatile LONG g_luaWidened = 0;

static void WidenLuaOnce(void)
{
	if (InterlockedCompareExchange(&g_luaWidened, 1, 0) != 0) return;

	const char* r = PatchLuaPool();
	if (strstr(r, "all ")) InterlockedExchange(&g_luaOk, 1);
	char line[192];
	int n = sprintf(line, "  modfix: CryScriptSystem.dll: %s%s", r, "\n");
	AppendFaultLog(line, (unsigned long)n);
}

// Widens one module if it is loaded and has not been done yet. Safe to call from anywhere.
static void WidenIfNeeded(int i)
{
	if (i < 0 || i > 2) return;
	if (InterlockedCompareExchange(&g_widened[i], 1, 0) != 0) return;
	if (!GetModuleHandleA(kWidenWork[i].name)) { g_widened[i] = 0; return; }

	const char* r = WidenModule(kWidenWork[i].name, kWidenWork[i].sites, kWidenWork[i].count);

	// "all N widened" is the only result that lets memory go high later. Anything else - one
	// site that did not match, a write that did not stick - leaves this module's allocator
	// storing 32 bits of a 64-bit pointer, and high memory would kill it in seconds.
	if (strstr(r, "all ")) InterlockedExchange(&g_widenOk[i], 1);

	char line[224];
	int n = sprintf(line, "  modfix: %s%s", r, "\n");
	AppendFaultLog(line, (unsigned long)n);
}

// The loader tells us the moment a module is mapped, before whoever called LoadLibrary gets
// control back. The watching thread below was too late: CrySoundSystem's allocator had already
// handed out memory and crashed by the time the thread noticed the module existed.
typedef struct { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } LDR_USTRING;

typedef struct {
	ULONG              Flags;
	const LDR_USTRING* FullDllName;
	const LDR_USTRING* BaseDllName;
	PVOID              DllBase;
	ULONG              SizeOfImage;
} LDR_NOTIFICATION_DATA;

typedef VOID (CALLBACK *PFN_LdrNotify)(ULONG, const LDR_NOTIFICATION_DATA*, PVOID);
typedef LONG (NTAPI *PFN_LdrRegister)(ULONG, PFN_LdrNotify, PVOID, PVOID*);

static PVOID g_ldrCookie = 0;

static VOID CALLBACK OnModuleLoaded(ULONG reason, const LDR_NOTIFICATION_DATA* data, PVOID)
{
	if (reason != 1 || !data || !data->BaseDllName || !data->BaseDllName->Buffer) return;

	// The name arrives as wide characters; compare the ASCII way, it is all ASCII here.
	char name[64];
	const PWSTR w = data->BaseDllName->Buffer;
	unsigned k = 0;
	for (; k < sizeof(name) - 1 && w[k]; k++) name[k] = (char)w[k];
	name[k] = 0;

	for (int i = 0; i < 3; i++)
		if (_stricmp(name, kWidenWork[i].name) == 0) WidenIfNeeded(i);

	if (_stricmp(name, "CryScriptSystem.dll") == 0) WidenLuaOnce();
}

static bool RegisterLoaderNotification(void)
{
	HMODULE nt = GetModuleHandleA("ntdll.dll");
	if (!nt) return false;

	PFN_LdrRegister reg = (PFN_LdrRegister)GetProcAddress(nt, "LdrRegisterDllNotification");
	if (!reg) return false;

	return reg(0, OnModuleLoaded, NULL, &g_ldrCookie) >= 0;
}

// Runs once each module is mapped. They map at very different times, so each gets its own wait.
static DWORD WINAPI ModuleFixThread(LPVOID)
{
	const bool early = RegisterLoaderNotification();

	char line[160];
	int n = sprintf(line, "  modfix: loader notification %s%s",
	                early ? "registered" : "UNAVAILABLE, polling instead", "\n");
	AppendFaultLog(line, (unsigned long)n);

	// Anything already mapped when we got here, plus a slow safety net if the notification is
	// not available on this Windows.
	for (int k = 0; k < 24000; k++)
	{
		for (int i = 0; i < 3; i++)
			if (!g_widened[i] && GetModuleHandleA(kWidenWork[i].name)) WidenIfNeeded(i);
		if (!g_luaWidened && GetModuleHandleA("CryScriptSystem.dll")) WidenLuaOnce();
		if (early && g_widened[0] && g_widened[1]) break;
		Sleep(5);
	}
	return 0;
}

// Which corrections to apply, one bit per site: -enginefix turns on all eight,
// -enginefix:1F only sites 1 to 5, -enginefix:20 only site 6, and so on.
//
// Bisecting by hand means a rebuild per guess; this makes it a command line away. The order
// is the order they were found: 1 slab store, 2 free-list store, 3 list walk, 4 arena
// cursor, 5 arena base, 6 second free-list store, 7 narrow read plus cursor, 8 cursor from
// the other end.
static unsigned g_fixMask = 0xFF;

#define FIX_SITE(n) ((g_fixMask & (1u << ((n) - 1))) != 0)

// Sites 6, 7 and 8, found by auditing every instruction that touches these globals instead of
// searching one function at a time.
//
// That audit is the reason to trust the set now: it lists all 44 accesses with their widths, and
// after these three the only remaining 32-bit ones are counters that are written and read at 32
// bits consistently. Patching five of eight was worse than patching none - half the pointers
// full, half truncated, and the allocator walking between them.
//
// Site 6 (0x0A1592), the second store into the array of free-list heads, in the function that
// puts a block back:
//     mov rax, qword ptr [r13+rbx*8+0x6F91A0]    <- read at 0x0A1586, 64 bits
//     mov dword ptr [r13+rbx*8+0x6F91A0], r12d   <- write, 32 bits
// Same shape as site 2, and missed for the same reason: the search that found site 2 stopped at
// one function. REX 45 -> 4D.
#define SITE6_RVA 0x0A1592
static const unsigned char kSite6Expect[]  = { 0x45, 0x89, 0xA4, 0xDD, 0xA0, 0x91, 0x6F, 0x00 };
static const unsigned char kSite6Patched[] = { 0x4D, 0x89, 0xA4, 0xDD, 0xA0, 0x91, 0x6F, 0x00 };

// Site 8 (0x0A18FE), the arena cursor published from the other end of the allocator, read back
// 64 bits wide three instructions earlier at 0x0A18F3. The instruction already carries a REX
// prefix because it uses r8d, so setting W costs nothing: 44 -> 4C.
#define SITE8_RVA 0x0A18FE
static const unsigned char kSite8Expect[]  = { 0x44, 0x89, 0x05, 0x2B, 0x7A, 0x65, 0x00 };
static const unsigned char kSite8Patched[] = { 0x4C, 0x89, 0x05, 0x2B, 0x7A, 0x65, 0x00 };

// Site 7 (0x0A18A3) is the one place where the read is narrow as well as the write:
//     mov eax, dword ptr [r8+0x10]      <- reads half of a pointer the same function stores
//     mov dword ptr [rip+0x657a83], eax    whole at 0x0A18FA, then publishes half of it
// Two instructions have to grow, so this one needs a trampoline. rbx is free here: it is loaded
// from [r8] by the very next instruction, which is where the trampoline returns to.
#define SITE7_RVA 0x0A18A3
static const unsigned char kSite7Expect[] = {
	0x41, 0x8B, 0x40, 0x10,                            // mov eax, [r8+0x10]
	0x89, 0x05, 0x83, 0x7A, 0x65, 0x00                 // mov [rip+0x657A83], eax
};

static const char* PatchListPointerWidth(unsigned char* cs)
{
	if (!g_cave)
	{
		g_cave = AllocCaveNear(cs);
		if (!g_cave) return "no space for trampolines";
	}
	g_siteHits = (unsigned long long*)(g_cave + CAVE_COUNTERS_OFF);

	unsigned char* at3 = cs + SITE3_RVA;
	unsigned char* at7 = cs + SITE7_RVA;
	unsigned char* at4 = cs + SITE4_RVA;
	unsigned char* at5 = cs + SITE5_RVA;
	if (at3[0] == 0xE9 && at4[0] == 0xE9 && at5[0] == 0xE9) return "list walk already";
	if (memcmp(at7, kSite7Expect, sizeof(kSite7Expect)) != 0 ||
	    memcmp(at3, kSite3Expect, sizeof(kSite3Expect)) != 0 ||
	    memcmp(at4, kSite4Expect, sizeof(kSite4Expect)) != 0 ||
	    memcmp(at5, kSite5Expect, sizeof(kSite5Expect)) != 0)
		return "list walk no match";

	// --- site 3: read and store the next pointer at full width ---
	unsigned char* body3 = g_cave + g_caveUsed;
	{
		unsigned char code[32];
		size_t n = 0;
		n += EmitHitCounter(code, body3, &g_siteHits[0]);
		code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0x06;              // mov rax, [rsi]
		code[n++] = 0x49; code[n++] = 0x89; code[n++] = 0x84; code[n++] = 0xED;
		code[n++] = 0xA0; code[n++] = 0x91; code[n++] = 0x6F; code[n++] = 0x00;  // mov [r13+rbp*8+..], rax
		if (!WriteBytes(body3, code, (unsigned long)n)) return "list walk write failed";
		g_caveUsed += n;
		// jump back to the instruction after the ten bytes being replaced
		if (!WriteJump(g_cave + g_caveUsed, at3 + sizeof(kSite3Expect), 5)) return "list walk far";
		g_caveUsed += 5;
	}

	// --- site 4: publish the arena pointer at full width, via r11 ---
	unsigned char* body4 = g_cave + g_caveUsed;
	{
		const ULONGLONG target = (ULONGLONG)(cs + GLOBAL_ARENA_RVA);
		unsigned char code[32];
		size_t n = 0;
		n += EmitHitCounter(code, body4, &g_siteHits[1]);
		code[n++] = 0x49; code[n++] = 0xBB;                                 // mov r11, imm64
		memcpy(code + n, &target, 8); n += 8;
		code[n++] = 0x49; code[n++] = 0x89; code[n++] = 0x0B;               // mov [r11], rcx
		if (!WriteBytes(body4, code, (unsigned long)n)) return "arena write failed";
		g_caveUsed += n;
		if (!WriteJump(g_cave + g_caveUsed, at4 + sizeof(kSite4Expect), 5)) return "arena far";
		g_caveUsed += 5;
	}

	// --- site 5: publish the arena base at full width, via r10 ---
	unsigned char* body5 = g_cave + g_caveUsed;
	{
		const ULONGLONG target = (ULONGLONG)(cs + GLOBAL_BASE_RVA);
		unsigned char code[32];
		size_t n = 0;
		n += EmitHitCounter(code, body5, &g_siteHits[2]);
		code[n++] = 0x49; code[n++] = 0xBA;                                 // mov r10, imm64
		memcpy(code + n, &target, 8); n += 8;
		code[n++] = 0x49; code[n++] = 0x89; code[n++] = 0x2A;               // mov [r10], rbp
		if (!WriteBytes(body5, code, (unsigned long)n)) return "base write failed";
		g_caveUsed += n;
		if (!WriteJump(g_cave + g_caveUsed, at5 + sizeof(kSite5Expect), 5)) return "base far";
		g_caveUsed += 5;
	}

	// --- site 7: read the block pointer and publish the cursor, both at full width ---
	unsigned char* body7 = g_cave + g_caveUsed;
	{
		const ULONGLONG target = (ULONGLONG)(cs + GLOBAL_ARENA_RVA);
		unsigned char code[48];
		size_t n = 0;
		n += EmitHitCounter(code, body7, &g_siteHits[3]);
		code[n++] = 0x49; code[n++] = 0x8B; code[n++] = 0x40; code[n++] = 0x10;  // mov rax,[r8+0x10]
		code[n++] = 0x48; code[n++] = 0xBB;                                       // mov rbx, imm64
		memcpy(code + n, &target, 8); n += 8;
		code[n++] = 0x48; code[n++] = 0x89; code[n++] = 0x03;                     // mov [rbx], rax
		if (!WriteBytes(body7, code, (unsigned long)n)) return "cursor write failed";
		g_caveUsed += n;
		if (!WriteJump(g_cave + g_caveUsed, at7 + sizeof(kSite7Expect), 5)) return "cursor far";
		g_caveUsed += 5;
	}

	if (FIX_SITE(3) && !WriteJump(at3, body3, sizeof(kSite3Expect))) return "list walk jump failed";
	if (FIX_SITE(5) && !WriteJump(at5, body5, sizeof(kSite5Expect))) return "base jump failed";
	if (FIX_SITE(7) && !WriteJump(at7, body7, sizeof(kSite7Expect))) return "cursor jump failed";

	{
		// Dump what was actually written. A trampoline that assembles wrongly looks exactly like
		// one that was never applied, and the engine dies too early to leave a crash log.
		FILE* f = fopen("launcher_trampoline.txt", "w");
		if (f)
		{
			int i;
			fprintf(f, "module     : %p\n", (void*)cs);
			fprintf(f, "cave       : %p (used %u)\n", (void*)g_cave, (unsigned)g_caveUsed);
			fprintf(f, "site3 at   : %p ->", (void*)at3);
			for (i = 0; i < 10; i++) fprintf(f, " %02X", at3[i]);
			fprintf(f, "\ncave body3 : %p ->", (void*)body3);
			for (i = 0; i < 16; i++) fprintf(f, " %02X", body3[i]);
			fprintf(f, "\nsite4 at   : %p ->", (void*)at4);
			for (i = 0; i < 6; i++) fprintf(f, " %02X", at4[i]);
			fprintf(f, "\ncave body4 : %p ->", (void*)body4);
			for (i = 0; i < 18; i++) fprintf(f, " %02X", body4[i]);
			fprintf(f, "\nreturn3 to : %p\nreturn4 to : %p\n",
			        (void*)(at3 + sizeof(kSite3Expect)), (void*)(at4 + sizeof(kSite4Expect)));
			fclose(f);
		}
	}
	if (FIX_SITE(4) && !WriteJump(at4, body4, sizeof(kSite4Expect))) return "arena jump failed";
	static char tramp[96];
	sprintf(tramp, "trampolines walk %s arena %s base %s cursor %s",
	        FIX_SITE(3) ? "on" : "off", FIX_SITE(4) ? "on" : "off",
	        FIX_SITE(5) ? "on" : "off", FIX_SITE(7) ? "on" : "off");
	return tramp;
}

// Site 2: the array of free-list heads is read 64 bits wide and written 32 bits wide.
//
//     mov rax, qword ptr [r8+r9*8+0x6F91A0]     <- read, REX.W set      (4B 8B ...)
//     mov dword ptr [r8+r9*8+0x6F91A0], r10d    <- write, REX.W clear   (47 89 ...)
//
// The index scales by 8, so the elements are pointers, and the store drops the top half of
// every one of them. Caught in the act: with the heap forced high this faults at 0x0A1720 in
// the same function, with the full pointer in rdx (0x100470AA8) and its truncated self in rax
// (0x470A90).
//
// Setting REX.W turns 47 into 4F and makes the store write the whole register. The instruction
// keeps its length, so nothing around it moves - this is the cheapest correction of the three.
#define SITE2_RVA 0x0A175D
static const unsigned char kSite2Expect[]  = { 0x47, 0x89, 0x94, 0xC8, 0xA0, 0x91, 0x6F, 0x00 };
static const unsigned char kSite2Patched[] = { 0x4F, 0x89, 0x94, 0xC8, 0xA0, 0x91, 0x6F, 0x00 };


// Sets REX.W on an instruction that already has room for it, leaving everything else alone.
static const char* WidenInPlace(unsigned char* at, const unsigned char* expect,
                                const unsigned char* patched, size_t len)
{
	if (memcmp(at, patched, len) == 0) return "already";
	if (memcmp(at, expect,  len) != 0) return "no match";
	return WriteBytes(at, patched, (unsigned long)len) ? "applied" : "failed";
}

static const char* PatchSlabPointerWidth(void)
{
	HMODULE cs = GetModuleHandleA("CrySystem.dll");
	if (!cs) return "CrySystem not loaded";

	static char result[192];
	unsigned char* base = (unsigned char*)cs;

	const char* one   = FIX_SITE(1) ? WidenInPlace(base + SITE1_RVA, kSite1Expect, kSite1Patched, sizeof(kSite1Expect)) : "off";
	const char* two   = FIX_SITE(2) ? WidenInPlace(base + SITE2_RVA, kSite2Expect, kSite2Patched, sizeof(kSite2Expect)) : "off";
	const char* six   = FIX_SITE(6) ? WidenInPlace(base + SITE6_RVA, kSite6Expect, kSite6Patched, sizeof(kSite6Expect)) : "off";
	const char* eight = FIX_SITE(8) ? WidenInPlace(base + SITE8_RVA, kSite8Expect, kSite8Patched, sizeof(kSite8Expect)) : "off";

	sprintf(result, "slab %s, free-list %s, free-list#2 %s, cursor %s, %s",
	        one, two, six, eight, PatchListPointerWidth(base));
	return result;
}

// Reserves every free region below the 4 GB line, which forces the engine's heap above it.
//
// This makes the failure above reproducible on demand instead of waiting for a machine whose
// address space happens to be laid out badly. Without the patch this reliably reproduces the
// startup box; with it, startup should be unaffected. Diagnostic use only: -forcehighheap.
// Counters for the reservation below, reported in the diagnostic file.
//
// They exist because the first version of this silently reserved 4 MB out of 4 GB and reported
// success, which made every measurement taken with -forcehighheap meaningless: the heap stayed
// exactly where it always was. A flag that quietly does nothing is worse than no flag.
static unsigned g_lowRegions = 0;      // free regions seen below the line
static unsigned g_lowTaken   = 0;      // reservations that succeeded
static unsigned g_lowFailed  = 0;      // reservations that failed
static unsigned g_lowLastErr = 0;      // GetLastError of the last failure
static size_t   g_lowLeft    = 0;      // small blocks left free on purpose

// Takes the large free blocks below the 4 GB line and leaves the small ones alone.
//
// Taking everything does not work: the engine then cannot start at all - it dies in five
// seconds without reaching the level, with or without the pointer corrections, because loading
// libraries and creating render buffers needs low memory too. That measures starvation, not
// truncation.
//
// Leaving the small holes free keeps startup working while denying the allocator a large
// contiguous block down there, so its slab has to go above the line - which is the condition
// worth testing. keepBelow is the size in bytes under which a free block is left alone.
static size_t ReserveLowAddressSpace(size_t keepBelow)
{
	const ULONG_PTR limit = (ULONG_PTR)0x100000000;
	const SIZE_T    gran  = 0x10000;          // allocation granularity: VirtualAlloc rounds to it
	size_t reserved = 0;
	ULONG_PTR a = 0x10000;
	MEMORY_BASIC_INFORMATION mbi;

	while (a < limit && VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) == sizeof(mbi))
	{
		ULONG_PTR next = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
		if (mbi.State == MEM_FREE)
		{
			g_lowRegions++;

			// A free region does not have to start on a 64 KB boundary - the piece before the
			// next boundary cannot be reserved at all, so skip forward to it.
			ULONG_PTR base = (ULONG_PTR)mbi.BaseAddress;
			ULONG_PTR aligned = (base + gran - 1) & ~(ULONG_PTR)(gran - 1);
			SIZE_T sz = (next > aligned) ? (SIZE_T)(next - aligned) : 0;
			if (aligned + sz > limit)
				sz = (aligned < limit) ? (SIZE_T)(limit - aligned) : 0;
			sz &= ~(SIZE_T)(gran - 1);        // whole granules only

			// Leave the first keepBelow bytes of free space alone, take everything after it.
			// Filtering by block size does not work here: what is free below the line is a
			// handful of large blocks and about 4 MB of scraps, so a size threshold either
			// takes all of it or none.
			if (sz)
			{
				if (g_lowLeft < keepBelow)
				{
					const SIZE_T spare = (SIZE_T)(keepBelow - g_lowLeft);
					const SIZE_T leave = (sz <= spare) ? sz : spare;
					g_lowLeft += leave;
					aligned += leave;
					sz -= leave;
				}
			}

			if (sz)
			{
				LPVOID got = VirtualAlloc((LPVOID)aligned, sz, MEM_RESERVE, PAGE_NOACCESS);
				if (got)
				{
					reserved += sz;
					g_lowTaken++;
					if (g_lowBlockCount < MAX_LOW_BLOCKS)
						g_lowBlocks[g_lowBlockCount++] = got;
					g_lowHeld = true;
				}
				else
				{
					g_lowFailed++;
					g_lowLastErr = GetLastError();
				}
			}
		}
		if (next <= a) break;
		a = next;
	}
	return reserved;
}

static const char* InstallHighSlab(SIZE_T thresholdBytes)
{
	// Two things together: the proxy, so the first large allocation can be recognised, and the
	// reservation, so that allocation has nowhere low to go. The reservation is released inside
	// the proxy as soon as that allocation returns, which is what keeps the renderer working.
	const char* proxy = InstallArenaProxy();
	if (strcmp(proxy, "installed") != 0 && strcmp(proxy, "already installed") != 0)
		return proxy;

	g_highThreshold = thresholdBytes;
	const size_t held = ReserveLowAddressSpace(0);
	if (!held) return "nothing could be reserved";

	static char msg[64];
	sprintf(msg, "holding %u MB until the arena is taken, via CrySystemCrtMalloc",
	        (unsigned)(held / (1024 * 1024)));
	return msg;
}

static void WriteDiagReport(const char* cmdLine, bool timerRaised, bool borderless,
                            const char* slabFix, size_t lowReserved, const char* allocTrace,
                            const char* highSlab)
{
	FILE* f = fopen("launcher_diag.txt", "w");
	if (!f) return;

	DiagLine(f, "=== crysis2-64bit launcher diagnostics ===");
	DiagLine(f, "launcher build : %s %s", __DATE__, __TIME__);
	DiagLine(f, "command line   : %s", (cmdLine && *cmdLine) ? cmdLine : "(none)");
	DiagLine(f, "timer 1ms      : %s", timerRaised ? "raised OK" : "FAILED (expect ~64 fps cap)");
	DiagLine(f, "borderless     : %s", borderless ? "enabled" : "disabled (-noborderless)");
	DiagLine(f, "engine fix     : %s", slabFix);
	DiagLine(f, "fix mask       : 0x%02X (sites 1-8, bit per site)", g_fixMask);
	DiagLine(f, "alloc trace    : %s", allocTrace);
	{
		size_t totalFree = 0;
		const size_t largest = LargestFreeBlockBelow4GB(&totalFree);
		DiagLine(f, "low address sp : %u MB free, largest single block %u MB",
		         (unsigned)(totalFree / (1024 * 1024)), (unsigned)(largest / (1024 * 1024)));
	}
	DiagLine(f, "high arena     : %s", g_highArenaMsg);
	DiagLine(f, "high slab      : %s (%u moved above 4 GB, %u could not be)",
	         highSlab, g_highTaken, g_highMissed);
	if (lowReserved || g_lowRegions)
	{
		DiagLine(f, "low space      : %u MB reserved, %u MB left free in small blocks "
		            "(%u regions: %u taken, %u refused, last error %u)",
		         (unsigned)(lowReserved / (1024 * 1024)),
		         (unsigned)(g_lowLeft / (1024 * 1024)),
		         g_lowRegions, g_lowTaken, g_lowFailed, g_lowLastErr);
	}
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

// Where the main thread is standing when it stops answering.
//
// The engine's watchdog reports "Runaway thread" and kills the process about twenty seconds
// after the counters flatten, which leaves nothing to look at afterwards. Taking a dump from
// outside needs administrator rights; a process can always suspend its own threads, so the
// watcher does it here: freeze the main thread, read RIP, and walk its stack for return
// addresses that land inside a loaded module. That is enough to name the function that is
// spinning.
static HANDLE g_mainThread = 0;

static void ReportMainThreadStack(const char* why)
{
	if (!g_mainThread) return;

	char buf[2048];
	int n = sprintf(buf, "=== main thread stalled: %s ===\n", why);

	if (SuspendThread(g_mainThread) == (DWORD)-1)
	{
		n += sprintf(buf + n, "  could not suspend the thread\n\n");
		AppendFaultLog(buf, (unsigned long)n);
		return;
	}

	CONTEXT ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	if (GetThreadContext(g_mainThread, &ctx))
	{
		ULONG_PTR rva = 0;
		const char* mod = ModuleAt((ULONG_PTR)ctx.Rip, &rva);
		n += sprintf(buf + n, "  rip : %s+0x%08llX\n", mod ? mod : "(unknown)",
		             (unsigned long long)rva);
		n += sprintf(buf + n, "  rsp : 0x%016llX  rbx=0x%016llX  rcx=0x%016llX  rdx=0x%016llX\n",
		             (unsigned long long)ctx.Rsp, (unsigned long long)ctx.Rbx,
		             (unsigned long long)ctx.Rcx, (unsigned long long)ctx.Rdx);

		// A frame pointer chain is not available here, so the stack is simply scanned: any value
		// that points into a loaded module is a plausible return address. False positives are
		// possible, but the repeated ones name the loop.
		const ULONG_PTR* sp = (const ULONG_PTR*)ctx.Rsp;
		int shown = 0;
		for (int i = 0; i < 256 && shown < 12; i++)
		{
			ULONG_PTR v = 0;
			if (!SafePeek(&sp[i], &v)) break;
			ULONG_PTR r = 0;
			const char* m = ModuleAt(v, &r);
			if (!m) continue;
			n += sprintf(buf + n, "  [rsp+%04X] %s+0x%08llX\n", (unsigned)(i * 8), m,
			             (unsigned long long)r);
			shown++;
		}
	}
	else
	{
		n += sprintf(buf + n, "  GetThreadContext failed\n");
	}

	ResumeThread(g_mainThread);
	n += sprintf(buf + n, "\n");
	AppendFaultLog(buf, (unsigned long)n);
}

// Reports where the allocator's own pointers actually are, and whether the corrected
// instructions are reached at all. Enabled with -sitewatch.
//
// The question it answers: with -highslab the arena came back from above the 4 GB line, and yet
// the game behaved identically with and without -enginefix. Either the corrected code never
// runs, or the allocator state being watched is not the state that moved. The counters settle
// the first, the globals settle the second.
//
// The array of free-list heads runs from 0x6F91A0 up to the globals at 0x6F9330, so 32 entries
// is well inside it.
// Makes the renderer's constant-buffer cache big enough for the sizes it is asked for.
//
// The cache is an array per shader stage and buffer type, indexed by the buffer's size in
// vectors, and the sizes are set by a switch at RVA 0x05A93A: 512 entries for type 0, 128 for
// types 1 and 2. Nothing checks that the requested size fits. The entry point at 0x036E10
// verifies only that offset + count fits the buffer, so a shader asking for a 224-vector
// buffer of type 1 reads entry 224 of a 128-entry array, takes whatever lies past its end for
// an ID3D11Buffer, and hands it to Map - which faults inside d3d11.dll reading [rdx+0xC9].
//
// Raising every array to 1024 entries costs 8 KB each, 240 KB across the whole table, and puts
// every size the engine can ask for inside the array. The instruction length does not change:
// the immediate of "mov esi, imm32" is simply a larger number.
#define CB_SIZE_T0_RVA 0x05A951      // mov esi, 0x200
#define CB_SIZE_T1_RVA 0x05A94A      // mov esi, 0x80
#define CB_SIZE_T2_RVA 0x05A958      // mov esi, 0x80
#define CB_CACHE_ENTRIES 1024

static const char* WidenOneCbArray(unsigned char* at, unsigned had)
{
	unsigned char want[5];
	want[0] = 0xBE;
	memcpy(want + 1, &had, 4);
	const unsigned entries = CB_CACHE_ENTRIES;

	unsigned char patched[5];
	patched[0] = 0xBE;
	memcpy(patched + 1, &entries, 4);

	if (memcmp(at, patched, 5) == 0) return "already";
	if (memcmp(at, want, 5) != 0) return "no match";
	return WriteBytes(at, patched, 5) ? "applied" : "failed";
}

// Runs before the renderer builds the table, so it polls for the module rather than waiting on
// a timer: the arrays are allocated during device creation, well after the DLL is mapped.
static DWORD WINAPI CbFixThread(LPVOID)
{
	unsigned char* rd = 0;
	for (int i = 0; i < 12000 && !rd; i++)
	{
		rd = (unsigned char*)GetModuleHandleA("CryRenderD3D11.dll");
		if (!rd) Sleep(5);
	}
	if (!rd) return 0;

	const char* a = WidenOneCbArray(rd + CB_SIZE_T0_RVA, 0x200);
	const char* b = WidenOneCbArray(rd + CB_SIZE_T1_RVA, 0x80);
	const char* c = WidenOneCbArray(rd + CB_SIZE_T2_RVA, 0x80);

	char line[224];
	int n = sprintf(line, "  cbfix: cache arrays -> %u entries (type0 %s, type1 %s, type2 %s)%s",
	                (unsigned)CB_CACHE_ENTRIES, a, b, c, "\n");
	AppendFaultLog(line, (unsigned long)n);
	return 0;
}

// Watches the renderer's constant-buffer table for corruption.
//
// The renderer keeps its D3D11 constant buffers in a 10 x 6 table of pointers to arrays of
// ID3D11Buffer*, built at CryRenderD3D11 RVA 0x05A8F6 and read at 0x036EFF. A run that dies
// in d3d11.dll!Map died because one entry of one of those arrays held 0x00A600A700A900A5 -
// data, not a pointer. By then the writer is long gone, so the table is checked here while
// the game runs: the first bad entry is reported with its neighbourhood, and what the
// neighbourhood contains says whose data landed there.
#define CB_TABLE_RVA 0x3433F0

// Entries per array, by type. Read out of the instruction that sets them rather than written
// down here: a hand-copied { 128, 512, 128 } (types 0 and 1 swapped, because the labels appear
// in the code in a different order than the jump table assigns them) produced an evening of
// false evidence. Reading 512 entries of a 128-entry array reports everything past its end as
// "corruption", always at index 128, and makes neighbouring blocks look like they overlap.
// Reading the number the code actually uses cannot be wrong, and it follows -cbfix for free.
static unsigned CbCount(const unsigned char* rd, unsigned type)
{
	static const unsigned kRva[6] = { CB_SIZE_T0_RVA, CB_SIZE_T1_RVA, CB_SIZE_T2_RVA, 0, 0, 0 };
	if (type >= 6 || !kRva[type]) return 0;
	const unsigned char* at = rd + kRva[type];
	if (at[0] != 0xBE) return 0;                  // not "mov esi, imm32" any more
	unsigned n = 0;
	memcpy(&n, at + 1, 4);
	return (n <= 65536) ? n : 0;
}

// Cheap first: real user-space pointers live below the canonical limit, and the corruption
// seen so far fails this test outright. VirtualQuery only confirms a suspicion.
static bool LooksLikePointer(ULONG_PTR v)
{
	if (v == 0) return true;                              // an empty slot is fine
	if (v < 0x10000) return false;
	if (v >= 0x0000800000000000ull) return false;         // not a canonical user address
	return true;
}

static void DumpAround(const unsigned char* at, int before, int after, char* buf, int* pn)
{
	int n = *pn;
	for (int row = -before; row < after; row += 16)
	{
		const unsigned char* p = at + row;
		ULONG_PTR probe = 0;
		if (!SafePeek(p, &probe)) continue;
		n += sprintf(buf + n, "    %+5d  %016llX:", row, (unsigned long long)(ULONG_PTR)p);
		for (int i = 0; i < 16; i++) n += sprintf(buf + n, " %02X", p[i]);
		n += sprintf(buf + n, "%s", "\n");
	}
	*pn = n;
}

// Thread enumeration, declared here: the minimal header set this launcher builds against has
// no tlhelp32.h, and the three entry points are resolved at run time rather than linked.
#define TH32CS_SNAPTHREAD_ 0x00000004
struct THREADENTRY32_
{
	DWORD dwSize;
	DWORD cntUsage;
	DWORD th32ThreadID;
	DWORD th32OwnerProcessID;
	LONG  tpBasePri;
	LONG  tpDeltaPri;
	DWORD dwFlags;
};
typedef HANDLE (WINAPI *PFN_Snapshot)(DWORD, DWORD);
typedef BOOL   (WINAPI *PFN_Thread32)(HANDLE, THREADENTRY32_*);

// A hardware watchpoint on one address, so the writer names itself.
//
// Everything up to here reads the damage after the fact: a table entry holds data instead of a
// pointer, and the address it sits at is an arena address with its top half cut off. That says
// what happened but not who did it, and the search space is every module that touches an object
// from the arena - including code that builds an address with 32-bit arithmetic, which no scan
// of the instruction stream picks out reliably.
//
// The processor can answer directly. DR0 holds the address, DR7 arms it for writes, and the
// next store to those eight bytes raises a single-step exception with RIP still pointing at the
// instruction that did it. Debug registers are per-thread, so every thread in the process gets
// the same setting, and threads created later are picked up by arming again.
// Off unless -cbwatch is given. Arming touches every thread in the process, and doing that
// on a timer hung the game solid: a thread suspended while it holds a lock stops everyone
// waiting on that lock. It is armed once, and only when explicitly asked for.
static bool  g_cbWatch = false;
static void* g_watchAddr = 0;
static volatile LONG g_watchHits = 0;

static unsigned g_watchThreads = 0;    // threads that actually took the setting
static unsigned g_watchSeen = 0;       // threads looked at

static void ArmWriteWatch(void* addr, bool enable)
{
	HMODULE k32 = GetModuleHandleA("kernel32.dll");
	PFN_Snapshot pSnap = (PFN_Snapshot)GetProcAddress(k32, "CreateToolhelp32Snapshot");
	PFN_Thread32 pFirst = (PFN_Thread32)GetProcAddress(k32, "Thread32First");
	PFN_Thread32 pNext  = (PFN_Thread32)GetProcAddress(k32, "Thread32Next");
	if (!pSnap || !pFirst || !pNext) return;

	const DWORD me = GetCurrentThreadId();
	HANDLE snap = pSnap(TH32CS_SNAPTHREAD_, 0);
	if (snap == INVALID_HANDLE_VALUE) return;

	THREADENTRY32_ te;
	te.dwSize = sizeof(te);
	const DWORD pid = GetCurrentProcessId();
	if (pFirst(snap, &te))
	{
		do
		{
			if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;

			HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
			                       THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
			if (!th) continue;

			if (SuspendThread(th) != (DWORD)-1)
			{
				CONTEXT ctx;
				memset(&ctx, 0, sizeof(ctx));
				ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				if (GetThreadContext(th, &ctx))
				{
					if (enable)
					{
						ctx.Dr0 = (DWORD64)(ULONG_PTR)addr;
						// L0 on; RW0 = 01 (write); LEN0 = 10 (eight bytes).
						ctx.Dr7 = (ctx.Dr7 & ~(DWORD64)0x000F0003ull) | 1ull | (1ull << 16) | (2ull << 18);
					}
					else
					{
						ctx.Dr0 = 0;
						ctx.Dr7 &= ~(DWORD64)0x000F0003ull;
					}
					ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
					g_watchSeen++;
					if (SetThreadContext(th, &ctx))
					{
						// Read it back: a debug register that silently refused to stick would
						// make the whole measurement a lie.
						CONTEXT back;
						memset(&back, 0, sizeof(back));
						back.ContextFlags = CONTEXT_DEBUG_REGISTERS;
						if (GetThreadContext(th, &back) && back.Dr0 == ctx.Dr0 && (back.Dr7 & 1) == (ctx.Dr7 & 1))
							g_watchThreads++;
					}
				}
				ResumeThread(th);
			}
			CloseHandle(th);
		} while (pNext(snap, &te));
	}
	CloseHandle(snap);
}

// Reports the instruction the watchpoint caught, then disarms so one write is enough.
static LONG CALLBACK WatchVEH(EXCEPTION_POINTERS* ep)
{
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
	CONTEXT* c = ep->ContextRecord;
	if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;

	c->Dr6 = 0;
	c->Dr7 &= ~(DWORD64)0x000F0003ull;

	if (InterlockedIncrement(&g_watchHits) <= 4)
	{
		char buf[1024];
		ULONG_PTR rva = 0;
		const char* mod = ModuleAt((ULONG_PTR)c->Rip, &rva);
		int n = sprintf(buf, "=== watchpoint hit: write to 0x%016llX ===\n"
		                     "  by            : %s+0x%08llX\n"
		                     "  thread        : %lu\n",
		                (unsigned long long)(ULONG_PTR)g_watchAddr,
		                mod ? mod : "(unknown)", (unsigned long long)rva,
		                (unsigned long)GetCurrentThreadId());
		static const char* const names[16] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
		                                       "r8 ","r9 ","r10","r11","r12","r13","r14","r15" };
		const ULONG_PTR regs[16] = { (ULONG_PTR)c->Rax, (ULONG_PTR)c->Rcx, (ULONG_PTR)c->Rdx,
		                             (ULONG_PTR)c->Rbx, (ULONG_PTR)c->Rsp, (ULONG_PTR)c->Rbp,
		                             (ULONG_PTR)c->Rsi, (ULONG_PTR)c->Rdi, (ULONG_PTR)c->R8,
		                             (ULONG_PTR)c->R9,  (ULONG_PTR)c->R10, (ULONG_PTR)c->R11,
		                             (ULONG_PTR)c->R12, (ULONG_PTR)c->R13, (ULONG_PTR)c->R14,
		                             (ULONG_PTR)c->R15 };
		for (int i = 0; i < 16; i += 4)
			n += sprintf(buf + n, "    %s=%016llX %s=%016llX %s=%016llX %s=%016llX\n",
			             names[i],   (unsigned long long)regs[i],
			             names[i+1], (unsigned long long)regs[i+1],
			             names[i+2], (unsigned long long)regs[i+2],
			             names[i+3], (unsigned long long)regs[i+3]);
		// The bytes at RIP name the instruction without needing a disassembler here.
		n += sprintf(buf + n, "  bytes at rip  :");
		for (int i = 0; i < 16; i++)
			n += sprintf(buf + n, " %02X", ((const unsigned char*)c->Rip)[i]);
		n += sprintf(buf + n, "\n  callers       :\n");
		{
			const ULONG_PTR* sp = (const ULONG_PTR*)c->Rsp;
			int shown = 0;
			for (int i = 0; i < 128 && shown < 8; i++)
			{
				ULONG_PTR v = 0;
				if (!SafePeek(&sp[i], &v)) break;
				ULONG_PTR r = 0;
				const char* m = ModuleAt(v, &r);
				if (!m) continue;
				n += sprintf(buf + n, "    [rsp+%04X] %s+0x%08llX\n", (unsigned)(i * 8), m,
				             (unsigned long long)r);
				shown++;
			}
		}
		n += sprintf(buf + n, "\n");
		AppendFaultLog(buf, (unsigned long)n);
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}

// Checks the renderer's constant-buffer arrays for overlap.
//
// Four arrays of 4096 bytes turned up 2080 bytes apart, which means the allocator handed out
// blocks that sit inside one another - and every corrupted entry was at the same offset of
// 1024 bytes. Whether that happens only when the arena is above the 4 GB line is the question
// this answers, so it runs with the flag on and off.
static void ReportCbLayout(unsigned char* rd)
{
	struct Blk { ULONG_PTR base; unsigned bytes; unsigned stage, type; };
	Blk b[60];
	int n = 0;
	ULONG_PTR* const table = (ULONG_PTR*)(rd + CB_TABLE_RVA);
	for (unsigned type = 0; type < 6; type++)
	{
		if (!CbCount(rd, type)) continue;
		for (unsigned stage = 0; stage < 10; stage++)
		{
			ULONG_PTR base = 0;
			if (!SafePeek(&table[stage + type * 10], &base) || !base) continue;
			b[n].base = base; b[n].bytes = CbCount(rd, type) * 8;
			b[n].stage = stage; b[n].type = type;
			n++;
		}
	}
	if (n < 2) return;

	for (int i = 0; i < n - 1; i++)          // simple sort, sixty entries at most
		for (int j = i + 1; j < n; j++)
			if (b[j].base < b[i].base) { Blk tmp = b[i]; b[i] = b[j]; b[j] = tmp; }

	char buf[4096];
	int m = sprintf(buf, "=== constant-buffer arrays: %d blocks ===%s", n, "\n");
	int overlaps = 0;
	for (int i = 0; i < n; i++)
	{
		const ULONG_PTR end = b[i].base + b[i].bytes;
		const char* note = "";
		if (i + 1 < n && b[i + 1].base < end) { note = "  <- OVERLAPS the next block"; overlaps++; }
		m += sprintf(buf + m, "  0x%016llX + %5u = 0x%016llX  stage %u type %u%s%s",
		             (unsigned long long)b[i].base, b[i].bytes,
		             (unsigned long long)end, b[i].stage, b[i].type, note, "\n");
	}
	m += sprintf(buf + m, "  overlapping blocks: %d%s%s", overlaps,
	             overlaps ? "  <- the allocator handed out memory twice" : "", "\n\n");
	AppendFaultLog(buf, (unsigned long)m);
}

static DWORD WINAPI CbWatchThread(LPVOID)
{
	unsigned char* rd = 0;
	for (int i = 0; i < 600 && !rd; i++)
	{
		rd = (unsigned char*)GetModuleHandleA("CryRenderD3D11.dll");
		if (!rd) Sleep(100);
	}
	if (!rd) return 0;

	char line[256];
	int n = sprintf(line, "--- cbwatch: CryRenderD3D11 at %p, table at %p ---\n",
	                (void*)rd, (void*)(rd + CB_TABLE_RVA));
	AppendTextFile("launcher_sites.txt", line, (unsigned long)n);

	// Keep watching after the first report. The entry is cleared once it is described, so
	// the renderer creates a fresh buffer and the watchpoint gets another chance at the
	// writer - the first write happens before this thread can arm anything.
	int reports = 0;
	bool laidOut = false;
	for (unsigned tick = 0; ; tick++)
	{
		ULONG_PTR* const table = (ULONG_PTR*)(rd + CB_TABLE_RVA);
		if (!laidOut)
		{
			ULONG_PTR probe = 0;
			if (SafePeek(&table[9 + 2 * 10], &probe) && probe)   // last array of the table
			{
				laidOut = true;
				ReportCbLayout(rd);
			}
		}
		for (unsigned type = 0; type < 6; type++)
		{
			const unsigned count = CbCount(rd, type);
			if (!count) continue;
			for (unsigned stage = 0; stage < 10; stage++)
			{
				ULONG_PTR base = 0;
				if (!SafePeek(&table[stage + type * 10], &base)) continue;
				if (!base || !LooksLikePointer(base)) continue;

				const ULONG_PTR* arr = (const ULONG_PTR*)base;

				// Index 128 of this particular array is what two runs in a row saw destroyed,
				// so that is where the watchpoint goes. Re-arming every second catches threads
				// the engine started after the last pass.
				if (g_cbWatch && type == 1 && stage == 0 && g_watchHits == 0 && !g_watchAddr)
				{
					void* want = (void*)&arr[128];
					if (g_watchAddr != want)
					{
						g_watchAddr = want;
						g_watchThreads = g_watchSeen = 0;
						ArmWriteWatch(want, true);
						char wl[256];
						ULONG_PTR now = 0;
						SafePeek(want, &now);
						int wn = sprintf(wl, "  watchpoint armed on 0x%016llX at tick %u: "
						                     "%u of %u threads took it, entry is now 0x%016llX%s",
						                 (unsigned long long)(ULONG_PTR)want, tick,
						                 g_watchThreads, g_watchSeen,
						                 (unsigned long long)now, "\n");
						AppendFaultLog(wl, (unsigned long)wn);
					}
				}
				for (unsigned i = 0; i < count; i++)
				{
					ULONG_PTR v = 0;
					if (!SafePeek(&arr[i], &v)) break;
					if (LooksLikePointer(v)) continue;

					// Confirm it is not a pointer into something freshly mapped before saying
					// anything: a false alarm here would cost another evening.
					MEMORY_BASIC_INFORMATION mbi;
					if (VirtualQuery((LPCVOID)v, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT)
						continue;

					// Report only. An earlier version cleared the entry to provoke a second write; with
					// the wrong array size that wrote zeroes into a neighbouring block and killed runs
					// that would otherwise have survived. A watcher must not touch what it watches.
					if (++reports > 4) break;
					char buf[4096];
					int m = sprintf(buf,
					                "=== constant-buffer table corrupted ===\n"
					                "  stage %u, type %u, index %u of %u\n"
					                "  array   : 0x%016llX (%u bytes)\n"
					                "  entry   : 0x%016llX  <- not a pointer\n"
					                "  around the entry:\n",
					                stage, type, i, count,
					                (unsigned long long)base, count * 8,
					                (unsigned long long)v);
					DumpAround((const unsigned char*)&arr[i], 64, 80, buf, &m);
					// Could this be an arena address that lost its top half? The pool covers two whole
					// gigabytes, so "the rebuilt address is inside the pool" is true of almost any low
					// address and proves nothing on its own. Only a hit inside a committed arena - the
					// first 512 KB of a 1 MB slot - is worth reporting.
					{
						const ULONG_PTR lo = (ULONG_PTR)g_arenaPool;
						const ULONG_PTR hi = lo + (ULONG_PTR)ARENA_POOL_SLOTS * ARENA_SLOT_STRIDE;
						const ULONG_PTR here = (ULONG_PTR)&arr[i];
						bool found = false;
						for (unsigned long long top = 1; top <= 16 && !found; top++)
						{
							const ULONG_PTR full = (ULONG_PTR)((top << 32) | (here & 0xFFFFFFFFull));
							if (full < lo || full >= hi) continue;
							if ((full - lo) % ARENA_SLOT_STRIDE >= BUCKET_ARENA_BYTES) continue;
							found = true;
							m += sprintf(buf + m, "  0x%016llX is 0x%016llX inside a committed arena%s",
							             (unsigned long long)here, (unsigned long long)full, "\n");
						}
					}

					// Every module carries its own copy of the engine's small-object allocator, and
					// each copy writes its free-list heads 32 bits wide. Which copies are actually
					// in use is a question only the running game answers - the renderer's copy,
					// for one, turned out to be dead. Report them all.
					m += sprintf(buf + m, "  free-list heads by module:%s", "\n");
					{
						static const char* const kMod[] = {
							"CrySystem.dll", "CrySoundSystem.dll", "CryRenderD3D11.dll",
							"Cry3DEngine.dll", "CryPhysics.dll", "CryGameReal.dll",
						};
						static const unsigned kRva[] = {
							0x6F91A0, 0x0CADD0, 0x3A3F40, 0x2C1AF8, 0x228618, 0xA32D00,
						};
						for (int k = 0; k < 6; k++)
						{
							unsigned char* mb = (unsigned char*)GetModuleHandleA(kMod[k]);
							if (!mb) { m += sprintf(buf + m, "    %-20s not loaded%s", kMod[k], "\n"); continue; }
							const ULONG_PTR* h = (const ULONG_PTR*)(mb + kRva[k]);
							unsigned used = 0, high = 0, bad = 0;
							ULONG_PTR first = 0;
							for (unsigned q = 0; q < 32; q++)
							{
								ULONG_PTR val = 0;
								if (!SafePeek(&h[q], &val) || !val) continue;
								used++;
								if (!first) first = val;
								if (val >= 0x100000000ull) high++;
								if (!LooksLikePointer(val)) bad++;
							}
							m += sprintf(buf + m,
							             "    %-20s %2u used, %2u above 4 GB, %u malformed, first 0x%016llX%s",
							             kMod[k], used, high, bad, (unsigned long long)first, "\n");
						}
					}
					m += sprintf(buf + m, "  start of the array:%s", "\n");
					DumpAround((const unsigned char*)arr, 32, 32, buf, &m);
					m += sprintf(buf + m, "%s", "\n");
					AppendFaultLog(buf, (unsigned long)m);
					break;
				}
			}
		}
		Sleep(100);
	}
	return 0;
}

static DWORD WINAPI SiteWatchThread(LPVOID)
{
	unsigned char* cs = (unsigned char*)GetModuleHandleA("CrySystem.dll");
	if (!cs) return 0;

	char line[512];
	int n = sprintf(line, "--- sitewatch: CrySystem at %p, counters %s ---\n",
	                (void*)cs, g_siteHits ? "armed" : "unavailable (no -enginefix)");
	AppendTextFile("launcher_sites.txt", line, (unsigned long)n);

	unsigned long long prev[7];
	memset(prev, 0xFF, sizeof(prev));

	for (unsigned tick = 0; ; tick++)
	{
		unsigned long long cur[7];
		cur[0] = *(unsigned long long*)(cs + 0x6F9338);
		cur[1] = *(unsigned long long*)(cs + GLOBAL_ARENA_RVA);
		cur[2] = *(unsigned long long*)(cs + GLOBAL_BASE_RVA);
		cur[3] = g_siteHits ? g_siteHits[0] : 0;
		cur[4] = g_siteHits ? g_siteHits[1] : 0;
		cur[5] = g_siteHits ? g_siteHits[2] : 0;
		cur[6] = g_siteHits ? g_siteHits[3] : 0;

		const unsigned long long* heads = (const unsigned long long*)(cs + 0x6F91A0);
		unsigned used = 0, above = 0;
		for (unsigned i = 0; i < 32; i++)
		{
			const unsigned long long h = heads[i];
			if (!h) continue;
			used++;
			if (h >= 0x100000000ull) above++;
		}

		// A stall is easier to see than a hang: this counter normally climbs by tens of
		// thousands per second, so a few dozen means the thread has effectively stopped.
		if (g_siteHits && tick > 8)
		{
			static unsigned long long lastWalk = 0;
			static int stalledTicks = 0;
			static bool reported = false;
			const unsigned long long walk = cur[3];
			if (walk - lastWalk < 1000) stalledTicks++; else stalledTicks = 0;
			lastWalk = walk;
			if (stalledTicks >= 2 && !reported)
			{
				reported = true;
				ReportMainThreadStack("list-walk counter stopped climbing");
			}
		}

		if (memcmp(cur, prev, sizeof(cur)) != 0)
		{
			memcpy(prev, cur, sizeof(cur));
			n = sprintf(line,
			            "[%4us] slab=0x%llX arena=0x%llX base=0x%llX"
			            "  hits: walk=%llu arena=%llu base=%llu cursor=%llu"
			            "  heads: %u used, %u above 4 GB\n",
			            tick * 2, cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6],
			            used, above);
			AppendTextFile("launcher_sites.txt", line, (unsigned long)n);
		}
		Sleep(2000);
	}
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
	if (g_topDown) CensusLine("before CrySystem");
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

	// CrySystem writes allocator pointers 32 bits wide in eight places while every read of
	// them is 64-bit, so the top half is dropped (see PatchSlabPointerWidth). On this build the
	// heap normally stays below the 4 GB line - the process is built against msvcr90 - and the
	// bug lies dormant; on a machine whose address space is laid out differently it does not.
	//
	// The corrections are applied by default now that they have been checked both ways. Each one
	// only widens a store that was already meant to be 64-bit, and nothing in CrySystem reads
	// the four bytes above any of these globals as a variable of its own (verified for every
	// site), so where the heap is low the patched code writes the same value with zeroes above
	// it and behaves identically. -noenginefix leaves the engine untouched for comparison, and
	// -enginefix:<mask> still selects individual sites.
	{
		const char* arg = lpCmdLine ? strstr(lpCmdLine, "-enginefix") : 0;
		if (arg && (arg[10] == ':' || arg[10] == '='))
		{
			unsigned v = 0;
			if (sscanf(arg + 11, "%x", &v) == 1 && v) g_fixMask = v;
		}
	}
	const char* slabFix = (lpCmdLine && strstr(lpCmdLine, "-noenginefix"))
	                    ? "off (engine untouched)"
	                    : PatchSlabPointerWidth();

	// The gate below reads this: without CrySystem's own eight sites, nothing else matters.
	// A partial result reads as a failure here on purpose: "applied" somewhere in the line is
	// not the same as every site applied, and the gate errs toward staying low.
	g_engineFixFailed = strstr(slabFix, "off") || strstr(slabFix, "not ") ||
	                    strstr(slabFix, "failed") || strstr(slabFix, "no match") ||
	                    !strstr(slabFix, "applied");

	// Diagnostic: watch what the engine asks the allocator for, and what it gets back.
	const char* allocTrace = (lpCmdLine && strstr(lpCmdLine, "-traceallocs"))
	                       ? InstallAllocProxy() : "off";

	// Diagnostic: force the heap above the 4 GB line to reproduce the failure on demand.
	// -highslab[:N] - make the bucket allocator build its arena above the 4 GB line, which is
	// the one honest test of the pointer corrections: only the allocator's own memory moves
	// up, so the renderer is unaffected.
	//
	// With no number it waits for an allocation of exactly the arena size and squeezes that
	// one. N (in KB) turns it back into a threshold, which catches unrelated blocks too.
	const char* highSlab = "off";
	{
		const char* arg = lpCmdLine ? strstr(lpCmdLine, "-highslab") : 0;
		if (arg)
		{
			// The arena is 0x80000 bytes and nothing else that size goes through this
			// door, so an exact match is a far better filter than any threshold.
			size_t kb = BUCKET_ARENA_BYTES / 1024;
			const char* sep = arg + 9;
			if (*sep == ':' || *sep == '=')
			{
				const int v = atoi(sep + 1);
				if (v > 0 && v < 65536) { kb = (size_t)v; g_arenaExact = false; }
			}
			highSlab = InstallHighSlab((SIZE_T)kb * 1024);
		}
	}

	// -arenahigh - every arena the bucket allocator builds is served from above the 4 GB
	// line, for as long as the game runs. This is the test -highslab could not be: it holds
	// the condition for the whole session instead of a single allocation, and it takes no low
	// memory away from the renderer.
	if (lpCmdLine && strstr(lpCmdLine, "-arenahigh"))
		g_highArenaMsg = InstallHighArena();

	// -forcehighheap[:N] - N is how many MB of low address space to leave free. Everything
	// above that is reserved, so large allocations have to go above the 4 GB line while the
	// engine still has room to load libraries and build its render buffers.
	size_t lowReserved = 0;
	{
		const char* arg = lpCmdLine ? strstr(lpCmdLine, "-forcehighheap") : 0;
		if (arg)
		{
			size_t keepMB = 512;
			const char* colon = arg + 14;
			if (*colon == ':' || *colon == '=')
			{
				const int v = atoi(colon + 1);
				if (v > 0 && v < 4096) keepMB = (size_t)v;
			}
			lowReserved = ReserveLowAddressSpace(keepMB * 1024 * 1024);
		}
	}

	SetCwdToGameRoot();

	// Diagnostic: report what the cutscene workarounds are dropping, if anything.
	if (lpCmdLine && strstr(lpCmdLine, "-moviestats"))
	{
		DWORD tid = 0;
		HANDLE th = CreateThread(NULL, 0, MovieStatsThread, NULL, 0, &tid);
		if (th) CloseHandle(th);
	}

	// Keep a handle to this thread: it is the one the engine's watchdog watches, and the one
	// whose stack is worth reading when everything stops.
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
	                GetCurrentProcess(), &g_mainThread, 0, FALSE, DUPLICATE_SAME_ACCESS);

	// Diagnostic: follow the allocator globals and the trampoline counters while the game runs.
	// Separate this run from the previous ones in the fault log, and keep the engine log.
	StartFaultSession(lpCmdLine);

	// -dumptest writes one dump immediately, to prove the mechanism works on this machine
	// rather than finding out it does not at the moment a crash finally happens.
	if (lpCmdLine && strstr(lpCmdLine, "-dumptest")) WriteRichDump(NULL);

	// Guard the AI and the sound engine against calling into a destroyed object. On by default
	// now that a play session has measured them: 16706 and 83861 calls in five minutes, none of
	// them turned away, so they cost a walk over a short list and touch nothing that is healthy.
	// -noaifix and -nosndfix turn them off.
	if (!lpCmdLine || !strstr(lpCmdLine, "-noaifix"))
	{
		DWORD tid = 0;
		HANDLE th = CreateThread(NULL, 0, AiFixThread, NULL, 0, &tid);
		if (th) CloseHandle(th);
	}

	if (!lpCmdLine || !strstr(lpCmdLine, "-nosndfix"))
	{
		DWORD tid = 0;
		HANDLE th = CreateThread(NULL, 0, SoundFixThread, NULL, 0, &tid);
		if (th) CloseHandle(th);
	}

	CheckGameBuild();

	{
		const char* am = lpCmdLine ? strstr(lpCmdLine, "-arenamax:") : 0;
		if (am) g_arenaMax = atoi(am + 10);
		const char* an = lpCmdLine ? strstr(lpCmdLine, "-arenamin:") : 0;
		if (an) g_arenaMin = atoi(an + 10);
		const char* aw = lpCmdLine ? strstr(lpCmdLine, "-arenawatch:") : 0;
		if (aw) g_arenaWatch = atoi(aw + 12);
		const char* pf = lpCmdLine ? strstr(lpCmdLine, "-peekfind:") : 0;
		if (pf)
		{
			pf += 10;
			int k = 0;
			while (pf[k] && pf[k] != ' ' && pf[k] != 124 && k < 63) { g_peekFind[k] = pf[k]; k++; }
			g_peekFind[k] = 0;
		}
		const char* aa = lpCmdLine ? strstr(lpCmdLine, "-arenaat:") : 0;
		if (aa) g_arenaAtMb = (unsigned long long)_atoi64(aa + 9);

		if (g_arenaWatch)
		{
			DWORD tid = 0;
			HANDLE th = CreateThread(NULL, 0, PeekThread, NULL, 0, &tid);
			if (th) CloseHandle(th);
		}
	}

	if (lpCmdLine && strstr(lpCmdLine, "-pakinfo")) g_pakInfo = true;
	if (lpCmdLine && strstr(lpCmdLine, "-movdump")) g_movDump = true;

	if (lpCmdLine && strstr(lpCmdLine, "-bp:"))
	{
		const char* at = lpCmdLine;
		while ((at = strstr(at, "-bp:")) != 0 && g_bpCount < BP_SLOTS)
		{
			at += 4;
			g_bpAt[g_bpCount++] = (ULONG_PTR)strtoul(at, 0, 16);
			while (*at && *at != ' ') at++;
		}
		const char* bm = strstr(lpCmdLine, "-bpmod:");
		if (bm)
		{
			bm += 7;
			int k = 0;
			while (bm[k] && bm[k] != ' ' && k < 39) { g_bpModule[k] = bm[k]; k++; }
			g_bpModule[k] = 0;
		}

		AddVectoredExceptionHandler(1, BreakpointVeh);
	}
	if (lpCmdLine && strstr(lpCmdLine, "-readtest")) RunReadTest();

	// -say:COMMAND[,SECONDS]
	if (lpCmdLine && strstr(lpCmdLine, "-norndfix")) g_rndFixOff = true;

	if (lpCmdLine && strstr(lpCmdLine, "-sayonce")) g_sayMax = 1;

	if (lpCmdLine && strstr(lpCmdLine, "-sayevery:"))
	{
		const int v = atoi(strstr(lpCmdLine, "-sayevery:") + 10);
		if (v > 0) g_sayEvery = (DWORD)v;
	}

	if (lpCmdLine && strstr(lpCmdLine, "-sayafter:"))
		g_sayAfter = (DWORD)atoi(strstr(lpCmdLine, "-sayafter:") + 10);

	if (lpCmdLine && strstr(lpCmdLine, "-say:"))
	{
		const char* s = strstr(lpCmdLine, "-say:") + 5;
		int n = 0;
		while (s[n] && n < 399) n++;
		memcpy(g_sayWhat, s, n);
		g_sayWhat[n] = 0;

		// Trailing flags are not part of the command.
		for (int i = 0; i < n; i++)
			if (g_sayWhat[i] == 124) { g_sayWhat[i] = 0; break; }

		// A command line cannot carry spaces through the scripts that drive these runs, so a
		// tilde stands in for one. Lua needs spaces between keywords, and asking the game a real
		// question - how many entities did you create, and of what class - takes a whole
		// statement, not a word.
		for (int i = 0; g_sayWhat[i]; i++)
			if (g_sayWhat[i] == 126) g_sayWhat[i] = 32;
	}

	if (lpCmdLine && strstr(lpCmdLine, "-trace"))
	{
		g_trace = true;
		DWORD tid = 0;
		HANDLE th = CreateThread(NULL, 0, CameraTraceThread, NULL, 0, &tid);
		if (th) CloseHandle(th);
	}

	// The hooks themselves cost nothing while steering is off - one trampoline, one branch per
	// allocation - and they have to be in before the engine starts asking for memory, because a
	// hook installed halfway through cannot move what is already placed.
	// lowguard is off until high memory is safe to switch on unattended.
	//
	// It was on by default, and that was wrong. High memory does not only risk a crash - it
	// quietly breaks the game: a cutscene takes the player's body and never gives it back,
	// leaving them able to look around and nothing else, on a level that cannot be finished.
	// A guard that turns that on by itself, on someone else's machine, without being asked, is
	// worse than the ceiling it was meant to avoid. It stays off until cutscenes survive.
	if (lpCmdLine && strstr(lpCmdLine, "-lowguard"))
	{
		g_lowGuardOn = true;
		const char* lg = strstr(lpCmdLine ? lpCmdLine : "", "-lowguard:");
		if (lg)
		{
			const unsigned mb = (unsigned)atoi(lg + 10);
			if (mb) g_lowGuardMb = (SIZE_T)mb;
		}

		const char* deep = HookNtAlloc();
		const char* deepEx = HookNtAllocEx();
		PrepareAttribution();
		StartRangeKeeper();

		char line[200];
		int n = sprintf(line, "  lowguard: watching, ntdll %s, ex %s, threshold %u MB%s",
		                deep, deepEx, (unsigned)g_lowGuardMb, "\n");
		AppendFaultLog(line, (unsigned long)n);
	}
	else
		g_lowGuardOn = false;

	// -memstress:GB asks the engine's own allocator for that many gigabytes once a level is up,
	// writes every page and reads it back. The exam the rest of this was built for.
	if (lpCmdLine && strstr(lpCmdLine, "-memstress"))
	{
		const char* ms = strstr(lpCmdLine, "-memstress:");
		g_stressGb = ms ? (unsigned)atoi(ms + 11) : 2;
		if (!g_stressGb || g_stressGb > 16) g_stressGb = 2;
		StartRangeKeeper();
	}

	// -topdown[:KB] steers large reservations to the top of the address space, where a lost upper
	// half is fatal instead of harmless. The exam for "is this really a 64-bit build".
	if (lpCmdLine && strstr(lpCmdLine, "-topdown"))
	{
		const char* at = strstr(lpCmdLine, "-topdown:");
		if (at)
		{
			const unsigned kb = (unsigned)atoi(at + 9);
			if (kb) g_topDownMin = (SIZE_T)kb * 1024;
		}
		g_topDown = true;
		if (strstr(lpCmdLine, "-topdown:max")) g_topDownMax = true;

		if (strstr(lpCmdLine, "-topmap")) g_topMap = true;
		if (strstr(lpCmdLine, "-memdebug")) g_memDebug = true;
		ParseKeepLow(lpCmdLine);
		if (strstr(lpCmdLine, "-highaction")) g_highAction = true;
		if (strstr(lpCmdLine, "-gamelow"))    g_gameLow    = true;
		{
			const char* hb = strstr(lpCmdLine, "-highbase:");
			if (hb)
			{
				const unsigned gb = (unsigned)atoi(hb + 10);
				if (gb)
				{
					g_highBase    = (LONGLONG)gb * 1024 * 1024 * 1024;
					g_highCursor  = g_highBase;
					g_highCeiling = g_highBase + 0x4000000000LL;
				}
			}
		}
		{
			const char* kb = strstr(lpCmdLine, "-keepband:");
			if (kb)
			{
				unsigned lo = 0, hi = 0;
				if (sscanf(kb + 10, "%u-%u", &lo, &hi) == 2 && hi >= lo)
				{
					g_bandLoKb = lo * 1024;
					g_bandHiKb = hi * 1024;
				}
				else if (strncmp(kb + 10, "off", 3) == 0)
				{
					g_bandLoKb = g_bandHiKb = 0;   // steer the arenas too, for investigating
				}
			}
		}
		PrepareAttribution();

		// -heaphigh[:KB]: large heap blocks out of the process heap entirely. Where the memory
		// below 4 GB actually is, measured rather than guessed.
		if (strstr(lpCmdLine, "-heaphigh"))
		{
			const char* hh = strstr(lpCmdLine, "-heaphigh:");
			if (hh)
			{
				const unsigned kb = (unsigned)atoi(hh + 10);
				if (kb) g_heapHighMin = (SIZE_T)kb * 1024;
			}

			HMODULE k = GetModuleHandleA("kernel32.dll");
			if (k)
			{
				g_origHeapAlloc   = (PFN_HeapAlloc)GetProcAddress(k, "HeapAlloc");
				g_origHeapFree    = (PFN_HeapFree)GetProcAddress(k, "HeapFree");
				g_origHeapReAlloc = (PFN_HeapReAlloc)GetProcAddress(k, "HeapReAlloc");
				g_origHeapSize    = (PFN_HeapSize)GetProcAddress(k, "HeapSize");
				g_heapHigh = g_origHeapAlloc && g_origHeapFree &&
				             g_origHeapReAlloc && g_origHeapSize;
			}

			char hl[140];
			int hn = sprintf(hl, "  heaphigh: %s, blocks of %u KB and up%s",
			                 g_heapHigh ? "on" : "could not resolve kernel32",
			                 (unsigned)(g_heapHighMin / 1024), "\n");
			AppendFaultLog(hl, (unsigned long)hn);
		}

		// -shadow[:MB] - see ReserveShadowStrip. Claimed before the hooks go in, while the low
		// address space is still empty.
		if (strstr(lpCmdLine, "-shadow"))
		{
			const char* sh = strstr(lpCmdLine, "-shadow:");
			if (sh)
			{
				const unsigned mb = (unsigned)atoi(sh + 8);
				if (mb >= 32 && mb <= 1024) g_shadowMb = (SIZE_T)mb;
			}
			const char* am0 = strstr(lpCmdLine, "-arenamax:");
			if (am0) g_arenaMax = atoi(am0 + 10);
			const char* hw = strstr(lpCmdLine, "-hunt:");
			if (hw)
			{
				const unsigned by = (unsigned)atoi(hw + 6);
				if (by >= 8 && by <= 16 * 1024 * 1024) g_huntWindow = (SIZE_T)by;
			}
			g_shadowOn = true;
			if (strstr(lpCmdLine, "-shadowdry")) g_shadowDry = true;
			ReserveShadowStrip();

			char sl[200];
			int sn = g_shadowBase
			       ? sprintf(sl, "  shadow: trap armed%s, %u MB at 0x%llX-0x%llX%s",
			                 g_shadowDry ? " (CONTROL RUN, arenas stay low)" : "",
			                 (unsigned)g_shadowMb, (unsigned long long)g_shadowBase,
			                 (unsigned long long)g_shadowEnd, "\n")
			       : sprintf(sl, "  shadow: could not reserve a strip below 2 GB%s", "\n");
			AppendFaultLog(sl, (unsigned long)sn);
		}

		g_lowGuardOn = false;                 // asked for explicitly, no need to wait for a wall

		const int hooked = HookTopDownEverywhere();
		const char* deep = HookNtAlloc();
		const char* maps = HookNtMapView();
		const char* deepEx = HookNtAllocEx();
		char line[224];
		int n = sprintf(line, "  topdown: on, %d import(s) redirected, ntdll %s, ex %s, "
		                "views %s%s, threshold %u KB%s",
		                hooked, deep, deepEx, maps, g_topMap ? " (steered)" : " (counted)",
		                (unsigned)(g_topDownMin / 1024), "\n");
		AppendFaultLog(line, (unsigned long)n);
		CensusLine("hooks in");
		StartRangeKeeper();   // it re-hooks modules as they map
	}

	// The same widening in the modules that carry their own copy of the allocator. On by default:
	// these are real bugs, not an experiment, and the correction costs nothing where memory is low
	// anyway - the upper half it now writes is zero there. Measured over a full campaign run.
	// -nomodfix turns it off.
	if (!lpCmdLine || !strstr(lpCmdLine, "-nomodfix"))
	{
		DWORD tid = 0;
		HANDLE th = CreateThread(NULL, 0, ModuleFixThread, NULL, 0, &tid);
		if (th) CloseHandle(th);
	}

	if (lpCmdLine && strstr(lpCmdLine, "-cbwatch")) g_cbWatch = true;

	// Enlarge the renderer's constant-buffer cache before it is built.
	//
	// On by default, because the crash it prevents belongs to the stock 64-bit renderer and not
	// to anything this launcher does: a control run with no launcher flags at all died after 55
	// seconds at CryRenderD3D11+0x036F0E, reading entry 224 of a 128-entry array. -nocbfix turns
	// it off for comparison.
	if (!lpCmdLine || !strstr(lpCmdLine, "-nocbfix"))
	{
		DWORD tid = 0;
		HANDLE th = CreateThread(NULL, 0, CbFixThread, NULL, 0, &tid);
		if (th) CloseHandle(th);
	}
	if (lpCmdLine && strstr(lpCmdLine, "-sitewatch"))
	{
		DWORD tid = 0;
		HANDLE th = CreateThread(NULL, 0, SiteWatchThread, NULL, 0, &tid);
		if (th) CloseHandle(th);

		// And the renderer's own table, which is where the surviving crash lands.
		th = CreateThread(NULL, 0, CbWatchThread, NULL, 0, &tid);
		if (th) CloseHandle(th);
	}

#ifndef NO_DETECTOR
	// Watch for pointers that lost their top half. Observes only; see TruncationVEH.
	AddVectoredExceptionHandler(1, TruncationVEH);
	// And the hardware watchpoint, which reports the writer rather than the damage.
	AddVectoredExceptionHandler(1, WatchVEH);
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
	WriteDiagReport(lpCmdLine, timerRaised, wantBorderless, slabFix, lowReserved, allocTrace,
	                highSlab);
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
			// The module's real bounds, read from its own header. The bounds used to be written
			// in as 0x34000000..0x34082000, which is where CryMovie happens to land today; a
			// rebased module would have made every sequence fail the check.
			unsigned long long mbLo = (unsigned long long)mb;
			unsigned long long mbHi = mbLo + 0x82000;
			{
				const long lfanew = *(const long*)(mb + 0x3C);
				if (lfanew > 0 && lfanew < 0x1000)
				{
					// IMAGE_NT_HEADERS64: signature 4 + file header 20, SizeOfImage at +0x38 of
					// the optional header.
					const unsigned long soi = *(const unsigned long*)(mb + lfanew + 24 + 0x38);
					if (soi > 0x1000 && soi < 0x10000000) mbHi = mbLo + soi;
				}
			}

			unsigned long long retNormal = (unsigned long long)(mb + 0xF25C);
			unsigned long long retSkip   = (unsigned long long)(mb + 0xF2EC);
			unsigned char* cave = (unsigned char*)VirtualAlloc(NULL, 512, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
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
				// guard1: is rcx a plausible pointer?
				//
				// This check used to read "high half non-zero means garbage, since all process
				// memory sits below 4 GB". That was true of the game it was written against and
				// false of the one we are building: with the arenas steered high, a sequence
				// pointer of 0x2_1CF88110 is perfectly valid. The guard threw every one of them
				// away, took the SKIP path past the clock update, and the cutscene stood still
				// forever with nothing logged - the symptom this whole hunt was chasing.
				//
				// What actually makes a pointer implausible is being null, being tiny, or being
				// outside the 47-bit range user-mode addresses live in. Check that instead.
				cave[i++]=0x48; cave[i++]=0x81; cave[i++]=0xF9; cave[i++]=0x00; cave[i++]=0x00; cave[i++]=0x01; cave[i++]=0x00; // cmp rcx,0x10000
				int j_lo = i; cave[i++]=0x72; cave[i++]=0x00;                                       // jb SKIP (null or small)
				cave[i++]=0x49; cave[i++]=0xBB; *(unsigned long long*)(cave+i)=0x00007FFFFFFFFFFFull; i+=8; // mov r11,user-mode top
				cave[i++]=0x4C; cave[i++]=0x39; cave[i++]=0xD9;                                     // cmp rcx,r11
				int j_hi = i; cave[i++]=0x77; cave[i++]=0x00;                                       // ja SKIP (not an address)
				cave[i++]=0x48; cave[i++]=0x8B; cave[i++]=0x01;                                     // mov rax,[rcx] (vtable), now safe
				// guard2: the vtable must lie within CryMovie's own bounds, since the nodes are
				// defined there - compared full-width against the module's real base and size.
				cave[i++]=0x49; cave[i++]=0xBB; *(unsigned long long*)(cave+i)=mbLo; i+=8;          // mov r11,CryMovie base
				cave[i++]=0x4C; cave[i++]=0x39; cave[i++]=0xD8;                                     // cmp rax,r11
				int j_vlo = i; cave[i++]=0x72; cave[i++]=0x00;                                      // jb SKIP (vtable < CryMovie)
				cave[i++]=0x49; cave[i++]=0xBB; *(unsigned long long*)(cave+i)=mbHi; i+=8;          // mov r11,CryMovie end
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
