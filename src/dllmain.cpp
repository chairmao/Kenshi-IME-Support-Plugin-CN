#include "ime_hook.h"

namespace
{
    DWORD WINAPI InitThread(LPVOID)
    {
        // WndProc hook only needs the window handle so its safe to install it here
        ImeHook::Install();
        return 0;
    }
}

// Called by RE_Kenshi after the game has fully initialised
__declspec(dllexport) void startPlugin()
{
    ImeHook::InstallKeyHooks();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}