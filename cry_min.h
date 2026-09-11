#pragma once
// A minimal, STL-free subset of the CryEngine interfaces, just enough to boot the engine.
//
// It exists because this launcher is compiled with the VC90 toolchain from WDK 7.1 (see
// build.ps1 for why), and that kit's bundled STL does not compile on its own. The launcher
// does not need STL anyway.
//
// The struct layouts must match what retail CrySystem.dll expects, so they are reproduced
// as the retail Windows build sees them (no OPEN_AUTOMATE, no LINUX). If a retail struct
// turns out to be slightly larger, the trailing _pad absorbs the difference and the extra
// fields simply read as zero.

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

	char _pad[512];   // headroom in case retail has more fields; they read as zero

	SSystemInitParams()
	{
		memset(this, 0, sizeof(*this));   // retail's constructor zeroes everything too
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

// Retail exports this undecorated. It is resolved at runtime rather than imported, so that a
// missing or misplaced engine can be explained by the launcher instead of by Windows, whose
// own message for a missing DLL tells the player to reinstall - which fixes nothing here.
typedef ISystem* (*CreateSystemInterfaceFn)(const SSystemInitParams &startupParams);
