#pragma once
#include <string>

struct UpdateInfo {
    bool         available = false;
    std::wstring version;  // tag without the leading v, e.g. "2.2.0" or "2.2.0-beta.1"
    std::wstring tag;      // e.g. "v2.2.0"
    std::wstring msiUrl;   // browser_download_url of the .msi asset
};

// Queries the GitHub Releases API for the newest release on the given channel
// (0 = stable only, 1 = include pre-releases) and compares it to currentVersion.
// Does network + JSON work, so call it off the UI thread. available=false on any error.
UpdateInfo CheckForUpdate(int channel, const wchar_t *currentVersion);

// Downloads the MSI to %TEMP%, writes a helper that waits for this process to exit,
// elevates msiexec to install, then relaunches the new version. Returns true once the
// download + helper launch succeeded, in which case the caller MUST exit the app so the
// installer can replace the running executable.
bool BeginInstallUpdate(const UpdateInfo &info);
