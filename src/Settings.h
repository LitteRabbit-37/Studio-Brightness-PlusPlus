#pragma once
#include <windows.h>
#include <atomic>

// Brightness step constraints (also used by Options dialog)
constexpr ULONG kDefaultBrightnessSteps = 10;
constexpr ULONG kMinBrightnessSteps     = 10;
constexpr ULONG kMaxBrightnessSteps     = 50;

// Custom hotkeys structure
struct HotkeySpec {
    UINT mods;
    UINT vk;
};

struct AppSettings {
    // Core Brightness Logic
    std::atomic<bool> autoAdjustEnabled{true};
    ULONG             brightnessSteps{10};

    // User Interface
    bool showOSD{true};  // New setting for OSD
    bool runAtStartup{false};

    // Input / Hotkeys
    bool       enableCustomHotkeys{false};
    HotkeySpec hkUp{0, 0};
    HotkeySpec hkDown{0, 0};

    // Methods
    void Load();
    void Save();
    bool IsStartupEnabled();
    void SetStartup(bool enable);
};

// Global accessor
extern AppSettings g_settings;
