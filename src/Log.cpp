#include "Log.h"
#include <algorithm>
#include <vector>
#include <ctime>
#include <cstdio>

static const wchar_t *kLogSettingsKey = L"Software\\StudioBrightnessPlusPlus";

static long long nowEpoch() { return (long long)time(nullptr); }

static std::wstring localAppDataLogs() {
	wchar_t base[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
	std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(base) : L".";
	std::wstring d1 = dir + L"\\StudioBrightnessPlusPlus";
	std::wstring d2 = d1 + L"\\logs";
	CreateDirectoryW(d1.c_str(), nullptr);
	CreateDirectoryW(d2.c_str(), nullptr);
	return d2;
}

static long long readPersistedUntil() {
	HKEY k;
	long long v = 0;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kLogSettingsKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
		DWORD val = 0, sz = sizeof(val), type = 0;
		if (RegQueryValueExW(k, L"LogUntil", nullptr, &type, (BYTE *)&val, &sz) == ERROR_SUCCESS && type == REG_DWORD)
			v = (long long)val;
		RegCloseKey(k);
	}
	return v;
}

static void writePersistedUntil(long long until) {
	HKEY k;
	DWORD disp;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kLogSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, &disp) == ERROR_SUCCESS) {
		DWORD val = (DWORD)until;
		RegSetValueExW(k, L"LogUntil", 0, REG_DWORD, (BYTE *)&val, sizeof(val));
		RegCloseKey(k);
	}
}

Log &Log::Instance() {
	static Log inst;
	return inst;
}

void Log::Add(LogLevel level, const wchar_t *fmt, va_list args) {
	wchar_t buf[1024];
	_vsnwprintf_s(buf, _TRUNCATE, fmt, args);

	LogEntry entry;
	entry.tick    = GetTickCount();
	entry.level   = level;
	entry.message = buf;

	Log &self = Instance();
	AcquireSRWLockExclusive(&self.lock_);
	self.entries_.push_back(std::move(entry));
	self.totalCount_++;
	if (self.entries_.size() > kMaxEntries)
		self.entries_.pop_front();
	if (self.file_ != INVALID_HANDLE_VALUE) {
		if (nowEpoch() >= self.fileUntil_)
			self.closeFile();
		else
			self.writeFileLine(level, buf);
	}
	ReleaseSRWLockExclusive(&self.lock_);
}

void Log::Info(const wchar_t *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	Add(LogLevel::Info, fmt, ap);
	va_end(ap);
}

void Log::Warn(const wchar_t *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	Add(LogLevel::Warn, fmt, ap);
	va_end(ap);
}

void Log::Error(const wchar_t *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	Add(LogLevel::Error, fmt, ap);
	va_end(ap);
}

size_t Log::GetEntries(std::vector<LogEntry> &out, size_t fromIndex) {
	Log &self = Instance();
	AcquireSRWLockShared(&self.lock_);

	size_t total   = self.totalCount_;
	size_t dropped = total - self.entries_.size();

	if (fromIndex < dropped)
		fromIndex = dropped;

	size_t startOffset = fromIndex - dropped;
	for (size_t i = startOffset; i < self.entries_.size(); ++i)
		out.push_back(self.entries_[i]);

	ReleaseSRWLockShared(&self.lock_);
	return total;
}

static const wchar_t *levelTag(LogLevel lv) {
	switch (lv) {
	case LogLevel::Info:  return L"INFO ";
	case LogLevel::Warn:  return L"WARN ";
	case LogLevel::Error: return L"ERROR";
	}
	return L"?????";
}

std::wstring Log::FormatRecent(size_t maxLines) {
	Log &self = Instance();
	AcquireSRWLockShared(&self.lock_);

	std::wstring result;
	size_t start = 0;
	if (self.entries_.size() > maxLines)
		start = self.entries_.size() - maxLines;

	for (size_t i = start; i < self.entries_.size(); ++i) {
		const auto &e = self.entries_[i];
		DWORD sec = e.tick / 1000;
		DWORD ms  = e.tick % 1000;
		DWORD h   = (sec / 3600) % 24;
		DWORD m   = (sec / 60) % 60;
		DWORD s   = sec % 60;

		wchar_t line[1200];
		_snwprintf_s(line, _TRUNCATE, L"[%02u:%02u:%02u.%03u] [%s] %s\r\n",
		             h, m, s, ms, levelTag(e.level), e.message.c_str());
		result += line;
	}

	ReleaseSRWLockShared(&self.lock_);
	return result;
}

bool Log::openFile(long long untilEpoch) {
	std::wstring folder = localAppDataLogs();
	SYSTEMTIME st;
	GetLocalTime(&st);
	wchar_t name[80];
	swprintf_s(name, L"sbpp-%04u%02u%02u-%02u%02u%02u.log",
	           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	std::wstring path = folder + L"\\" + name;
	HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
	                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return false;
	if (file_ != INVALID_HANDLE_VALUE)
		CloseHandle(file_);
	file_      = f;
	fileUntil_ = untilEpoch;
	filePath_  = path;
	const unsigned char bom[3] = {0xEFu, 0xBBu, 0xBFu};
	DWORD wn;
	WriteFile(file_, bom, 3, &wn, nullptr);
	char hdr[160];
	int hn = sprintf_s(hdr, "==== Studio Brightness++ file log, %04u-%02u-%02u %02u:%02u:%02u local ====\r\n",
	                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	if (hn > 0)
		WriteFile(file_, hdr, (DWORD)hn, &wn, nullptr);
	FlushFileBuffers(file_);
	return true;
}

void Log::writeFileLine(LogLevel level, const wchar_t *msg) {
	SYSTEMTIME st;
	GetLocalTime(&st);
	const wchar_t *tag = (level == LogLevel::Warn) ? L"WARN " : (level == LogLevel::Error) ? L"ERROR" : L"INFO ";
	wchar_t wline[1300];
	int wn = _snwprintf_s(wline, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] %s\r\n",
	                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tag, msg);
	if (wn <= 0)
		return;
	int n8 = WideCharToMultiByte(CP_UTF8, 0, wline, wn, nullptr, 0, nullptr, nullptr);
	if (n8 <= 0)
		return;
	std::vector<char> u8((size_t)n8);
	WideCharToMultiByte(CP_UTF8, 0, wline, wn, u8.data(), n8, nullptr, nullptr);
	DWORD written;
	WriteFile(file_, u8.data(), (DWORD)n8, &written, nullptr);
	FlushFileBuffers(file_);
}

void Log::closeFile() {
	if (file_ != INVALID_HANDLE_VALUE) {
		CloseHandle(file_);
		file_ = INVALID_HANDLE_VALUE;
	}
	fileUntil_ = 0;
	filePath_.clear();
}

bool Log::StartFileLog(int minutes) {
	long long until = nowEpoch() + (long long)minutes * 60;
	Log &self = Instance();
	AcquireSRWLockExclusive(&self.lock_);
	bool ok = self.openFile(until);
	ReleaseSRWLockExclusive(&self.lock_);
	if (ok)
		writePersistedUntil(until);
	return ok;
}

void Log::StopFileLog() {
	Log &self = Instance();
	AcquireSRWLockExclusive(&self.lock_);
	self.closeFile();
	ReleaseSRWLockExclusive(&self.lock_);
	writePersistedUntil(0);
}

void Log::ResumeIfPending() {
	long long until = readPersistedUntil();
	if (until <= nowEpoch())
		return;
	Log &self = Instance();
	AcquireSRWLockExclusive(&self.lock_);
	self.openFile(until);
	ReleaseSRWLockExclusive(&self.lock_);
}

bool Log::FileLogActive() {
	Log &self = Instance();
	AcquireSRWLockShared(&self.lock_);
	bool a = (self.file_ != INVALID_HANDLE_VALUE) && (nowEpoch() < self.fileUntil_);
	ReleaseSRWLockShared(&self.lock_);
	return a;
}

int Log::RemainingSeconds() {
	Log &self = Instance();
	AcquireSRWLockShared(&self.lock_);
	long long r = (self.file_ != INVALID_HANDLE_VALUE) ? (self.fileUntil_ - nowEpoch()) : 0;
	ReleaseSRWLockShared(&self.lock_);
	return r > 0 ? (int)r : 0;
}

std::wstring Log::LogsFolderPath() {
	return localAppDataLogs();
}
