#pragma once
// [x64launcher approach B] Минимальный STL-free интерфейс CryEngine для сборки VC90 (WDK 7.1).
// WDK-STL (stl70) не компилируется сам по себе (внутр. рассинхрон версий заголовков), а лаунчеру
// STL и не нужен. Определения скопированы ДОСЛОВНО из retail-заголовков
// (ISystem.h / IGameStartup.h / IGameRef.h), #ifdef разрешены как в retail-Windows-сборке
// (без OPEN_AUTOMATE, без LINUX) — layout совпадает с тем, чего ждёт retail CrySystem.dll.
// В конце добавлен запас _pad[] на случай, если retail-структура чуть больше (лишние поля=0).

#include <string.h>  // memset

struct ILog; struct ILogCallback; struct ISystemUserCallback; struct IValidator;
struct IOutputPrintSink; struct ISystem; struct SCvarsDefault; struct IGame;

enum { eProtectedFuncsLast = 10 };

struct SSystemInitParams
{
	void *hInstance;
	void *hWnd;
	ILog *pLog;
	ILogCallback *pLogCallback;
	ISystemUserCallback *pUserCallback;
	const char* sLogFileName;
	IValidator *pValidator;
	IOutputPrintSink *pPrintSync;
	char szSystemCmdLine[2048];
	char szUserPath[256];
	char szBinariesDir[256];

	bool bEditor;
	bool bPreview;
	bool bTestMode;
	bool bDedicatedServer;
	bool bExecuteCommandLine;
	bool bUIFramework;
	bool bSkipFont;
	bool bSkipRenderer;
	bool bSkipConsole;
	bool bSkipNetwork;
	bool bMinimal;
	bool bSkipInput;
	bool bTesting;
	bool bNoRandom;
	bool bShaderCacheGen;

	ISystem *pSystem;
	void *pCheckFunc;

	typedef void* (*ProtectedFunction)( void *param1,void *param2 );
	ProtectedFunction pProtectedFunctions[eProtectedFuncsLast];

	SCvarsDefault	*pCvarsDefault;

	char _pad[512]; // запас на случай доп. retail-полей (читаются как 0)

	SSystemInitParams()
	{
		memset(this, 0, sizeof(*this)); // все поля 0/false/NULL (как в retail-конструкторе)
		bExecuteCommandLine = true;
	}
};

struct IGameRef
{
	IGameRef(): m_ptr(0) {}
	IGameRef(IGame **ptr): m_ptr(ptr) {}
	~IGameRef() {}
	IGame *operator ->() const { return m_ptr ? *m_ptr : 0; }
	operator IGame*() const { return m_ptr ? *m_ptr : 0; }
	IGameRef &operator =(IGame **ptr) { m_ptr = ptr; return *this; }
private:
	IGame **m_ptr;
};

struct IGameStartup
{
	virtual ~IGameStartup(){}
	typedef IGameStartup *(*TEntryFunction)();
	virtual IGameRef Init(SSystemInitParams &startupParams) = 0;
	virtual void Shutdown() = 0;
	virtual int Update(bool haveFocus, unsigned int updateFlags) = 0;
	virtual bool GetRestartLevel(char** levelName) = 0;
	virtual const char* GetPatch() const = 0;
	virtual bool GetRestartMod(char* pModName, int nameLenMax) = 0;
	virtual int Run( const char * autoStartLevelName ) = 0;
};

// retail экспортирует undecorated (extern "C"); стат-импорт через CrySystem.lib.
extern "C" ISystem* CreateSystemInterface(const SSystemInitParams &startupParams);
