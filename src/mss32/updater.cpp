#include "updater.h"

#include <windows.h>
#include <wininet.h>

#include "shared.h"
#include "hwid.h"
#include "system.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_client.h"
#include "../shared/cod2_net.h"
#include "../shared/cod2_cmd.h"
#include "../shared/cod2_shared.h"


#define cl_updateAvailable (*(dvar_t **)(0x0096b644))
#define cl_updateVersion (*(dvar_t **)(0x0096b640))
#define cl_updateOldVersion (*(dvar_t **)(0x0096b64c))
#define cl_updateFiles (*(dvar_t **)(0x0096b5d4))

#define clientState                   (*((clientState_e*)0x00609fe0))
#define demo_isPlaying                (*((int*)0x0064a170))

int                 updater_lastClientState = -1;
int                 updater_lastMainMenuClosed = 0;
bool                updater_waitingForResponse = false;
int                 updater_waitingForResponseRepeats = 0;
DWORD               updater_waitingForResponseTime = 0;
bool                updater_forcedUpdate = false;
struct netaddr_s    updater_address = { NA_INIT, {0}, 0, {0} };
dvar_t*             sv_update;

extern dvar_t *cl_hwid;
extern dvar_t *cl_hwid2;
extern char hwid_old[33];  
extern char hwid_regid[33];
extern char hwid_changed_diff[1024];
extern int hwid_changed_count;


void updater_showForceUpdateDialog() {
    Com_Error(ERR_DROP, "Update required\n\nA new version of CoD2x must be installed.\nPlease update to version %s.\n", cl_updateVersion->value.string);
}

bool updater_downloadDLL(const char *url, const char *downloadPath, char *errorBuffer, size_t errorBufferSize, int retryCount = 0) {
    // Initialize WinINet
    HINTERNET hInternet = InternetOpenA("CoD2x " APP_VERSION " Update Downloader", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) {
        snprintf(errorBuffer, errorBufferSize, "Failed to initialize WinINet.");
        return false;
    }

    // Open URL
    HINTERNET hFile = InternetOpenUrlA(hInternet, url, NULL, 0, INTERNET_FLAG_RELOAD, 0);
    if (!hFile) {
        snprintf(errorBuffer, errorBufferSize, "Failed to open URL '%s'.", url);
        InternetCloseHandle(hInternet);
        return false;
    }

    // Check HTTP status code
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!HttpQueryInfoA(hFile, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusCodeSize, NULL)) {
        snprintf(errorBuffer, errorBufferSize, "Failed to query HTTP status code for URL '%s'.", url);
        InternetCloseHandle(hFile);
        InternetCloseHandle(hInternet);
        return false;
    }

    // Handle HTTP errors (e.g., 404 Not Found)
    if (statusCode != 200) {
        snprintf(errorBuffer, errorBufferSize, "HTTP error %lu encountered for URL '%s'.", statusCode, url);
        InternetCloseHandle(hFile);
        InternetCloseHandle(hInternet);
        return false;
    }

    // Retrieve expected file size from the Content-Length header.
    DWORD contentLength = 0;
    DWORD contentLengthSize = sizeof(contentLength);
    if (!HttpQueryInfoA(hFile, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &contentLength, &contentLengthSize, NULL)) {
        snprintf(errorBuffer, errorBufferSize, "Failed to retrieve content length for URL '%s'.", url);
        InternetCloseHandle(hFile);
        InternetCloseHandle(hInternet);
        return false;
    }

    // Create file using CreateFile
    HANDLE hLocalFile = CreateFileA(
        downloadPath,
        GENERIC_WRITE,           // Write access
        FILE_SHARE_READ,         // Allow read sharing
        NULL,                    // Default security
        CREATE_ALWAYS,           // Create new file or overwrite
        FILE_ATTRIBUTE_NORMAL,   // Normal file
        NULL                     // No template
    );

    if (hLocalFile == INVALID_HANDLE_VALUE) {
        snprintf(errorBuffer, errorBufferSize, "Failed to create file at '%s'.", downloadPath);
        InternetCloseHandle(hFile);
        InternetCloseHandle(hInternet);
        return false;
    }

    // Download and write data
    char buffer[4096];
    DWORD bytesRead;
    DWORD bytesWritten;
    DWORD totalBytesDownloaded = 0;

    while (InternetReadFile(hFile, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        totalBytesDownloaded += bytesRead;
        if (!WriteFile(hLocalFile, buffer, bytesRead, &bytesWritten, NULL)) {
            snprintf(errorBuffer, errorBufferSize, "Failed to write to file '%s'.", downloadPath);
            CloseHandle(hLocalFile);
            InternetCloseHandle(hFile);
            InternetCloseHandle(hInternet);
            return false;
        }

        if (bytesWritten != bytesRead) {
            snprintf(errorBuffer, errorBufferSize, "Mismatch in bytes read and written to file '%s'.", downloadPath);
            CloseHandle(hLocalFile);
            InternetCloseHandle(hFile);
            InternetCloseHandle(hInternet);
            return false;
        }
    }

    // Validate the download integrity
    if (totalBytesDownloaded != contentLength) {
        CloseHandle(hLocalFile);
        InternetCloseHandle(hFile);
        InternetCloseHandle(hInternet);

        // Try again
        retryCount++;

        if (retryCount > 3) {
            snprintf(errorBuffer, errorBufferSize, "File download incomplete: expected %lu bytes, got %lu bytes.", contentLength, totalBytesDownloaded);
            return false; // Too many retries
        }

        return updater_downloadDLL(url, downloadPath, errorBuffer, errorBufferSize, retryCount);
    }

    // Clean up
    CloseHandle(hLocalFile);
    InternetCloseHandle(hFile);
    InternetCloseHandle(hInternet);

    return true; // Success
}


bool updater_downloadAndReplaceDllFile(const char *url, char *errorBuffer, size_t errorBufferSize) { 

    const char destinationFileName[] = "mss32_new.dll";
    const char oldFileName[] = "mss32_old.dll";
    const char currentFileName[] = "mss32.dll";

    char dllFilePathNew[sizeof(EXE_DIRECTORY_PATH) + sizeof(destinationFileName) + 1];
    char dllFilePathOld[sizeof(EXE_DIRECTORY_PATH) + sizeof(oldFileName) + 1];
    char dllFilePathCurrent[sizeof(EXE_DIRECTORY_PATH) + sizeof(currentFileName) + 1];

    // Construct the destination path (same directory as the executable)
    snprintf(dllFilePathNew, sizeof(dllFilePathNew), "%s\\%s", EXE_DIRECTORY_PATH, destinationFileName);  
    snprintf(dllFilePathOld, sizeof(dllFilePathOld), "%s\\%s", EXE_DIRECTORY_PATH, oldFileName);
    snprintf(dllFilePathCurrent, sizeof(dllFilePathCurrent), "%s\\%s", EXE_DIRECTORY_PATH, currentFileName);


    // Download the DLL
    bool ok = updater_downloadDLL(url, dllFilePathNew, errorBuffer, errorBufferSize);
    if (!ok) return false;


    // Rename the existing DLL
    if (!MoveFileEx(dllFilePathCurrent, dllFilePathOld, MOVEFILE_REPLACE_EXISTING)) {
        snprintf(errorBuffer, errorBufferSize, "Error renaming DLL from '%s' to '%s'.", dllFilePathCurrent, dllFilePathOld);

        return false;
    }

    // Rename the new DLL to the original name
    if (!MoveFileEx(dllFilePathNew, dllFilePathCurrent, FALSE)) {
        snprintf(errorBuffer, errorBufferSize, "Error copying new DLL from '%s' to '%s'.", dllFilePathNew, dllFilePathCurrent);
        
        // Attempt to restore the original DLL name
        MoveFileEx(dllFilePathOld, dllFilePathCurrent, MOVEFILE_REPLACE_EXISTING);
        return false;
    }

    // Schedule the deletion of the old DLL
    // Since the DLL is locked by the application, we can't delete it immediately
    if (!MoveFileEx(dllFilePathOld, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        // Since its possible to execute only with admin rights, dont show error
        //snprintf(errorBuffer, errorBufferSize, "Error scheduling old DLL deletion: '%s'.", dllFilePathOld);
        //return false;
    }

    return true;
}


bool updater_resolveServerAddress() {
    // Resolve the Auto-Update server address if not already resolved
    if (updater_address.type == NA_INIT) 
    {
        Com_DPrintf("Resolving AutoUpdate Server %s...\n", SERVER_UPDATE_URI);
        if (!NET_StringToAdr(SERVER_UPDATE_URI, &updater_address))
        {
            Com_Printf("\nFailed to resolve AutoUpdate server %s.\n", SERVER_UPDATE_URI);
            return false;
        }

        updater_address.port = BigShort(SERVER_UPDATE_PORT); // Swap the port bytes

        Com_DPrintf("AutoUpdate resolved to %s\n", NET_AdrToString(updater_address));
    }
    return true;
}



// This function is called on startup to check for updates
// Original func: 0x0041162f
bool updater_sendRequest() {

    // Resolve the Auto-Update server address if not already resolved
    updater_resolveServerAddress();

    Com_Printf("Auto-Updater: Checking for updates...\n");

    char udpPayload[4096];

    if (dedicated->value.boolean == 0) // Client
    {
        int hwid = cl_hwid ? cl_hwid->value.integer : 0;
        const char* hwid2 = cl_hwid2 ? cl_hwid2->value.string : "";

        char CDKeyHash[34];
        CL_BuildMd5StrFromCDKey(CDKeyHash);

        char hwid_changed_diff_escaped[1024] = {};
        // Escape the hwid_changed_diff for use in the UDP payload
        for (size_t i = 0; i < sizeof(hwid_changed_diff) - 1 && hwid_changed_diff[i] != '\0'; ++i) {
            if (hwid_changed_diff[i] == '"') {
                hwid_changed_diff_escaped[i] = '\'';
            } else {
                hwid_changed_diff_escaped[i] = hwid_changed_diff[i];
            }
        }

        char data[2048] = {0};
        snprintf(data, sizeof(data), "\"%s\" \"%s\" \"%i\" \"%s\" \"%s\" \"%i\" \"%s\" \"%s\"",
            CDKeyHash, hwid_regid, hwid, hwid2, hwid_old, hwid_changed_count, hwid_changed_diff_escaped, SYS_VERSION_INFO);

        // Base64 encode the data
        char encodedData[2800] = {0};
        int base64status = base64_encode((const uint8_t*)data, strlen(data), encodedData, sizeof(encodedData));
        if (base64status < 0) {
            SHOW_ERROR("Failed to encode data for Auto-Update request.");
            exit(1);
        }

        // Calculate the CRC16 of the data
        uint16_t crc = crc16_ccitt((const uint8_t*)encodedData, strlen(encodedData));

        // Format the UDP payload
        snprintf(udpPayload, sizeof(udpPayload),
            "getUpdateInfo3 \"CoD2x MP\" \"" APP_VERSION "\" \"win-x86\" \"client\" \"%04x\" \"%s\"\n",
            crc, encodedData);

    } else {
        // Server
        snprintf(udpPayload, sizeof(udpPayload),
            "getUpdateInfo2 \"CoD2x MP\" \"" APP_VERSION "\" \"win-x86\" \"server\"\n");
    }



    bool status = NET_OutOfBandPrint(NS_CLIENT, updater_address, udpPayload);

    updater_waitingForResponse = true;
    updater_waitingForResponseTime = GetTickCount();

    Com_Printf("-----------------------------------\n");

    return status;
}


// This function is called when the game receives a response from the Auto-Update server
// Original func: 0x0040ef9c CL_UpdateInfoPacket()
void updater_updatePacketResponse(struct netaddr_s addr)
{
    if (updater_address.type == NA_BAD) {
        Com_DPrintf("Auto-Updater has bad address\n");
        return;
    }

    Com_DPrintf("Auto-Updater response from %s\n", NET_AdrToString(addr));
    
    if (NET_CompareBaseAdrSigned(&updater_address, &addr))
    {
        Com_DPrintf("Received update packet from unexpected IP.\n");
        return;
    }

    Com_Printf("-----------------------------------\n");

    updater_waitingForResponse = false;
    updater_waitingForResponseRepeats = 0;

    hwid_clearRegistryFromHWIDChange();

    const char* updateAvailableNumber = Cmd_Argv(1);
    int updateAvailable = atol(updateAvailableNumber);

    if (updateAvailable) {
        const char* updateFile = Cmd_Argv(2);
        const char* newVersionString = Cmd_Argv(3);
        
        Com_Printf("CoD2x: Update available: %s -> %s\n", APP_VERSION, newVersionString);

        // Server
        if (dedicated->value.boolean > 0) {
            Com_Printf("Downloading file '%s'...\n", updateFile);
            char errorBuffer[1024];
            bool ok = updater_downloadAndReplaceDllFile(updateFile, errorBuffer, sizeof(errorBuffer));  
            if (ok) {
                Com_Printf("Successfully downloaded and replaced file.\n");
                Com_Printf("The update will take effect after the next server restart.\n");
            } else {
                Com_Printf("Failed to download and replace file.\n");
                Com_Printf("Error: %s\n", errorBuffer);
            }

        // Client
        } else {
            Dvar_SetBool(cl_updateAvailable, 1);
            Dvar_SetString(cl_updateFiles, updateFile);
            Dvar_SetString(cl_updateVersion, newVersionString);
            Dvar_SetString(cl_updateOldVersion, APP_VERSION);

            // Forced update
            if (updateAvailable == 2) {
                updater_forcedUpdate = true;

                if (dedicated->value.integer == 0 && clientState > CLIENT_STATE_DISCONNECTED && clientState < CLIENT_STATE_CONNECTED && demo_isPlaying == 0) {
                    updater_showForceUpdateDialog();
                }
            }
        }

    } else {
        Com_Printf("CoD2x: No updates available.\n");

        if (dedicated->value.boolean == 0) {
            Dvar_SetBool(cl_updateAvailable, 0);
        }
    }

    Com_Printf("-----------------------------------\n");
}




// This function is called when the user confirms the update dialog
// Original func: 0x0053bc40
void updater_dialogConfirmed() {
    char errorBuffer[1024];
    bool ok = updater_downloadAndReplaceDllFile(cl_updateFiles->value.string, errorBuffer, sizeof(errorBuffer));
    if (ok) {
        // Restart the application with command line arguments
        ShellExecute(NULL, "open", EXE_PATH, EXE_COMMAND_LINE, NULL, SW_SHOWNORMAL);
        ExitProcess(0);
    } else {
        Com_Error(ERR_DROP, "Failed to download and replace file.\n\n%s", errorBuffer);
    }
}



void updater_checkForUpdate() {
    // Server
    if (dedicated->value.boolean > 0) {
        // Send the request to the Auto-Update server
        if (sv_update->value.boolean && sv_running->value.boolean) {
            updater_sendRequest();
        }
    } else {
        updater_sendRequest();
    }
}


/** Called on renderer load (also after vid_restart) */
void updater_renderer() {
    updater_lastClientState = -1; // Reset last client state to ensure we check for updates on next frame
}

/** Called every frame on frame start. */
void updater_frame() {
    // If the forced update is available and player leaves main menu, show error
    if (updater_forcedUpdate && dedicated->value.integer == 0 && clientState > CLIENT_STATE_DISCONNECTED && clientState < CLIENT_STATE_CONNECTED && demo_isPlaying == 0) {
        updater_showForceUpdateDialog();
    }  
    
    // If player disconnect (so main menu is opened), check for updates again
    if (clientState != updater_lastClientState && clientState == CLIENT_STATE_DISCONNECTED) {
        updater_waitingForResponseRepeats = 0;
        updater_checkForUpdate(); // Check for updates again
    }

    // If we opened main menu while we were connected to the server, check for updates
    if (clientState == CLIENT_STATE_ACTIVE && !cg.isMainMenuClosed && cg.isMainMenuClosed != updater_lastMainMenuClosed) {
        updater_waitingForResponseRepeats = 0;
        updater_checkForUpdate(); // Check for updates again
    }

    // We did not receive a response from the Auto-Update server within 5 seconds
    if (updater_waitingForResponse && (GetTickCount() - updater_waitingForResponseTime) > 5000 && updater_waitingForResponseRepeats < 5) {
        Com_Printf("Auto-Updater: Request timed out.\n");
        // Check again for updates
        updater_waitingForResponse = false;
        updater_waitingForResponseRepeats++;
        updater_checkForUpdate();
    }

    updater_lastClientState = clientState;
    updater_lastMainMenuClosed = cg.isMainMenuClosed; // Update last main menu closed state
}


/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void updater_init() {
    sv_update = Dvar_RegisterBool("sv_update", true, (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
}

/** Called before the entry point is called. Used to patch the memory. */
void updater_patch() {

    // Hook function that was called when update UDP packet was received from update server
    patch_call(0x0040ef9c, (unsigned int)&updater_updatePacketResponse);
    // Hook function was was called when user confirm the update dialog (it previously open url)
    patch_jump(0x0053bc40, (unsigned int)&updater_dialogConfirmed);

    // Disable original call to function that sends request to update server
    patch_nop(0x0041162f, 5);

}