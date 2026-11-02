// http_download.h pulls in mongoose, it has to be included first
#include "../shared/http_download.h"

#include "updater.h"

#include <stdio.h>
#include <stdlib.h>
#include <cstdlib>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <string>

#include "shared.h"
#include "../shared/common.h"
#include "../shared/cod2_common.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_net.h"
#include "../shared/cod2_cmd.h"
#include "../shared/updater_https.h"
#include "../shared/error.h"
#include "../shared/files.h"

// A file of an interrupted download is removed after this time
#define UPDATER_DOWNLOAD_MAX_AGE_SECONDS 3600


struct netaddr_s updater_address;

dvar_t* sv_update;

static HttpDownload updater_download;
static char updater_downloadingVersion[64];   // version of the running download
static std::string updater_libraryStamp;      // the library this process was started from
static time_t updater_downloadStartTime;
static bool updater_logHeld = false;          // the attempt repeats the last logged failure, see updater_logHold()
static std::string updater_logPending;        // lines of a held attempt


static std::string updater_getLogPath() {
    return std::string(LIB_PATH) + ".log";
}

// With the process id, server instances sharing the folder must not write to the same file
static std::string updater_getNewFilePath() {
    return std::string(LIB_PATH) + ".new." + std::to_string(getpid());
}

// The library on the disk is not the one this process runs: it was already updated by this process or by
// another server instance sharing the folder. Replacing it again would overwrite the .old backup.
static bool updater_isLibraryReplaced() {
    std::string stamp = file_getStamp(LIB_PATH);
    return !updater_libraryStamp.empty() && !stamp.empty() && stamp != updater_libraryStamp;
}

static void updater_logWrite(const char* text) {
    FILE* f = fopen(updater_getLogPath().c_str(), "a");
    if (!f)
        return;
    fputs(text, f);
    fclose(f);
}

// Writes to the console and to libCoD2x.so.log, so admins can see what happened to the file.
// Returns the message.
static std::string updater_log(const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    Com_Printf("%s\n", message);

    char timeText[32];
    time_t now = time(NULL);
    strftime(timeText, sizeof(timeText), "%Y-%m-%d %H:%M:%S", localtime(&now));

    std::string line = std::string(timeText) + "  " + message + "\n";
    if (updater_logHeld)
        updater_logPending += line;
    else
        updater_logWrite(line.c_str());

    return message;
}

// A failing update is tried with every update check, the log must not grow with each attempt.
// If the log already ends with a failure of this version, the attempt is held back: it is written
// by updater_logRelease() when something new happens, a failure is dropped by updater_logDrop().
// The log is read so it also works after a server restart.
static void updater_logHold(const char* version) {
    char tail[512] = {0};
    FILE* f = fopen(updater_getLogPath().c_str(), "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, size > (long)sizeof(tail) - 1 ? size - ((long)sizeof(tail) - 1) : 0, SEEK_SET);
        size_t length = fread(tail, 1, sizeof(tail) - 1, f);
        tail[length] = '\0';
        fclose(f);
    }

    char failure[128];
    snprintf(failure, sizeof(failure), "Update %s not installed", version);
    updater_logHeld = strstr(tail, failure) != NULL;
    updater_logPending.clear();
}

static void updater_logRelease() {
    updater_logWrite(updater_logPending.c_str());
    updater_logPending.clear();
    updater_logHeld = false;
}

static void updater_logDrop() {
    updater_logPending.clear();
    updater_logHeld = false;
}

// Replaces the library with the downloaded file. The running process keeps the old file open.
static bool updater_replaceLibrary(std::string* error) {
    std::string libNew = updater_getNewFilePath();
    std::string libOld = std::string(LIB_PATH) + "." + APP_VERSION + ".old";

    struct stat st;
    if (stat(LIB_PATH, &st) == 0)
        chmod(libNew.c_str(), st.st_mode);

    if (rename(LIB_PATH, libOld.c_str()) != 0) {
        *error = updater_log("Failed to rename '%s' to '%s': %s", LIB_PATH, libOld.c_str(), strerror(errno));
        return false;
    }
    updater_log("Backup:          %s", libOld.c_str());

    if (rename(libNew.c_str(), LIB_PATH) != 0) {
        *error = updater_log("Failed to rename '%s' to '%s': %s", libNew.c_str(), LIB_PATH, strerror(errno));
        if (rename(libOld.c_str(), LIB_PATH) == 0) {
            updater_log("Restored '%s' from the backup", LIB_PATH);
        } else {
            *error += ". " + updater_log("Failed to restore '%s' from the backup: %s", LIB_PATH, strerror(errno));
            updater_logRelease(); // the library is missing, this is always written
        }
        return false;
    }
    updater_log("Replaced:        %s", LIB_PATH);

    return true;
}

static bool updater_installDownloadedFile(std::string* error) {
    FileLock lock = file_lockFolder(LIB_FOLDER_PATH, "update");

    bool ok;
    if (lock != FILE_LOCK_NONE && !updater_isLibraryReplaced()) {
        ok = updater_replaceLibrary(error);
    } else {
        updater_log("'%s' was replaced by another server instance", LIB_PATH);
        remove(updater_getNewFilePath().c_str());
        ok = true;
    }

    if (lock != FILE_LOCK_NONE)
        file_unlockFolder(lock);
    return ok;
}

static void updater_startDownload(const char* url, const char* version) {
    std::string libNew = updater_getNewFilePath();

    updater_logHold(version);
    updater_log("==== Update Started ===============================================");
    updater_log("Current Version: %s", APP_VERSION);
    updater_log("New Version:     %s", version);
    updater_log("Url:             %s", url);
    updater_log("Destination:     %s", libNew.c_str());

    snprintf(updater_downloadingVersion, sizeof(updater_downloadingVersion), "%s", version);
    updater_downloadStartTime = time(NULL);
    updater_download.start(url, libNew, "\x7f" "ELF");
}

// Called every frame, finishes the download when it ends
static void updater_processDownload() {
    HttpDownload::State state = updater_download.getState();
    if (state == HttpDownload::Idle || state == HttpDownload::Running)
        return;

    updater_download.join();

    updater_log("Downloaded:      %u bytes in %d seconds", (unsigned int)updater_download.getDownloaded(), (int)(time(NULL) - updater_downloadStartTime));

    std::string error;
    if (state == HttpDownload::Failed) {
        error = updater_log("Download failed: %s", updater_download.getError().c_str());
    } else if (!updater_download.isComplete()) {
        error = updater_log("Download failed: received %u of %u bytes", (unsigned int)updater_download.getDownloaded(), (unsigned int)updater_download.getTotal());
    } else if (updater_installDownloadedFile(&error)) {
        updater_log("Update %s installed, it will be loaded after application restart", updater_downloadingVersion);
        updater_log("===================================================================");
        updater_logRelease();
        return;
    }

    // Tried again with the next update check
    remove(updater_getNewFilePath().c_str());
    updater_log("Update %s not installed", updater_downloadingVersion);
    updater_log("===================================================================");
    updater_logDrop();
    error_report(ERROR_KIND_COD2X_UPDATE, std::string("CoD2x update ") + updater_downloadingVersion + " not installed: " + error);
}


// Sends the HTTPS update check (runs on a background thread). A dedicated server has no HWID/install id.
// If it fails, the UDP request is sent as a fallback.
static void updater_sendHttpsRequest(bool periodic) {
    UpdaterHttpsRequest request;
    request.type = "server";
    request.periodic = periodic;

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

// Process the update check response (from UDP or HTTPS)
static void updater_processResponse(int updateAvailable, const char* updateFile, const char* newVersionString)
{
    Com_Printf("-----------------------------------\n");

    if (updateAvailable) {
        Com_Printf("CoD2x: Update available: %s -> %s\n", APP_VERSION, newVersionString);

        if (updater_isLibraryReplaced()) {
            Com_Printf("New version will be loaded after application restart.\n");
        } else if (updater_download.getState() == HttpDownload::Idle) {
            updater_startDownload(updateFile, newVersionString);
        }

    } else {
        Com_Printf("CoD2x: No updates available.\n");
    }

    Com_Printf("-----------------------------------\n");

    return;
}

static bool updater_isUpdateEnabled() {
    return sv_update->value.boolean && sv_running->value.boolean;
}

// Called on the main thread when the HTTPS update check has finished
static void updater_processHttpsResult(const UpdaterHttpsResult& result)
{
    bool applyUpdate = updater_isUpdateEnabled();

    if (result.ok) {
        zpam_applyManifest(result.zpam);

        if (applyUpdate)
            updater_processResponse(result.available, result.url.c_str(), result.version.c_str());
        return;
    }

    Com_Printf("CoD2x: Failed to check for updates: %s\n", result.error.c_str());

    // Fallback to the legacy UDP request (the master server logs UDP requests from new versions as HTTPS failures)
    if (applyUpdate && !result.periodic) {
        Com_Printf("Trying UDP...\n");
        updater_sendRequest();
    }
}


// zPAM is checked on server start even if no map is running yet
static bool updater_isCheckEnabled() {
    return updater_isUpdateEnabled() || zpam_isUpdateCheckNeeded();
}

void updater_checkForUpdate() {
    // Send the request to the Auto-Update server
    if (updater_isCheckEnabled()) {
        updater_sendHttpsRequest(false);
    }
}


/** Called every frame. Processes the result of the HTTPS update check (the request runs on a background thread). */
void updater_frame() {
    UpdaterHttpsResult result;
    if (updater_https_pollResult(&result)) {
        updater_processHttpsResult(result);
    }

    updater_processDownload();

    if (updater_https_isPeriodicCheckDue() && updater_isCheckEnabled()) {
        updater_sendHttpsRequest(true);
    }
}


/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void updater_init() {
    sv_update = Dvar_RegisterBool("sv_update", true, (dvarFlags_e)(DVAR_CHANGEABLE_RESET));

    // Interrupted downloads, the process id in the name is different after a restart
    const char* libName = strrchr(LIB_PATH, '/');
    std::string prefix = std::string(libName ? libName + 1 : LIB_PATH) + ".new.";
    file_removeOld(LIB_FOLDER_PATH, prefix.c_str(), "", UPDATER_DOWNLOAD_MAX_AGE_SECONDS);
}


/** Called before the entry point is called. Used to patch the memory. */
void updater_patch() {
    updater_libraryStamp = file_getStamp(LIB_PATH);
}
