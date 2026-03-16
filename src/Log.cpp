#include "Log.h"
#include <algorithm>

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
