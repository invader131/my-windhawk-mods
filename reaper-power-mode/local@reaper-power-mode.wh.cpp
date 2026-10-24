// ==WindhawkMod==
// @id              reaper-power-mode
// @name            REAPER Power Mode
// @description     Switches to the "Music" power plan and Best Performance power mode while reaper.exe is running, then restores the previous settings
// @version         1.0
// @author          you
// @include         explorer.exe
// @compilerOptions -lpowrprof
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# REAPER Power Mode

While `reaper.exe` is running, this mod:
1. activates the power plan named **Music** (name is configurable), and
2. sets the Windows power mode (overlay) to **Best performance**.

When the last REAPER process closes, the power plan and power mode that were
active before are restored.

The mod runs a small watcher thread inside explorer.exe (only one instance
runs, guarded by a named mutex). It also restores the settings when the mod is
disabled while REAPER is running.

Notes:
- The power plan "Music" must already exist (`powercfg /list`).
- The power mode (overlay) API needs Windows 10 1709 or newer.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- planName: Music
  $name: Power plan name
- processName: reaper.exe
  $name: Process to watch
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <powrprof.h>
#include <tlhelp32.h>
#include <string>

// "Best performance" power mode overlay
static const GUID kOverlayBestPerformance =
    {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};

typedef DWORD (WINAPI *PowerSetActiveOverlayScheme_t)(GUID OverlaySchemeGuid);
typedef DWORD (WINAPI *PowerGetActualOverlayScheme_t)(GUID* ActualOverlayGuid);

static PowerSetActiveOverlayScheme_t pSetOverlay = nullptr;
static PowerGetActualOverlayScheme_t pGetOverlay = nullptr;

// Per-power-source power mode (what Settings > Power mode shows for
// "On battery" and "Plugged in"). Undocumented, loaded dynamically.
typedef DWORD (WINAPI *PowerSetUserMode_t)(const GUID* PowerModeGuid);
typedef DWORD (WINAPI *PowerGetUserMode_t)(GUID* PowerModeGuid);

static PowerSetUserMode_t pSetAcMode = nullptr;
static PowerSetUserMode_t pSetDcMode = nullptr;
static PowerGetUserMode_t pGetAcMode = nullptr;
static PowerGetUserMode_t pGetDcMode = nullptr;

static GUID g_savedAcMode = {};
static GUID g_savedDcMode = {};
static bool g_haveSavedAc = false;
static bool g_haveSavedDc = false;

static HANDLE g_thread = nullptr;
static HANDLE g_stopEvent = nullptr;
static HANDLE g_mutex = nullptr;

static bool g_applied = false;
static GUID g_savedScheme = {};
static GUID g_savedOverlay = {};
static bool g_haveSavedOverlay = false;

static GUID g_plan = {};

// Display settings that live inside each power plan. Without syncing them,
// switching plans changes brightness and turns off adaptive (auto) brightness.
static const GUID kVideoSubgroup =
    {0x7516b95f, 0xf776, 0x4464, {0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99}};
static const GUID kBrightness =
    {0xaded5e82, 0xb909, 0x4619, {0x99, 0x49, 0xf5, 0xd7, 0x1d, 0xac, 0x0b, 0xcb}};
static const GUID kAdaptiveBrightness =
    {0xfbd9aa66, 0x9553, 0x4097, {0xba, 0x44, 0xed, 0x6e, 0x9d, 0x65, 0xea, 0xb8}};

static void CopyVideoSetting(const GUID& from, const GUID& to, const GUID& setting) {
    DWORD v;
    if (PowerReadACValueIndex(nullptr, &from, &kVideoSubgroup, &setting, &v) == ERROR_SUCCESS) {
        PowerWriteACValueIndex(nullptr, &to, &kVideoSubgroup, &setting, v);
    }
    if (PowerReadDCValueIndex(nullptr, &from, &kVideoSubgroup, &setting, &v) == ERROR_SUCCESS) {
        PowerWriteDCValueIndex(nullptr, &to, &kVideoSubgroup, &setting, v);
    }
}

static void SyncDisplaySettings(const GUID& from, const GUID& to) {
    CopyVideoSetting(from, to, kBrightness);
    CopyVideoSetting(from, to, kAdaptiveBrightness);
}

static std::wstring GetSetting(PCWSTR name, PCWSTR fallback) {
    PCWSTR v = Wh_GetStringSetting(name);
    std::wstring s = (v && *v) ? v : fallback;
    Wh_FreeStringSetting(v);
    return s;
}

static bool IsProcessRunning(const std::wstring& exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = {sizeof(pe)};
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static bool FindPlanByName(const std::wstring& name, GUID* out) {
    for (ULONG i = 0;; i++) {
        GUID guid;
        DWORD size = sizeof(guid);
        DWORD r = PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, i,
                                 (UCHAR*)&guid, &size);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;

        WCHAR buf[256] = {};
        DWORD bufSize = sizeof(buf) - sizeof(WCHAR);
        if (PowerReadFriendlyName(nullptr, &guid, nullptr, nullptr,
                                  (PUCHAR)buf, &bufSize) == ERROR_SUCCESS) {
            if (_wcsicmp(buf, name.c_str()) == 0) {
                *out = guid;
                return true;
            }
        }
    }
    return false;
}

static void ApplyPerformance(const std::wstring& planName) {
    if (g_applied) return;

    GUID plan;
    if (!FindPlanByName(planName, &plan)) {
        Wh_Log(L"Power plan '%s' not found", planName.c_str());
        // Still mark as applied-attempted? No: retry next poll in case it is created later.
        return;
    }

    // Save current state
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) == ERROR_SUCCESS && active) {
        g_savedScheme = *active;
        LocalFree(active);
    } else {
        return;
    }

    g_haveSavedOverlay = false;
    if (pGetOverlay && pGetOverlay(&g_savedOverlay) == ERROR_SUCCESS) {
        g_haveSavedOverlay = true;
    }
    g_haveSavedAc = pGetAcMode && pGetAcMode(&g_savedAcMode) == ERROR_SUCCESS;
    g_haveSavedDc = pGetDcMode && pGetDcMode(&g_savedDcMode) == ERROR_SUCCESS;

    // Carry brightness + auto brightness over into the Music plan before switching
    g_plan = plan;
    SyncDisplaySettings(g_savedScheme, plan);

    // Plan first, then overlay (switching plans can reset the overlay)
    PowerSetActiveScheme(nullptr, &plan);
    // Both "plugged in" and "on battery", then the currently active overlay
    if (pSetAcMode) pSetAcMode(&kOverlayBestPerformance);
    if (pSetDcMode) pSetDcMode(&kOverlayBestPerformance);
    if (pSetOverlay) {
        pSetOverlay(kOverlayBestPerformance);
    }

    g_applied = true;
    Wh_Log(L"Applied plan '%s' + Best performance", planName.c_str());
}

static void RestoreSettings() {
    if (!g_applied) return;

    // Carry any brightness change made during the session back to the original plan
    SyncDisplaySettings(g_plan, g_savedScheme);
    PowerSetActiveScheme(nullptr, &g_savedScheme);
    if (pSetAcMode && g_haveSavedAc) pSetAcMode(&g_savedAcMode);
    if (pSetDcMode && g_haveSavedDc) pSetDcMode(&g_savedDcMode);
    if (pSetOverlay && g_haveSavedOverlay) {
        pSetOverlay(g_savedOverlay);
    }

    g_applied = false;
    Wh_Log(L"Restored previous power settings");
}

static DWORD WINAPI WatchThread(LPVOID) {
    do {
        std::wstring proc = GetSetting(L"processName", L"reaper.exe");
        std::wstring plan = GetSetting(L"planName", L"Music");

        bool running = IsProcessRunning(proc);
        if (running && !g_applied) {
            ApplyPerformance(plan);
        } else if (!running && g_applied) {
            RestoreSettings();
        }
    } while (WaitForSingleObject(g_stopEvent, 1500) == WAIT_TIMEOUT);

    RestoreSettings();
    return 0;
}

BOOL Wh_ModInit() {
    // Only one watcher across all explorer.exe instances
    g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\WindhawkReaperPowerModeMutex");
    if (!g_mutex) return TRUE;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        return TRUE;
    }

    HMODULE powrprof = LoadLibraryW(L"powrprof.dll");
    if (powrprof) {
        pSetOverlay = (PowerSetActiveOverlayScheme_t)GetProcAddress(
            powrprof, "PowerSetActiveOverlayScheme");
        pGetOverlay = (PowerGetActualOverlayScheme_t)GetProcAddress(
            powrprof, "PowerGetActualOverlayScheme");
        pSetAcMode = (PowerSetUserMode_t)GetProcAddress(
            powrprof, "PowerSetUserConfiguredACPowerMode");
        pSetDcMode = (PowerSetUserMode_t)GetProcAddress(
            powrprof, "PowerSetUserConfiguredDCPowerMode");
        pGetAcMode = (PowerGetUserMode_t)GetProcAddress(
            powrprof, "PowerGetUserConfiguredACPowerMode");
        pGetDcMode = (PowerGetUserMode_t)GetProcAddress(
            powrprof, "PowerGetUserConfiguredDCPowerMode");
    }
    if (!pSetAcMode || !pSetDcMode) {
        Wh_Log(L"Per-source power mode API not available; only the active power mode will be changed");
    }
    if (!pSetOverlay) {
        Wh_Log(L"Power mode overlay API not available; only the power plan will be switched");
    }

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    return TRUE;
}

void Wh_ModUninit() {
    if (g_thread) {
        SetEvent(g_stopEvent);
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    if (g_mutex) {
        ReleaseMutex(g_mutex);
        CloseHandle(g_mutex);
        g_mutex = nullptr;
    }
}