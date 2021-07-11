#include "pch.h"
#include "hooks.h"
#include "hookutils.h"
#include "dedicated.h"
#include "tier0.h"

#include <wchar.h>
#include <iostream>
#include <vector>

typedef HMODULE(*LoadLibraryExAType)(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
HMODULE LoadLibraryExAHook(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);

typedef HMODULE(*LoadLibraryExWType)(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
HMODULE LoadLibraryExWHook(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);

LoadLibraryExAType LoadLibraryExAOriginal;
LoadLibraryExWType LoadLibraryExWOriginal;

void InstallInitialHooks()
{
	//AllocConsole();

	if (MH_Initialize() != MH_OK)
		Error("MH_Initialize failed");
	
	HookEnabler hook;
	ENABLER_CREATEHOOK(hook, &LoadLibraryExA, &LoadLibraryExAHook, reinterpret_cast<LPVOID*>(&LoadLibraryExAOriginal));
	ENABLER_CREATEHOOK(hook, &LoadLibraryExW, &LoadLibraryExWHook, reinterpret_cast<LPVOID*>(&LoadLibraryExWOriginal));
}

// dll load callback stuff
// this allows for code to register callbacks to be run as soon as a dll is loaded, mainly to allow for patches to be made on dll load
struct DllLoadCallback
{
	std::string dll;
	DllLoadCallbackFuncType callback;
	bool called;
};

std::vector<DllLoadCallback*> dllLoadCallbacks;

void AddDllLoadCallback(std::string dll, DllLoadCallbackFuncType callback)
{
	DllLoadCallback* callbackStruct = new DllLoadCallback;
	callbackStruct->dll = dll;
	callbackStruct->callback = callback;
	callbackStruct->called = false;

	dllLoadCallbacks.push_back(callbackStruct);
}

HMODULE LoadLibraryExAHook(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
	HMODULE moduleAddress = LoadLibraryExAOriginal(lpLibFileName, hFile, dwFlags);
	
	if (moduleAddress)
	{
		for (auto& callbackStruct : dllLoadCallbacks)
		{
			if (!callbackStruct->called && strstr(lpLibFileName + (strlen(lpLibFileName) - strlen(callbackStruct->dll.c_str())), callbackStruct->dll.c_str()) != nullptr)
			{
				callbackStruct->callback(moduleAddress);
				callbackStruct->called = true;
			}
		}
	}

	return moduleAddress;
}

HMODULE LoadLibraryExWHook(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
	HMODULE moduleAddress = LoadLibraryExWOriginal(lpLibFileName, hFile, dwFlags);

	if (moduleAddress)
	{
		for (auto& callbackStruct : dllLoadCallbacks)
		{
			const wchar_t* callbackDll = std::wstring(callbackStruct->dll.begin(), callbackStruct->dll.end()).c_str();
			if (!callbackStruct->called && wcsstr(lpLibFileName + (wcslen(lpLibFileName) - wcslen(callbackDll)), callbackDll) != nullptr)
			{
				callbackStruct->callback(moduleAddress);
				callbackStruct->called = true;
			}
		}
	}

	return moduleAddress;
}