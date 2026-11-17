#pragma once
#include <windows.h>
#include <atomic>

// Brightness step constraints (also used by Options dialog)
constexpr ULONG kDefaultBrightnessSteps = 10;
constexpr ULONG kMinBrightnessSteps     = 10;
constexpr ULONG kMaxBrightnessSteps     = 50;

// Auto-brightness ramp durations in milliseconds (also used by Options dialog).
// The ramp is asymmetric, Apple-style: fast brightening, slow and gentle dimming.
constexpr ULONG kDefaultRampBrightenMs = 1500;
constexpr ULONG kDefaultRampDimMs      = 5000;
constexpr ULONG kMinRampMs             = 0;     // 0 = jump straight to the new level
constexpr ULONG kMaxRampMs             = 60000;

// Auto-brightness limits (also used by Options dialog): how far an automatic change may take the
// brightness from the level set by hand, in percent of the whole brightness range, the unit of
// the brightness slider. A limit of 100 spans the whole range from anywhere, so it never binds.
constexpr ULONG kDefaultAutoLimit = 100;
constexpr ULONG kMinAutoLimit     = 0; // 0 = never past the level set by hand
constexpr ULONG kMaxAutoLimit     = 100;

// Custom hotkeys structure
struct HotkeySpec {
    UINT mods;
    UINT vk;
};

struct AppSettings {
    // Core Brightness Logic
    std::atomic<bool> autoAdjustEnabled{true};
    // On by default. Safe because the watcher only reacts to a physical change it has
    // observed; it never rotates the screen at startup on its own.
    std::atomic<bool> autoRotateEnabled{true};
    ULONG             brightnessSteps{10};
    // How long an automatic change takes to reach its new level. Atomic like the toggles
    // above: the Options dialog writes them while the worker thread reads them.
    std::atomic<ULONG> rampBrightenMs{kDefaultRampBrightenMs};
    std::atomic<ULONG> rampDimMs{kDefaultRampDimMs};
    // How far above and below the level set by hand an automatic change may go. Atomic for the
    // same reason.
    std::atomic<ULONG> autoBrightenLimit{kDefaultAutoLimit};
    std::atomic<ULONG> autoDimLimit{kDefaultAutoLimit};

    // User Interface
    bool showOSD{true};
    bool runAtStartup{false};

    // Multi-display
    bool  linkedMode{true};
    ULONG activeDisplayIndex{0};

    // Input / Hotkeys
    bool       enableCustomHotkeys{false};
    HotkeySpec hkUp{0, 0};
    HotkeySpec hkDown{0, 0};

    // Updates: 0 = stable only, 1 = include beta (pre-release) versions
    int updateChannel{0};

    // Methods
    void Load();
    void Save();
    bool IsStartupEnabled();
    void SetStartup(bool enable);
};

// Global accessor
extern AppSettings g_settings;
