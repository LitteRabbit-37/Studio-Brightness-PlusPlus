#include "Settings.h"
#include <windows.h>
#include <string>
#include <algorithm>

AppSettings g_settings;

static const wchar_t* kRegKeyPath = L"Software\\StudioBrightnessPlusPlus";
static const wchar_t* kStartupKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// Helpers
static void SetRegDWORD(HKEY hKey, const wchar_t* name, DWORD value) {
    RegSetValueExW(hKey, name, 0, REG_DWORD, (BYTE*)&value, sizeof(DWORD));
}

static DWORD GetRegDWORD(HKEY hKey, const wchar_t* name, DWORD defValue) {
    DWORD val, size = sizeof(DWORD);
    if (RegQueryValueExW(hKey, name, nullptr, nullptr, (BYTE*)&val, &size) == ERROR_SUCCESS) {
        return val;
    }
    return defValue;
}

void AppSettings::Load() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKeyPath, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        autoAdjustEnabled.store(GetRegDWORD(hKey, L"AutoBrightnessEnabled", 1) != 0);
        autoRotateEnabled.store(GetRegDWORD(hKey, L"AutoRotateEnabled", 1) != 0);
        showOSD = (GetRegDWORD(hKey, L"ShowOSD", 1) != 0); // Default to True
        runAtStartup = (GetRegDWORD(hKey, L"RunAtStartup", 0) != 0);
        enableCustomHotkeys = (GetRegDWORD(hKey, L"CustomHotkeysEnabled", 0) != 0);
        
        hkUp.mods = GetRegDWORD(hKey, L"HotkeyUpMods", 0);
        hkUp.vk   = GetRegDWORD(hKey, L"HotkeyUpVK", 0);
        
        hkDown.mods = GetRegDWORD(hKey, L"HotkeyDownMods", 0);
        hkDown.vk   = GetRegDWORD(hKey, L"HotkeyDownVK", 0);
        
        DWORD steps = GetRegDWORD(hKey, L"BrightnessSteps", 10);
        brightnessSteps = std::clamp(steps, kMinBrightnessSteps, kMaxBrightnessSteps);

        linkedMode = (GetRegDWORD(hKey, L"LinkedMode", 1) != 0);
        activeDisplayIndex = GetRegDWORD(hKey, L"ActiveDisplayIndex", 0);
        updateChannel = (int)GetRegDWORD(hKey, L"UpdateChannel", 0);

        RegCloseKey(hKey);
    }
}

void AppSettings::Save() {
    HKEY hKey;
    DWORD disp;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, &disp) == ERROR_SUCCESS) {
        SetRegDWORD(hKey, L"AutoBrightnessEnabled", autoAdjustEnabled.load() ? 1 : 0);
        SetRegDWORD(hKey, L"AutoRotateEnabled", autoRotateEnabled.load() ? 1 : 0);
        SetRegDWORD(hKey, L"ShowOSD", showOSD ? 1 : 0);
        SetRegDWORD(hKey, L"CustomHotkeysEnabled", enableCustomHotkeys ? 1 : 0);
        SetRegDWORD(hKey, L"RunAtStartup", runAtStartup ? 1 : 0);
        
        SetRegDWORD(hKey, L"HotkeyUpMods", hkUp.mods);
        SetRegDWORD(hKey, L"HotkeyUpVK", hkUp.vk);
        
        SetRegDWORD(hKey, L"HotkeyDownMods", hkDown.mods);
        SetRegDWORD(hKey, L"HotkeyDownVK", hkDown.vk);
        
        SetRegDWORD(hKey, L"BrightnessSteps", brightnessSteps);

        SetRegDWORD(hKey, L"LinkedMode", linkedMode ? 1 : 0);
        SetRegDWORD(hKey, L"ActiveDisplayIndex", activeDisplayIndex);
        SetRegDWORD(hKey, L"UpdateChannel", (DWORD)updateChannel);

        RegCloseKey(hKey);
    }
}

bool AppSettings::IsStartupEnabled() {
    HKEY hKey;
    bool enabled = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH * 2] = {0};
        DWORD   size = sizeof(value) - sizeof(wchar_t); // leave room to force-terminate
        DWORD   type = 0;
        if (RegQueryValueExW(hKey, L"StudioBrightnessPlusPlus", nullptr, &type, (BYTE*)value, &size) == ERROR_SUCCESS
            && type == REG_SZ) {
            value[size / sizeof(wchar_t)] = L'\0'; // registry strings are not guaranteed null-terminated

            // Tolerate a quoted path (we write one so spaces in Program Files are handled).
            wchar_t* stored = value;
            int vlen = lstrlenW(stored);
            if (vlen >= 2 && stored[0] == L'"' && stored[vlen - 1] == L'"') {
                stored[vlen - 1] = L'\0';
                ++stored;
            }

            // Enabled only if the stored path still points to THIS executable. Merely having the
            // Run value present is not enough: a moved/renamed exe leaves a stale, broken entry.
            wchar_t exePath[MAX_PATH];
            DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            enabled = (len > 0 && len < MAX_PATH && lstrcmpiW(stored, exePath) == 0);
        }
        RegCloseKey(hKey);
    }
    return enabled;
}

void AppSettings::SetStartup(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupKey, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH];
            DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                // Quote the path so spaces (e.g. "C:\Program Files\...") are handled correctly.
                wchar_t quoted[MAX_PATH + 4];
                quoted[0] = L'"';
                lstrcpyW(quoted + 1, exePath);
                lstrcatW(quoted, L"\"");
                RegSetValueExW(hKey, L"StudioBrightnessPlusPlus", 0, REG_SZ,
                               (BYTE*)quoted, (DWORD)((lstrlenW(quoted) + 1) * sizeof(wchar_t)));
            }
        } else {
            RegDeleteValueW(hKey, L"StudioBrightnessPlusPlus");
        }
        RegCloseKey(hKey);
    }
}
