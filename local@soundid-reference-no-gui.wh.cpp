// ==WindhawkMod==
// @id              soundid-reference-no-gui
// @name            SoundID Reference - No GUI
// @description     Prevents the SoundID Reference GUI from opening while keeping the audio engine running.
// @version         1.0
// @author          Evgen
// @include         SoundID Reference.exe
// @compilerOptions -luser32
// ==/WindhawkMod==

#include <windows.h>
#include <string>

namespace {

using ShowWindow_t = BOOL (WINAPI*)(HWND, int);
using SetWindowPos_t = BOOL (WINAPI*)(
    HWND,
    HWND,
    int,
    int,
    int,
    int,
    UINT
);

ShowWindow_t OriginalShowWindow = nullptr;
SetWindowPos_t OriginalSetWindowPos = nullptr;

DWORD g_processId = 0;

thread_local bool g_insideHook = false;


// ============================================================
// Check if this is a SoundID window
// ============================================================

bool IsSoundIDWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd))
        return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    if (pid != g_processId)
        return false;

    wchar_t title[512] = {};
    wchar_t className[256] = {};

    GetWindowTextW(
        hwnd,
        title,
        ARRAYSIZE(title)
    );

    GetClassNameW(
        hwnd,
        className,
        ARRAYSIZE(className)
    );

    std::wstring titleText(title);
    std::wstring classText(className);

    auto contains = [](const std::wstring& text,
                       const wchar_t* value) {

        if (text.empty() || !value)
            return false;

        std::wstring a = text;
        std::wstring b = value;

        for (auto& c : a)
            c = towlower(c);

        for (auto& c : b)
            c = towlower(c);

        return a.find(b) != std::wstring::npos;
    };


    // SoundID title
    if (contains(titleText, L"SoundID"))
        return true;

    if (contains(titleText, L"Reference"))
        return true;


    // SoundID / JUCE classes
    if (contains(classText, L"SoundID"))
        return true;

    return false;
}


// ============================================================
// ShowWindow hook
// ============================================================

BOOL WINAPI HookShowWindow(
    HWND hwnd,
    int nCmdShow
) {
    if (g_insideHook)
        return OriginalShowWindow(hwnd, nCmdShow);

    if (IsSoundIDWindow(hwnd)) {

        // Block commands that would make the GUI visible.
        switch (nCmdShow) {

            case SW_SHOW:
            case SW_SHOWNORMAL:
            case SW_SHOWDEFAULT:
            case SW_SHOWMAXIMIZED:
            case SW_SHOWMINIMIZED:
            case SW_RESTORE:
            case SW_SHOWNOACTIVATE:
            case SW_SHOWNA:

                return TRUE;
        }
    }

    return OriginalShowWindow(
        hwnd,
        nCmdShow
    );
}


// ============================================================
// SetWindowPos hook
// ============================================================

BOOL WINAPI HookSetWindowPos(
    HWND hwnd,
    HWND hWndInsertAfter,
    int X,
    int Y,
    int cx,
    int cy,
    UINT uFlags
) {
    if (g_insideHook)
        return OriginalSetWindowPos(
            hwnd,
            hWndInsertAfter,
            X,
            Y,
            cx,
            cy,
            uFlags
        );


    if (IsSoundIDWindow(hwnd)) {

        // Prevent SetWindowPos from activating/showing
        // the SoundID GUI.
        if (uFlags & SWP_SHOWWINDOW) {

            uFlags &= ~SWP_SHOWWINDOW;
            uFlags |= SWP_NOACTIVATE;
        }
    }


    return OriginalSetWindowPos(
        hwnd,
        hWndInsertAfter,
        X,
        Y,
        cx,
        cy,
        uFlags
    );
}


// ============================================================
// Worker thread
// ============================================================

DWORD WINAPI WorkerThread(
    LPVOID
) {
    // Hooks are installed by Windhawk's runtime.
    // Keep thread alive for SoundID.
    while (true) {
        Sleep(1000);
    }

    return 0;
}

}


// ============================================================
// Windhawk
// ============================================================

BOOL Wh_ModInit() {

    g_processId = GetCurrentProcessId();

    // Windhawk automatically resolves these hooks.
    Wh_SetFunctionHook(
        (void*)ShowWindow,
        (void*)HookShowWindow,
        (void**)&OriginalShowWindow
    );

    Wh_SetFunctionHook(
        (void*)SetWindowPos,
        (void*)HookSetWindowPos,
        (void**)&OriginalSetWindowPos
    );

    return TRUE;
}


void Wh_ModUninit() {

    // Windhawk automatically restores hooks.

}