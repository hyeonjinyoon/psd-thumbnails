#include "psdthumb.h"
#include <cstdarg>
#include <cstdio>
#include <string>

namespace {
int g_logEnabled = -1;
std::wstring g_logPath;

bool LogEnabled() {
    if (g_logEnabled < 0) {
        wchar_t v[8] = {};
        const DWORD n = GetEnvironmentVariableW(L"PSDTHUMB_DEBUG", v, 8);
        g_logEnabled = (n > 0 && n < 8 && v[0] != L'0') ? 1 : 0;
        if (g_logEnabled == 1) {
            wchar_t tmp[MAX_PATH];
            const DWORD len = GetTempPathW(MAX_PATH, tmp);
            if (len > 0 && len < MAX_PATH) {
                g_logPath = tmp;
                g_logPath += L"psd-thumbnails.log";
            }
        }
    }
    return g_logEnabled == 1;
}
}  // namespace

void PsdLog(const char* fmt, ...) {
    if (!LogEnabled()) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1200];
    snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [pid %lu tid %lu] %s\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             GetCurrentProcessId(), GetCurrentThreadId(), msg);
    OutputDebugStringA(line);
    if (!g_logPath.empty()) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_logPath.c_str(), L"ab") == 0 && f) {
            fputs(line, f);
            fclose(f);
        }
    }
}
