#include "log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace
{
	std::mutex g_lock;
	FILE *g_file = nullptr;
}

namespace hslog
{
	void init(void *module)
	{
		wchar_t path[MAX_PATH];
		GetModuleFileNameW(static_cast<HMODULE>(module), path, MAX_PATH);
		if (wchar_t *dot = wcsrchr(path, L'.'))
			wcscpy_s(dot, MAX_PATH - (dot - path), L".log");
		std::lock_guard<std::mutex> lock(g_lock);
		if (g_file == nullptr)
			g_file = _wfsopen(path, L"w", _SH_DENYWR); // readable while the game runs
	}

	void info(const char *format, ...)
	{
		char buffer[2048];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		SYSTEMTIME t;
		GetLocalTime(&t);
		std::lock_guard<std::mutex> lock(g_lock);
		if (g_file == nullptr)
			return;
		fprintf(g_file, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buffer);
		fflush(g_file);
	}
}
