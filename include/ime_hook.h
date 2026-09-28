#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <imm.h>

namespace ImeHook
{
    // Called from DllMain/InitThread to install the WndProc hook
    bool Install();

    // Called from startPlugin() to install the MyGUI::InputManager::injectKeyPress hook
    void InstallKeyHooks();

    LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
}
