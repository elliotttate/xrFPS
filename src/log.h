#pragma once

#include <windows.h>
#include <cstdarg>
#include <cstdio>

// Event-rate logging only — never call this per frame.
inline void flog(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    // Time and process id: one log file serves every game, so a session's lines
    // need something to tie them to when it ran.
    SYSTEMTIME now;
    GetLocalTime(&now);
    char msg[1100];
    snprintf(msg, sizeof(msg), "[xrFPS] %02u:%02u:%02u.%03u [%lu] %s\n", now.wHour, now.wMinute,
             now.wSecond, now.wMilliseconds, GetCurrentProcessId(), line);
    OutputDebugStringA(msg);

    static FILE* file = [] {
        char path[MAX_PATH]{};
        if (GetEnvironmentVariableA("LOCALAPPDATA", path, sizeof(path)) > 0) {
            char full[MAX_PATH];
            snprintf(full, sizeof(full), "%s\\xrFPS", path);
            CreateDirectoryA(full, nullptr);
            snprintf(full, sizeof(full), "%s\\xrFPS\\xrfps.log", path);
            return fopen(full, "a");
        }
        return (FILE*)nullptr;
    }();
    if (file != nullptr) {
        fputs(msg, file);
        fflush(file);
    }
}
