// ==WindhawkMod==
// @id              hide-explorer-taskbar-icon
// @name            Hide File Explorer taskbar icon
// @description     Collapses the File Explorer button on the Windows 11 taskbar
// @version         0.1.0
// @author          you
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// ==WindhawkModSettings==
/*
- appIdMatch: Microsoft.Windows.Explorer
  $name: App ID substring to hide
  $description: Taskbar buttons whose automation ID contains this text are hidden
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>

using namespace winrt::Windows::UI::Xaml;

static std::wstring g_appIdMatch;

static void LoadSettings() {
    PCWSTR v = Wh_GetStringSetting(L"appIdMatch");
    g_appIdMatch = v ? v : L"Microsoft.Windows.Explorer";
    Wh_FreeStringSetting(v);
}

// Hide (or restore) one TaskListButton element based on its automation ID.
static void Apply(FrameworkElement const& button) {
    try {
        auto id = Automation::AutomationProperties::GetAutomationId(button);
        if (!g_appIdMatch.empty() &&
            std::wstring_view(id).find(g_appIdMatch) != std::wstring_view::npos) {
            button.Visibility(Visibility::Collapsed);
        }
    } catch (...) {
    }
}

using TaskListButton_UpdateVisualStates_t = void(WINAPI*)(void* pThis);
TaskListButton_UpdateVisualStates_t TaskListButton_UpdateVisualStates_Original;

void WINAPI TaskListButton_UpdateVisualStates_Hook(void* pThis) {
    TaskListButton_UpdateVisualStates_Original(pThis);

    // The implementation object holds its XAML element as an IInspectable
    // inside the object; fetch it via IUnknown at the object's base.
    FrameworkElement element = nullptr;
    winrt::copy_from_abi(element, *(void**)((BYTE*)pThis + sizeof(void*)));
    if (element) {
        Apply(element);
    }
}

BOOL Wh_ModInit() {
    LoadSettings();

    HMODULE taskbarView = LoadLibraryW(L"Taskbar.View.dll");
    if (!taskbarView) {
        taskbarView = LoadLibraryW(L"ExplorerExtensions.dll");
    }
    if (!taskbarView) {
        Wh_Log(L"Taskbar.View.dll not found");
        return FALSE;
    }

    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {
            {LR"(private: void __cdecl winrt::Taskbar::implementation::TaskListButton::UpdateVisualStates(void))"},
            (void**)&TaskListButton_UpdateVisualStates_Original,
            (void*)TaskListButton_UpdateVisualStates_Hook,
            false,
        },
    };

    if (!WindhawkUtils::HookSymbols(taskbarView, hooks, ARRAYSIZE(hooks))) {
        Wh_Log(L"Failed to hook TaskListButton::UpdateVisualStates");
        return FALSE;
    }

    return TRUE;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

void Wh_ModUninit() {}