
#include "Globals.h"  // NOTE: MSVC stupidness requires this to be the same across all modules

#include "Root.h"
#include "tclap/CmdLine.h"

#include <exception>
#include <csignal>
#include <stdlib.h>

#ifdef _MSC_VER
	#include <dbghelp.h>
#endif  // _MSC_VER

#include "OSSupport/NetworkSingleton.h"
#include "BuildInfo.h"

#include "MemorySettingsRepository.h"



/** If something has told the server to stop; checked periodically in cRoot */
bool cRoot::m_TerminateEventRaised = false;

/** Set to true when the server terminates, so our CTRL handler can then tell the OS to close the console. */
static bool g_ServerTerminated = false;

/** If set to true, the protocols will log each player's incoming (C->S) communication to a per-connection logfile */
bool g_ShouldLogCommIn;

/** If set to true, the protocols will log each player's outgoing (S->C) communication to a per-connection logfile */
bool g_ShouldLogCommOut;

/** If set to true, binary will attempt to run as a service on Windows */
bool cRoot::m_RunAsService = false;





#if defined(_WIN32)
	SERVICE_STATUS_HANDLE g_StatusHandle  = NULL;
	HANDLE                g_ServiceThread = INVALID_HANDLE_VALUE;
	#define               SERVICE_NAME      "MCServerService"
#endif





/** If defined, a thorough leak finder will be used (debug MSVC only); leaks will be output to the Output window */
// _X 2014_02_20: Disabled for canon repo, it makes the debug version too slow in MSVC2013
// and we haven't had a memory leak for over a year anyway.
// #define ENABLE_LEAK_FINDER





#if defined(_MSC_VER) && defined(_DEBUG) && defined(ENABLE_LEAK_FINDER)
	#pragma warning(push)
	#pragma warning(disable:4100)
	#include "LeakFinder.h"
	#pragma warning(pop)
#endif





void NonCtrlHandler(int a_Signal)
{
	LOGD("Terminate event raised from std::signal");
	cRoot::m_TerminateEventRaised = true;

	switch (a_Signal)
	{
		case SIGSEGV:
		{
			std::signal(SIGSEGV, SIG_DFL);
			LOGERROR("  D:    | MCServer has encountered an error and needs to close");
			LOGERROR("Details | SIGSEGV: Segmentation fault");
			#ifdef BUILD_ID
			LOGERROR("MCServer " BUILD_SERIES_NAME " build id: " BUILD_ID);
			LOGERROR("from commit id: " BUILD_COMMIT_ID " built at: " BUILD_DATETIME);
			#endif
			PrintStackTrace();
			abort();
		}
		case SIGABRT:
		#ifdef SIGABRT_COMPAT
		case SIGABRT_COMPAT:
		#endif
		{
			std::signal(a_Signal, SIG_DFL);
			LOGERROR("  D:    | MCServer has encountered an error and needs to close");
			LOGERROR("Details | SIGABRT: Server self-terminated due to an internal fault");
			#ifdef BUILD_ID
			LOGERROR("MCServer " BUILD_SERIES_NAME " build id: " BUILD_ID);
			LOGERROR("from commit id: " BUILD_COMMIT_ID " built at: " BUILD_DATETIME);
			#endif
			PrintStackTrace();
			abort();
		}
		case SIGINT:
		case SIGTERM:
		{
			std::signal(a_Signal, SIG_IGN);  // Server is shutting down, wait for it...
			break;
		}
		default: break;
	}
}





#if defined(_WIN32) && !defined(_WIN64) && defined(_MSC_VER)
////////////////////////////////////////////////////////////////////////////////
// Windows 32-bit stuff: when the server crashes, create a "dump file" containing the callstack of each thread and some variables; let the user send us that crash file for analysis

typedef BOOL  (WINAPI *pMiniDumpWriteDump)(
	HANDLE hProcess,
	DWORD ProcessId,
	HANDLE hFile,
	MINIDUMP_TYPE DumpType,
	PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
	PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
	PMINIDUMP_CALLBACK_INFORMATION CallbackParam
);

pMiniDumpWriteDump g_WriteMiniDump;  // The function in dbghlp DLL that creates dump files

char g_DumpFileName[MAX_PATH];  // Filename of the dump file; hes to be created before the dump handler kicks in
char g_ExceptionStack[128 * 1024];  // Substitute stack, just in case the handler kicks in because of "insufficient stack space"
MINIDUMP_TYPE g_DumpFlags = MiniDumpNormal;  // By default dump only the stack and some helpers





/** This function gets called just before the "program executed an illegal instruction and will be terminated" or similar.
Its purpose is to create the crashdump using the dbghlp DLLs
*/
LONG WINAPI LastChanceExceptionFilter(__in struct _EXCEPTION_POINTERS * a_ExceptionInfo)
{
	char * newStack = &g_ExceptionStack[sizeof(g_ExceptionStack)];
	char * oldStack;

	// Use the substitute stack:
	// This code is the reason why we don't support 64-bit (yet)
	_asm
	{
		mov oldStack, esp
		mov esp, newStack
	}

	MINIDUMP_EXCEPTION_INFORMATION  ExcInformation;
	ExcInformation.ThreadId = GetCurrentThreadId();
	ExcInformation.ExceptionPointers = a_ExceptionInfo;
	ExcInformation.ClientPointers = 0;

	// Write the dump file:
	HANDLE dumpFile = CreateFile(g_DumpFileName, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	g_WriteMiniDump(GetCurrentProcess(), GetCurrentProcessId(), dumpFile, g_DumpFlags, (a_ExceptionInfo) ? &ExcInformation : nullptr, nullptr, nullptr);
	CloseHandle(dumpFile);

	// Print the stack trace for the basic debugging:
	PrintStackTrace();

	// Revert to old stack:
	_asm
	{
		mov esp, oldStack
	}

	return 0;
}

#endif  // _WIN32 && !_WIN64





#ifdef _WIN32
// Handle CTRL events in windows, including console window close
BOOL CtrlHandler(DWORD fdwCtrlType)
{
	cRoot::m_TerminateEventRaised = true;
	LOGD("Terminate event raised from the Windows CtrlHandler");

	while (!g_ServerTerminated)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));  // Delay as much as possible to try to get the server to shut down cleanly
	}

	return TRUE;
}
#endif





////////////////////////////////////////////////////////////////////////////////
// universalMain - Main startup logic for both standard running and as a service

void universalMain(std::unique_ptr<cSettingsRepositoryInterface> overridesRepo)
{
	#ifdef _WIN32
	if (!SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, TRUE))
	{
		LOGERROR("Could not install the Windows CTRL handler!");
	}
	#endif

	// Initialize logging subsystem:
	cLogger::InitiateMultithreading();

	// Initialize LibEvent:
	cNetworkSingleton::Get();

	#if !defined(ANDROID_NDK)
	try
	#endif
	{
		cRoot Root;
		Root.Start(std::move(overridesRepo));
	}
	#if !defined(ANDROID_NDK)
	catch (std::exception & e)
	{
		LOGERROR("Standard exception: %s", e.what());
	}
	catch (...)
	{
		LOGERROR("Unknown exception!");
	}
	#endif

	g_ServerTerminated = true;

	// Shutdown all of LibEvent:
	cNetworkSingleton::Get().Terminate();
}




#if defined(_WIN32)
////////////////////////////////////////////////////////////////////////////////
// serviceWorkerThread: Keep the service alive

DWORD WINAPI serviceWorkerThread(LPVOID lpParam)
{
	UNREFERENCED_PARAMETER(lpParam);

	// Do the normal startup
	universalMain(cpp14::make_unique<cMemorySettingsRepository>());

	return ERROR_SUCCESS;
}




////////////////////////////////////////////////////////////////////////////////
// serviceSetState: Set the internal status of the service

void serviceSetState(DWORD acceptedControls, DWORD newState, DWORD exitCode)
{
	SERVICE_STATUS serviceStatus;
	ZeroMemory(&serviceStatus, sizeof(SERVICE_STATUS));
	serviceStatus.dwCheckPoint = 0;
	serviceStatus.dwControlsAccepted = acceptedControls;
	serviceStatus.dwCurrentState = newState;
	serviceStatus.dwServiceSpecificExitCode = 0;
	serviceStatus.dwServiceType = SERVICE_WIN32;
	serviceStatus.dwWaitHint = 0;
	serviceStatus.dwWin32ExitCode = exitCode;

	if (SetServiceStatus(g_StatusHandle, &serviceStatus) == FALSE)
	{
		LOGERROR("SetServiceStatus() failed\n");
	}
}




////////////////////////////////////////////////////////////////////////////////
// serviceCtrlHandler: Handle stop events from the Service Control Manager

void WINAPI serviceCtrlHandler(DWORD CtrlCode)
{
	switch (CtrlCode)
	{
		case SERVICE_CONTROL_STOP:
		{
			cRoot::m_ShouldStop = true;
			serviceSetState(0, SERVICE_STOP_PENDING, 0);
			break;
		}

		default:
		{
			break;
		}
	}
}




////////////////////////////////////////////////////////////////////////////////
// serviceMain: Startup logic for running as a service

void WINAPI serviceMain(DWORD argc, TCHAR *argv[])
{
	#if defined(_DEBUG) && defined(DEBUG_SERVICE_STARTUP)
	Sleep(10000);
	#endif
	
	char applicationFilename[MAX_PATH];
	char applicationDirectory[MAX_PATH];

	GetModuleFileName(NULL, applicationFilename, sizeof(applicationFilename));  // This binary's file path.

	// Strip off the filename, keep only the path:
	strncpy_s(applicationDirectory, sizeof(applicationDirectory), applicationFilename, (strrchr(applicationFilename, '\\') - applicationFilename));
	applicationDirectory[strlen(applicationDirectory)] = '\0';  // Make sure new path is null terminated

	// Services are run by the SCM, and inherit its working directory - usually System32.
	// Set the working directory to the same location as the binary.
	SetCurrentDirectory(applicationDirectory);

	g_StatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, serviceCtrlHandler);
	
	if (g_StatusHandle == NULL)
	{
		OutputDebugStringA("RegisterServiceCtrlHandler() failed\n");
		serviceSetState(0, SERVICE_STOPPED, GetLastError());
		return;
	}
	
	serviceSetState(SERVICE_ACCEPT_STOP, SERVICE_RUNNING, 0);
	
	g_ServiceThread = CreateThread(NULL, 0, serviceWorkerThread, NULL, 0, NULL);
	if (g_ServiceThread == NULL)
	{
		OutputDebugStringA("CreateThread() failed\n");
		serviceSetState(0, SERVICE_STOPPED, GetLastError());
		return;
	}
	WaitForSingleObject(g_ServiceThread, INFINITE);  // Wait here for a stop signal.
	
	CloseHandle(g_ServiceThread);
	
	serviceSetState(0, SERVICE_STOPPED, 0);
}
#endif



std::unique_ptr<cMemorySettingsRepository> parseArguments(int argc, char **argv)
{
	try
	{
		TCLAP::CmdLine cmd("MCServer");

		TCLAP::ValueArg<int> slotsArg("s", "max-players", "Maximum number of slots for the server to use, overrides setting in setting.ini", false, -1, "number", cmd);

		TCLAP::MultiArg<int> portsArg("p", "port", "The port number the server should listen to", false, "port", cmd);

		TCLAP::SwitchArg commLogArg("", "log-comm", "Log server client communications to file", cmd);

		TCLAP::SwitchArg commLogInArg("", "log-comm-in", "Log inbound server client communications to file", cmd);
		
		TCLAP::SwitchArg commLogOutArg("", "log-comm-out", "Log outbound server client communications to file", cmd);

		TCLAP::SwitchArg noBufArg("", "no-output-buffering", "Disable output buffering", cmd);

		TCLAP::SwitchArg runAsServiceArg("d", "run-as-service", "Run as a service on Windows", cmd);

		cmd.ignoreUnmatched(true);

		cmd.parse(argc, argv);

		auto repo = cpp14::make_unique<cMemorySettingsRepository>();

		if (slotsArg.isSet())
		{

			int slots = slotsArg.getValue();

			repo->AddValue("Server", "MaxPlayers", static_cast<Int64>(slots));

		}

		if (portsArg.isSet())
		{
			std::vector<int> ports = portsArg.getValue();
			for (auto port : ports)
			{
				repo->AddValue("Server", "Port", static_cast<Int64>(port));
			}
		}

		if (commLogArg.getValue())
		{
			g_ShouldLogCommIn = true;
			g_ShouldLogCommOut = true;
		}
		else
		{
			g_ShouldLogCommIn = commLogInArg.getValue();
			g_ShouldLogCommOut = commLogOutArg.getValue();
		}

		if (noBufArg.getValue())
		{
			setvbuf(stdout, nullptr, _IONBF, 0);
		}

		if (runAsServiceArg.getValue())
		{
			cRoot::m_RunAsService = true;
		}

		repo->SetReadOnly();

		return repo;
	}
	catch (TCLAP::ArgException &e)
	{
		printf("error reading command line %s for arg %s", e.error().c_str(), e.argId().c_str());
		return cpp14::make_unique<cMemorySettingsRepository>();
	}
}


////////////////////////////////////////////////////////////////////////////////
// main:

int main(int argc, char **argv)
{
	
	#if defined(_MSC_VER) && defined(_DEBUG) && defined(ENABLE_LEAK_FINDER)
	InitLeakFinder();
	#endif

	// Magic code to produce dump-files on Windows if the server crashes:
	#if defined(_WIN32) && !defined(_WIN64) && defined(_MSC_VER)
	HINSTANCE hDbgHelp = LoadLibrary("DBGHELP.DLL");
	g_WriteMiniDump = (pMiniDumpWriteDump)GetProcAddress(hDbgHelp, "MiniDumpWriteDump");
	if (g_WriteMiniDump != nullptr)
	{
		_snprintf_s(g_DumpFileName, ARRAYCOUNT(g_DumpFileName), _TRUNCATE, "crash_mcs_%x.dmp", GetCurrentProcessId());
		SetUnhandledExceptionFilter(LastChanceExceptionFilter);
		
		// Parse arguments for minidump flags:
		for (int i = 0; i < argc; i++)
		{
			if (_stricmp(argv[i], "/cdg") == 0)
			{
				// Add globals to the dump
				g_DumpFlags = (MINIDUMP_TYPE)(g_DumpFlags | MiniDumpWithDataSegs);
			}
			else if (_stricmp(argv[i], "/cdf") == 0)
			{
				// Add full memory to the dump (HUUUGE file)
				g_DumpFlags = (MINIDUMP_TYPE)(g_DumpFlags | MiniDumpWithFullMemory);
			}
		}  // for i - argv[]
	}
	#endif  // _WIN32 && !_WIN64
	// End of dump-file magic

	#if defined(_DEBUG) && defined(_MSC_VER)
	_CrtSetDbgFlag ( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	
	// _X: The simple built-in CRT leak finder - simply break when allocating the Nth block ({N} is listed in the leak output)
	// Only useful when the leak is in the same sequence all the time
	// _CrtSetBreakAlloc(85950);
	
	#endif  // _DEBUG && _MSC_VER

	#ifndef _DEBUG
	std::signal(SIGSEGV, NonCtrlHandler);
	std::signal(SIGTERM, NonCtrlHandler);
	std::signal(SIGINT,  NonCtrlHandler);
	std::signal(SIGABRT, NonCtrlHandler);
	#ifdef SIGABRT_COMPAT
	std::signal(SIGABRT_COMPAT, NonCtrlHandler);
	#endif  // SIGABRT_COMPAT
	#endif

	// DEBUG: test the dumpfile creation:
	// *((int *)0) = 0;
	
	auto argsRepo = parseArguments(argc, argv);
	
	#if defined(_WIN32)
	// Attempt to run as a service
	if (cRoot::m_RunAsService)
	{
		SERVICE_TABLE_ENTRY ServiceTable[] =
		{
			{ SERVICE_NAME, (LPSERVICE_MAIN_FUNCTION)serviceMain },
			{ NULL, NULL }
		};

		if (StartServiceCtrlDispatcher(ServiceTable) == FALSE)
		{
			LOGERROR("Attempted, but failed, service startup.");
			return GetLastError();
		}
	}
	else
	#endif
	{
		// Not running as a service, do normal startup
		universalMain(std::move(argsRepo));
	}

	#if defined(_MSC_VER) && defined(_DEBUG) && defined(ENABLE_LEAK_FINDER)
	DeinitLeakFinder();
	#endif

	return EXIT_SUCCESS;
}




