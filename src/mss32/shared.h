// shared.h
#ifndef SHARED_H
#define SHARED_H

#include "../shared/shared.h"
#include "hook.h"

#include <windows.h>
#include <stdio.h>  // For snprintf

#define APP_MODULE_NAME "mss32.dll"

// Global variables for client state and server data
#define clientState (*((clientState_e *)0x00609fe0)) // Client state, possibly controlling game behavior

#define svr_players ((int *)0x001518F80) // Pointer to the number of players on the server
#define clc_stringData ((PCHAR)0x0096FD5C) // Pointer to client string data
#define clc_stringOffsets ((PINT)0x0096DD5C) // Pointer to offsets in client string data

#define cs0 (clc_stringData + clc_stringOffsets[0]) 
#define cs1 (clc_stringData + clc_stringOffsets[1]) 

void getErrorMessage(DWORD errorCode, char* buffer, size_t bufferSize);
void showErrorBox(const char *file, const char *function, int line, const char *format, ...);
void showErrorBoxWithLastError(const char *file, const char *function, int line, const char *format, ...);
void showCoD2ErrorWithLastError(enum errorParm_e code, const char *format, ...);


// Macros to preserve __FILE__, __FUNCTION__, and __LINE__
#define SHOW_ERROR(format, ...) \
    showErrorBox(__FILE__, __FUNCTION__, __LINE__, format, ##__VA_ARGS__)

#define SHOW_ERROR_WITH_LAST_ERROR(format, ...) \
    showErrorBoxWithLastError(__FILE__, __FUNCTION__, __LINE__, format, ##__VA_ARGS__)



#endif // SHARED_H
