#include "updater.h"

#include <stdio.h>
#include <stdlib.h>
#include <cstdlib>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>

#include "shared.h"
#include "../shared/common.h"
#include "../shared/cod2_common.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_net.h"
#include "../shared/cod2_cmd.h"
#include "../shared/updater_https.h"


struct netaddr_s updater_address;

dvar_t* sv_update;


void download_and_swap_async(const char *current_version, const char *new_version, const char *url) {
    
    char libCoD2x_new[512], libCoD2x_old[512], libCoD2x_log[512], cmd[2048];

    snprintf(libCoD2x_new, sizeof(libCoD2x_new), "%s.new", LIB_PATH);
    snprintf(libCoD2x_old, sizeof(libCoD2x_old), "%s.%s.old", LIB_PATH, current_version);
    snprintf(libCoD2x_log, sizeof(libCoD2x_log), "%s.log", LIB_PATH);

    putenv((char*)"LD_PRELOAD");  // Remove LD_PRELOAD

    // Construct the full shell command
    int off = 0; // Offset for command buffer
    off += snprintf(cmd + off, sizeof(cmd) - off, "nohup sh -c '");
    off += snprintf(cmd + off, sizeof(cmd) - off, "set -e; "); // Exit on failure
    off += snprintf(cmd + off, sizeof(cmd) - off, "{ ");
    off += snprintf(cmd + off, sizeof(cmd) - off, "echo \"==== Update Started at $(date \"+%%Y-%%m-%%d %%H:%%M:%%S\") ===============================================\" >> \"%s\" 2>&1 && ", libCoD2x_log);
    off += snprintf(cmd + off, sizeof(cmd) - off, "echo \"Current Version: %s\"; ", current_version); // Log current version
    off += snprintf(cmd + off, sizeof(cmd) - off, "echo \"New Version:     %s\"; ", new_version); // Log new version
    off += snprintf(cmd + off, sizeof(cmd) - off, "echo \"Url:             %s\"; ", url);
    off += snprintf(cmd + off, sizeof(cmd) - off, "echo \"Destination:     %s\"; ", libCoD2x_new);
    off += snprintf(cmd + off, sizeof(cmd) - off, "rm -f \"%s\"; ", libCoD2x_old); // Remove old backup
    off += snprintf(cmd + off, sizeof(cmd) - off, "curl --connect-timeout 2 --max-time 8 --retry 3 --retry-delay 2 -o \"%s\" -L -s -S -f -w \"Downloaded: %%{size_download} bytes at %%{speed_download} bytes/sec in %%{time_total} seconds\\n\" \"%s\"; ", libCoD2x_new, url);
    off += snprintf(cmd + off, sizeof(cmd) - off, "mv -f \"%s\" \"%s\"; ", LIB_PATH, libCoD2x_old); // Move current lib to .old
    off += snprintf(cmd + off, sizeof(cmd) - off, "mv -f \"%s\" \"%s\"; ", libCoD2x_new, LIB_PATH); // Replace with new lib
    off += snprintf(cmd + off, sizeof(cmd) - off, "echo \"==========================================================================================\"; ");
    off += snprintf(cmd + off, sizeof(cmd) - off, "} >> \"%s\" 2>&1' </dev/null >/dev/null 2>&1 &", libCoD2x_log); // Redirect all output to log and run in background

    Com_DPrintf("\nShell:\n");
    Com_DPrintf("%s", cmd);
    Com_DPrintf("\n\n");

    // Execute the constructed command asynchronously
    system(cmd);
}


void updater_downloadFile(const char* updateFile, const char* newVersionString) {
    Com_Printf("Downloading file '%s' in background.\n", updateFile);
    Com_Printf("New version will be loaded after application restart.\n");

    download_and_swap_async(APP_VERSION, newVersionString, updateFile);
}




// Sends the HTTPS update check (runs on a background thread). A dedicated server has no HWID/install id.
// If it fails, the UDP request is sent as a fallback.
static void updater_sendHttpsRequest() {
    UpdaterHttpsRequest request;
    request.type = "server";

    if (updater_https_checkForUpdate(request)) {
        Com_Printf("Checking for updates...\n");
    }
}

bool updater_sendRequest() {
    Com_DPrintf("Resolving AutoUpdate Server %s...\n", SERVER_UPDATE_URI);
    if (!NET_StringToAdr(SERVER_UPDATE_URI, &updater_address))
    {
        Com_Printf("\nFailed to resolve AutoUpdate server %s.\n", SERVER_UPDATE_URI);
        return 0;
    }

    updater_address.port = BigShort(SERVER_UPDATE_PORT); // Swap the port bytes

    Com_DPrintf("AutoUpdate resolved to %s\n", NET_AdrToString(updater_address));

    Com_Printf("Checking for updates...\n");

    // Send the request to the Auto-Update server
    const char* udpPayload = "getUpdateInfo2 \"CoD2x MP\" \"" APP_VERSION "\" \"linux-i386\" \"server\"";

    bool status = NET_OutOfBandPrint(NS_CLIENT, updater_address, udpPayload);

    Com_Printf("-----------------------------------\n");

    return status;
}

static void updater_processResponse(int updateAvailable, const char* updateFile, const char* newVersionString);

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

    updater_processResponse(atol(Cmd_Argv(1)), Cmd_Argv(2), Cmd_Argv(3));
}

// True if the string contains only characters that are safe inside the download shell command
static bool updater_isSafeShellArg(const char* s) {
    if (!s || !*s)
        return false;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(":/._-", c)))
            return false;
    }
    return true;
}

// Process the update check response (from UDP or HTTPS)
static void updater_processResponse(int updateAvailable, const char* updateFile, const char* newVersionString)
{
    Com_Printf("-----------------------------------\n");

    if (updateAvailable) {
        Com_Printf("CoD2x: Update available: %s -> %s\n", APP_VERSION, newVersionString);

        // URL and version are placed into a shell command, so allow only safe characters
        if (updater_isSafeShellArg(updateFile) && updater_isSafeShellArg(newVersionString))
            updater_downloadFile(updateFile, newVersionString);
        else
            Com_Printf("CoD2x: Invalid update URL or version, update skipped.\n");

    } else {
        Com_Printf("CoD2x: No updates available.\n");
    }

    Com_Printf("-----------------------------------\n");

    return;
}

// Called on the main thread when the HTTPS update check has finished
static void updater_processHttpsResult(const UpdaterHttpsResult& result)
{
    if (result.ok) {
        updater_processResponse(result.available, result.url.c_str(), result.version.c_str());
        return;
    }

    Com_Printf("CoD2x: Failed to check for updates: %s\n", result.error.c_str());

    // Fallback to the legacy UDP request (the master server logs UDP requests from new versions as HTTPS failures)
    Com_Printf("Trying UDP...\n");
    updater_sendRequest();
}


void updater_checkForUpdate() {
    // Send the request to the Auto-Update server
    if (sv_update->value.boolean && sv_running->value.boolean) {
        updater_sendHttpsRequest();
    }
}


/** Called every frame. Processes the result of the HTTPS update check (the request runs on a background thread). */
void updater_frame() {
    UpdaterHttpsResult result;
    if (updater_https_pollResult(&result)) {
        updater_processHttpsResult(result);
    }
}


/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void updater_init() {
    sv_update = Dvar_RegisterBool("sv_update", true, (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
}


/** Called before the entry point is called. Used to patch the memory. */
void updater_patch() {
}
