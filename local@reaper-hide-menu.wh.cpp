// ==WindhawkMod==
// @id              reaper-hide-menu
// @name            REAPER - Hide Native Menu Bar
// @description     Detaches the native Win32 menu bar from the main REAPER window without resizing it, and keeps it hidden.
// @version         1.0.0
// @author          you
// @include         reaper.exe
// @compilerOptions -luser32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# REAPER - Hide Native Menu Bar

Hides the native menu bar of the main REAPER window (class `REAPERwnd`).

* The HMENU is never destroyed; it is only detached with SetMenu(hwnd, NULL).
* GetMenu() is hooked for the main window so REAPER still "sees" its own menu
  (it keeps populating/using it normally); SetMenu() is hooked so REAPER cannot
  re-attach it and cause a flash.
* A 200 ms timer-style wait (thread blocked on an event, ~0% CPU) re-detaches
  any menu that still appears (window recreated, menu handle replaced, ...).
* Disabling the mod re-attaches the original menu.
* Debug output: view with DebugView / Windhawk's log viewer. All lines start
  with "[REAPER MENU]".
*/
// ==/WindhawkModReadme==

#include <windows.h>
#include <wchar.h>
#include <atomic>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static const WCHAR kMainWindowClass[] = L"REAPERwnd";
static const DWORD kPollIntervalMs = 200;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// Main REAPER window we manage (nullptr if none).
static std::atomic<HWND> g_reaperWindow{nullptr};

// The menu that belongs to REAPER and that we detached. Never destroyed by us.
// It is re-validated with IsMenu() before every use because REAPER may destroy
// or replace it.
static std::atomic<HMENU> g_detachedMenu{nullptr};

static HANDLE g_stopEvent = nullptr;
static HANDLE g_thread = nullptr;

// Original function pointers.
typedef BOOL (WINAPI *SetMenu_t)(HWND, HMENU);
typedef HMENU (WINAPI *GetMenu_t)(HWND);
typedef HWND (WINAPI *CreateWindowExW_t)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int,
                                          int, int, HWND, HMENU, HINSTANCE, LPVOID);

static SetMenu_t SetMenu_Original = nullptr;
static GetMenu_t GetMenu_Original = nullptr;
static CreateWindowExW_t CreateWindowExW_Original = nullptr;

// ---------------------------------------------------------------------------
// Window classification
// ---------------------------------------------------------------------------

// True if hwnd looks like the main REAPER top-level window of THIS process.
// Rejects child windows, tool windows, owned windows (dialogs, floating FX,
// plugin windows) and anything whose class is not REAPER's main class.
static bool IsMainWindowCandidate(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return false;
    }

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (style & WS_CHILD) {
        return false;
    }
    if (exStyle & WS_EX_TOOLWINDOW) {
        return false;
    }
    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return false;
    }

    WCHAR cls[64] = {};
    if (!GetClassNameW(hwnd, cls, ARRAYSIZE(cls))) {
        return false;
    }
    return _wcsicmp(cls, kMainWindowClass) == 0;
}

struct FindContext {
    HWND best = nullptr;
    int bestScore = -1;
    HWND current = nullptr;
};

static BOOL CALLBACK FindMainWindowProc(HWND hwnd, LPARAM lParam) {
    FindContext* ctx = reinterpret_cast<FindContext*>(lParam);

    if (!IsMainWindowCandidate(hwnd)) {
        return TRUE;
    }

    bool hasMenu = (GetMenu_Original(hwnd) != nullptr) ||
                   (hwnd == ctx->current && g_detachedMenu.load() != nullptr);
    bool visible = IsWindowVisible(hwnd) != FALSE;

    // Prefer the window that owns the application menu, then a visible one,
    // then stay with the window we already manage.
    int score = (hasMenu ? 4 : 0) + (visible ? 2 : 0) + (hwnd == ctx->current ? 1 : 0);
    if (score > ctx->bestScore) {
        ctx->bestScore = score;
        ctx->best = hwnd;
    }
    return TRUE;
}

static HWND FindMainWindow() {
    FindContext ctx;
    ctx.current = g_reaperWindow.load();
    EnumWindows(FindMainWindowProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// ---------------------------------------------------------------------------
// Menu detach / restore
// ---------------------------------------------------------------------------

static void DetachMenu(HWND hwnd, HMENU menu) {
    // Remember it first so the GetMenu hook can answer for REAPER immediately.
    g_detachedMenu.store(menu);
    SetMenu_Original(hwnd, nullptr);  // does NOT destroy the menu
    DrawMenuBar(hwnd);
}

static void RestoreMenu(HWND hwnd) {
    HMENU menu = g_detachedMenu.load();
    if (hwnd && IsWindow(hwnd) && menu && IsMenu(menu) &&
        GetMenu_Original(hwnd) == nullptr) {
        SetMenu_Original(hwnd, menu);
        DrawMenuBar(hwnd);
        Wh_Log(L"[REAPER MENU] Menu restored on %p", hwnd);
    }
}

// ---------------------------------------------------------------------------
// Monitoring tick (runs only from the worker thread; logs only on changes)
// ---------------------------------------------------------------------------

static void MonitorTick() {
    HWND previous = g_reaperWindow.load();
    HWND found = FindMainWindow();

    if (found != previous) {
        if (previous) {
            if (IsWindow(previous)) {
                // Another window became the main one: give the old one its menu back.
                RestoreMenu(previous);
                Wh_Log(L"[REAPER MENU] Main window changed from %p", previous);
            } else {
                Wh_Log(L"[REAPER MENU] REAPER window destroyed (%p)", previous);
            }
            // A menu belongs to its window; do not carry it over.
            g_detachedMenu.store(nullptr);
        }
        g_reaperWindow.store(found);
        if (found) {
            Wh_Log(L"[REAPER MENU] REAPER window detected (%p)", found);
        }
    }

    if (!found) {
        return;  // e.g. REAPER still initializing or between window recreations
    }

    // Drop a stored handle REAPER has destroyed.
    HMENU stored = g_detachedMenu.load();
    if (stored && !IsMenu(stored)) {
        Wh_Log(L"[REAPER MENU] Stored menu %p is no longer valid", stored);
        g_detachedMenu.store(nullptr);
        stored = nullptr;
    }

    // Real (attached) menu. Bypasses our GetMenu hook.
    HMENU current = GetMenu_Original(found);
    if (!current) {
        return;  // already hidden: nothing to do
    }

    if (!stored) {
        Wh_Log(L"[REAPER MENU] Menu detected (%p)", current);
    } else if (current != stored) {
        Wh_Log(L"[REAPER MENU] New menu detected (%p, old %p)", current, stored);
    } else {
        Wh_Log(L"[REAPER MENU] Menu re-attached (%p)", current);
    }

    DetachMenu(found, current);
    Wh_Log(L"[REAPER MENU] Menu detached (%p)", current);
}

static DWORD WINAPI MonitorThreadProc(LPVOID) {
    Wh_Log(L"[REAPER MENU] Monitoring started");
    for (;;) {
        MonitorTick();
        // Blocks on an event: ~zero CPU, wakes immediately on stop.
        if (WaitForSingleObject(g_stopEvent, kPollIntervalMs) != WAIT_TIMEOUT) {
            break;
        }
    }
    Wh_Log(L"[REAPER MENU] Monitoring stopped");
    return 0;
}

// ---------------------------------------------------------------------------
// Hooks (run on whatever thread REAPER calls from)
// ---------------------------------------------------------------------------

// REAPER asks for its window menu: keep answering with the detached menu so it
// can keep populating/using it as if it were attached.
static HMENU WINAPI GetMenu_Hook(HWND hwnd) {
    HMENU result = GetMenu_Original(hwnd);
    if (!result && hwnd) {
        HMENU detached = g_detachedMenu.load();
        if (detached && IsMenu(detached) &&
            (hwnd == g_reaperWindow.load() || IsMainWindowCandidate(hwnd))) {
            return detached;
        }
    }
    return result;
}

// REAPER tries to (re)attach a menu to its main window: remember it, keep it hidden.
static BOOL WINAPI SetMenu_Hook(HWND hwnd, HMENU menu) {
    if (hwnd && (hwnd == g_reaperWindow.load() || IsMainWindowCandidate(hwnd))) {
        if (menu) {
            if (menu != g_detachedMenu.load()) {
                Wh_Log(L"[REAPER MENU] New menu detected via SetMenu (%p)", menu);
            }
            g_detachedMenu.store(menu);
            if (g_reaperWindow.load() == nullptr) {
                g_reaperWindow.store(hwnd);
            }
            // Pretend success; the window stays menu-less (no flash).
            return TRUE;
        }
        // SetMenu(hwnd, NULL) is what we want anyway: let it through.
    }
    return SetMenu_Original(hwnd, menu);
}

// Window created with the menu in the CreateWindowEx call: strip it right away
// so the menu bar is never shown, not even for one frame.
static HWND WINAPI CreateWindowExW_Hook(DWORD exStyle, LPCWSTR className,
                                        LPCWSTR windowName, DWORD style, int x,
                                        int y, int width, int height,
                                        HWND parent, HMENU menu,
                                        HINSTANCE instance, LPVOID param) {
    bool isMain = className && !IS_INTRESOURCE(className) &&
                  _wcsicmp(className, kMainWindowClass) == 0 &&
                  !(style & WS_CHILD) && !parent &&
                  !(exStyle & WS_EX_TOOLWINDOW);

    HMENU stolen = nullptr;
    if (isMain && menu) {
        stolen = menu;
        menu = nullptr;
        // Set before creation so GetMenu during WM_CREATE already works.
        g_detachedMenu.store(stolen);
    }

    HWND hwnd = CreateWindowExW_Original(exStyle, className, windowName, style, x,
                                         y, width, height, parent, menu, instance,
                                         param);

    if (isMain) {
        if (hwnd) {
            g_reaperWindow.store(hwnd);
            Wh_Log(L"[REAPER MENU] REAPER window created (%p), menu %p withheld",
                   hwnd, stolen);
        } else if (stolen) {
            g_detachedMenu.store(nullptr);
        }
    }
    return hwnd;
}

// ---------------------------------------------------------------------------
// Windhawk entry points
// ---------------------------------------------------------------------------

BOOL Wh_ModInit() {
    // Safety net: only ever operate inside reaper.exe.
    WCHAR path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    const WCHAR* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    if (_wcsicmp(name, L"reaper.exe") != 0) {
        return FALSE;
    }

    Wh_Log(L"[REAPER MENU] Initializing");

    // Resolve originals for the functions we call directly even if hooks fail.
    SetMenu_Original = SetMenu;
    GetMenu_Original = GetMenu;
    CreateWindowExW_Original = CreateWindowExW;

    // Wh_SetFunctionHook overwrites the *_Original pointers with trampolines.
    Wh_SetFunctionHook(reinterpret_cast<void*>(GetMenu),
                       reinterpret_cast<void*>(GetMenu_Hook),
                       reinterpret_cast<void**>(&GetMenu_Original));
    Wh_SetFunctionHook(reinterpret_cast<void*>(SetMenu),
                       reinterpret_cast<void*>(SetMenu_Hook),
                       reinterpret_cast<void**>(&SetMenu_Original));
    Wh_SetFunctionHook(reinterpret_cast<void*>(CreateWindowExW),
                       reinterpret_cast<void*>(CreateWindowExW_Hook),
                       reinterpret_cast<void**>(&CreateWindowExW_Original));

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        return FALSE;
    }
    g_thread = CreateThread(nullptr, 0, MonitorThreadProc, nullptr, 0, nullptr);
    if (!g_thread) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        return FALSE;
    }
    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L"[REAPER MENU] Uninitializing");

    if (g_thread) {
        SetEvent(g_stopEvent);
        // Pump sent-messages while waiting: the worker's SetMenu/DrawMenuBar can
        // synchronously message the UI thread, which may be the thread running us.
        DWORD start = GetTickCount();
        for (;;) {
            DWORD r = MsgWaitForMultipleObjects(1, &g_thread, FALSE, 100, QS_SENDMESSAGE);
            if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) {
                break;
            }
            if (GetTickCount() - start > 3000) {
                Wh_Log(L"[REAPER MENU] Monitor thread did not stop in time");
                break;
            }
        }
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }

    // Give REAPER its menu back; the HMENU was never destroyed.
    RestoreMenu(g_reaperWindow.load());
    g_reaperWindow.store(nullptr);
    g_detachedMenu.store(nullptr);

    Wh_Log(L"[REAPER MENU] Cleanup done");
}
