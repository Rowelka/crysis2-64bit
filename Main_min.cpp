// A 64-bit game launcher for Crysis 2, booting the x64 engine DLLs from the Mod SDK.
//
// It is deliberately STL-free and built with the VC90 compiler from WDK 7.1. That is not
// legacy baggage: retail CrySystem contains a bucket allocator that stores slab addresses
// truncated to 32 bits, so in a 64-bit process whose heap sits above the 4 GB line those
// pointers get corrupted. Building against msvcr90 as the primary CRT (/MD) puts the heap
// low, exactly where it lands in the editor, and the truncation becomes harmless.
// CrySystem is imported statically so it loads alongside msvcr90 in the right order.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "cry_min.h"

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
static LONG CALLBACK MovieVEH(EXCEPTION_POINTERS* ep)
{
	if (ep && ep->ExceptionRecord && g_movieCave) {
		unsigned long long caveLo = (unsigned long long)g_movieCave;
		unsigned long long caveHi = caveLo + 128;
		unsigned long long rip = (unsigned long long)ep->ContextRecord->Rip;
		// A: faulted inside the cave itself, dereferencing an element pointer that was unmapped.
		if (rip >= caveLo && rip < caveHi) {
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
				ep->ContextRecord->Rip = g_cryMovieBase + 0x2B604;                    // ret
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			// D: walking the node hierarchy reached a sub-object whose memory was reused, leaving a
			// garbage vtable. Leave through the function's own "not found" exit, which unwinds
			// the stack correctly.
			unsigned long long h_lo = g_cryMovieBase + 0x3A630;
			unsigned long long h_hi = g_cryMovieBase + 0x3A69B;
			if (rip >= h_lo && rip < h_hi) {
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
					ep->ContextRecord->Rip = g_cryMovieBase + 0x213E;    // skip this node, keep iterating
					return EXCEPTION_CONTINUE_EXECUTION;
				} else {                                                 // the call jumped to data
					unsigned long long rsp = (unsigned long long)ep->ContextRecord->Rsp;
					unsigned long long ret = *(unsigned long long*)rsp;
					if (ret >= fl && ret < fh) {
						ep->ContextRecord->Rsp = rsp + 8;                // drop the failed call's return
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
					ep->ContextRecord->Rip = g_cryMovieBase + 0x5AE5C;   // → je 0x5bacf → inc r14 → next track
					return EXCEPTION_CONTINUE_EXECUTION;
				} else {                                                 // the call jumped to data
					unsigned long long rsp = (unsigned long long)ep->ContextRecord->Rsp;
					unsigned long long ret = *(unsigned long long*)rsp;
					if (ret >= gl && ret < gh) {
						ep->ContextRecord->Rsp = rsp + 8;                // drop the failed call's return
						ep->ContextRecord->Rax = 0;
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
extern "C" __declspec(dllimport) unsigned __stdcall timeBeginPeriod(unsigned uPeriod);
extern "C" __declspec(dllimport) unsigned __stdcall timeEndPeriod(unsigned uPeriod);

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
			if (f) { fprintf(f, "[run91] borderless applied: hwnd=%p %dx%d\n", (void*)s.found, w, hgt); fclose(f); }
			logged = true;
		}
	}
}

// Diagnostic report for testers.
//
// Problems on other people's machines are invisible to us. One tester saw a 60 FPS cap and
// Alt-Tab crashes; another saw neither. With nothing written down about the hardware and the
// mode the launcher started in, such reports stay descriptions in chat and cannot be debugged.
// Now the tester just sends launcher_diag.txt.
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

static void WriteDiagReport(const char* cmdLine, bool timerRaised, bool borderless)
{
	FILE* f = fopen("launcher_diag.txt", "w");
	if (!f) return;

	DiagLine(f, "=== crysis2-64bit launcher diagnostics ===");
	DiagLine(f, "launcher build : %s %s", __DATE__, __TIME__);
	DiagLine(f, "command line   : %s", (cmdLine && *cmdLine) ? cmdLine : "(none)");
	DiagLine(f, "timer 1ms      : %s", timerRaised ? "raised OK" : "FAILED (expect ~64 fps cap)");
	DiagLine(f, "borderless     : %s", borderless ? "enabled" : "disabled (-noborderless)");
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

	HDC hdc = GetDC(NULL);
	if (hdc) {
		int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
		DiagLine(f, "DPI scale      : %d%% (%d dpi)", (dpi * 100) / 96, dpi);
		ReleaseDC(NULL, hdc);
	}
	DiagLine(f, "");

	DiagLine(f, "--- Bin64 modules (size / modified) ---");
	static const char* mods[] = {
		"CrySystem.dll", "CryRenderD3D11.dll", "CryGameCrysis2.dll", "CryAction.dll",
		"CryPhysics.dll", "Cry3DEngine.dll", "CryAnimation.dll", "mechanics.dll", 0
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

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
	// Before engine init: 15.625 ms -> 1 ms (see the note above timeBeginPeriod).
	const bool timerRaised = (timeBeginPeriod(1) == 0);

	SetCwdToGameRoot();

	SSystemInitParams startupParams;
	startupParams.hInstance = GetModuleHandleA(NULL);
	startupParams.sLogFileName = "Game.log";
	strncpy(startupParams.szSystemCmdLine, lpCmdLine ? lpCmdLine : "", sizeof(startupParams.szSystemCmdLine) - 1);

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
	// r_DisplayInfo draws a debug overlay whose status line ends in "DevMode", which a tester
	// reported as the build shipping with debug facilities active. It needs no reversing, just
	// this CVar. Pass -keepintro to restore the original behaviour.
	if (!(lpCmdLine && strstr(lpCmdLine, "-keepintro")))
	{
		strncat(startupParams.szSystemCmdLine,
		        " +g_skipIntro 1 +sys_rendersplashscreen 0 +sys_intromoviesduringinit 0 +r_DisplayInfo 0",
		        sizeof(startupParams.szSystemCmdLine) - strlen(startupParams.szSystemCmdLine) - 1);
	}

	// Write the report before engine init, so the file survives a crash during startup and the
	// tester still has something to send.
	WriteDiagReport(lpCmdLine, timerRaised, wantBorderless);

	// Bring up the engine's memory system first, in the same order the editor does.
	ISystem* pSystem = CreateSystemInterface(startupParams);
	if (!pSystem) { MessageBoxA(0, "CreateSystemInterface failed (engine init)!", "Launcher", MB_OK); return 0; }
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
	if (!gameDll) { MessageBoxA(0, "Failed to load CryGameCrysis2.dll!", "Launcher", MB_OK); return 0; }

	IGameStartup::TEntryFunction pCreate = (IGameStartup::TEntryFunction)GetProcAddress(gameDll, "CreateGameStartup2");
	if (!pCreate) { MessageBoxA(0, "CreateGameStartup2 not found in game DLL!", "Launcher", MB_OK); return 0; }

	IGameStartup* pGameStartup = pCreate();
	if (!pGameStartup) { MessageBoxA(0, "CreateGameStartup failed!", "Launcher", MB_OK); return 0; }

	// Initialise the game (reusing the system created above) and enter the main loop.
	if (pGameStartup->Init(startupParams))   // IGameRef converts to non-null on success
	{
		pGameStartup->Run(NULL);
	}
	pGameStartup->Shutdown();
	if (timerRaised) timeEndPeriod(1);   // hand the original quantum back to the system
	return 0;
}
