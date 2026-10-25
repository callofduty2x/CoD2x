#ifndef EXCEPTION_H
#define EXCEPTION_H

#include <windows.h>

// Custom exception codes raised by CoD2x so fatal errors that would otherwise end the process
// silently (or with a CRT message box) go through the CoD2x crash handler.
#define EXCEPTION_COD2X_CRT_RUNTIME_ERROR   0xE0C2D201  // game CRT _amsg_exit / abort / pure virtual call; params: [0] = rterrnum, [1] = message
#define EXCEPTION_COD2X_CRT_SECURITY_ERROR  0xE0C2D202  // game CRT /GS buffer overrun; params: [0] = code
#define EXCEPTION_COD2X_FATAL               0xE0C2D203  // CoD2x DLL fatal error (std::terminate, abort); params: [0] = message

bool exception_createMiniDump(EXCEPTION_POINTERS* pExceptionInfo, DWORD threadId, bool fullMemory, const char* kind, char* pathOut, size_t pathOutSize);
void exception_reportFreeze(const char* reason, DWORD frozenMs);
size_t exception_describeAddress(unsigned int addr, char* out, size_t outSize);
size_t exception_captureCallerStack(char* out, size_t outSize, int framesToSkip);

void exception_patch();
void exception_init();
void exception_frame();
void exception_unload();

#endif
