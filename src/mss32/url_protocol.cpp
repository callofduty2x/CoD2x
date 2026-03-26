#include "url_protocol.h"

#include <windows.h>

#include "shared.h"


#define CMD_LINE (*((char (*)[1024])(0x00d53a80)))

/** 
 * Register the URL protocol for the application.
 * It registers the "cod2x://" protocol in the Windows registry
 * It does not require admin rights to register the protocol.
 */
bool url_protocol_registerUrlProtocol() {
    const char* protocolKeyPath = "Software\\Classes\\CoD2x";
    const char* commandFormat = "\"%s\" \"%%1\"";
    char command[1024] = {0};

    snprintf(command, sizeof(command), commandFormat, EXE_PATH);

    // Check if protocol exists and matches (in HKCU)
    LONG lResult;
    HKEY hKey;
    lResult = RegOpenKeyExA(HKEY_CURRENT_USER, protocolKeyPath, 0, KEY_READ, &hKey);
    if (lResult == ERROR_SUCCESS) {
        char subKey[256];
        snprintf(subKey, sizeof(subKey), "%s\\shell\\open\\command", protocolKeyPath);

        HKEY hCommandKey;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, subKey, 0, KEY_READ, &hCommandKey) == ERROR_SUCCESS) {
            char currentValue[MAX_PATH * 2] = {0};
            DWORD currentValueSize = sizeof(currentValue);
            DWORD dwType = 0;
            if (RegQueryValueExA(hCommandKey, NULL, NULL, &dwType, (LPBYTE)currentValue, &currentValueSize) == ERROR_SUCCESS) {
                if (_stricmp(currentValue, command) == 0) {
                    RegCloseKey(hCommandKey);
                    RegCloseKey(hKey);
                    // Already registered with correct command
                    return true;
                }
            }
            RegCloseKey(hCommandKey);
        }
        RegCloseKey(hKey);
    }

    // Create the registry key for the protocol
    LONG result = RegCreateKeyExA(HKEY_CURRENT_USER, protocolKeyPath, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (result != ERROR_SUCCESS) {
        showErrorMessage("CoD2x Error", "Failed to register cod2x:// protocol in user registry.\n\n");
        return false;
    }

    // Set the default value and "URL Protocol"
    const char* description = "URL:Call of Duty 2 Protocol";
    RegSetValueExA(hKey, NULL, 0, REG_SZ, (const BYTE*)description, (DWORD)strlen(description) + 1);
    RegSetValueExA(hKey, "URL Protocol", 0, REG_SZ, (const BYTE*)"", 1);

    // Create the shell\open\command subkey
    HKEY hCmdKey;
    result = RegCreateKeyExA(hKey, "shell\\open\\command", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hCmdKey, NULL);
    if (result == ERROR_SUCCESS) {
        // Set the command to launch the game with the URL as parameter
        RegSetValueExA(hCmdKey, NULL, 0, REG_SZ, (const BYTE*)command, (DWORD)strlen(command) + 1);
        RegCloseKey(hCmdKey);
    }
    RegCloseKey(hKey);
    return true;
}


char url_protocol_fromHex(char c) {
    return (char)(isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
}
void url_protocol_urlDecode(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        if ((*src == '%') && src[1] && src[2] &&
            isxdigit(src[1]) && isxdigit(src[2])) {
            *dst++ = (url_protocol_fromHex(src[1]) << 4) | url_protocol_fromHex(src[2]);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';  // Replace + with space
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/** Called only once on game start before common inicialization. */
void url_protocol_init() {

    //MessageBoxA(NULL, CMD_LINE, "CoD2x", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);

    // When the game is launched via URL link in the browser, the command line starts with "cod2x://"
    // Example:
    //   "cod2x://%20%2Bconnect%20192.168.1.1%3A28960%20%2Bpassword%20pivo%20%2Brcon%20login%20aaa/"
    //   "cod2x://%2Bconnect+88.198.58.188:27397/"

    // Handle the URL protocol
    if (strnicmp(CMD_LINE, "\"cod2x://", 9) == 0) {

        // Remove the "cod2x://" prefix and url decode the rest
        char* url = CMD_LINE + 9; // Skip "cod2x://
        url_protocol_urlDecode(url);

        // Remove trailing quote and optional slash before it
        size_t len = strlen(url);
        if (len >= 1 && url[len - 1] == '"') {
            url[len - 1] = '\0';
            len--;
            if (len >= 1 && url[len - 1] == '/') {
                url[len - 1] = '\0';
            }
        }

        // Save the adjusted string back to CMD_LINE
        strncpy(CMD_LINE, url, sizeof(CMD_LINE) - 1);
        CMD_LINE[sizeof(CMD_LINE) - 1] = '\0';

        //MessageBoxA(NULL, CMD_LINE, "CoD2x URL", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    }


    // Register the URL protocol
    bool ok = url_protocol_registerUrlProtocol();
    (void)ok;

    #if DEBUG
    if (!ok) {
        MessageBoxA(NULL, "Failed to register URL protocol 'cod2x://'.", "CoD2x Error", MB_OK | MB_ICONERROR);
    }
    #endif
}