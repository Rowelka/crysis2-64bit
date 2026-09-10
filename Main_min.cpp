// [x64launcher approach B] Минимальный STL-free лаунчер, собираемый компилятором VC90 (WDK 7.1).
// Цель: процесс с ПЕРВИЧНЫМ CRT = msvcr90 (VC90 /MD) → его heap ложится в нижние 4 ГБ →
// bucket-аллокатор retail CrySystem (хранит адрес слаба 32-битным, баг усечения) работает,
// как в редакторе. Статический импорт CrySystem (CrySystem.lib) => грузится на init процесса
// вместе с msvcr90 в правильном порядке.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "cry_min.h"

// VC90 CRT SxS (msvcr90). VC90 /MD добавляет её сам, но продублируем явно на всякий случай.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.VC90.CRT' version='9.0.21022.8' processorArchitecture='amd64' publicKeyToken='1fc8b3b9a1e18e3b'\"")

static void SetCwdToGameRoot()
{
	char exePath[MAX_PATH];
	GetModuleFileNameA(NULL, exePath, MAX_PATH);
	char* p = strrchr(exePath, '\\'); if (p) *p = 0;   // .../Bin64/launcher64.exe -> .../Bin64
	p = strrchr(exePath, '\\'); if (p) *p = 0;          // .../Bin64 -> .../Crysis 2 (game root)
	SetCurrentDirectoryA(exePath);
}

// [x64launcher] Патч одного байта в памяти (VirtualProtect + write). Для обхода release-ассертов CryAction.
static void PatchByte(unsigned char* addr, unsigned char val)
{
	DWORD oldProt = 0;
	if (VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
		*addr = val;
		VirtualProtect(addr, 1, oldProt, &oldProt);
	}
}

// [x64launcher] VEH-страховка для movie-update. Массив дескрипторов CMovieSystem при выгрузке слоя
// во время precache катсцены (Battery Park) переиспользуется → элементы мусорные. Дешёвые guard'ы в
// code-cave (см. блок 1c) отсекают явный мусор, НО указатель с нулевыми старшими битами, ведущий в
// размапленную дыру (напр. 0x6A357E0E), их проходит и падает на mov rax,[rcx]. VEH ловит эту A/V
// ВНУТРИ cave и переставляет RIP на пропуск элемента (0xF2EC=add rbx,0x20) - цикл идёт со следующего
// (rbx/r14 не тронуты). Что бы ни было за мусор - не крашим, максимум теряем анимацию битой ноды.
// ВАЖНО: ловим ЛЮБОЙ код исключения при RIP внутри cave, НЕ только 0xC0000005 - битый rcx может попасть
// в guard-страницу → 0x80000001 (STATUS_GUARD_PAGE_VIOLATION), тоже A/V по сути. В cave ничего, кроме
// доступа к памяти по битому указателю, упасть не может, поэтому код исключения не проверяем.
static unsigned char*      g_movieCave = 0;
static unsigned long long  g_movieRetSkip = 0;
// [run87] reallocation-aware итерация CMovieSystem::Update. ПЕРВОПРИЧИНА порчи (подтв. исходником
// Crysis2 Movie.cpp:742 "Animate() can invalidate iterator"): ps.sequence->Animate() внутри цикла
// стартует вложенные секвенции (Battery Park intro) → m_playingSequences.push_back → реаллокация
// std::vector → итератор rbx повисает на старом буфере. Оригинал итерирует по итератору (UB).
// g_movieOldBegin хранит begin с прошлой итерации; при реаллокации ([r14+0x50]!=old) трамплин
// пересчитывает rbx = new_begin + (rbx - old_begin) → продолжает по НОВОМУ буферу (живые ноды там).
static unsigned long long  g_movieOldBegin = 0;
// [run87] база CryMovie.dll - для VEH-обхода OOB аксессора ключей трека (спавн-краш Intro).
static unsigned long long  g_cryMovieBase = 0;
static LONG CALLBACK MovieVEH(EXCEPTION_POINTERS* ep)
{
	if (ep && ep->ExceptionRecord && g_movieCave) {
		unsigned long long caveLo = (unsigned long long)g_movieCave;
		unsigned long long caveHi = caveLo + 128;
		unsigned long long rip = (unsigned long long)ep->ContextRecord->Rip;
		// СЛУЧАЙ A: краш ВНУТРИ cave (mov rax,[rcx] по битому/размапленному/guard-page rcx).
		if (rip >= caveLo && rip < caveHi) {
			ep->ContextRecord->Rip = g_movieRetSkip;   // прыжок на 0xF2EC (пропуск битого элемента)
			return EXCEPTION_CONTINUE_EXECUTION;
		}
		// СЛУЧАЙ B: `call [rax+0x80]` из cave прыгнул в мусорный/NULL target (RIP=0x0 или мусор ВНЕ cave),
		// но call успел запушить return-адрес (внутри cave) на вершину стека. Снимаем его и пропускаем
		// элемент. Признак: A/V + [rsp] указывает в cave. (Мусорный vtable со старшим байтом 0x34, но
		// [vtable+0x80]==0 → call 0x0; guard'ы такой пропускают, ловим тут.)
		if (ep->ExceptionRecord->ExceptionCode == 0xC0000005) {
			unsigned long long rsp = (unsigned long long)ep->ContextRecord->Rsp;
			unsigned long long ret = *(unsigned long long*)rsp;
			if (ret >= caveLo && ret < caveHi) {
				ep->ContextRecord->Rsp = rsp + 8;         // снять return от неудавшегося call
				ep->ContextRecord->Rip = g_movieRetSkip;  // пропуск элемента
				return EXCEPTION_CONTINUE_EXECUTION;
			}
		}
		// СЛУЧАЙ C: OOB в аксессоре ключа трека CryMovie (GetKeyValue по битому глобальному индексу -
		// спавн-краш Intro: movss xmm0,[buffer+index*8], index битый → чтение вне массива ключей). При A/V
		// в аксессоре [0x2B5F0,0x2B604) вернуть 0.0f (xmm0=0) и ret (0x2B604). Срабатывает ТОЛЬКО при OOB,
		// валидные вызовы не трогает. Первопричина (откуда битый индекс) - в бэклоге (levels_feedback.md).
		if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 && g_cryMovieBase) {
			unsigned long long accLo = g_cryMovieBase + 0x2B5F0;
			unsigned long long accHi = g_cryMovieBase + 0x2B604;
			if (rip >= accLo && rip < accHi) {
				ep->ContextRecord->Xmm0.Low = 0; ep->ContextRecord->Xmm0.High = 0;  // вернуть 0.0f
				ep->ContextRecord->Rip = g_cryMovieBase + 0x2B604;                    // ret
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			// СЛУЧАЙ D: обход иерархии нод CryMovie 0x3A630 [0x3A630,0x3A69B) (this->GetParent/Child slot55
			// → под-объект, slot2=GetType). На спавне Intro под-объект = переиспользованная movie-память
			// (XML-данные, битый vtable 0x3600000000) → краш. Возврат «не найдено» (eax=0) через путь
			// 0x3A67A (xor eax,eax; add rsp,0x20; pop rbx; ret - стек восстанавливается корректно).
			unsigned long long h_lo = g_cryMovieBase + 0x3A630;
			unsigned long long h_hi = g_cryMovieBase + 0x3A69B;
			if (rip >= h_lo && rip < h_hi) {
				ep->ContextRecord->Rip = g_cryMovieBase + 0x3A67A;   // xor eax,eax; add rsp,0x20; pop rbx; ret
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			// СЛУЧАЙ E: `call [node_vtable+0x1b8]` (slot55) ВНУТРИ обхода иерархии 0x3A630 прыгнул на ДАННЫЕ
			// (битый vtable под-объекта: слот ведёт не в код, а в .rdata CryMovie → EXECUTE A/V, напр. 0x644E8).
			// RIP улетел ВНЕ функции, но call запушил свой return-адрес (в [0x3A630,0x3A69B)) на вершину стека.
			// Признак: A/V + [rsp] в диапазоне функции иерархии, а сам RIP - вне. Снимаем фейковый return call
			// (rsp+=8) и выходим «узел не найден» через 0x3A67A. Стек сходится (проверено по дампу 26568).
			if (!(rip >= h_lo && rip < h_hi)) {
				unsigned long long rsp = (unsigned long long)ep->ContextRecord->Rsp;
				unsigned long long ret = *(unsigned long long*)rsp;
				if (ret >= h_lo && ret < h_hi) {
					ep->ContextRecord->Rsp = rsp + 8;                    // снять return от неудавшегося call
					ep->ContextRecord->Rip = g_cryMovieBase + 0x3A67A;   // выход «не найдено» (eax=0)
					return EXCEPTION_CONTINUE_EXECUTION;
				}
			}
			// СЛУЧАЙ F: обход вектора нод секвенции CryMovie 0x2050, тело цикла [0x2104,0x2148):
			//   rsi=*it=нода; rax=[rsi](vtable); call [rax+0x10](slot2 GetType, 0x210D) / [rax+0x1c0](slot56
			//   0x211B) / [rax+0x108](slot33 Animate, 0x2138). На спавне Intro нода = переиспользованная
			//   память (vtable в КУЧУ 0x45xxxxxx, не в модуль → slot ведёт на данные CryAction → EXECUTE A/V;
			//   либо read A/V внутри). ПРОПУСКАЕМ битую ноду → 0x213E (add rbx,8; ++it; loop), остальные
			//   ноды секвенции сохраняются (rbx=it/rdi=this/rsi целы - упало на 1-м байте вызова).
			if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 && g_cryMovieBase) {
				unsigned long long fl = g_cryMovieBase + 0x2104;
				unsigned long long fh = g_cryMovieBase + 0x2148;
				if (rip >= fl && rip < fh) {                              // read A/V внутри тела цикла
					ep->ContextRecord->Rip = g_cryMovieBase + 0x213E;    // пропуск ноды (++it; loop)
					return EXCEPTION_CONTINUE_EXECUTION;
				} else {                                                 // call улетел на данные (EXECUTE A/V)
					unsigned long long rsp = (unsigned long long)ep->ContextRecord->Rsp;
					unsigned long long ret = *(unsigned long long*)rsp;
					if (ret >= fl && ret < fh) {
						ep->ContextRecord->Rsp = rsp + 8;                // снять фейковый return call
						ep->ContextRecord->Rip = g_cryMovieBase + 0x213E; // пропуск ноды
						return EXCEPTION_CONTINUE_EXECUTION;
					}
				}
			}
			// СЛУЧАЙ G: обход треков ноды CAnimNode::Animate (0x5AE20), загрузка/1-й vcall трека [0x5AE4F,0x5AE5C):
			//   rax=[rbx+0x50](m_tracks); r12=m_tracks[r14].track; rax=[r12](vtable); call [rax+0x60](slot12
			//   GetNumKeys, 0x5AE59). На спавне Intro трек ОСВОБОЖДЁН, память отдана прекешу шейдеров катсцены
			//   (vtable=ASCII-мусор, напр. "Cloth@Common_SG_VS..._RT_ALPHATEST") → read A/V. ЭМУЛИРУЕМ
			//   "GetNumKeys()==0" (eax=0) → RIP=0x5AE5C (test eax; je 0x5bacf → inc r14 → jl 0x5ae40 next track):
			//   битый трек пропускается ШТАТНЫМ путём функции, счётчики esi/r14 и регистры целы.
			if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 && g_cryMovieBase) {
				unsigned long long gl = g_cryMovieBase + 0x5AE4F;
				unsigned long long gh = g_cryMovieBase + 0x5AE5C;
				if (rip >= gl && rip < gh) {                              // read A/V на vtable/операнде трека
					ep->ContextRecord->Rax = 0;                          // eax=0: "0 ключей"
					ep->ContextRecord->Rip = g_cryMovieBase + 0x5AE5C;   // → je 0x5bacf → inc r14 → next track
					return EXCEPTION_CONTINUE_EXECUTION;
				} else {                                                 // call [rax+0x60] улетел (EXECUTE A/V)
					unsigned long long rsp = (unsigned long long)ep->ContextRecord->Rsp;
					unsigned long long ret = *(unsigned long long*)rsp;
					if (ret >= gl && ret < gh) {
						ep->ContextRecord->Rsp = rsp + 8;                // снять фейковый return call
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

// [run91] ★ПЕРВОПРИЧИНА FPS-лока ~64: КВАНТ СИСТЕМНОГО ТАЙМЕРА Windows (15.625 мс), а НЕ нагрузка.
// Проверено разбором импортов: Crysis2.exe / Editor.exe / CrySystem.dll тянут из WINMM ТОЛЬКО
// timeGetTime - timeBeginPeriod не зовёт НИКТО, разрешение таймера остаётся системным дефолтом.
// Main-поток ждёт физ-барьер (замер: WaitPhys ~15.5 мс при Phys 0.1 мс и GPU 5.6 мс = железо
// простаивает) → ожидание округляется ВВЕРХ до кванта → 1000/15.625 = ровно 64.0 FPS.
// Тем же объясняются спонтанные срывы на 200-300: когда ожидания нет, кадр идёт на полной скорости
// (бинарность «64 или 300» без промежуточных = признак кванта, не нагрузки).
// timeBeginPeriod на Win10 2004+ действует per-process → поднимаем СЕБЕ, движок не трогаем.
extern "C" __declspec(dllimport) unsigned __stdcall timeBeginPeriod(unsigned uPeriod);
extern "C" __declspec(dllimport) unsigned __stdcall timeEndPeriod(unsigned uPeriod);

// [run91] BORDERLESS WINDOWED FULLSCREEN - окно без рамки на весь экран.
// Наблюдение юзера (10.09): в ОКНЕ игра идёт плавно и БЕЗ разрывов; в эксклюзивном fullscreen -
// разрывы; r_VSync 1 роняет до 60 FPS и добавляет инпут-лаг. Объяснение: в оконном режиме кадры
// проходят через композитор Windows (DWM), который синхронизирует вывод сам - тиринг невозможен
// by design, при этом кадры не ограничены и задержки vsync нет. В эксклюзивном fullscreen движок
// выводит напрямую, а частоту взять неоткуда: в CryRenderD3D11 есть ТОЛЬКО r_Fullscreen и r_VSync,
// CVar частоты обновления не существует → DXGI отдаёт 60 Гц, монитор 165 Гц не используется
// (потому vsync и даёт 60, а не 165).
// Побочно чинит alt-tab: в windowed нет эксклюзивного device, терять на фокус-свитче нечего.
// САМОЗАЩИТА: если игра всё же в эксклюзивном fullscreen, её окно уже WS_POPUP на весь экран →
// проверка needFix даёт false и мы НЕ трогаем ничего. Отключается флагом -noborderless.
struct SBorderlessSearch { DWORD pid; HWND found; };

static BOOL CALLBACK BorderlessEnumProc(HWND h, LPARAM lp)
{
	SBorderlessSearch* s = (SBorderlessSearch*)lp;
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid != s->pid) return TRUE;
	if (!IsWindowVisible(h)) return TRUE;
	if (GetWindow(h, GW_OWNER) != NULL) return TRUE;          // диалоги/сплэши пропускаем
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
	// Отдельный поток, опрос раз в 500мс (НЕ в кадровом цикле - правило hot-path соблюдено).
	// Не «нашли и вышли»: движок создаёт окно не сразу и может пересоздать его при смене
	// видеорежима, поэтому следим всё время игры и переприменяем стиль.
	for (int tick = 0; ; tick++)
	{
		// Первые ~10 секунд опрашиваем часто: окно создаётся на старте игры, и полсекунды
		// с рамкой заметны глазом (мелькание при загрузке). Дальше редко - там это уже
		// только страховка на случай пересоздания окна при смене видеорежима.
		Sleep(tick < 200 ? 50 : 500);
		SBorderlessSearch s;
		s.pid = GetCurrentProcessId();
		s.found = NULL;
		EnumWindows(BorderlessEnumProc, (LPARAM)&s);
		if (!s.found) continue;
		if (IsIconic(s.found)) continue;                       // свёрнуто - не мешаем alt-tab
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

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
	// [run91] ДО инициализации движка: 15.625 мс -> 1 мс.
	const bool timerRaised = (timeBeginPeriod(1) == 0);

	SetCwdToGameRoot();

	SSystemInitParams startupParams;
	startupParams.hInstance = GetModuleHandleA(NULL);
	startupParams.sLogFileName = "Game.log";
	strncpy(startupParams.szSystemCmdLine, lpCmdLine ? lpCmdLine : "", sizeof(startupParams.szSystemCmdLine) - 1);

	// [run91] Borderless по умолчанию (см. блок выше). -noborderless возвращает прежнее поведение.
	const bool wantBorderless = !(lpCmdLine && strstr(lpCmdLine, "-noborderless"));
	if (wantBorderless)
	{
		// Просим движок стартовать в окне и в разрешении экрана ('+' = команда консоли в CryEngine).
		// Если профиль игры перекроет это на fullscreen - ничего не сломается: окно эксклюзива уже
		// без рамки, и поток его не тронет.
		char extra[128];
		sprintf(extra, " +r_Fullscreen 0 +r_Width %d +r_Height %d",
		        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
		strncat(startupParams.szSystemCmdLine, extra,
		        sizeof(startupParams.szSystemCmdLine) - strlen(startupParams.szSystemCmdLine) - 1);

		DWORD tid = 0;
		HANDLE th = CreateThread(NULL, 0, BorderlessThread, NULL, 0, &tid);
		if (th) CloseHandle(th);
	}

	// 1) Поднять систему памяти движка ПЕРВОЙ (editor-порядок).
	ISystem* pSystem = CreateSystemInterface(startupParams);
	if (!pSystem) { MessageBoxA(0, "CreateSystemInterface failed (engine init)!", "Launcher", MB_OK); return 0; }
	startupParams.pSystem = pSystem; // game Init переиспользует готовую систему

	// 1b) Пропатчить CryAction: обойти release-ассерты на пути CLevelSystem::LoadLevel (в Bin64-билде
	// они форс-крашат). Адреса из open-source c2-launcher (CryAction 1.1.1.217). Грузим CryAction явно
	// (форсим до game Init), патчим, дальше game использует уже пропатченную копию.
	{
		HMODULE cryAction = LoadLibraryA("CryAction.dll");
		if (cryAction) {
			unsigned char* b = (unsigned char*)cryAction;
			PatchByte(b + 0xBC7C, 0xEB); // je -> jmp: проскок форс-краха 0xBC83 (наш краш загрузки уровня)
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

	// 1c) Фикс краха CryMovie в game-mode (цикл CMovieSystem-update, loop-body @0xF250, back-edge
	// @0xF2F4→0xF250). Обход списка элементов [r14+0x50..0x58], на каждом: rcx=[rbx]; rax=[rcx](vtable);
	// call [rax+0x80] (slot16). ДВА вида битых элементов ловим:
	//   (1) NULL-vtable ([rcx]==0) - спавн-краш после катсцены (был первый фикс);
	//   (2) ВИСЯЧИЙ объект (use-after-free) - при выгрузке слоя во время precache катсцены память
	//       элемента переиспользуется под XML-данные, vtable становится НЕ-null, но мусорным
	//       (указывает в кучу 0x26xxxxxx, не в модуль) → call [мусор+0x80] → execute-краш на Battery Park.
	// Полные дампы (fulldump.py): (2а) vtable=0x2633ADE0 (heap) → call[heap+0x80] execute-fault;
	// (2б) [rbx]=0x500000001 (сам XML-тег-мусор) → mov rax,[0x500000001] read-fault. Т.е. битый и объект,
	// и vtable, и сам объект (3-й дамп: мусор [rbx]=0x30286B50 указал в Cry3DEngine и прошёл широкий
	// диапазон → упал во 2-м call'е тела 0xF2B5). Массив дескрипторов CMovieSystem [r14+0x50..0x58]
	// (stride 0x20, 3 vcall'а на элемент: 0xF256/0xF2B5/0xF2E6) переиспользован. ДВА guard'а: (g1) rcx=[rbx]
	// валиден - старшие 32 бита == 0 (вся память процесса < 4ГБ; мусор-тег 0x5_00000001 отсекается) И
	// rcx>=0x10000 (не NULL/мелкий); (g2) vtable=[rcx] в ТОЧНЫХ границах CryMovie.dll [0x34000000,0x34082000)
	// (ноды CMovie там). Любой промах → SKIP всего элемента (jmp 0xF2EC=конец тела, минуя ВСЕ 3 call'а),
	// иначе штатно (call [rax+0x80], jmp 0xF25C). Что прошло guard'ы, но упало - ловит MovieVEH (см. выше).
	// Патч на 0xF250 = x64-absolute (mov rax,cave; jmp rax).
	{
		HMODULE cryMovie = LoadLibraryA("CryMovie.dll");
		if (cryMovie) {
			unsigned char* mb = (unsigned char*)cryMovie;
			unsigned long long retNormal = (unsigned long long)(mb + 0xF25C);
			unsigned long long retSkip   = (unsigned long long)(mb + 0xF2EC);
			unsigned char* cave = (unsigned char*)VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (cave) {
				int i = 0;
				// [run87] reallocation-aware: если vector m_playingSequences реаллоцирован (Animate→push_back
				// сдвинул begin=[r14+0x50]), скорректировать итератор rbx в НОВЫЙ буфер. self-init: rbx==begin
				// ⇒ первая итерация цикла (sync без коррекции). Иначе begin!=old ⇒ реаллокация ⇒ rbx=begin+(rbx-old).
				unsigned long long obAddr = (unsigned long long)&g_movieOldBegin;
				cave[i++]=0x4D; cave[i++]=0x8B; cave[i++]=0x56; cave[i++]=0x50;                     // mov r10,[r14+0x50] (new_begin)
				cave[i++]=0x49; cave[i++]=0x3B; cave[i++]=0xDA;                                     // cmp rbx,r10
				int j_first = i; cave[i++]=0x74; cave[i++]=0x00;                                    // je L_sync (первая итерация)
				cave[i++]=0x48; cave[i++]=0xB8; *(unsigned long long*)(cave+i)=obAddr; i+=8;        // mov rax,&g_movieOldBegin
				cave[i++]=0x48; cave[i++]=0x8B; cave[i++]=0x00;                                     // mov rax,[rax] (old_begin)
				cave[i++]=0x49; cave[i++]=0x3B; cave[i++]=0xC2;                                     // cmp rax,r10
				int j_noreal = i; cave[i++]=0x74; cave[i++]=0x00;                                   // je L_sync (begin не менялся)
				cave[i++]=0x48; cave[i++]=0x2B; cave[i++]=0xD8;                                     // sub rbx,rax (offset = rbx-old)
				cave[i++]=0x49; cave[i++]=0x03; cave[i++]=0xDA;                                     // add rbx,r10 (rbx = new_begin+offset)
				int L_sync = i;
				cave[i++]=0x48; cave[i++]=0xB8; *(unsigned long long*)(cave+i)=obAddr; i+=8;        // mov rax,&g_movieOldBegin
				cave[i++]=0x4C; cave[i++]=0x89; cave[i++]=0x10;                                     // mov [rax],r10 (old = new_begin)
				// [run88 hang-fix] revalidate итератора: rbx ДОЛЖЕН быть в [begin,end). Вложенный movie-update
				// (под-секвенции Intro: _20 играет ambient_motion) перезаписывает НЕreentrant глобал old_begin →
				// коррекция внешнего цикла даёт МУСОР (rbx=0x965E0140 unmapped) → mov rcx,[rbx] A/V → VEH retSkip →
				// add rbx,0x20 → cmp!=end (rbx>>end) → БЕСКОНЕЧНЫЙ цикл A/V→VEH = ЗАВИСАНИЕ. Проверяем rbx в
				// [begin,end); если вне (мусор ИЛИ >=end) → ВЫХОД из loop (jmp 0xF2FA) вместо продолжения.
				cave[i++]=0x4D; cave[i++]=0x8B; cave[i++]=0x5E; cave[i++]=0x58;                     // mov r11,[r14+0x58] (end)
				cave[i++]=0x49; cave[i++]=0x3B; cave[i++]=0xDA;                                     // cmp rbx,r10 (begin)
				int j_exlo = i; cave[i++]=0x72; cave[i++]=0x00;                                     // jb EXIT (rbx<begin)
				cave[i++]=0x49; cave[i++]=0x3B; cave[i++]=0xDB;                                     // cmp rbx,r11 (end)
				int j_exhi = i; cave[i++]=0x73; cave[i++]=0x00;                                     // jae EXIT (rbx>=end)
				cave[j_first+1]  = (unsigned char)(L_sync - (j_first+2));                           // rel8 -> L_sync
				cave[j_noreal+1] = (unsigned char)(L_sync - (j_noreal+2));
				cave[i++]=0x48; cave[i++]=0x8B; cave[i++]=0x0B;                                     // mov rcx,[rbx]  (указатель на объект-элемент)
				// guard1: rcx - валидный указатель? Вся память процесса в младших 4ГБ (модули 0x1C..0x39,
				// куча/стек тоже). Мусор-тег 0x500000001 имеет старшие 32 бита != 0. И NULL/мелкий мусор < 0x10000.
				cave[i++]=0x49; cave[i++]=0x89; cave[i++]=0xCB;                                     // mov r11,rcx
				cave[i++]=0x49; cave[i++]=0xC1; cave[i++]=0xEB; cave[i++]=0x20;                      // shr r11,32 (старшие 32 бита)
				int j_hi = i; cave[i++]=0x75; cave[i++]=0x00;                                       // jnz SKIP (rcx>=4ГБ = мусор 0x5_00000001)
				cave[i++]=0x48; cave[i++]=0x81; cave[i++]=0xF9; cave[i++]=0x00; cave[i++]=0x00; cave[i++]=0x01; cave[i++]=0x00; // cmp rcx,0x10000
				int j_lo = i; cave[i++]=0x72; cave[i++]=0x00;                                       // jb SKIP (NULL/мелкий мусор)
				cave[i++]=0x48; cave[i++]=0x8B; cave[i++]=0x01;                                     // mov rax,[rcx]  (vtable) - теперь безопасно
				// guard2: vtable в ТОЧНЫХ границах CryMovie.dll [0x34000000, 0x34082000). Ноды CMovie
				// определены в CryMovie, их vtable там. Мусор 0x3474754F (старший байт 0x34, НО за концом
				// модуля 0x34082000) раньше проходил проверку только-старшего-байта → теперь отсекается.
				cave[i++]=0x48; cave[i++]=0x3D; *(unsigned int*)(cave+i)=0x34000000; i+=4;          // cmp rax,0x34000000
				int j_vlo = i; cave[i++]=0x72; cave[i++]=0x00;                                      // jb SKIP (vtable < CryMovie)
				cave[i++]=0x41; cave[i++]=0xBB; *(unsigned int*)(cave+i)=0x34082000; i+=4;          // mov r11d,0x34082000
				cave[i++]=0x4C; cave[i++]=0x39; cave[i++]=0xD8;                                     // cmp rax,r11
				int j_vhi = i; cave[i++]=0x73; cave[i++]=0x00;                                      // jae SKIP (vtable >= конца CryMovie)
				cave[i++]=0xFF; cave[i++]=0x90; cave[i++]=0x80; cave[i++]=0x00; cave[i++]=0x00; cave[i++]=0x00; // call [rax+0x80]
				cave[i++]=0x49; cave[i++]=0xBB; *(unsigned long long*)(cave+i)=retNormal; i+=8;     // mov r11, 0xF25C
				cave[i++]=0x41; cave[i++]=0xFF; cave[i++]=0xE3;                                     // jmp r11
				int skip = i;
				cave[i++]=0x49; cave[i++]=0xBB; *(unsigned long long*)(cave+i)=retSkip; i+=8;       // SKIP: mov r11, 0xF2EC
				cave[i++]=0x41; cave[i++]=0xFF; cave[i++]=0xE3;                                     // jmp r11
				int L_exit = i;                                                                     // [run88] EXIT: корректный выход из loop
				cave[i++]=0x49; cave[i++]=0xBA; *(unsigned long long*)(cave+i)=(unsigned long long)(mb+0xF2FA); i+=8; // mov r10, 0xF2FA
				cave[i++]=0x41; cave[i++]=0xFF; cave[i++]=0xE2;                                     // jmp r10 (выход из loop, минуя cmp!=end)
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
				// VEH-страховка: ловит A/V внутри cave (битый rcx в размапленную память) → пропуск элемента.
				g_movieCave = cave;
				g_movieRetSkip = retSkip;
				g_cryMovieBase = (unsigned long long)mb;   // для VEH-обхода OOB аксессора ключей (Intro)
				AddVectoredExceptionHandler(1, MovieVEH);
			}
		}
	}

	// 1d) Фикс лакуны editor-сборки CrySystem: memory-сервис @CrySystem+0x458200 ("CryPak Heap") имеет
	// slot34/35 = _purecall (CrySystem+0x1AFB72=jmp[import _purecall]) - В EDITOR-БИЛДЕ эти методы НЕ
	// реализованы, а game-код (CUIElement::~ dtor, CryGameReal 0x5E2F60) вызывает slot35 при teardown UI
	// катсцены Battery Park (Pier_Birds) → FATAL "Pure function call". Диагноз: дамп 38652, наш movie-cave
	// в стеке → валидная нода → CUIElement dtor → getter(CrySystem 0xA2070)→объект@+0x6FA230 vtable@+0x458200
	// → call[vtbl+0x118]=_purecall. Все DLL 1.1.1.217, НЕ mismatch - это editor-build лакуна (x64 DLL=ModSDK).
	// ЧИСТЫЙ ФИКС: заменить каждый _purecall в этой vtable на ШТАТНУЮ движковую заглушку CrySystem+0x68340
	// (`ret 0`) - движок сам ею заполнил мн. др. слоты этого сервиса (slot3/4/5/7/10..14). Метод (cleanup UI)
	// становится no-op: макс. мелкая утечка при teardown катсцены, НЕ краш, НЕ ломает игру (консистентно с
	// движком). Патчим vtable (в .rdata) через VirtualProtect. base CrySystem = уже загружен (стат-импорт).
	{
		HMODULE cs = GetModuleHandleA("CrySystem.dll");
		if (cs) {
			unsigned char* b = (unsigned char*)cs;
			unsigned long long* vt = (unsigned long long*)(b + 0x458200);
			unsigned long long purecall = (unsigned long long)(b + 0x1AFB72);
			unsigned long long stub     = (unsigned long long)(b + 0x68340);
			DWORD oldp = 0;
			if (VirtualProtect(vt, 48 * 8, PAGE_READWRITE, &oldp)) {
				for (int s = 0; s < 48; s++) if (vt[s] == purecall) vt[s] = stub; // только _purecall → ret 0
				VirtualProtect(vt, 48 * 8, oldp, &oldp);
			}
		}
	}

	// 2) Загрузить нашу game-DLL (обёртка -> retail CryGameReal), взять точку входа.
	HMODULE gameDll = LoadLibraryA("CryGameCrysis2.dll");
	if (!gameDll) { MessageBoxA(0, "Failed to load CryGameCrysis2.dll!", "Launcher", MB_OK); return 0; }

	IGameStartup::TEntryFunction pCreate = (IGameStartup::TEntryFunction)GetProcAddress(gameDll, "CreateGameStartup2");
	if (!pCreate) { MessageBoxA(0, "CreateGameStartup2 not found in game DLL!", "Launcher", MB_OK); return 0; }

	IGameStartup* pGameStartup = pCreate();
	if (!pGameStartup) { MessageBoxA(0, "CreateGameStartup failed!", "Launcher", MB_OK); return 0; }

	// 3) Init (переиспользует pSystem) и главный игровой цикл.
	if (pGameStartup->Init(startupParams))   // IGameRef -> operator IGame*() != 0 при успехе
	{
		pGameStartup->Run(NULL);
	}
	pGameStartup->Shutdown();
	if (timerRaised) timeEndPeriod(1);   // [run91] вернуть системе исходный квант
	return 0;
}
