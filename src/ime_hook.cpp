#include "ime_hook.h"

#include "Debug.h"
#include "core\Functions.h"
#include "mygui\MyGUI_InputManager.h"
#include <queue>
#include <stdio.h>

// Deferred digits / spaces are flushed by this timer, on the game thread,
// through the subclassed window procedure.
#define IME_FLUSH_TIMER_ID 0xADE1
#define IME_FLUSH_DELAY_MS 80

// How long a committed CJK character keeps counting as "the IME is in use".
#define IME_CJK_ACTIVE_MS 5000

namespace
{
    // Cached handle to the game's main window
    HWND g_hwnd = nullptr;

    // Original window procedure (so we can forward messages after hooking)
    WNDPROC g_originalWndProc = nullptr;

    // Tracks IME state
    bool g_imeOpen = false;        // IME is enabled (e.g. Japanese/Chinese input active)
    bool g_imeComposing = false;  // IME is actively composing text (pre-confirmation phase)

    // WH_GETMESSAGE hook handle to capture WM_CHAR / WM_IME_CHAR from TSF IMEs
    HHOOK g_hGetMsgHook = nullptr;

    // FIFO for characters captured by the hook (UTF-16 wchar_t)
    std::queue<wchar_t> g_charQueue;

    // Synchronize access to g_charQueue
    CRITICAL_SECTION g_cs;

    // Defined below, but also used by ProcessQueuedChars above it.
    void ImeDiagLog(const char* source, wchar_t ch, const char* status);

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

    LRESULT CALLBACK GetMsgProc(int nCode, WPARAM wParam, LPARAM lParam)
    {
        if (nCode >= 0)
        {
            MSG* pMsg = (MSG*)lParam;

            // Capture WM_CHAR/WM_IME_CHAR for the game window and queue them
            if (pMsg->hwnd == g_hwnd || IsChild(g_hwnd, pMsg->hwnd))
            {
                if (pMsg->message == WM_CHAR || pMsg->message == WM_IME_CHAR)
                {
                    EnterCriticalSection(&g_cs);
                    g_charQueue.push((wchar_t)pMsg->wParam);
                    LeaveCriticalSection(&g_cs);
                }
            }
        }
        return CallNextHookEx(g_hGetMsgHook, nCode, wParam, lParam);
    }

    void ProcessQueuedChars()
    {
        if (!MyGUI::InputManager::getInstancePtr())
            return;

        auto& im = MyGUI::InputManager::getInstance();
        if (!im.isFocusKey())
            return;

        // Drain queued characters and inject them into MyGUI as text input.
        EnterCriticalSection(&g_cs);
        while (!g_charQueue.empty())
        {
            wchar_t ch = g_charQueue.front();
            g_charQueue.pop();
            LeaveCriticalSection(&g_cs);

            // Inject as text-only key press (KeyCode::None)
            im.injectKeyPress(MyGUI::KeyCode::None, (MyGUI::Char)ch);
            im.injectKeyRelease(MyGUI::KeyCode::None);
            ImeDiagLog("queue-flush", ch, "injected");

            EnterCriticalSection(&g_cs);
        }
        LeaveCriticalSection(&g_cs);
    }

    // -------------------------------------------------------------------
    // Character injection (fixed).
    //
    // The original code queued WM_CHAR / WM_IME_CHAR into g_charQueue and
    // only flushed that queue from inside the MyGUI injectKeyPress hook.
    // The WH_GETMESSAGE hook was installed with GetCurrentThreadId() from
    // the worker thread that DllMain created, and that thread never pumps
    // messages, so GetMsgProc never ran, the queue stayed empty, and every
    // character swallowed by WndProc was lost for good. That is why a few
    // Chinese characters could not be typed at all.
    //
    // Characters are now injected right here in WndProc. Only when the GUI
    // or the keyboard focus is not ready yet is a character queued, and the
    // injectKeyPress hook flushes that queue later.
    // -------------------------------------------------------------------

#define IME_DIAG_LOG 0

#if IME_DIAG_LOG
    void ImeDiagLog(const char* source, wchar_t ch, const char* status)
    {
        char buf[192];
        sprintf_s(buf, sizeof(buf), "IME: %s U+%04X %s\n", source,
            (unsigned int)(ch & 0xFFFF), status ? status : "");
        ErrorLog(buf);
    }
#else
    void ImeDiagLog(const char* /*source*/, wchar_t /*ch*/, const char* /*status*/) { }
#endif

    void QueueChar(wchar_t ch)
    {
        EnterCriticalSection(&g_cs);
        g_charQueue.push(ch);
        LeaveCriticalSection(&g_cs);
    }

    int QueuedCharCount()
    {
        EnterCriticalSection(&g_cs);
        int count = (int)g_charQueue.size();
        LeaveCriticalSection(&g_cs);
        return count;
    }

    bool IsDigitOrSpaceChar(wchar_t ch)
    {
        return ch == 0x20 || (ch >= 0x30 && ch <= 0x39);
    }

    // Throw away deferred digits / spaces: a real commit proved that they
    // were the keystrokes used to pick a candidate.
    void PurgeQueuedDigitsAndSpaces()
    {
        std::queue<wchar_t> kept;

        EnterCriticalSection(&g_cs);
        while (!g_charQueue.empty())
        {
            wchar_t ch = g_charQueue.front();
            g_charQueue.pop();
            if (!IsDigitOrSpaceChar(ch))
                kept.push(ch);
        }
        g_charQueue = kept;
        LeaveCriticalSection(&g_cs);
    }

    // Hold a digit / space back until we know whether the IME consumed it.
    void DeferChar(wchar_t ch)
    {
        QueueChar(ch);
        if (g_hwnd)
            SetTimer(g_hwnd, IME_FLUSH_TIMER_ID, IME_FLUSH_DELAY_MS, NULL);
    }

    // Inject now. Returns false when that is impossible at the moment
    // (GUI not ready yet, or no keyboard focus).
    bool TryInjectChar(wchar_t ch)
    {
        if (!MyGUI::InputManager::getInstancePtr())
            return false;

        auto& im = MyGUI::InputManager::getInstance();
        if (!im.isFocusKey())
            return false;

        im.injectKeyPress(MyGUI::KeyCode::None, (MyGUI::Char)ch);
        im.injectKeyRelease(MyGUI::KeyCode::None);
        return true;
    }

    // -------------------------------------------------------------------
    // Twin-message bookkeeping for a single IME commit.
    //
    // Log evidence from RE_Kenshi_log.txt (Sogou, Unicode window):
    //
    //   IME: WM_IME_CHAR   U+662F injected
    //   IME: GCS_RESULTSTR U+662F injected        (about 1 ms later)
    //
    // One commit is delivered through more than one message, so the same
    // character has to be inserted exactly once. Every delivery records
    // what it did as (character, tick, source), and a delivery only skips
    // a character when the record comes from a different source. Records
    // of the same source never match each other, so typing one character
    // twice in a row is never swallowed by accident.
    // -------------------------------------------------------------------
    enum ImeCharSource
    {
        IME_SRC_MSG = 1,     // WM_IME_CHAR, or WM_CHAR during composition
        IME_SRC_COMP = 2,    // GCS_RESULTSTR composition result
        IME_SRC_PASS = 3     // plain WM_CHAR, handed over to the game
    };

    struct ImeRecentChar
    {
        wchar_t ch;
        DWORD tick;
        int source;
    };

    ImeRecentChar g_recentChars[32];
    int g_recentCount = 0;
    const DWORD IME_DUP_WINDOW_MS = 150;

    // IME activity bookkeeping for the digits / Space decision.
    bool g_candidateOpen = false;           // IMN_OPENCANDIDATE .. IMN_CLOSECANDIDATE
    DWORD g_lastCommitTick = 0;             // last WM_IME_CHAR / GCS_RESULTSTR commit
    DWORD g_lastCjkCommitTick = 0;          // last committed non-ASCII character
    const DWORD IME_COMMIT_GUARD_MS = 250;  // same keystroke as the commit

    void DropExpiredRecords(DWORD now)
    {
        int kept = 0;
        for (int i = 0; i < g_recentCount; ++i)
        {
            if (now - g_recentChars[i].tick <= IME_DUP_WINDOW_MS)
                g_recentChars[kept++] = g_recentChars[i];
        }

        g_recentCount = kept;
    }

    void RememberChar(wchar_t ch, int source)
    {
        DWORD now = GetTickCount();
        DropExpiredRecords(now);

        if (g_recentCount >= 32)
        {
            for (int j = 0; j < 31; ++j)
                g_recentChars[j] = g_recentChars[j + 1];

            g_recentCount = 31;
        }

        g_recentChars[g_recentCount].ch = ch;
        g_recentChars[g_recentCount].tick = now;
        g_recentChars[g_recentCount].source = source;
        ++g_recentCount;
    }

    // True when ch was already delivered by sourceToMatch a moment ago.
    // The matching record is consumed.
    bool ConsumeTwin(wchar_t ch, int sourceToMatch)
    {
        DropExpiredRecords(GetTickCount());

        for (int i = 0; i < g_recentCount; ++i)
        {
            if (g_recentChars[i].ch == ch && g_recentChars[i].source == sourceToMatch)
            {
                for (int j = i; j < g_recentCount - 1; ++j)
                    g_recentChars[j] = g_recentChars[j + 1];

                --g_recentCount;
                return true;
            }
        }
        return false;
    }

    // wParam of WM_CHAR / WM_IME_CHAR: a UTF-16 code unit for a Unicode
    // window, a single byte for an ANSI window (DBCS lead + trail bytes
    // have to be paired up before the conversion).
    wchar_t DecodeCharMessage(HWND hWnd, WPARAM wParam)
    {
        if (IsWindowUnicode(hWnd))
            return (wchar_t)(wParam & 0xFFFF);

        static BYTE s_leadByte = 0;
        BYTE b = (BYTE)(wParam & 0xFF);
        wchar_t wch = 0;

        if (s_leadByte != 0)
        {
            char pair[2];
            pair[0] = (char)s_leadByte;
            pair[1] = (char)b;
            s_leadByte = 0;

            if (MultiByteToWideChar(CP_ACP, 0, pair, 2, &wch, 1) != 1)
                wch = 0;

            return wch;
        }

        if (IsDBCSLeadByteEx(CP_ACP, b))
        {
            s_leadByte = b;
            return 0;
        }

        if (MultiByteToWideChar(CP_ACP, 0, (const char*)&b, 1, &wch, 1) != 1)
            wch = 0;

        return wch;
    }

    // A character arrived: inject it now when possible, otherwise queue it
    // so the injectKeyPress hook can flush it later.
    void HandleIncomingChar(wchar_t ch, const char* source)
    {
        if (ch == 0)
            return;

        // A committed character means the IME just handled a keystroke.
        g_lastCommitTick = GetTickCount();
        if (ch >= 0x80)
            g_lastCjkCommitTick = g_lastCommitTick;

        // A committed character that is not a digit or space proves the IME
        // was using the keyboard: any digit still waiting in the deferred
        // queue was a candidate-selection key, not text.
        if (!IsDigitOrSpaceChar(ch) && QueuedCharCount() > 0)
        {
            PurgeQueuedDigitsAndSpaces();
            ImeDiagLog("commit", ch, "purged deferred digits/spaces");
        }

        bool injected = TryInjectChar(ch);
        if (!injected)
            QueueChar(ch);

        ImeDiagLog(source, ch, injected ? "injected" : "queued (no focus)");
    }

    bool InstallTSF()
    {
        // Injection happens directly in WndProc now (see the WM_CHAR and
        // WM_IME_CHAR cases), so the WH_GETMESSAGE hook is not needed:
        //
        //   SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, nullptr, GetCurrentThreadId())
        //
        // was called from ImeHook::Install(), which runs on the worker
        // thread created in DllMain. A WH_GETMESSAGE hook only sees the
        // messages of the thread it is bound to, and that worker thread
        // never pumps messages, so GetMsgProc never executed.
        InitializeCriticalSection(&g_cs);
        g_hGetMsgHook = nullptr;
        return true;
    }

    void CleanupTSF()
    {
        // Uninstall hook and cleanup
        if (g_hGetMsgHook)
        {
            UnhookWindowsHookEx(g_hGetMsgHook);
            g_hGetMsgHook = nullptr;
        }
        DeleteCriticalSection(&g_cs);
    }

    // Pointer to original MyGUI input function
    bool (*injectKeyPress_orig)(MyGUI::InputManager* thisptr, MyGUI::KeyCode _key, MyGUI::Char _text) = 0;

    // True while the IME really has pre-edit text open. Unlike the
    // WM_IME_STARTCOMPOSITION / WM_IME_ENDCOMPOSITION pair this cannot get
    // stuck, so it is used to correct g_imeComposing.
    bool IsCompositionActive(HWND hWnd)
    {
        HIMC hIMC = ImmGetContext(hWnd);
        if (!hIMC)
            return false;

        LONG size = ImmGetCompositionStringW(hIMC, GCS_COMPSTR, NULL, 0);
        ImmReleaseContext(hWnd, hIMC);
        return size > 0;
    }

    // True while the IME is using the keyboard: pre-edit text, an open
    // candidate list, or the moment right after a commit (the keystroke
    // that picked the candidate arrives together with the commit).
    // Digits, the numpad and Space must not reach the game in that state.
    bool IsImeConsumingKeys(HWND hWnd)
    {
        if (g_imeComposing || g_candidateOpen)
            return true;

        if (g_lastCommitTick != 0 &&
            GetTickCount() - g_lastCommitTick <= IME_COMMIT_GUARD_MS)
            return true;

        HIMC hIMC = ImmGetContext(hWnd);
        if (!hIMC)
            return false;

        DWORD count = 0;
        BOOL have = ImmGetCandidateListCountW(hIMC, &count);
        ImmReleaseContext(hWnd, hIMC);
        return have && count > 0;
    }

    // True when the IME should own the digits / space keys. Sogou never
    // sends IMN_SETOPENSTATUS, so g_imeOpen alone is not enough: a recently
    // committed CJK character or a live ImmGetOpenStatus answer counts too.
    bool ImeActiveNow(HWND hWnd)
    {
        if (g_imeOpen || g_imeComposing || g_candidateOpen)
            return true;

        if (g_lastCjkCommitTick != 0 &&
            GetTickCount() - g_lastCjkCommitTick <= IME_CJK_ACTIVE_MS)
            return true;

        return IsImeOpen(hWnd);
    }

    // Keys that must keep working while an IME is open even though the
    // whitelist above does not list them: digits and the numeric keypad.
    // A Chinese IME uses digits to pick candidates, and with no composition
    // active they are ordinary text that has to reach the game.
    bool IsImeExtraAllowedKey(MyGUI::KeyCode key)
    {
        switch (key.getValue())
        {
        case MyGUI::KeyCode::Zero:
        case MyGUI::KeyCode::One:
        case MyGUI::KeyCode::Two:
        case MyGUI::KeyCode::Three:
        case MyGUI::KeyCode::Four:
        case MyGUI::KeyCode::Five:
        case MyGUI::KeyCode::Six:
        case MyGUI::KeyCode::Seven:
        case MyGUI::KeyCode::Eight:
        case MyGUI::KeyCode::Nine:
        case MyGUI::KeyCode::Numpad0:
        case MyGUI::KeyCode::Numpad1:
        case MyGUI::KeyCode::Numpad2:
        case MyGUI::KeyCode::Numpad3:
        case MyGUI::KeyCode::Numpad4:
        case MyGUI::KeyCode::Numpad5:
        case MyGUI::KeyCode::Numpad6:
        case MyGUI::KeyCode::Numpad7:
        case MyGUI::KeyCode::Numpad8:
        case MyGUI::KeyCode::Numpad9:
        case MyGUI::KeyCode::NumpadEnter:
            return true;
        default:
            return false;
        }
    }

    // Hooked version of MyGUI::InputManager::injectKeyPress
    bool InjectKeyPress_hook(MyGUI::InputManager* thisptr, MyGUI::KeyCode key, MyGUI::Char text)
    {
        ProcessQueuedChars();

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
                // Digits, the numpad and Space are ordinary text while the
                // IME is idle, but they are the IME own keys while it
                // composes or lists candidates ("1 = mingtian"). Only the
                // idle case may reach the game.
                if (IsImeExtraAllowedKey(key) ||
                    key.getValue() == MyGUI::KeyCode::Space)
                {
                    if (IsImeConsumingKeys(g_hwnd))
                    {
                        ImeDiagLog("key-ime", (wchar_t)key.getValue(),
                            "blocked (IME is using this key)");
                        return false;
                    }

                    // A digit reaches MyGUI twice: once as this key event
                    // and once as the WM_CHAR that the character path
                    // already handles. While an IME is open, let only the
                    // character path type it, otherwise it lands twice.
                    // The deferred character is flushed right away anyway,
                    // because this very key event runs the hook first.
                    if (ImeActiveNow(g_hwnd) && IsImeExtraAllowedKey(key))
                    {
                        ImeDiagLog("key-ime", (wchar_t)key.getValue(),
                            "blocked (digit typed through the character path)");
                        return false;
                    }

                    ImeDiagLog("key-ime", (wchar_t)key.getValue(),
                        "allowed (IME idle)");
                    break;
                }

                // Ctrl combinations must keep working while an IME is open:
                // Ctrl+V paste, Ctrl+C/X/A/Z and so on. Raw IME pre-edit
                // input never uses Ctrl, so this cannot leak pinyin letters.
                if (thisptr && thisptr->isControlPressed())
                {
                    ImeDiagLog("key-ctrl", (wchar_t)key.getValue(),
                        "allowed (ime open)");
                    break;
                }

                return false; // block all other keys (prevents double input / garbage input)
            }
        }

        // Keep g_imeComposing honest. The message flags alone are not
        // reliable: some IMEs start a composition without ever sending
        // WM_IME_ENDCOMPOSITION, which left the flag stuck and blocked
        // Backspace, digits and Ctrl+V until the game was restarted.
        if ((g_imeComposing || g_imeOpen) && key != MyGUI::KeyCode::None)
        {
            bool live = IsCompositionActive(g_hwnd);
            if (live != g_imeComposing)
            {
                ImeDiagLog("composing", live ? (wchar_t)1 : (wchar_t)0,
                    live ? "set (live composition)" : "cleared (stale flag)");
                g_imeComposing = live;
            }
        }

        // While IME is composing (candidate selection phase), block key input
        // into MyGUI. Ctrl combinations stay allowed (an IME never uses Ctrl
        // for pre-edit text).
        if (g_imeComposing && key != MyGUI::KeyCode::None)
        {
            if (thisptr && thisptr->isControlPressed())
            {
                ImeDiagLog("key-ctrl", (wchar_t)key.getValue(),
                    "allowed (composing)");
                return injectKeyPress_orig(thisptr, key, text);
            }

            ImeDiagLog("key-blocked", (wchar_t)key.getValue(),
                "blocked (live composition)");
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

        {
            char buf[128];
            sprintf_s(buf, sizeof(buf), "IME: WndProc hooked unicode=%d\n",
                IsWindowUnicode(g_hwnd) ? 1 : 0);
            ErrorLog(buf);
        }

        return InstallTSF();
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
        else
        {
            ErrorLog("IME: MyGUI injectKeyPress hook installed\n");
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

        // Deferred digits / spaces are flushed here, on the game thread.
        case WM_TIMER:
            if (wParam == IME_FLUSH_TIMER_ID)
            {
                KillTimer(hWnd, IME_FLUSH_TIMER_ID);
                ProcessQueuedChars();

                if (QueuedCharCount() > 0)
                    SetTimer(hWnd, IME_FLUSH_TIMER_ID, IME_FLUSH_DELAY_MS, NULL);
                else
                    ImeDiagLog("timer", (wchar_t)0, "deferred queue flushed");

                return 0;
            }
            return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);

        case WM_IME_NOTIFY:
            // IME open/close state changed
            if (wParam == IMN_SETOPENSTATUS)
            {
                g_imeOpen = IsImeOpen(hWnd);
                ImeDiagLog("notify", (wchar_t)wParam,
                    g_imeOpen ? "IMN_SETOPENSTATUS open" : "IMN_SETOPENSTATUS closed");
            }
            else if (wParam == IMN_OPENCANDIDATE)
            {
                g_candidateOpen = true;
                ImeDiagLog("notify", (wchar_t)wParam, "IMN_OPENCANDIDATE");
            }
            else if (wParam == IMN_CLOSECANDIDATE)
            {
                g_candidateOpen = false;
                ImeDiagLog("notify", (wchar_t)wParam, "IMN_CLOSECANDIDATE");
            }
            else
            {
                ImeDiagLog("notify", (wchar_t)wParam, "other notify");
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
            {
                // During a composition a WM_CHAR is IME input, not committed
                // text: the digit that picks a candidate, the space that
                // confirms one, pre-edit letters. Injecting it here is what
                // put the selection digit into the text box. The commit
                // itself arrives as WM_IME_CHAR / GCS_RESULTSTR, which are
                // injected separately. g_imeComposing is kept truthful by
                // the live IMM32 check, so this only fires on a real
                // composition. Dropped, but never silently.
                ImeDiagLog("WM_CHAR(composing)",
                    (wchar_t)(wParam & 0xFFFF),
                    "dropped (IME input, not a commit)");
                return 0;
            }
            else
            {
                wchar_t ch = DecodeCharMessage(hWnd, wParam);

                // Digits and space: while an IME is open they may be the
                // keys that pick a candidate ("1 = mingtian"). No IME state
                // probe can see Sogou candidate window, so the character is
                // deferred instead: if a real commit follows, it was a
                // selection key and is thrown away; otherwise the flush
                // timer types it as ordinary text.
                if (IsDigitOrSpaceChar(ch))
                {
                    if (IsImeConsumingKeys(hWnd))
                    {
                        ImeDiagLog("WM_CHAR", ch,
                            "dropped (IME is using this key)");
                        return 0;
                    }

                    if (ImeActiveNow(hWnd))
                    {
                        DeferChar(ch);
                        ImeDiagLog("WM_CHAR", ch, "deferred (IME active)");
                        return 0;
                    }

                    ImeDiagLog("WM_CHAR", ch, "passed to game (IME idle)");
                }

                // A plain WM_CHAR that is the twin of a delivery we already
                // made for the same commit: letting it through would make
                // the game insert the character a second time.
                if (ch >= 0x20 &&
                    (ConsumeTwin(ch, IME_SRC_MSG) || ConsumeTwin(ch, IME_SRC_COMP)))
                {
                    ImeDiagLog("WM_CHAR", ch, "twin of IME delivery, swallowed");
                    return 0;
                }

                if (ch >= 0x20)
                {
                    RememberChar(ch, IME_SRC_PASS);
                    ImeDiagLog("WM_CHAR", ch, "passed to game");
                }
            }
            return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);

        // WM_IME_CHAR: the old code always returned 0, assuming the
        // composition result path had already injected the character.
        // It actually arrives BEFORE GCS_RESULTSTR (see the log above), so
        // that assumption silently dropped every character delivered this
        // way. This message now injects the character itself and only gives
        // way to a delivery that already happened.
        case WM_IME_CHAR:
        {
            wchar_t ch = DecodeCharMessage(hWnd, wParam);
            if (ch >= 0x20)
            {
                if (ConsumeTwin(ch, IME_SRC_COMP) || ConsumeTwin(ch, IME_SRC_PASS))
                {
                    ImeDiagLog("WM_IME_CHAR", ch, "twin of another delivery, dropped");
                    return 0;
                }

                RememberChar(ch, IME_SRC_MSG);
                HandleIncomingChar(ch, "WM_IME_CHAR");
            }
            else
            {
                ImeDiagLog("WM_IME_CHAR", (wchar_t)(wParam & 0xFFFF),
                    "control/lead byte, dropped");
            }
            return 0;
        }

        case WM_IME_COMPOSITION:
        {
            // Call original WndProc first
            LRESULT result = CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);

            // If composition produced a finalized string
            if (lParam & GCS_RESULTSTR)
            {
                // The commit ends the composition, even when the IME never
                // sends WM_IME_ENDCOMPOSITION.
                if (g_imeComposing)
                {
                    g_imeComposing = false;
                    ImeDiagLog("composing", (wchar_t)0, "cleared (commit)");
                }

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

                        // The very same commit also arrives as WM_IME_CHAR
                        // (one message per character) and possibly as a plain
                        // WM_CHAR. Those deliveries record themselves, so a
                        // character that is already in the game is skipped
                        // here instead of being inserted a second time.
                        for (size_t i = 0; i < text.size(); ++i)
                        {
                            wchar_t ch = text[i];
                            if (ConsumeTwin(ch, IME_SRC_MSG) || ConsumeTwin(ch, IME_SRC_PASS))
                            {
                                ImeDiagLog("GCS_RESULTSTR", ch, "twin already delivered, skipped");
                                continue;
                            }

                            RememberChar(ch, IME_SRC_COMP);
                            HandleIncomingChar(ch, "GCS_RESULTSTR");
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