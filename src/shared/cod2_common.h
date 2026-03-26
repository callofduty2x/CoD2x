#ifndef COD2_COMMON_H
#define COD2_COMMON_H

#include "shared.h"
#include <stdio.h>

#define com_last_error      ((char*)ADDR(0x00c26280, 0x081a2280))
#define com_error_type      (*((errorParm_e*)ADDR(0x00c26270, 0x081a2264)))
#define com_errorEntered    (*((int*)ADDR(0x00c28b2c, 0x081a21c0)))

enum errorParm_e
{
    // Show message box, close window, show console window
	ERR_FATAL = 0x0,
    // Show ingame error
	ERR_DROP = 0x1,
	ERR_SERVERDISCONNECT = 0x2,
	ERR_DISCONNECT = 0x3,
	ERR_SCRIPT = 0x4,
	ERR_SCRIPT_DROP = 0x5,
	ERR_LOCALIZATION = 0x6,
	ERR_MAPLOADERRORSUMMARY = 0x7,
};

/**
 * Show error with level, for example ERR_LEVEL
 * Example:
 *  Com_Error(ERR_DROP, "Error message %s %i", "test", 1337);
 */
inline void Com_Error(enum errorParm_e type, const char* format, ...) {
    va_list val;
    va_start(val, format);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, val);
    va_end(val);
    ((void(__cdecl*)(int, const char*, const char*))(ADDR(0x004324c0, 0x08061124)))(type, "%s", buffer);
}

/**
 * Print message to console
 * Example:
 *  Com_Printf("Hello %s %i\n", "world", 1337);
 */
inline int Com_Printf(const char* format, ...) {
    va_list val;
    va_start(val, format);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, val);
    va_end(val);
    return ((int(__cdecl*)(const char*, const char*))(ADDR(0x00431ee0, 0x08060dea)))("%s", buffer);
}

/**
 * Print message to console when developer mode is enabled
 * Example:
 *  Com_DPrintf("Hello %s %i\n", "world", 1337);
 */
inline int Com_DPrintf(const char* format, ...) {
    va_list val;
    va_start(val, format);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, val);
    va_end(val);
    return ((int(__cdecl*)(const char*, const char*))(ADDR(0x00431f30, 0x08060e3a)))("%s", buffer);
}

/**
 * The returned token points to within the original buffer.
 * Repeated calls to Com_Parse will extract subsequent tokens from the buffer.
 */
inline char *Com_Parse(const char **data_p)
{
    char *ret;
    ASM_CALL(RETURN(ret), ADDR(0x00449850, 0x080b6db2), WL(0, 1), WL(ECX, PUSH)(data_p));
    return ret;
}


#endif