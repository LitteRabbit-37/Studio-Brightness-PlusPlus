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
        showOSD = (GetRegDWORD(hKey, L"ShowOSD", 1) != 0); // Default to True
        runAtStartup = (GetRegDWORD(hKey, L"RunAtStartup", 0) != 0);
        enableCustomHotkeys = (GetRegDWORD(hKey, L"CustomHotkeysEnabled", 0) != 0);
        
        hkUp.mods = GetRegDWORD(hKey, L"HotkeyUpMods", 0);
        hkUp.vk   = GetRegDWORD(hKey, L"HotkeyUpVK", 0);
        
        hkDown.mods = GetRegDWORD(hKey, L"HotkeyDownMods", 0);
        hkDown.vk   = GetRegDWORD(hKey, L"HotkeyDownVK", 0);
        
        DWORD steps = GetRegDWORD(hKey, L"BrightnessSteps", 10);
        brightnessSteps = std::clamp(steps, kMinBrightnessSteps, kMaxBrightnessSteps);
        
        RegCloseKey(hKey);
    }
}

void AppSettings::Save() {
    HKEY hKey;
    DWORD disp;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, &disp) == ERROR_SUCCESS) {
        SetRegDWORD(hKey, L"AutoBrightnessEnabled", autoAdjustEnabled.load() ? 1 : 0);
        SetRegDWORD(hKey, L"ShowOSD", showOSD ? 1 : 0);
        SetRegDWORD(hKey, L"CustomHotkeysEnabled", enableCustomHotkeys ? 1 : 0);
        SetRegDWORD(hKey, L"RunAtStartup", runAtStartup ? 1 : 0);
        
        SetRegDWORD(hKey, L"HotkeyUpMods", hkUp.mods);
        SetRegDWORD(hKey, L"HotkeyUpVK", hkUp.vk);
        
        SetRegDWORD(hKey, L"HotkeyDownMods", hkDown.mods);
        SetRegDWORD(hKey, L"HotkeyDownVK", hkDown.vk);
        
        SetRegDWORD(hKey, L"BrightnessSteps", brightnessSteps);
        
        RegCloseKey(hKey);
    }
}

bool AppSettings::IsStartupEnabled() {
    HKEY hKey;
    bool enabled = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH];
        DWORD   size = sizeof(value);
        enabled = (RegQueryValueExW(hKey, L"StudioBrightnessPlusPlus", nullptr, nullptr, (BYTE*)value, &size) == ERROR_SUCCESS);
        RegCloseKey(hKey);
    }
    return enabled;
}

void AppSettings::SetStartup(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupKey, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            RegSetValueExW(hKey, L"StudioBrightnessPlusPlus", 0, REG_SZ, (BYTE*)exePath, (DWORD)((wcslen(exePath) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"StudioBrightnessPlusPlus");
        }
        RegCloseKey(hKey);
    }
}
