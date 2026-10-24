// ==WindhawkMod==
// @id              bluetooth-lid-toggle
// @name            Bluetooth Lid Toggle
// @description     Turns Bluetooth off when the laptop lid is closed and back on when it is opened
// @version         1.0.0
// @author          you
// @include         explorer.exe
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -luser32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Bluetooth Lid Toggle

Turns Bluetooth off when the laptop lid is closed and on again when it is opened.

- Uses the Windows lid-switch power notification (`GUID_LIDSWITCH_STATE_CHANGE`).
- Uses the Windows Radio API (the same one the Settings toggle uses), so no admin rights are needed.
- Only turns Bluetooth back on if this mod was the one that turned it off, so if you had
  Bluetooth off yourself before closing the lid, it stays off.

Note: if your power plan is set to "Sleep" or "Hibernate" when the lid closes, the
machine may suspend before or while the radio is switched off. Set
*Control Panel > Power Options > Choose what closing the lid does* to "Do nothing"
if you want the mod to act while the laptop keeps running (e.g. with an external monitor).
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- restoreOnOpen: true
  $name: Turn Bluetooth back on when the lid opens
  $description: Only restores it if this mod turned it off.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Radios.h>

using namespace winrt::Windows::Devices::Radios;

// {BA3E0F4D-B817-4094-A2D1-D56379E6A0F3}
static const GUID kLidSwitchGuid = {
    0xba3e0f4d, 0xb817, 0x4094, {0xa2, 0xd1, 0xd5, 0x63, 0x79, 0xe6, 0xa0, 0xf3}};

static HANDLE g_mutex = nullptr;
static HANDLE g_thread = nullptr;
static HWND g_hwnd = nullptr;
static bool g_restoreOnOpen = true;
static bool g_weDisabled = false;

static void LoadSettings() {
    g_restoreOnOpen = Wh_GetIntSetting(L"restoreOnOpen") != 0;
}

static void SetBluetooth(bool on) {
    try {
        auto access = Radio::RequestAccessAsync().get();
        if (access != RadioAccessStatus::Allowed) {
            Wh_Log(L"Radio access denied: %d", (int)access);
            return;
        }

        auto radios = Radio::GetRadiosAsync().get();
        for (auto const& radio : radios) {
            if (radio.Kind() != RadioKind::Bluetooth) continue;

            if (!on) {
                if (radio.State() == RadioState::On) {
                    radio.SetStateAsync(RadioState::Off).get();
                    g_weDisabled = true;
                    Wh_Log(L"Bluetooth turned off");
                }
            } else {
                if (g_weDisabled && radio.State() == RadioState::Off) {
                    radio.SetStateAsync(RadioState::On).get();
                    Wh_Log(L"Bluetooth turned on");
                }
            }
        }
        if (on) g_weDisabled = false;
    } catch (winrt::hresult_error const& e) {
        Wh_Log(L"Radio API error: 0x%08X", (unsigned)e.code().value);
    } catch (...) {
        Wh_Log(L"Unknown error toggling Bluetooth");
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_POWERBROADCAST && wParam == PBT_POWERSETTINGCHANGE) {
        auto* ps = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
        if (ps && IsEqualGUID(ps->PowerSetting, kLidSwitchGuid) &&
            ps->DataLength >= sizeof(DWORD)) {
            DWORD state = *reinterpret_cast<DWORD*>(ps->Data);  // 0 = closed, 1 = open
            Wh_Log(L"Lid state: %s", state == 0 ? L"closed" : L"open");
            if (state == 0) {
                SetBluetooth(false);
            } else if (g_restoreOnOpen) {
                SetBluetooth(true);
            }
        }
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI MessageThread(LPVOID) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WindhawkBluetoothLidToggle";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        Wh_Log(L"CreateWindowEx failed: %u", GetLastError());
        winrt::uninit_apartment();
        return 1;
    }

    HPOWERNOTIFY notify = RegisterPowerSettingNotification(
        g_hwnd, &kLidSwitchGuid, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!notify) {
        Wh_Log(L"RegisterPowerSettingNotification failed: %u", GetLastError());
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (notify) UnregisterPowerSettingNotification(notify);
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    // Don't leave Bluetooth off if the mod is unloaded while the lid is closed.
    if (g_weDisabled) SetBluetooth(true);

    winrt::uninit_apartment();
    return 0;
}

BOOL Wh_ModInit() {
    // explorer.exe can have several instances; only let one run the listener.
    g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\WindhawkBluetoothLidToggleMutex");
    if (!g_mutex) return FALSE;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        return TRUE;  // another instance already handles it
    }

    LoadSettings();
    g_thread = CreateThread(nullptr, 0, MessageThread, nullptr, 0, nullptr);
    return g_thread != nullptr;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

void Wh_ModUninit() {
    if (g_thread) {
        if (g_hwnd) PostMessageW(g_hwnd, WM_QUIT, 0, 0);
        PostThreadMessageW(GetThreadId(g_thread), WM_QUIT, 0, 0);
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_mutex) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
    }
}