#include "error.h"

#include <stdarg.h>
#include <stdio.h>
#include <string>

#include "shared.h"
#include "../shared/assembly.h"
#include "../shared/error.h"


ErrorPlatformInfo error_getPlatformInfo() {
    ErrorPlatformInfo info;
    info.type = "linux-server";
    return info;
}


// 080d3b3c    void Sys_Error(char* error, ...) __noreturn
void Sys_Error(const char* error, ...) {
    char message[1024];
    va_list args;
    va_start(args, error);
    vsnprintf(message, sizeof(message), error, args);
    va_end(args);

    // CoD2x: report the fatal error, the process exits right after it
    // Only once, Sys_Error is also called for a recursive error in Com_Error
    static bool reported = false;
    if (!reported) {
        reported = true;
        error_reportFatal(message);
    }
    // CoD2x: End

    // Call the original function
    const char* format = "%s";
    char* text = message;
    ASM_CALL(RETURN_VOID, 0x080d3b3c, 2, PUSH(format), PUSH(text));
}


/** Called before the entry point is called. Used to patch the memory. */
void error_patch() {
    patch_call(0x080610b8, (unsigned int)Sys_Error); // fatal Com_Error, in the cleanup after the longjmp
    patch_call(0x0806114e, (unsigned int)Sys_Error); // Com_Error: recursive error
    patch_call(0x0806235b, (unsigned int)Sys_Error); // Com_Init_Try
    patch_call(0x080d6727, (unsigned int)Sys_Error); // failed to create the home directory
}
