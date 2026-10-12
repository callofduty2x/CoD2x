#include "hotreload.h"

#include <windows.h>

#include "shared.h"
#include "../shared/cod2_cmd.h"

bool volatile hotreload_requested = false;

void hotreload_loadDLL_cmd() {
    hotreload_requested = true;
}

void hotreload_loadDLL() {

    if (!hotreload_requested) {
        return;
    }

    char newDllPath[MAX_PATH];
    int suffix = 0;

    // Try copying the DLL with a numeric suffix until it succeeds
    do {
        snprintf(newDllPath, MAX_PATH, "mss32_hotreload_%d.dll", suffix++);
    } while (CopyFileA(APP_MODULE_NAME, newDllPath, FALSE) == FALSE && GetLastError() == ERROR_SHARING_VIOLATION);

    if (GetLastError() != ERROR_SUCCESS) {
        SHOW_ERROR_WITH_LAST_ERROR("Failed to copy DLL to new location with suffix");
        hotreload_requested = false;
        return;
    }

    // Load the copied DLL
    HMODULE hDll = LoadLibraryA(newDllPath);
    if (!hDll) {
        SHOW_ERROR_WITH_LAST_ERROR("Failed to load copied DLL");
        hotreload_requested = false;
        return;
    }

    // Wait for the DLL to be loaded
    while (GetModuleHandleA(newDllPath) == NULL) {
        Sleep(10);
    }
}


bool hotreload_getDllWriteTime(FILETIME* lastWriteTime, bool showError) {
    HANDLE hFile = CreateFileA(APP_MODULE_NAME, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        if (showError) {
            SHOW_ERROR_WITH_LAST_ERROR("Failed to open DLL file for getting write time");
        }
        return false;
    }

    if (!GetFileTime(hFile, NULL, NULL, lastWriteTime)) {
        if (showError) {
            SHOW_ERROR_WITH_LAST_ERROR("Failed to get last write time of DLL file");
        }
        CloseHandle(hFile);
        return false;
    }

    CloseHandle(hFile);

    return true;
}



void hotreload_watch_dll_thread() {
    thread_setName(GetCurrentThread(), L"CoD2x HotReload");

    FILETIME lastWriteTime;
    if (!hotreload_getDllWriteTime(&lastWriteTime, true)) {
        return;
    }

    while (!hotreload_requested) {
        Sleep(250);

        FILETIME currentWriteTime;
        if (!hotreload_getDllWriteTime(&currentWriteTime, false)) {
            continue; // Skip this cycle if we can't get the write time
        }

        // Check if the file's last write time has changed
        if (CompareFileTime(&lastWriteTime, &currentWriteTime) != 0) {
            // File has changed, wait for stabilization
            Sleep(150); // Stabilization time

            FILETIME stabilizedWriteTime;
            if (!hotreload_getDllWriteTime(&stabilizedWriteTime, false)) {
                continue; // Skip this cycle if we can't get the write time
            }

            // Confirm the file is stable (write time hasn't changed during stabilization period)
            if (CompareFileTime(&currentWriteTime, &stabilizedWriteTime) == 0) {
                lastWriteTime = stabilizedWriteTime;
                hotreload_requested = true; // Request reload
                return;
            }
        }
    }

}


#if DEBUG

void hotreload_watch_dll() {
    // Spawn thread to watch the DLL for changes
    CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)hotreload_watch_dll_thread, NULL, 0, NULL);
}


/** Called once when hot-reloading is activated */
void hotreload_unload() {
    Cmd_RemoveCommand("reload");
}


/** Called once at game start after common initialization. Used to initialize variables, cvars, etc. */
void hotreload_init() {
    Cmd_AddCommand("reload", hotreload_loadDLL_cmd);
}

#endif