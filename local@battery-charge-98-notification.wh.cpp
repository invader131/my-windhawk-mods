// ==WindhawkMod==
// @id              battery-charge-98-notification
// @name            Battery 98% Charging Notification
// @description     Shows a Windows notification when the battery reaches 98% while charging.
// @version         1.0
// @author          Evgen
// @include         explorer.exe
// @compilerOptions -lole32 -luser32
// ==/WindhawkMod==

#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>


// ------------------------------------------------------------
// Settings
// ------------------------------------------------------------

#define BATTERY_THRESHOLD 98
#define CHECK_INTERVAL_MS 10000

// ------------------------------------------------------------
// Notification state
// ------------------------------------------------------------

static HANDLE g_thread = nullptr;
static HANDLE g_stopEvent = nullptr;

static bool g_notifiedThisCharge = false;

// ------------------------------------------------------------
// Simple Windows balloon notification
// ------------------------------------------------------------

void ShowBatteryNotification()
{
    NOTIFYICONDATAW nid = {};
    
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.uFlags = NIF_INFO | NIF_ICON | NIF_TIP;

    nid.hWnd = GetDesktopWindow();
    nid.uID = 1001;

    nid.hIcon = LoadIconW(nullptr, IDI_INFORMATION);

    StringCchCopyW(
        nid.szTip,
        ARRAYSIZE(nid.szTip),
        L"Battery Monitor"
    );

    StringCchCopyW(
        nid.szInfoTitle,
        ARRAYSIZE(nid.szInfoTitle),
        L"Battery reached 98%"
    );

    StringCchCopyW(
        nid.szInfo,
        ARRAYSIZE(nid.szInfo),
        L"Your PC is still connected to the charger. You can unplug it now."
    );

    nid.dwInfoFlags = NIIF_INFO;

    Shell_NotifyIconW(NIM_ADD, &nid);

    // Keep the icon long enough for Windows to display
    Sleep(100);

    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// ------------------------------------------------------------
// Battery status
// ------------------------------------------------------------

bool IsCharging()
{
    SYSTEM_POWER_STATUS status = {};

    if (!GetSystemPowerStatus(&status))
        return false;

    return status.ACLineStatus == 1;
}

int GetBatteryPercent()
{
    SYSTEM_POWER_STATUS status = {};

    if (!GetSystemPowerStatus(&status))
        return -1;

    return static_cast<int>(status.BatteryLifePercent);
}

// ------------------------------------------------------------
// Monitoring thread
// ------------------------------------------------------------

DWORD WINAPI BatteryMonitorThread(LPVOID)
{
    while (true)
    {
        DWORD result = WaitForSingleObject(
            g_stopEvent,
            CHECK_INTERVAL_MS
        );

        if (result == WAIT_OBJECT_0)
            break;

        bool charging = IsCharging();
        int battery = GetBatteryPercent();

        // Charger disconnected:
        // reset the notification state so the next charge cycle
        // can trigger another notification.
        if (!charging)
        {
            g_notifiedThisCharge = false;
            continue;
        }

        // Charger connected and battery reached threshold.
        if (charging &&
            battery >= BATTERY_THRESHOLD &&
            !g_notifiedThisCharge)
        {
            g_notifiedThisCharge = true;

            ShowBatteryNotification();
        }
    }

    return 0;
}

// ------------------------------------------------------------
// Windhawk lifecycle
// ------------------------------------------------------------

BOOL Wh_ModInit()
{
    g_stopEvent = CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr
    );

    if (!g_stopEvent)
        return FALSE;

    g_thread = CreateThread(
        nullptr,
        0,
        BatteryMonitorThread,
        nullptr,
        0,
        nullptr
    );

    if (!g_thread)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        return FALSE;
    }

    return TRUE;
}

void Wh_ModUninit()
{
    if (g_stopEvent)
    {
        SetEvent(g_stopEvent);
    }

    if (g_thread)
    {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }

    if (g_stopEvent)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}