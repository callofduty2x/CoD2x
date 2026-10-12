#include "system.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "shared.h"

char SYS_WIN_BUILD[64] = {0};
char SYS_WIN_EDITION[64] = {0};
char SYS_WIN_DISPLAY_VERSION[32] = {0};
char SYS_WINE_VERSION[64] = {0};
char SYS_WINE_BUILD[64] = {0};
char SYS_VERSION_INFO[256] = {0};
char SYS_VM_NAME[64] = {0};
char SYS_COMPATIBILITY_FLAGS[64] = {0};
char SYS_LANGUAGE_CODE[8] = {0};
char SYS_VIRTUALIZATION[128] = {0};
bool SYS_IS_ELEVATED = false;

extern bool admin_isElevated();


// Retrieves Windows version information from the registry to get the real data (to bypass compatibility modes).
void system_getWindowsVersionInfoFromRegistry() {
    HKEY hKey;
    LONG status;

    // Try 64-bit registry view first
    status = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        0,
        KEY_READ | KEY_WOW64_64KEY,
        &hKey
    );

    if (status != ERROR_SUCCESS) {
        // Fallback: try 32-bit view (or 32-bit OS)
        status = RegOpenKeyExA(
            HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
            0,
            KEY_READ,
            &hKey
        );
    }

    if (status != ERROR_SUCCESS) {
        // Failed to open registry key
        return;
    }

    DWORD size;

    // Build number (required)
    size = sizeof(SYS_WIN_BUILD);
    RegQueryValueExA(hKey, "CurrentBuildNumber", NULL, NULL, (LPBYTE)SYS_WIN_BUILD, &size);

    // EditionID (optional)
    size = sizeof(SYS_WIN_EDITION);
    RegQueryValueExA(hKey, "EditionID", NULL, NULL, (LPBYTE)SYS_WIN_EDITION, &size);

    // DisplayVersion (optional)
    size = sizeof(SYS_WIN_DISPLAY_VERSION);
    RegQueryValueExA(hKey, "DisplayVersion", NULL, NULL, (LPBYTE)SYS_WIN_DISPLAY_VERSION, &size);

    RegCloseKey(hKey);
}

// Retrieves the Wine version string and build info if running under Wine.
bool system_getWineVersion() {
    SYS_WINE_VERSION[0] = '\0';

    HMODULE hNtDll = GetModuleHandleA("ntdll.dll");
    if (!hNtDll) return false;

    typedef const char* (__cdecl *wine_get_version_t)(void);
    typedef const char* (__cdecl *wine_get_build_id_t)(void);

    wine_get_version_t wine_get_version = (wine_get_version_t)(void*)GetProcAddress(hNtDll, "wine_get_version");
    wine_get_build_id_t wine_get_build_id = (wine_get_build_id_t)(void*)GetProcAddress(hNtDll, "wine_get_build_id");

    if (!wine_get_version) return false;

    const char* version = wine_get_version();
    const char* build_id = wine_get_build_id ? wine_get_build_id() : NULL;

    // Copy strings
    if (version) {
        snprintf(SYS_WINE_VERSION, sizeof(SYS_WINE_VERSION), "%s", version);
    }
    if (build_id) {
        snprintf(SYS_WINE_BUILD, sizeof(SYS_WINE_BUILD), "%s", build_id);
    }

    return true;
}


/**
 * Check if "Run as Administrator" or compatibility flag is set for the specified executable.
 * Returns true if any flag is set, false otherwise.
 */
void system_getCompatibilityFlags() {
    HKEY hKey;
    const char* registryPath = "Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
    LONG result;
    char valueData[512] = {0};
    DWORD valueSize = sizeof(valueData);

    // Open the Layers registry key for querying value
    result = RegOpenKeyExA(HKEY_CURRENT_USER, registryPath, 0, KEY_QUERY_VALUE, &hKey);
    if (result != ERROR_SUCCESS) return; // Key does not exist, so no flags are set

    // Query the value for the specified executable path
    result = RegQueryValueExA(hKey, EXE_PATH, NULL, NULL, (LPBYTE)valueData, &valueSize);

    RegCloseKey(hKey);

    if (result != ERROR_SUCCESS) return; // Value does not exist, so no flags are set

    // List of known compatibility flags to check
    // "RUNASADMIN", "WIN95", "WIN98", "WINXPSP2", "WINXPSP3", "VISTARTM", "VISTASP1", "WIN7RTM", "WIN8RTM",

    strncpy(SYS_COMPATIBILITY_FLAGS, valueData, sizeof(SYS_COMPATIBILITY_FLAGS) - 1);
}


// Checks if the system is running under a virtual machine by checking the BIOS information.
bool system_isVirtualMachineBios() {
    char buffer[256] = {0};
    DWORD bufferSize = sizeof(buffer);

    SYS_VM_NAME[0] = '\0';

    LSTATUS result = RegGetValueA(
        HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\BIOS",
        "SystemManufacturer",
        RRF_RT_REG_SZ,
        NULL,
        buffer,
        &bufferSize
    );

    if (result != ERROR_SUCCESS)
        return false;

    // Convert to lowercase for case-insensitive match
    CharLowerA(buffer);

    if (strstr(buffer, "vmware") != NULL) {
        strncpy(SYS_VM_NAME, "VMware", sizeof(SYS_VM_NAME) - 1);
        return true;
    }
    if (strstr(buffer, "virtualbox") != NULL) {
        strncpy(SYS_VM_NAME, "VirtualBox", sizeof(SYS_VM_NAME) - 1);
        return true;
    }
    if (strstr(buffer, "qemu") != NULL) {
        strncpy(SYS_VM_NAME, "QEMU", sizeof(SYS_VM_NAME) - 1);
        return true;
    }
    if (strstr(buffer, "microsoft corporation") != NULL) { // Hyper-V
        strncpy(SYS_VM_NAME, "Hyper-V", sizeof(SYS_VM_NAME) - 1);
        return true;
    }
    if (strstr(buffer, "parallels") != NULL) {
        strncpy(SYS_VM_NAME, "Parallels", sizeof(SYS_VM_NAME) - 1);
        return true;
    }

    return false;
}

void system_getLanguageCode(char *buffer, int bufferSize) {
    if (!GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SISO3166CTRYNAME, buffer, bufferSize)) {
        snprintf(buffer, bufferSize, "??");
    }
}


void system_getInfo() {
    // Get Windows version info
    system_getWindowsVersionInfoFromRegistry();

    // Get Wine version if running under Wine
    bool isWine = system_getWineVersion();

    // Get virtualization info
    bool isVM = system_isVirtualMachineBios();

    char virtualization[128];
    if (isWine) {
        snprintf(virtualization, sizeof(virtualization), "wine-%s-%s", SYS_WINE_VERSION, SYS_WINE_BUILD);
    } else if (isVM) {
        snprintf(virtualization, sizeof(virtualization), "VM-%s", SYS_VM_NAME);
    } else {
        snprintf(virtualization, sizeof(virtualization), "0");
    }

    // Get language code
    char languageCode[8] = {0};
    system_getLanguageCode(languageCode, sizeof(languageCode)); 

    // Get compatibility flags
    system_getCompatibilityFlags();

    // If running with admin rights, append "admin" to the virtualization string
    bool isElevated = admin_isElevated();

    // Store computed values as globals
    strncpy(SYS_LANGUAGE_CODE, languageCode, sizeof(SYS_LANGUAGE_CODE) - 1);
    strncpy(SYS_VIRTUALIZATION, virtualization, sizeof(SYS_VIRTUALIZATION) - 1);
    SYS_IS_ELEVATED = isElevated;

    // Compose final version info string
    // Format: "<build>-<edition>-<displayVersion>-<language>-<virtualization>-<compatibilityFlags>-<admin>"
    snprintf(SYS_VERSION_INFO, sizeof(SYS_VERSION_INFO), "%s\n%s\n%s\n%s\n%s\n%s\n%s", SYS_WIN_BUILD, SYS_WIN_EDITION, SYS_WIN_DISPLAY_VERSION, SYS_LANGUAGE_CODE, virtualization, SYS_COMPATIBILITY_FLAGS, isElevated ? "admin" : "user");

    //MessageBoxA(NULL, SYS_VERSION_INFO, "System Info", MB_OK | MB_ICONINFORMATION);
}