#include "pch.h"
#include "rpakfilesystem.h"
#include "hooks.h"
#include "hookutils.h"
#include "modmanager.h"
#include "dedicated.h"

typedef void* (*LoadCommonPaksForMapType)(char* map);
LoadCommonPaksForMapType LoadCommonPaksForMap;

typedef void* (*LoadPakSyncType)(const char* path, void* unknownSingleton, int flags);
typedef int (*LoadPakAsyncType)(const char* path, void* unknownSingleton, int flags, void* callback0, void* callback1);
typedef void* (*UnloadPakType)(int pakHandle, void* callback);
typedef void* (*ReadFullFileFromDiskType)(const char* requestedPath, void* a2);

// there are more i'm just too lazy to add
struct PakLoadFuncs
{
	void* unk0[2];
	LoadPakSyncType LoadPakSync;
	LoadPakAsyncType LoadPakAsync;
	void* unk1[2];
	UnloadPakType UnloadPak;
	void* unk2[17];
	ReadFullFileFromDiskType ReadFullFileFromDisk;
};

PakLoadFuncs* g_pakLoadApi;
void** pUnknownPakLoadSingleton;

PakLoadManager* g_PakLoadManager;
void PakLoadManager::LoadPakSync(const char* path)
{
	g_pakLoadApi->LoadPakSync(path, *pUnknownPakLoadSingleton, 0);
}
void PakLoadManager::LoadPakAsync(const char* path, bool bMarkForUnload)
{
	int handle = g_pakLoadApi->LoadPakAsync(path, *pUnknownPakLoadSingleton, 2, nullptr, nullptr);

	if (bMarkForUnload)
		m_pakHandlesToUnload.push_back(handle);
}

void PakLoadManager::UnloadPaks()
{
	for (int pakHandle : m_pakHandlesToUnload)
		g_pakLoadApi->UnloadPak(pakHandle, nullptr);

	m_pakHandlesToUnload.clear();
}

void HandlePakAliases(char** map)
{
	// convert the pak being loaded to it's aliased one, e.g. aliasing mp_hub_timeshift => sp_hub_timeshift
	for (int64_t i = g_ModManager->m_loadedMods.size() - 1; i > -1; i--)
	{
		Mod* mod = &g_ModManager->m_loadedMods[i];
		if (!mod->Enabled)
			continue;

		if (mod->RpakAliases.find(*map) != mod->RpakAliases.end())
		{
			*map = &mod->RpakAliases[*map][0];
			return;
		}
	}
}

void LoadPreloadPaks()
{
	// note, loading from ./ is necessary otherwise paks will load from gamedir/r2/paks
	for (Mod& mod : g_ModManager->m_loadedMods)
	{
		if (!mod.Enabled)
			continue;

		// need to get a relative path of mod to mod folder
		fs::path modPakPath("./" / mod.ModDirectory / "paks");

		for (ModRpakEntry& pak : mod.Rpaks)
			if (pak.m_bAutoLoad)
				g_PakLoadManager->LoadPakAsync((modPakPath / pak.m_sPakName).string().c_str(), false);
	}
}

void LoadCustomMapPaks(char** pakName, bool* bNeedToFreePakName)
{
	// whether the vanilla game has this rpak
	bool bHasOriginalPak = fs::exists(fs::path("./r2/paks/Win64/") / *pakName);

	// note, loading from ./ is necessary otherwise paks will load from gamedir/r2/paks
	for (Mod& mod : g_ModManager->m_loadedMods)
	{
		if (!mod.Enabled)
			continue;

		// need to get a relative path of mod to mod folder
		fs::path modPakPath("./" / mod.ModDirectory / "paks");

		for (ModRpakEntry& pak : mod.Rpaks)
		{
			if (!pak.m_bAutoLoad && !pak.m_sPakName.compare(*pakName))
			{
				// if the game doesn't have the original pak, let it handle loading this one as if it was the one it was loading originally
				if (!bHasOriginalPak)
				{
					std::string path = (modPakPath / pak.m_sPakName).string();
					*pakName = new char[path.size() + 1];
					strcpy(*pakName, &path[0]);
					(*pakName)[path.size()] = '\0';

					bHasOriginalPak = true;
					*bNeedToFreePakName =
						true; // we can't free this memory until we're done with the pak, so let whatever's calling this deal with it
				}
				else
					g_PakLoadManager->LoadPakAsync((modPakPath / pak.m_sPakName).string().c_str(), true);
			}
		}
	}
}

LoadPakAsyncType LoadPakAsyncOriginal;
int LoadPakAsyncHook(char* path, void* unknownSingleton, int flags, void* callback0, void* callback1)
{
	HandlePakAliases(&path);

	bool bNeedToFreePakName = false;

	static bool bShouldLoadPaks = true;
	if (bShouldLoadPaks)
	{
		// make a copy of the path for comparing to determine whether we should load this pak on dedi, before it could get overwritten by LoadCustomMapPaks
		std::string originalPath(path);

		// disable preloading while we're doing this
		bShouldLoadPaks = false;

		LoadPreloadPaks();
		LoadCustomMapPaks(&path, &bNeedToFreePakName);

		bShouldLoadPaks = true;

		// do this after custom paks load and in bShouldLoadPaks so we only ever call this on the root pakload call
		// todo: could probably add some way to flag custom paks to not be loaded on dedicated servers in rpak.json
		if (IsDedicated() && strncmp(&originalPath[0], "common", 6)) // dedicated only needs common and common_mp
		{
			spdlog::info("Not loading pak {} for dedicated server", originalPath);
			return -1;	
		}
	}

	int ret = LoadPakAsyncOriginal(path, unknownSingleton, flags, callback0, callback1);
	spdlog::info("LoadPakAsync {} {}", path, ret);

	if (bNeedToFreePakName)
		delete[] path;

	return ret;
}

UnloadPakType UnloadPakOriginal;
void* UnloadPakHook(int pakHandle, void* callback)
{
	static bool bShouldUnloadPaks = true;
	if (bShouldUnloadPaks)
	{
		bShouldUnloadPaks = false;
		g_PakLoadManager->UnloadPaks();
		bShouldUnloadPaks = true;
	}

	spdlog::info("UnloadPak {}", pakHandle);
	return UnloadPakOriginal(pakHandle, callback);
}

// we hook this exclusively for resolving stbsp paths, but seemingly it's also used for other stuff like vpk and rpak loads
// possibly just async loading all together?
ReadFullFileFromDiskType ReadFullFileFromDiskOriginal;
void* ReadFullFileFromDiskHook(const char* requestedPath, void* a2)
{
	fs::path path(requestedPath);
	char* allocatedNewPath = nullptr;

	if (path.extension() == ".stbsp")
	{
		fs::path filename = path.filename();
		spdlog::info("LoadStreamBsp: {}", filename.string());

		// resolve modded stbsp path so we can load mod stbsps
		auto modFile = g_ModManager->m_modFiles.find(fs::path("maps" / filename).lexically_normal().string());
		if (modFile != g_ModManager->m_modFiles.end())
		{
			// need to allocate a new string for this
			std::string newPath = (modFile->second.owningMod->ModDirectory / "mod" / modFile->second.path).string();
			allocatedNewPath = new char[newPath.size() + 1];
			strncpy(allocatedNewPath, newPath.c_str(), newPath.size());
			allocatedNewPath[newPath.size()] = '\0';
			requestedPath = allocatedNewPath;
		}
	}

	void* ret = ReadFullFileFromDiskOriginal(requestedPath, a2);
	if (allocatedNewPath)
		delete[] allocatedNewPath;

	return ret;
}

ON_DLL_LOAD("engine.dll", RpakFilesystem, (HMODULE baseAddress)
{
	g_PakLoadManager = new PakLoadManager;

	g_pakLoadApi = *(PakLoadFuncs**)((char*)baseAddress + 0x5BED78);
	pUnknownPakLoadSingleton = (void**)((char*)baseAddress + 0x7C5E20);

	HookEnabler hook;
	ENABLER_CREATEHOOK(hook, g_pakLoadApi->LoadPakAsync, &LoadPakAsyncHook, reinterpret_cast<LPVOID*>(&LoadPakAsyncOriginal));
	ENABLER_CREATEHOOK(hook, g_pakLoadApi->UnloadPak, &UnloadPakHook, reinterpret_cast<LPVOID*>(&UnloadPakOriginal));
	ENABLER_CREATEHOOK(
		hook, g_pakLoadApi->ReadFullFileFromDisk, &ReadFullFileFromDiskHook, reinterpret_cast<LPVOID*>(&ReadFullFileFromDiskOriginal));
})