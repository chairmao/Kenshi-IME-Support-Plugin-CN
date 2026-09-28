#include "ime_hook.h"

#include "Debug.h"
#include "core\Functions.h"
#include "mygui\MyGUI_InputManager.h"

namespace
{
    // Cached handle to the game's main window
    HWND g_hwnd = nullptr;

    // Original window procedure (so we can forward messages after hooking)
    WNDPROC g_originalWndProc = nullptr;

    // Tracks IME state
    bool g_imeOpen = false;        // IME is enabled (e.g. Japanese/Chinese input active)
    bool g_imeComposing = false;  // IME is actively composing text (pre-confirmation phase)

    // Enumerates all top-level windows to find this process's main visible window
    BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);

        // Only consider windows belonging to this process
        if (pid == GetCurrentProcessId())
        {
            // Only pick a top-level, visible window (skip owned/hidden ones)
            if (GetWindow(hwnd, GW_OWNER) == NULL && IsWindowVisible(hwnd))
            {
                g_hwnd = hwnd;
                return FALSE; // Stop enumeration once found
            }
        }
        return TRUE; // Continue searching
    }

    // Attempts to locate the game's main window
    HWND FindGameWindow()
    {
        g_hwnd = nullptr;
        EnumWindows(EnumWindowsCallback, 0);
        return g_hwnd;
    }

    // Queries Windows IME subsystem to determine if IME is currently open for this window
    bool IsImeOpen(HWND hWnd)
    {
        HIMC hIMC = ImmGetContext(hWnd);
        if (!hIMC) return false;

        BOOL open = ImmGetOpenStatus(hIMC);
        ImmReleaseContext(hWnd, hIMC);

        return open == TRUE;
    }


    // Pointer to original MyGUI input function
    bool (*injectKeyPress_orig)(MyGUI::InputManager* thisptr, MyGUI::KeyCode _key, MyGUI::Char _text) = 0;

    // Hooked version of MyGUI::InputManager::injectKeyPress
    bool InjectKeyPress_hook(MyGUI::InputManager* thisptr, MyGUI::KeyCode key, MyGUI::Char text)
    {
        // If IME is open, suppress normal key presses
        if (g_imeOpen && key != MyGUI::KeyCode::None)
        {
            switch (key.getValue())
            {
            // Explicit whitelist of non-text keys that should still work while IME is open
            case MyGUI::KeyCode::Backspace:
            case MyGUI::KeyCode::Delete:
            case MyGUI::KeyCode::ArrowLeft:
            case MyGUI::KeyCode::ArrowRight:
            case MyGUI::KeyCode::ArrowUp:
            case MyGUI::KeyCode::ArrowDown:
            case MyGUI::KeyCode::Return:
            case MyGUI::KeyCode::Escape:

            case MyGUI::KeyCode::Home:
            case MyGUI::KeyCode::End:
            case MyGUI::KeyCode::PageUp:
            case MyGUI::KeyCode::PageDown:
            case MyGUI::KeyCode::Tab:
            case MyGUI::KeyCode::Insert:

            case MyGUI::KeyCode::LeftShift:
            case MyGUI::KeyCode::RightShift:
            case MyGUI::KeyCode::LeftControl:
            case MyGUI::KeyCode::RightControl:
            case MyGUI::KeyCode::LeftAlt:
            case MyGUI::KeyCode::RightAlt:

            case MyGUI::KeyCode::Convert:
                break; // allowed
            default:
                return false; // block all other keys (prevents double input / garbage input)
            }
        }

        // While IME is composing (candidate selection phase), block ALL key input into MyGUI
        if (g_imeComposing && key != MyGUI::KeyCode::None)
        {
            return false;
        }

        // Forward to original function if not blocked
        return injectKeyPress_orig(thisptr, key, text);
    }

}

namespace ImeHook
{
    bool Install()
    {
        // Wait until the game window is available
        while (!(g_hwnd = FindGameWindow()))
            Sleep(100);

        // Capture original WndProc
        g_originalWndProc = (WNDPROC)GetWindowLongPtr(g_hwnd, GWLP_WNDPROC);
        if (!g_originalWndProc)
        {
            return false;
        }

        // Replace WndProc with our hook
        SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
        return true;
    }

    void InstallKeyHooks()
    {
        HMODULE hMyGUI = nullptr;

        // Wait until MyGUI module is loaded
        while (!(hMyGUI = GetModuleHandleW(L"MyGUIEngine_x64.dll")))
            Sleep(100);

        // Resolve mangled symbol for InputManager::injectKeyPress
        intptr_t injectKeyPressAddr = (intptr_t)GetProcAddress(
            hMyGUI,
            "?injectKeyPress@InputManager@MyGUI@@QEAA_NUKeyCode@2@I@Z"
        );

        // Install hook
        if (KenshiLib::SUCCESS != KenshiLib::AddHook(injectKeyPressAddr, &InjectKeyPress_hook, &injectKeyPress_orig))
        {
			ErrorLog("Failed to install injectKeyPress hook\n");
        }
    }

    void EnsureInstalled()
    {
        // Re-hook WndProc if something overwrote it
        if (!g_hwnd) return;

        WNDPROC current = (WNDPROC)GetWindowLongPtr(g_hwnd, GWLP_WNDPROC);
        if (current != WndProc)
        {
            g_originalWndProc = current;
            SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
        }
    }

    // Custom window procedure to intercept IME + keyboard messages
    LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        switch (uMsg)
        {
        // Let system key events pass through untouched
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);

        // Input language changed (IME switching etc) — don't interfere
        case WM_INPUTLANGCHANGE:
            return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);

        // IME context setup — pass through
        case WM_IME_SETCONTEXT:
            return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);

        case WM_IME_NOTIFY:
            // IME open/close state changed
            if (wParam == IMN_SETOPENSTATUS)
            {
                g_imeOpen = IsImeOpen(hWnd);
            }
            return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);

        case WM_IME_STARTCOMPOSITION:
            // User started composing (pre-confirmation text)
            g_imeComposing = true;
            return DefWindowProc(hWnd, uMsg, wParam, lParam);

        case WM_IME_ENDCOMPOSITION:
            // Composition finalized or cancelled
            g_imeComposing = false;
            return DefWindowProc(hWnd, uMsg, wParam, lParam);

        case WM_IME_REQUEST:
            return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);

        // Block raw key input while IME is composing
        case WM_KEYDOWN:
            if (g_imeComposing)
                return 0;
            return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);

        case WM_KEYUP:
            if (g_imeComposing)
                return 0;
            return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);

        case WM_CHAR:
            if (g_imeComposing)
                return 0;
            return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);

        // Ignore IME-generated WM_IME_CHAR (we handle composition manually)
        case WM_IME_CHAR:
            return 0;

        case WM_IME_COMPOSITION:
        {
            // Let original WndProc process it first
            LRESULT result = CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);

            // If composition produced a finalized string
            if (lParam & GCS_RESULTSTR)
            {
                HIMC hIMC = ImmGetContext(hWnd);
                if (hIMC)
                {
                    // Get required buffer size
                    LONG size = ImmGetCompositionStringW(hIMC, GCS_RESULTSTR, NULL, 0);

                    if (size > 0)
                    {
                        // Retrieve composed UTF-16 string
                        std::wstring text(size / sizeof(wchar_t), L'\0');
                        ImmGetCompositionStringW(hIMC, GCS_RESULTSTR, &text[0], size);

                        // Inject resulting characters into MyGUI manually
                        if (MyGUI::InputManager::getInstancePtr())
                        {
                            auto& im = MyGUI::InputManager::getInstance();

                            // Only inject if GUI currently accepts input
                            if (im.isFocusKey())
                            {
                                for (size_t i = 0; i < text.size(); ++i)
                                {
                                    // Inject each character as a "text-only" keypress
                                    im.injectKeyPress(MyGUI::KeyCode::None, (MyGUI::Char)text[i]);
                                    im.injectKeyRelease(MyGUI::KeyCode::None);
                                }
                            }
                        }
                    }
                    ImmReleaseContext(hWnd, hIMC);
                }
            }
            return result;
        }

        default:
            // Forward all unhandled messages
            return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);
        }
    }
}