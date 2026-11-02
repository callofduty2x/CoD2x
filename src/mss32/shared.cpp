#include "shared.h"

#include <windows.h>

#include "../shared/cod2_common.h"
#include "../shared/error.h"

void getErrorMessage(DWORD errorCode, char* buffer, size_t bufferSize) {
    // Temporary buffer for the system error message
    char messageBuffer[256] = {0};

    // Get the error message string from the system
    FormatMessage(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
        messageBuffer,
        sizeof(messageBuffer),
        NULL
    );

    // Format the final message into the caller-provided buffer
    snprintf(buffer, bufferSize, "%s (error %lu)", messageBuffer, errorCode);
}

void showErrorMessage(const char *title, const char *message, ...) {
    char formattedMessage[1024];
    va_list args;
    va_start(args, message);
    vsnprintf(formattedMessage, sizeof(formattedMessage), message, args);
    va_end(args);

    error_reportFatal(formattedMessage);

    MessageBox(NULL, formattedMessage, title, MB_ICONERROR | MB_OK | MB_TOPMOST);
}

void showErrorBox(const char *file, const char *function, int line, const char *format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    const char *errorTemplate = 
        "An error occurred in " APP_NAME " module '" APP_MODULE_NAME "' " APP_VERSION ":\n\n"
        "%s\n\n"
        "File: %s\n"
        "Function: %s\n"
        "Line: %d\n\n"
        "Try reinstalling " APP_NAME " or check for updates. If the issue persists, report the error to developers.";

    char fullMessage[1024 + 512];
    snprintf(fullMessage, sizeof(fullMessage), errorTemplate, message, file, function, line);

    error_reportFatal(fullMessage);

    MessageBox(NULL, fullMessage, APP_NAME " - Error", MB_ICONERROR | MB_OK | MB_TOPMOST);
}


void showErrorBoxWithLastError(const char *file, const char *function, int line, const char *format, ...) {
    char message2[512 - 2];
    va_list args;
    va_start(args, format);
    vsnprintf(message2, sizeof(message2), format, args);
    va_end(args);

    char errorMessage[512];
    DWORD lastError = GetLastError();
    getErrorMessage(lastError, errorMessage, sizeof(errorMessage));

    showErrorBox(file, function, line, "%s\n\n%s", message2, errorMessage);
}


void showCoD2ErrorWithLastError(enum errorParm_e code, const char *format, ...) {
    char message[512 - 2];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    char errorMessage[512];
    DWORD lastError = GetLastError();
    getErrorMessage(lastError, errorMessage, sizeof(errorMessage));

    char message2[1024];
    snprintf(message2, sizeof(message2), "%s\n%s", message, errorMessage);

    Com_Error(code, message2);
}