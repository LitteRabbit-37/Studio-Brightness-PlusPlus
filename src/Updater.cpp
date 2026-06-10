#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <string>
#include <vector>
#include "Updater.h"
#include "Log.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "oleaut32.lib")

using namespace winrt::Windows::Data::Json;

namespace {

// HTTPS GET. Follows redirects (default WinHTTP policy stays HTTPS->HTTPS). Fills `body` with the
// raw response bytes. HTTP, non-200 status, or any failure returns false.
bool httpGet(const std::wstring &url, std::vector<char> &body, const wchar_t *accept) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host;   uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath  = path;   uc.dwUrlPathLength  = _countof(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) return false;  // refuse anything but HTTPS

    HINTERNET hSession = WinHttpOpen(L"StudioBrightnessPlusPlus",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    bool ok = false;
    if (HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0)) {
        if (HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
            std::wstring headers;
            if (accept) { headers = L"Accept: "; headers += accept; }
            if (WinHttpSendRequest(hRequest,
                    headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                    headers.empty() ? 0 : (DWORD)-1L,
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                && WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD status = 0, sz = sizeof(status);
                WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    DWORD avail = 0;
                    do {
                        avail = 0;
                        if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
                        size_t old = body.size();
                        body.resize(old + avail);
                        DWORD read = 0;
                        if (!WinHttpReadData(hRequest, body.data() + old, avail, &read)) { ok = false; break; }
                        body.resize(old + read);
                        ok = true;
                    } while (avail > 0);
                    if (body.empty()) ok = true;  // empty 200 is still "success"
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}

std::wstring utf8ToW(const std::vector<char> &bytes) {
    if (bytes.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), w.data(), n);
    return w;
}

struct SemVer { int major = 0, minor = 0, patch = 0; std::wstring pre; bool valid = false; };

SemVer parseSemver(std::wstring v) {
    if (!v.empty() && (v[0] == L'v' || v[0] == L'V')) v.erase(0, 1);
    SemVer s;
    size_t dash = v.find(L'-');
    std::wstring core = (dash == std::wstring::npos) ? v : v.substr(0, dash);
    if (dash != std::wstring::npos) s.pre = v.substr(dash + 1);
    if (swscanf_s(core.c_str(), L"%d.%d.%d", &s.major, &s.minor, &s.patch) == 3) s.valid = true;
    return s;
}

// SemVer precedence: -1 if a<b, 0 if equal, 1 if a>b. A release outranks its pre-releases.
int cmpSemver(const SemVer &a, const SemVer &b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre.empty() && b.pre.empty()) return 0;
    if (a.pre.empty()) return 1;   // 2.2.0 > 2.2.0-beta.1
    if (b.pre.empty()) return -1;
    if (a.pre == b.pre) return 0;
    return a.pre < b.pre ? -1 : 1; // lexical: beta.1 < beta.2
}

bool endsWithMsi(const std::wstring &name) {
    return name.size() >= 4 && _wcsicmp(name.c_str() + name.size() - 4, L".msi") == 0;
}

} // namespace

UpdateInfo CheckForUpdate(int channel, const wchar_t *currentVersion) {
    UpdateInfo result;

    std::vector<char> body;
    const std::wstring api =
        L"https://api.github.com/repos/LitteRabbit-37/Studio-Brightness-PlusPlus/releases?per_page=30";
    if (!httpGet(api, body, L"application/vnd.github+json")) {
        Log::Warn(L"Update check: request to GitHub API failed");
        return result;
    }
    const std::wstring json = utf8ToW(body);
    const SemVer cur = parseSemver(currentVersion);

    bool didInit = false;
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); didInit = true; } catch (...) {}

    SemVer bestVer; std::wstring bestTag, bestUrl; bool haveBest = false;
    try {
        JsonArray arr = JsonArray::Parse(json);
        for (uint32_t i = 0; i < arr.Size(); ++i) {
            try {
                JsonObject rel = arr.GetObjectAt(i);
                if (rel.GetNamedBoolean(L"draft", false)) continue;
                bool pre = rel.GetNamedBoolean(L"prerelease", false);
                if (channel == 0 && pre) continue;  // stable channel ignores pre-releases

                std::wstring tag = rel.GetNamedString(L"tag_name", L"").c_str();
                SemVer v = parseSemver(tag);
                if (!v.valid) continue;

                std::wstring msiUrl;
                JsonArray assets = rel.GetNamedArray(L"assets", nullptr);
                if (assets) {
                    for (uint32_t a = 0; a < assets.Size(); ++a) {
                        JsonObject as = assets.GetObjectAt(a);
                        std::wstring name = as.GetNamedString(L"name", L"").c_str();
                        if (endsWithMsi(name)) {
                            msiUrl = as.GetNamedString(L"browser_download_url", L"").c_str();
                            break;
                        }
                    }
                }
                if (msiUrl.empty()) continue;  // no installer asset on this release

                if (!haveBest || cmpSemver(v, bestVer) > 0) {
                    bestVer = v; bestTag = tag; bestUrl = msiUrl; haveBest = true;
                }
            } catch (...) { /* skip a malformed release */ }
        }
    } catch (...) {
        Log::Warn(L"Update check: could not parse the GitHub response");
    }

    if (didInit) winrt::uninit_apartment();

    if (haveBest && cmpSemver(bestVer, cur) > 0) {
        result.available = true;
        result.tag = bestTag;
        result.msiUrl = bestUrl;
        result.version = bestTag;
        if (!result.version.empty() && (result.version[0] == L'v' || result.version[0] == L'V'))
            result.version.erase(0, 1);
        Log::Info(L"Update available: %s (current %s)", result.version.c_str(), currentVersion);
    } else {
        Log::Info(L"Update check: up to date (current %s)", currentVersion);
    }
    return result;
}

bool BeginInstallUpdate(const UpdateInfo &info) {
    if (info.msiUrl.empty()) return false;

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring msiPath = std::wstring(tmp) + L"studio-brightness-plusplus-update.msi";
    const std::wstring cmdPath = std::wstring(tmp) + L"sbpp-update.cmd";

    std::vector<char> body;
    if (!httpGet(info.msiUrl, body, nullptr) || body.empty()) {
        Log::Warn(L"Update: MSI download failed");
        return false;
    }
    HANDLE h = CreateFileW(msiPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL wok = WriteFile(h, body.data(), (DWORD)body.size(), &written, nullptr);
    CloseHandle(h);
    if (!wok || written != body.size()) { Log::Warn(L"Update: could not save MSI"); return false; }

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const DWORD pid = GetCurrentProcessId();

    // Helper (non-elevated): wait for this app to exit, elevate msiexec to install (one UAC prompt
    // for Windows Installer), then relaunch the new version non-elevated, then delete itself.
    std::wstring s;
    s += L"@echo off\r\n";
    s += L":wait\r\n";
    s += L"tasklist /FI \"PID eq " + std::to_wstring(pid) + L"\" 2>nul | find \"" + std::to_wstring(pid) + L"\" >nul\r\n";
    s += L"if not errorlevel 1 ( timeout /t 1 /nobreak >nul & goto wait )\r\n";
    s += L"powershell -NoProfile -Command \"Start-Process -Wait -Verb RunAs -FilePath 'msiexec' -ArgumentList '/i','" + msiPath + L"','/qb'\"\r\n";
    s += L"start \"\" \"" + std::wstring(exe) + L"\"\r\n";
    s += L"del \"%~f0\"\r\n";

    HANDLE hc = CreateFileW(cmdPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hc == INVALID_HANDLE_VALUE) return false;
    int n = WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::vector<char> ansi((size_t)(n > 0 ? n - 1 : 0));
    if (n > 1) WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, ansi.data(), n, nullptr, nullptr);
    DWORD w2 = 0;
    WriteFile(hc, ansi.data(), (DWORD)ansi.size(), &w2, nullptr);
    CloseHandle(hc);

    std::wstring params = L"/c \"" + cmdPath + L"\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) { Log::Warn(L"Update: could not launch the updater helper"); return false; }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    Log::Info(L"Update: installer launched for %s; exiting to let it replace files", info.version.c_str());
    return true;
}
