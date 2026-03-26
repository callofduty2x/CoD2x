#include "error.h"

#include <iostream>
#include <string>

#include "shared.h"


#include "../shared/cod2_net.h"
#include "../shared/cod2_dvars.h"
#include "updater.h"
#include "system.h"

extern struct netaddr_s updater_address;
extern dvar_t *cl_hwid;
extern dvar_t *cl_hwid2;
extern char hwid_regid[33];

bool error_updaterCanResolve = false;



void error_sendCrashData(unsigned int exceptionCode, unsigned int exceptionAddress, const char* moduleName, unsigned int fileOffset, const char* stackDump) {
    
    if (updater_address.type == NA_INIT && error_updaterCanResolve) {
        updater_resolveServerAddress();
    }

    // Send error data to the updater server if available
    if (updater_address.type > NA_BAD) {
        
        // Load data
        const char *hwid2 = cl_hwid2 ? cl_hwid2->value.string : "";
        const char *regid = hwid_regid[0] ? hwid_regid : "";
        char log[1024];
        logger_get_recent(log, sizeof(log));
        for (char *p = log; *p; ++p) {
            if (*p == '"') *p = '\'';
        }

        char udpPayload[2048];
        snprintf(
            udpPayload, sizeof(udpPayload),
            //logCrashData    "1.0.0"          "win-x86" "{HWID2}" "{REGID}" "{CODE}" "{ADDRESS}" "{MODULE_NAME}" "{MODULE_OFFSET}" "{STACK_DUMP}" "{LOG}" "{SYSTEM_INFO}"
            "logCrashData \"" APP_VERSION "\" \"win-x86\" \"%s\" \"%s\" \"0x%08x\" \"0x%08x\" \"%s\" \"0x%08x\" \"%s\" \"%s\" \"%s\"\n",
            hwid2, regid, exceptionCode, exceptionAddress, moduleName, fileOffset, stackDump, log, SYS_VERSION_INFO
        );
        NET_OutOfBandPrint(NS_CLIENT, updater_address, udpPayload);
    }
}

void error_sendErrorData(const char* message) {

    #if DEBUG
        return;
    #endif
    
    if (updater_address.type == NA_INIT && error_updaterCanResolve) {
        updater_resolveServerAddress();
    }
    
    // Send error data to the updater server if available
    if (updater_address.type > NA_BAD) {

        // Load data
        const char *hwid2 = cl_hwid2 ? cl_hwid2->value.string : "";
        const char *regid = hwid_regid[0] ? hwid_regid : "";
        char error[1024];
        strncpy(error, message, sizeof(error) - 1);
        error[sizeof(error) - 1] = '\0'; // Ensure null-termination
        char log[1024];
        logger_get_recent(log, sizeof(log));

        // Replace double quotes with single quotes in the log to avoid issues with UDP payload
        for (char *p = error; *p; ++p) {
            if (*p == '"') *p = '\'';
        }
        for (char *p = log; *p; ++p) {
            if (*p == '"') *p = '\'';
        }

        char udpPayload[2048];
        snprintf(udpPayload, sizeof(udpPayload), 
            // errorData      "1.4.4.3"        "win-x86"  "{HWID2}" "{REGID}" "{ERROR}" "{LOG}" "{SYSTEM_INFO}"
            "logErrorData \"" APP_VERSION "\" \"win-x86\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"\n",
            hwid2, regid, message, log, SYS_VERSION_INFO);

        //MessageBoxA(NULL, udpPayload, "Error Data", MB_ICONERROR | MB_OK | MB_TOPMOST);
        
        NET_OutOfBandPrint(NS_CLIENT, updater_address, udpPayload);
    }
}

/** Called when a fatal error occurs. */
int Sys_Error_MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {
    
    error_sendErrorData(lpText);

    return MessageBoxA(hWnd, lpText, lpCaption, uType);
}

/** Called when a DirectX fatal error occurs. */
int Sys_DirectXFatalError_MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {
    
    error_sendErrorData(lpText);

    return MessageBoxA(hWnd, lpText, lpCaption, uType);
}




/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void error_init() {

    // Init is called after the NET is initialized
    // Since the updater address is resolved after first frame, with this flag we can force the updater to resolve the address sooner
    error_updaterCanResolve = true;
}

/** Called before the entry point is called. Used to patch the memory. */
void error_patch() {

    // Hook call to MessageBoxA in Sys_Error that is called when fatal game error occurs
    patch_nop(0x004657f9, 6); // first replace 6th byte, becuase next function writes only to 5 bytes
    patch_call(0x004657f9, (unsigned int)Sys_Error_MessageBoxA);

    // Hook call to MessageBoxA in Sys_DirectXFatalError that is called when DirectX error occurs
    patch_call(0x00465590, (unsigned int)Sys_DirectXFatalError_MessageBoxA);
}