#pragma once
#include <windows.h>
#include <string>
#include <deque>
#include <vector>
#include <cstdarg>

enum class LogLevel { Info, Warn, Error };

struct LogEntry {
	DWORD       tick;
	LogLevel    level;
	std::wstring message;
};

class Log {
public:
	static void Info(const wchar_t *fmt, ...);
	static void Warn(const wchar_t *fmt, ...);
	static void Error(const wchar_t *fmt, ...);

	// Retrieve entries starting from `fromIndex`.
	// Returns the index of the next entry (use as new fromIndex).
	static size_t GetEntries(std::vector<LogEntry> &out, size_t fromIndex);

	// Format up to `maxLines` recent entries as a single string (for clipboard).
	static std::wstring FormatRecent(size_t maxLines);

private:
	static void     Add(LogLevel level, const wchar_t *fmt, va_list args);
	static Log     &Instance();

	SRWLOCK                lock_ = SRWLOCK_INIT;
	std::deque<LogEntry>   entries_;
	size_t                 totalCount_ = 0;

	static constexpr size_t kMaxEntries = 2000;
};
