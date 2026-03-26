#ifndef EXCEPTION_H
#define EXCEPTION_H

#include <windows.h>

bool exception_createMiniDump(EXCEPTION_POINTERS* pExceptionInfo, char* pathOut = nullptr, size_t pathOutSize = 0);
void exception_init();

#endif