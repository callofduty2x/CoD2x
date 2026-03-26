#include "main.h"

#include <windows.h>

#include "shared.h"
#include "hook.h"
#include "system.h"
#include "mss32_original.h"


// Define a buffer to save the original bytes of the entry point
BYTE originalBytes[5];
void* originalEntryPoint = NULL;

HMODULE EXE_HMODULE = NULL;
char EXE_PATH[MAX_PATH] = {0}; 
char EXE_DIRECTORY_PATH[MAX_PATH] = {0};
char EXE_COMMAND_LINE[MAX_PATH] = {0}; // Command line of the executable
char DLL_PATH[MAX_PATH] = {0}; 
bool DLL_HOTRELOAD = false; // set to true when the DLL was hot-reloaded in debug mode

/**
 * Load the CoD2x patches
 */
bool main_load() {  
    bool ok = FALSE;

    // Show warning message
    /*if (APP_VERSION_IS_TEST) {
        MessageBoxA(NULL, 
            "You successfully installed CoD2x " APP_VERSION ".\n\n"
            "This is a test version, please uninstall it after trying it!\n(use auto-update to go back to the latest stable version)", "CoD2x TEST version", MB_OK | MB_ICONEXCLAMATION);
    }*/

    system_getInfo(); // Get system info (Windows version, Wine version, etc.)

    // Patch the game
    ok = hook_patch(); 
    if (!ok) return FALSE;

    return TRUE;
}


/**
 * New entry point for the our DLL.
 * This function is called when the application is started.
 * It gets called by the JMP instruction at the original entry point.
 */
void __cdecl main_newEntryPoint() {
    
    // Restore the original bytes at the original entry point
    patch_copy((unsigned int)originalEntryPoint, originalBytes, sizeof(originalBytes));

    // Run our code
    bool ok = main_load();
    if (!ok) {
        ExitProcess(EXIT_FAILURE);
        return;
    }

    // Jump back to the original entry point
    ((void (__cdecl *)(void))originalEntryPoint)();
}


/**
 * Hook the application's entry point
 */
bool main_patchEntryPoint() {

    // Load the original mss32.dll functions
    bool ok = mss32_load();
    if (!ok) return FALSE;

    // Check if process is SinglePlayer, if it is, exit
    char processName[MAX_PATH];
    GetModuleFileNameA(EXE_HMODULE, processName, MAX_PATH);
    char* baseName = strrchr(processName, '\\');
    baseName = baseName ? baseName + 1 : processName;
    if (strnicmp(baseName, "CoD2SP_s", 8) == 0) {
        return TRUE;
    }

    // Check if this is CoD2MP version 1.3
    char* cod2 = (char*)0x0059b6c0;
    if (strcmp(cod2, "pc_1.3_1_1") != 0) {
        MessageBoxA(NULL, 
            "CoD2x " APP_VERSION " is not installed correctly.\n\n"
            "You have to install patch 1.3 before installing CoD2x!", "CoD2x error", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    // Get name of the DLL file
    char* lastBackslash = strrchr(DLL_PATH, '\\');
    if (!lastBackslash) {
        SHOW_ERROR("Failed to get DLL file name.");
        return FALSE;
    }

    // Check if this DLL file name is mss32.dll
    if (stricmp(lastBackslash + 1, APP_MODULE_NAME) != 0) {      
        #if DEBUG
            // If the DLL name is not mss32.dll, we can assume that the DLL is being hot-reloaded in debug mode
            DLL_HOTRELOAD = true;
            ok = hook_patch(); 
            if (!ok) {
                ExitProcess(EXIT_FAILURE);
                return false;
            }
            return TRUE;
        #else
            MessageBoxA(NULL, 
                "CoD2x " APP_VERSION " is not installed correctly.\n\n"
                "Invalid name of mss32.dll file!", "CoD2x error", MB_OK | MB_ICONERROR);
            return FALSE;
        #endif       
    }


    originalEntryPoint = (void*)0x0057db54; // Entry point of CoD2MP_s.exe

    // Save the original bytes at the entry point
    memcpy(originalBytes, originalEntryPoint, sizeof(originalBytes));

    // Patch the entry point with a jump to our new entry point
    patch_jump((unsigned int)originalEntryPoint, (unsigned int)&main_newEntryPoint);

    return TRUE;
}


/**
 * Get the module, executable path and directory
 */
bool main_getExeData() {
    WCHAR exePathW[MAX_PATH];
    char exePath[MAX_PATH];
    char exeDirectory[MAX_PATH];

    // Get the base address of the application
    HMODULE hModule = GetModuleHandle(NULL);
    if (!hModule) {
        SHOW_ERROR_WITH_LAST_ERROR("Failed to get application base address.");
        return false; 
    }
    EXE_HMODULE = hModule;

    // Get the full path of the current executable in ANSI
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0) {
        SHOW_ERROR_WITH_LAST_ERROR("Failed to get the executable path.");
        return false;
    }
    // Get the full path of the current executable in Unicode
    DWORD lenW = GetModuleFileNameW(NULL, exePathW, MAX_PATH);
    if (lenW == 0) {
        SHOW_ERROR_WITH_LAST_ERROR("Failed to get the executable path in Unicode.");
        return false;
    }
    if (lenW >= MAX_PATH - 30) { // 30 is a reserve
        // Path may be truncated; recommend using a larger buffer or dynamic allocation for long paths
        showErrorMessage("Path too long",
            "Call of Duty 2 directory path is too long.\n\n"
            "Please move the game to a directory with a shorter path.\n\n"
            "Current path: \n%s\n\n", exePath);
        return false;
    }

    // Check if the path contains non-standard characters
    // Convert to ANSI using system default code page
    CHAR exePathWConverted[MAX_PATH];
    int lenA = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, exePathW, -1, exePathWConverted, MAX_PATH, NULL, NULL);
    bool containsWideChars = (lenA == 0 || lenA >= MAX_PATH);
    // Check for '?' replacements (common for unconvertible Unicode)
    for (int i = 0; i < lenA; i++) {
        if (exePathWConverted[i] == '?') {
            containsWideChars = true; // Found unrepresentable character
            break;
        }
    }
    if (containsWideChars) {
        showErrorMessage("Non-standard characters in path",  
            "Call of Duty 2 directory path contains non-standard characters.\n\n"
            "Please move the game folder or rename the directory path to a path that contains only standard characters (A-Z, a-z, 0-9, and symbols like _).\n\n"
            "Path: \n%s\n\n"
            "'?' - question mark represents the invalid character", exePathWConverted);
        return false;
    }

    // Extract the directory from the executable path
    strncpy(exeDirectory, exePath, MAX_PATH);
    char* lastBackslash = strrchr(exeDirectory, '\\');
    if (lastBackslash) {
        *lastBackslash = '\0'; // Terminate the string at the last backslash
    } else {
        SHOW_ERROR_WITH_LAST_ERROR("Failed to determine executable directory.");
        return false;
    }

    // Save the paths
    strncpy(EXE_PATH, exePath, MAX_PATH - 1);
    strncpy(EXE_DIRECTORY_PATH, exeDirectory, MAX_PATH - 1);



    // Save the command line of the executable
    LPSTR cmdLine = GetCommandLineA();
    //   cmdLine = "\"C:\\Games\\Call of Duty 2\\CoD2SP_s.exe\" +set fs_game mods/test"
    //   cmdLine = "C:\\Games\\Call of Duty 2\\CoD2SP_s.exe +set fs_game mods/test"
    if (cmdLine && cmdLine[0] == '"') {
        cmdLine = strchr(cmdLine + 1, '"');
        if (cmdLine) ++cmdLine;
    } else if (cmdLine) {
        cmdLine = strchr(cmdLine, ' ');
    }
    if (cmdLine) {
        while (*cmdLine == ' ') ++cmdLine;
    } else {
        cmdLine = (LPSTR)"";
    }
    strncpy(EXE_COMMAND_LINE, cmdLine, MAX_PATH - 1);




    // Ensure CWD is set to the executable directory
    if (!SetCurrentDirectoryA(EXE_DIRECTORY_PATH)) {
        SHOW_ERROR_WITH_LAST_ERROR("Failed to set current directory");
        return false;
    }

    

    // Get this DLL path
    HMODULE dllHandle = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&main_getExeData, &dllHandle)) {
        SHOW_ERROR_WITH_LAST_ERROR("Failed to get DLL handle");
        return false;
    }
    char dllPath[MAX_PATH];
    if (!GetModuleFileNameA(dllHandle, dllPath, MAX_PATH)) {
        SHOW_ERROR_WITH_LAST_ERROR("Failed to get current DLL path");
        return false;
    }
    strncpy(DLL_PATH, dllPath, MAX_PATH - 1);
    

    return true;
}


/**
 * Entry point for the DLL.
 * When the system calls the DllMain function with the DLL_PROCESS_ATTACH value, the function returns TRUE if it succeeds or FALSE if initialization fails.
 */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) 
    {
        case DLL_PROCESS_ATTACH: 
        {
            bool ok;

            ok = main_getExeData();
            if (!ok) ExitProcess(EXIT_FAILURE);

            ok = main_patchEntryPoint();
            if (!ok) ExitProcess(EXIT_FAILURE);
            
            break;
        }

        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}