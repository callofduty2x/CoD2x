// shared.h
#ifndef SHARED_H
#define SHARED_H

#include <windows.h>
#include <stdio.h>  // For snprintf

#include "../shared/shared.h"
#include "../shared/cod2_common.h"
#include "main.h"
#include "hook.h"

#define APP_MODULE_NAME "mss32.dll"

// Set a human-readable name on a thread, visible in debuggers (WinDbg, x64dbg, VS).
// Uses SetThreadDescription (Win10 1607+). Safe no-op on older Windows.
static inline void thread_setName(HANDLE h, const wchar_t* name) {
    typedef HRESULT (WINAPI *PFN)(HANDLE, PCWSTR);
    static PFN pfn = (PFN)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
    if (pfn) pfn(h, name);
}

void getErrorMessage(DWORD errorCode, char* buffer, size_t bufferSize);
void showErrorBox(const char *file, const char *function, int line, const char *format, ...);
void showErrorBoxWithLastError(const char *file, const char *function, int line, const char *format, ...);
void showCoD2ErrorWithLastError(enum errorParm_e code, const char *format, ...);
void showErrorMessage(const char *title, const char *message, ...);

// Macros to preserve __FILE__, __FUNCTION__, and __LINE__
#define SHOW_ERROR(format, ...) \
    showErrorBox(__FILE__, __FUNCTION__, __LINE__, format, ##__VA_ARGS__)

#define SHOW_ERROR_WITH_LAST_ERROR(format, ...) \
    showErrorBoxWithLastError(__FILE__, __FUNCTION__, __LINE__, format, ##__VA_ARGS__)



#endif // SHARED_H
