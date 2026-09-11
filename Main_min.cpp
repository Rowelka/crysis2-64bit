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

// Replaces every import that currently points at 'from'. Matching by address rather than by name
// catches the api-ms-win-core-memory forwarders as well, which is what most of these DLLs import.
static int RedirectImportsByAddress(HMODULE mod, void* from, void* to)
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
	return done;
}

static bool g_topDown = false;

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
static volatile LONGLONG  g_highCursor  = 0x200000000LL;      // 8 GB, and climbing

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
static const char* const kNotReadyYet[] = {
	"CryScriptSystem.dll",
	"dsound.dll",
	"dsound",
};

// Whether this allocation is being made on behalf of a module that is not ready. Reading the
// stack costs something, but only large reservations get here - a few hundred over a whole run.
static bool CalledByUnreadyModule(void)
{
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
			if (_stricmp(name, kNotReadyYet[k]) == 0) return true;
	}
	return false;
}

static volatile LONG g_highSkipped = 0;   // allocations left low on purpose

static LONG TryHighAt(PFN_NtAllocVM orig, HANDLE proc, PVOID* base, SIZE_T* size,
                      ULONG type, ULONG protect)
{
	const SIZE_T want = *size;

	for (int attempt = 0; attempt < 24; attempt++)
	{
		const LONGLONG at = InterlockedExchangeAdd64(&g_highCursor, 0x4000000LL);  // 64 MB apart
		if (at > 0x4000000000LL) return -1;                                        // past 256 GB

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

static LONG __stdcall SteeredNtAlloc(HANDLE proc, PVOID* base, ULONG_PTR zeroBits,
                                     SIZE_T* size, ULONG type, ULONG protect)
{
	const bool steer = base && (*base == NULL) && size && (*size >= g_topDownMin) &&
	                   ((type & MEM_RESERVE) != 0) && (proc == (HANDLE)(LONG_PTR)-1);

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
			InterlockedIncrement(&g_topDownCalls);
			if (a > 0xFFFFFFFFull) InterlockedIncrement(&g_topDownHigh);
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
	unsigned elapsed = 0, nextReport = 300;
	for (int i = 0; ; i++)
	{
		const unsigned step = (i < 60) ? 1 : 5;
		Sleep(step * 1000);
		elapsed += step;

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
				                " top 0x%llX%s", g_topDownHigh, g_topDownCalls,
				                g_topDownHighest, "\n");
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

		if (g_topDown)
		{
			int m = sprintf(line, "  topdown: %ld steered, %ld above 4 GB, 0x%llX..0x%llX%s",
			                g_topDownCalls, g_topDownHigh,
			                (g_topDownLowest == ~0ull) ? 0 : g_topDownLowest,
			                g_topDownHighest, "\n");
			AppendFaultLog(line, (unsigned long)m);
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
		return WriteBytes(at, &rex, 1) ? 0 : "write failed";
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

// Which modules carry a copy, and where.
static const struct { const char* name; const WidenSite* sites; unsigned count; } kWidenWork[] = {
	{ "CrySoundSystem.dll", kSndSites, 8 },
	{ "CryRenderD3D11.dll", kR11Sites, 8 },
	{ "CryRenderD3D9.dll",  kR9Sites,  8 },
};

static volatile LONG g_widened[3] = { 0, 0, 0 };

// Widens one module if it is loaded and has not been done yet. Safe to call from anywhere.
static void WidenIfNeeded(int i)
{
	if (i < 0 || i > 2) return;
	if (InterlockedCompareExchange(&g_widened[i], 1, 0) != 0) return;
	if (!GetModuleHandleA(kWidenWork[i].name)) { g_widened[i] = 0; return; }

	const char* r = WidenModule(kWidenWork[i].name, kWidenWork[i].sites, kWidenWork[i].count);
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

// Pushes the engine's own large allocations above the 4 GB line.
//
// This replaces -forcehighheap as the way to test the pointer corrections. Reserving all the low
// address space does move the heap up, but it also takes that space away from the renderer,
// which maps its resources there - the game then stops during "Init textures management" whether
// the pointers are corrected or not, so it measures nothing.
//
// Here only CrySystem's own VirtualAlloc calls are intercepted, and only the large ones: the
// allocator's slab lands above the line while everything else, the renderer included, keeps
// getting memory where it always did. If the engine runs with its slab up there, the truncation
// really is gone rather than merely dormant.
// Redirects one imported function of a loaded module to a replacement, returning the original.
static bool HookImport(HMODULE mod, const char* dll, const char* func, void* repl, void** orig)
{
	unsigned char* base = (unsigned char*)mod;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

	const IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir->VirtualAddress) return false;

	const IMAGE_IMPORT_DESCRIPTOR* imp = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
	for (; imp->Name; imp++)
	{
		const char* name = (const char*)(base + imp->Name);
		if (_stricmp(name, dll) != 0) continue;

		// OriginalFirstThunk keeps the names, FirstThunk the addresses the code actually calls.
		const IMAGE_THUNK_DATA64* names =
			(const IMAGE_THUNK_DATA64*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
			                                                            : imp->FirstThunk));
		IMAGE_THUNK_DATA64* addrs = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);

		for (; names->u1.AddressOfData; names++, addrs++)
		{
			if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
			const IMAGE_IMPORT_BY_NAME* byName =
				(const IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
			if (strcmp((const char*)byName->Name, func) != 0) continue;

			DWORD old = 0;
			if (!VirtualProtect(&addrs->u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &old))
				return false;
			if (orig) *orig = (void*)(ULONG_PTR)addrs->u1.Function;
			addrs->u1.Function = (ULONGLONG)(ULONG_PTR)repl;
			VirtualProtect(&addrs->u1.Function, sizeof(ULONGLONG), old, &old);
			return true;
		}
	}
	return false;
}


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

		const int hooked = HookTopDownEverywhere();
		const char* deep = HookNtAlloc();
		char line[160];
		int n = sprintf(line, "  topdown: on, %d import(s) redirected, ntdll %s, threshold %u KB%s",
		                hooked, deep, (unsigned)(g_topDownMin / 1024), "\n");
		AppendFaultLog(line, (unsigned long)n);
		StartRangeKeeper();   // it re-hooks modules as they map
	}

	// The same widening in the modules that carry their own copy of the allocator. Behind a flag
	// until it has been run with: CrySystem's copy took a week to get right.
	if (lpCmdLine && strstr(lpCmdLine, "-modfix"))
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
