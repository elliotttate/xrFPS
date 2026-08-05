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

    char msg[1100];
    snprintf(msg, sizeof(msg), "[xrFPS] %s\n", line);
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
