#include "error.h"

#include <windows.h>
#include <string>
#include <vector>

#include "shared.h"
#include "console.h"
#include "../shared/cod2_dvars.h"
#include "../shared/error.h"

#define ERROR_TEST_DELAY_MS 5000

extern char hwid_regid[33];
extern dvar_t* cl_hwid2;


ErrorPlatformInfo error_getPlatformInfo() {
    ErrorPlatformInfo info;

    bool isServer = dedicated && dedicated->value.boolean;
    info.type = isServer ? "win-server" : "win-client";
    info.installId = hwid_regid;
    if (cl_hwid2 && cl_hwid2->value.string)
        info.hwid2 = cl_hwid2->value.string;

    // The console mirror at the time of the error
    std::vector<char> logsBuf(CONSOLE_MIRROR_CAPACITY + 1, 0);
    console_getLogs(logsBuf.data(), logsBuf.size());
    info.logLines = logsBuf.data();

    return info;
}

/** Called when a fatal error occurs. */
int Sys_Error_MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {

    error_reportFatal(lpText);

    return MessageBoxA(hWnd, lpText, lpCaption, uType);
}

/** Called when a DirectX fatal error occurs. */
int Sys_DirectXFatalError_MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {

    error_reportFatal(lpText);

    return MessageBoxA(hWnd, lpText, lpCaption, uType);
}




#if ERROR_TEST
static DWORD WINAPI error_testThread(LPVOID) {
    Sleep(ERROR_TEST_DELAY_MS);

    // Simulate an error to verify sending of the report.
    error_reportFatal("Simulated test error for error report verification");

    return 0;
}
#endif

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void error_init() {
#if ERROR_TEST
    // Spawn a thread that triggers a simulated error after ERROR_TEST_DELAY_MS ms.
    HANDLE hThread = CreateThread(NULL, 0, error_testThread, NULL, 0, NULL);
    if (hThread)
        CloseHandle(hThread);
#endif
}

/** Called before the entry point is called. Used to patch the memory. */
void error_patch() {

    // Hook call to MessageBoxA in Sys_Error that is called when fatal game error occurs
    patch_nop(0x004657f9, 6); // first replace 6th byte, becuase next function writes only to 5 bytes
    patch_call(0x004657f9, (unsigned int)Sys_Error_MessageBoxA);

    // Hook call to MessageBoxA in Sys_DirectXFatalError that is called when DirectX error occurs
    patch_call(0x00465590, (unsigned int)Sys_DirectXFatalError_MessageBoxA);
}
