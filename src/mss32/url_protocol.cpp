#include "url_protocol.h"
#include <stdio.h>
#include "../shared/cod2.h"

/**
 * @brief Retrieves the Call of Duty 2 executable path from the Windows registry.
 *
 * This function opens the registry key that contains the path to the Call of Duty 2 executable
 * and reads the value into the provided buffer.
 *
 * @param exe_path A pointer to a buffer where the executable path will be stored.
 * @param size The size of the buffer to hold the executable path.
 *
 * @return 1 if the executable path is successfully retrieved, 0 if there is an error.
 */
int get_cod2_exe_path(char *exe_path, DWORD size)
{
    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_LOCAL_MACHINE, REG_COD2_PATH, 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS)
    {
        Com_Printf("Error: Unable to open the Call of Duty 2 registry key.\n");
        return 0; // Return 0 if the registry key cannot be opened.
    }

    result = RegQueryValueEx(hKey, REG_COD2_KEY, NULL, NULL, (LPBYTE)exe_path, &size);
    RegCloseKey(hKey);

    if (result != ERROR_SUCCESS)
    {
        Com_Printf("Error: Unable to read the executable path.\n");
        return 0; // Return 0 if the executable path cannot be read.
    }

    return 1; // Return 1 if successful.
}

/**
 * @brief Checks if the cod2:// protocol is already registered in the Windows registry.
 *
 * This function checks if the specified protocol (cod2://) is registered in the system by
 * looking for the protocol key in the Windows registry.
 *
 * @return 1 if the protocol is found, 0 if it is not registered.
 */
int check_protocol_exists()
{
    HKEY hKey;
    char keyPath[256];
    snprintf(keyPath, sizeof(keyPath), "SOFTWARE\\Classes\\%s", PROTOCOL_NAME); // Generate the registry key path.

    // Check if the registry key exists.
    if (RegOpenKeyEx(HKEY_CURRENT_USER, keyPath, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        return 1; // Return 1 if the protocol key exists.
    }
    return 0; // Return 0 if the protocol key does not exist.
}

/**
 * @brief Registers the cod2:// protocol in the Windows registry.
 *
 * This function registers the cod2:// protocol by adding the necessary keys and values in the
 * Windows registry, so that the system knows how to handle URLs with this protocol.
 *
 * @param exe_path The path to the Call of Duty 2 executable to use when the protocol is invoked.
 *
 * @return 1 if the protocol is successfully registered, 0 if there is an error.
 */
int register_protocol(const char *exe_path)
{
    HKEY hKey;
    DWORD disposition;
    char command[512];
    char keyPath[256];

    snprintf(keyPath, sizeof(keyPath), "SOFTWARE\\Classes\\%s", PROTOCOL_NAME); // Set the registry path for the protocol.

    // Create the protocol key in the registry.
    if (RegCreateKeyEx(HKEY_CURRENT_USER, keyPath, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &disposition) != ERROR_SUCCESS)
    {
        Com_Printf("[URL Protocol] Error creating protocol key.\n");
        return 0; // Return 0 if the key creation fails.
    }

    // Set protocol description.
    RegSetValueEx(hKey, NULL, 0, REG_SZ, (BYTE *)"URL:cod2 Protocol", strlen("URL:cod2 Protocol") + 1);
    RegSetValueEx(hKey, "URL Protocol", 0, REG_SZ, (BYTE *)"", 1);
    RegCloseKey(hKey); // Close the protocol key.

    // Set up the command to run when the protocol is invoked.
    snprintf(keyPath, sizeof(keyPath), "SOFTWARE\\Classes\\%s\\shell\\open\\command", PROTOCOL_NAME);
    if (RegCreateKeyEx(HKEY_CURRENT_USER, keyPath, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &disposition) != ERROR_SUCCESS)
    {
        Com_Printf("[URL Protocol] Error creating command key.\n");
        return 0; // Return 0 if the command key creation fails.
    }

    // Define the command to run when the protocol is triggered.
    snprintf(command, sizeof(command), "\"%s\" +openurl \"\"%s\"\"", exe_path, "%%1" + 7); // Strip 'cod2://' prefix from URL.
    RegSetValueEx(hKey, NULL, 0, REG_SZ, (BYTE *)command, strlen(command) + 1);
    RegCloseKey(hKey); // Close the command key.

    Com_Printf("[URL Protocol] cod2:// protocol registered successfully!\n");
    return 1; // Return 1 if the protocol is registered successfully.
}

/**
 * @brief Initializes the cod2:// URL protocol by checking if it is already registered.
 *
 * This function checks if the cod2:// protocol is registered on the system. If it is not,
 * it attempts to register the protocol by locating the Call of Duty 2 executable and creating
 * the necessary registry keys.
 *
 * @return 0 on success, 1 if an error occurs during initialization or registration.
 */
int url_protocol_init()
{
    if (check_protocol_exists())
    {
        Com_Printf("[URL Protocol] The cod2:// protocol is already registered.\n");
    }
    else
    {
        Com_Printf("[URL Protocol] cod2:// protocol not found. Registering...\n");

        char exe_path[MAX_PATH] = {0};
        // Attempt to get the Call of Duty 2 executable path from the registry.
        if (!get_cod2_exe_path(exe_path, sizeof(exe_path)))
        {
            Com_Printf("[URL Protocol] Error obtaining the Call of Duty 2 executable path.\n");
            return 1; // Return 1 if the executable path cannot be obtained.
        }

        Com_Printf("[URL Protocol] Found executable path: %s\n", exe_path);

        // Attempt to register the protocol.
        if (register_protocol(exe_path))
        {
            Com_Printf("[URL Protocol] Registration completed successfully!\n");
        }
        else
        {
            Com_Printf("[URL Protocol] Failed to register the protocol.\n");
        }
    }
    return 0; // Return 0 if the protocol is either already registered or successfully registered.
}

/**
 * @brief Processes the +openurl command from the URL protocol and attempts to connect to the specified IP address.
 *
 * This function is triggered when a URL like `cod2://127.0.0.1` is provided, extracts the IP address,
 * and constructs a command to connect to the specified IP in Call of Duty 2.
 */
void url_connect()
{
    // Get the full argument string passed after the command.
    const char *args = Cmd_Argv(1); // Expects something like "cod2://127.0.0.1"
    const char *prefix = "cod2://";
    size_t prefixLen = strlen(prefix);

    // Check if the argument starts with the "cod2://" prefix.
    if (strncmp(args, prefix, prefixLen) != 0)
    {
        Com_Printf("[URL Protocol] Invalid URL! It must start with '%s'.\n", prefix);
        return; // Return if the URL is invalid.
    }

    // Extract the IP address from the URL (everything after "cod2://").
    const char *ip = args + prefixLen;
    if (ip[0] == '\0')
    {
        Com_Printf("[URL Protocol] No IP provided after '%s'.\n", prefix);
        return; // Return if no IP is provided.
    }

    // Construct the "connect <IP>" command for the game.
    char connectCmd[256];
    snprintf(connectCmd, sizeof(connectCmd), "connect %s\n", ip);

    // Send the command to the game console.
    Cbuf_AddText(connectCmd);
    Com_Printf("[URL Protocol] Sending command: %s", connectCmd);
}
