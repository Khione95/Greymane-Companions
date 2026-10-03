// Greymane Companions by Khione
//
// Recruit the Greymanes: they follow you and fight until you send them away.
// Experiment 2: Follow (F9) makes the nearest Greymane comrade follow you and
// keeps her with you; Bring back (F10) brings your comrades to you. The keys
// are in bin64\GreymaneCompanions.ini. Everything goes to the log.

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "hotkeys.h"
#include "log.h"
#include "overlay.h"
#include "toast.h"

static HMODULE g_module = NULL;

static bool IsGameProcess()
{
    char path[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    return _stricmp(name, "CrimsonDesert.exe") == 0;
}

// The keys, while the game's window is in front.
static DWORD WINAPI KeyThread(LPVOID)
{
    bool was[KEY_COUNT] = {};

    for (;;)
    {
        Sleep(30);
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        bool front = pid == GetCurrentProcessId();

        for (int i = 0; i < KEY_COUNT; ++i)
        {
            bool down = front && HotkeyDown((Key)i);

            if (down && !was[i] && i == KEY_RESCUE)
            {
                Log("%S: your comrades come to you", HotkeyName(KEY_RESCUE));
                GameRequest(REQUEST_RESCUE);
            }

            was[i] = down;
        }
    }
}

// The overlay (the notices) watches for the game's swap chain: after Party HUD
// (1.2 s), Character Creator and Seasons of Pywel (it chains onto them), well
// before the game makes it.
static DWORD WINAPI OverlayThread(LPVOID)
{
    Sleep(1400);
    ToastInit();
    OverlaySetDrawing(true);
    OverlayEarlyInit();
    return 0;
}

static DWORD WINAPI StartThread(LPVOID)
{
    // Give the game time to start: at 4 s, after Party HUD (3 s) and before
    // Character Creator (~5 s) hook game code - no two mods' hooks at once.
    Sleep(4000);

    if (GameInit())
        CloseHandle(CreateThread(NULL, 0, KeyThread, NULL, 0, NULL));

    return 0;
}

static void Start()
{
    char folder[MAX_PATH];
    GetModuleFileNameA(g_module, folder, MAX_PATH);
    char* slash = strrchr(folder, '\\');

    if (slash)
        *slash = 0;

    char bin64[MAX_PATH];
    strcpy_s(bin64, folder);
    strcat_s(folder, "\\GreymaneCompanions");
    CreateDirectoryA(folder, NULL);
    char log[MAX_PATH];
    sprintf_s(log, "%s\\GreymaneCompanions.log", folder);
    LogOpen(log);
    Log("Greymane Companions %s", GREYMANE_VERSION);
    HotkeysLoad(bin64);
    CloseHandle(CreateThread(NULL, 0, OverlayThread, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, StartThread, NULL, 0, NULL));
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);

        if (IsGameProcess())
            Start();
    }

    return TRUE;
}
