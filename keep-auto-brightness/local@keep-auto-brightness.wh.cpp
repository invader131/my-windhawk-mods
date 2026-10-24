// ==WindhawkMod==
// @id              keep-auto-brightness
// @name            Keep Auto Brightness Enabled
// @description     Makes sure "adaptive brightness" (change brightness automatically when lighting changes) stays enabled in the active power plan, on battery and plugged in
// @version         1.0
// @author          you
// @include         explorer.exe
// @compilerOptions -lpowrprof
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Keep Auto Brightness Enabled

Checks the active power plan every few seconds. If adaptive brightness has been
switched off (for either AC or battery), the mod switches it back on.

Notes:
- Your device needs an ambient light sensor for auto brightness to do anything.
- If the setting can't be changed, try running Windhawk with administrator rights.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- checkIntervalSeconds: 5
  $name: Check interval (seconds)
  $description: How often to verify that adaptive brightness is still enabled
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <powrprof.h>

// GUID_VIDEO_SUBGROUP
static const GUID kSubVideo = {0x7516b95f, 0xf776, 0x4464,
                               {0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99}};
// GUID_VIDEO_ADAPTIVE_DISPLAY_BRIGHTNESS
static const GUID kAdaptiveBrightness = {0xfbd9aa66, 0x9553, 0x4097,
                                         {0xba, 0x44, 0xed, 0x6e, 0x9d, 0x65, 0xea, 0xb8}};

static HANDLE g_stopEvent = nullptr;
static HANDLE g_thread = nullptr;
static DWORD g_intervalMs = 5000;

static void EnforceAutoBrightness() {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) {
        return;
    }

    bool changed = false;
    DWORD value = 0;

    if (PowerReadACValueIndex(nullptr, scheme, &kSubVideo, &kAdaptiveBrightness, &value) == ERROR_SUCCESS &&
        value != 1) {
        if (PowerWriteACValueIndex(nullptr, scheme, &kSubVideo, &kAdaptiveBrightness, 1) == ERROR_SUCCESS) {
            changed = true;
            Wh_Log(L"Re-enabled adaptive brightness (AC)");
        }
    }

    if (PowerReadDCValueIndex(nullptr, scheme, &kSubVideo, &kAdaptiveBrightness, &value) == ERROR_SUCCESS &&
        value != 1) {
        if (PowerWriteDCValueIndex(nullptr, scheme, &kSubVideo, &kAdaptiveBrightness, 1) == ERROR_SUCCESS) {
            changed = true;
            Wh_Log(L"Re-enabled adaptive brightness (battery)");
        }
    }

    if (changed) {
        // Re-apply the scheme so the change takes effect immediately.
        PowerSetActiveScheme(nullptr, scheme);
    }

    LocalFree(scheme);
}

static DWORD WINAPI WorkerThread(LPVOID) {
    do {
        EnforceAutoBrightness();
    } while (WaitForSingleObject(g_stopEvent, g_intervalMs) == WAIT_TIMEOUT);
    return 0;
}

static void LoadSettings() {
    int seconds = Wh_GetIntSetting(L"checkIntervalSeconds");
    if (seconds < 1) seconds = 1;
    g_intervalMs = (DWORD)seconds * 1000;
}

BOOL Wh_ModInit() {
    LoadSettings();

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        return FALSE;
    }

    g_thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (!g_thread) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        return FALSE;
    }

    return TRUE;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

void Wh_ModUninit() {
    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}