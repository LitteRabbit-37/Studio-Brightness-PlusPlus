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

	// File logging (debug): mirror every log line to a timestamped UTF-8 file, flushed per line so it
	// survives a crash or hard reset. Time-boxed; the deadline is persisted so the session resumes
	// after a relaunch if still within the window (lets a blank-screen reset still capture the logs).
	static bool         StartFileLog(int minutes);
	static void         StopFileLog();
	static void         ResumeIfPending();  // call once at startup
	static bool         FileLogActive();
	static int          RemainingSeconds();
	static std::wstring LogsFolderPath();

private:
	static void     Add(LogLevel level, const wchar_t *fmt, va_list args);
	static Log     &Instance();
	bool            openFile(long long untilEpoch);                   // lock held
	void            writeFileLine(LogLevel level, const wchar_t *msg); // lock held
	void            closeFile();                                       // lock held

	SRWLOCK                lock_ = SRWLOCK_INIT;
	std::deque<LogEntry>   entries_;
	size_t                 totalCount_ = 0;

	HANDLE       file_      = INVALID_HANDLE_VALUE;
	long long    fileUntil_ = 0;  // unix epoch seconds; 0 = inactive
	std::wstring filePath_;

	static constexpr size_t kMaxEntries = 2000;
};
