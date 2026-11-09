// http_download.h pulls in mongoose -> winsock2.h, which must be included before windows.h.
#include "../shared/http_download.h"

#include "downloading.h"

#include "shared.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../shared/cod2_common.h"
#include "../shared/cod2_cmd.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_file.h"
#include "../shared/cod2_shared.h"
#include "../shared/cod2_client.h"
#include "../shared/zpam.h"


// How a missing file gets to the client (client = CL_*, server = SV_* in the Linux database):
//
//   CL_InitDownloads     - FS_CompareIwds builds the list "@remote@local@remote@local"
//   CL_NextDownload      - takes one file, CL_BeginDownload sends "download <remote>"
//   SV_WriteDownloadToClient - with sv_wwwDownload sends a redirect (URL, size, flags), otherwise the file
//   CL_ParseWWWDownload  - reads the redirect, sends "wwwdl ack", calls DL_BeginDownload
//   CL_Frame             - while dl_running calls CL_WWWDownload, which finishes the file with DL_DownloadLoop
//   CL_DownloadsComplete - when the list is empty sends "donedl", the server sends the gamestate again
//
// CoD2x replaces only the transport: DL_BeginDownload, DL_DownloadLoop and DL_CancelDownload use HttpDownload
// for https (libwww can not do it), http stays on libwww. Everything around them works the same.
//
// CoD2x also starts downloads of zPAM iwds on its own in CL_BeginDownload, see zpam_getDownloadUrl().
// The server does not know about them, SV_WWWDownload_f drops a client that sends wwwdl without a redirect.


#define dl_running            (*((int*)0x019a1c00)) // CL_Frame calls CL_WWWDownload when this is above 0
#define dl_isMotd             (*((bool*)0x014b2cd0)) // the running download is the message of the day
#define autoupdateStarted     (*((int*)0x00609fcc))
#define cl_download           (*((int*)0x00966ffc)) // file handle of the download from the server

static HttpDownload downloading_https;

// Not empty if the running download was started by the client, the server does not know about it
static std::string downloading_url;
static char downloading_remoteName[64];
// A file is tried only once. A server with a modified iwd would ask for it again after every download.
static std::vector<std::string> downloading_triedNames;


int EventListTimerHandler (void * timer, void * param) {

    // CoD2x: Fix crash at 0x0054e658 when trying to access param->timeouts
    if (param == NULL || timer == NULL || IsBadReadPtr(param, 0x20)) {
        Com_Printf("EventListTimerHandler: preventing crash\n");
        return -1; // HT_ERROR
    }

    int ret;
    ASM_CALL(RETURN(ret), 0x0054e650, 2, PUSH(timer), PUSH(param));

    return ret;
}


static void downloading_cancelHttps() {
    if (downloading_https.getState() != HttpDownload::Idle) {
        downloading_https.cancel();
        dl_running = 0;
    }
}

// Full path of the file in fs_homepath
static void downloading_getOSPath(char* ospath, const char* name) {
    FS_BuildOSPath(ospath, "", (void*)name, fs_homePath->value.string);
    ospath[strlen(ospath) - 1] = '\0'; // trailing slash of the empty qpath
}


// Stops the running www download and frees its request.
// 004b59c0    void* DL_CancelDownload()
void DL_CancelDownload() {
    // CoD2x: https download
    downloading_cancelHttps();
    // An interrupted download was not really tried, the next connect starts it again
    if (!downloading_url.empty()) {
        downloading_triedNames.pop_back();
        downloading_url.clear();
    }
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x004b59c0);
}


// Starts a www download of the URL remoteName into the file localName (any previous content is replaced).
// Returns true if the request was started, the download then runs from CL_WWWDownload in CL_Frame.
// 004b5ad0    int32_t DL_BeginDownload(char* localName, char* remoteName)
int DL_BeginDownload(char* localName, const char* remoteName) {
    // CoD2x: libwww can not do https
    downloading_cancelHttps();

    if (I_strnicmp(remoteName, "https://", 8) == 0) {
        FS_CreatePath(localName);

        snprintf(legacyHacks->downloadName, sizeof(legacyHacks->downloadName), "%s", remoteName);
        downloading_https.start(remoteName, localName, "PK");
        dl_running = 1;
        return 1;
    }
    // CoD2x: End

    // Call the original function
    int ret;
    ASM_CALL(RETURN(ret), 0x004b5ad0, 2, PUSH(localName), PUSH(remoteName));
    return ret;
}


// Runs the www download. Returns 0 if it is still running, 1 if finished, 2 if failed.
// 004b5ee0    int32_t DL_DownloadLoop()
static int DL_DownloadLoop() {
    // CoD2x: https download
    HttpDownload::State state = downloading_https.getState();
    if (state != HttpDownload::Idle) {
        // The server sends the size in the redirect, a download started by the client gets it here
        if (downloading_https.getTotal() > 0)
            legacyHacks->downloadSize = (int)downloading_https.getTotal();
        legacyHacks->downloadCount = (int)downloading_https.getDownloaded();

        if (state == HttpDownload::Running)
            return 0;

        downloading_https.join();
        dl_running = 0;

        // Without a known size a cut connection looks like the end of the file
        if (state == HttpDownload::Finished && downloading_https.getDownloaded() == (size_t)legacyHacks->downloadSize)
            return 1;

        if (state == HttpDownload::Finished)
            Com_Printf("DL_DownloadLoop: size does not match, expected %d bytes\n", legacyHacks->downloadSize);
        else
            Com_Printf("DL_DownloadLoop: request terminated with failure: %s\n", downloading_https.getError().c_str());
        return 2;
    }
    // CoD2x: End

    // Call the original function
    int ret;
    ASM_CALL(RETURN(ret), 0x004b5ee0);
    return ret;
}


// Starts a download started by the client.
// Same setup as the www redirect from the server in CL_ParseWWWDownload, only without the wwwdl commands.
static void downloading_begin() {
    char tempName[256];
    const char* url = downloading_url.c_str();

    snprintf(tempName, sizeof(tempName), "%s.tmp", cl_downloadLocalName);
    downloading_getOSPath(cl_downloadTempName, tempName);

    snprintf(cl_downloadName, 256, "%s", url);
    cl_wwwDlFlags = 0;
    legacyHacks->downloadSize = 0;
    legacyHacks->downloadCount = 0;
    legacyHacks->downloadTime = clc_realTime;

    Com_Printf("Downloading '%s'\n", url);
    DL_BeginDownload(cl_downloadTempName, cl_downloadName);
    cl_downloadingWWW = 1;
}

// A download started by the client failed, asks the server for the file as usual
static void downloading_askServer() {
    downloading_url.clear();
    remove(cl_downloadTempName);
    cl_downloadingWWW = 0;

    // Call the original function
    const char* localName = cl_downloadLocalName;
    ASM_CALL(RETURN_VOID, 0x0040dde0, 1, EAX(localName), PUSH(downloading_remoteName));
}


// Starts the download of a missing file by asking the server for it. Both names point into the download list.
// 0040dde0    int32_t CL_BeginDownload(char* localName @ eax, char* remoteName)
void CL_BeginDownload(const char* localName, const char* remoteName) {
    // CoD2x: zPAM iwds are downloaded directly, also if the server has www download disabled
    dvar_t* cl_wwwDownload = Dvar_GetDvarByName("cl_wwwDownload");

    bool tried = std::find(downloading_triedNames.begin(), downloading_triedNames.end(), remoteName) != downloading_triedNames.end();

    downloading_url.clear();
    if (cl_wwwDownload && cl_wwwDownload->value.boolean && !tried)
        downloading_url = zpam_getDownloadUrl(remoteName);

    if (!downloading_url.empty()) {
        downloading_triedNames.push_back(remoteName);
        snprintf(cl_downloadLocalName, 64, "%s", localName);
        snprintf(downloading_remoteName, sizeof(downloading_remoteName), "%s", remoteName);

        downloading_begin();
        return;
    }
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x0040dde0, 1, EAX(localName), PUSH(remoteName));
}

void CL_BeginDownload_Win32(const char* remoteName) {
    const char* localName;
    ASM( movr, localName, "eax" );

    CL_BeginDownload(localName, remoteName);
}


// Called every frame while downloading, finishes the file when the download ends.
// 004b5160    void CL_WWWDownload()
void CL_WWWDownload() {
    char ospath[256];

    int ret = DL_DownloadLoop();
    if (ret == 0)
        return;

    if (dl_isMotd) {
        if (ret == 1) {
            // CL_ReadMotd
            ASM_CALL(RETURN_VOID, 0x004b50f0);
        }
        cl_downloadingWWW = 0;
        return;
    }

    if (ret != 1) {
        // CoD2x: a download started by the client, the server must not get wwwdl fail
        if (!downloading_url.empty()) {
            downloading_askServer();
            return;
        }
        // CoD2x: End

        if (cl_wwwDlDisconnected) {
            char* message = va("Download failure while getting '%s'\n", cl_downloadName);
            cl_wwwDlDisconnected = 0;
            CL_ClearStaticDownload();
            Com_Error(ERR_DROP, "%s", message);
            return;
        }

        Com_Printf("Download failure while getting '%s'\n", cl_downloadName);
        CL_AddReliableCommand("wwwdl fail");
        cl_downloadingWWW = 0;
        return;
    }

    cl_download = 0;

    downloading_getOSPath(ospath, cl_downloadLocalName);

    if (rename(cl_downloadTempName, ospath)) {
        FS_CopyFile(cl_downloadTempName, ospath);
        remove(cl_downloadTempName);
    }

    cl_downloadName[0] = '\0';
    cl_downloadTempName[0] = '\0';
    legacyHacks->downloadName[0] = '\0';

    if (cl_wwwDlDisconnected) {
        if (!autoupdateStarted)
            Cbuf_AddText("reconnect\n");
    }
    // CoD2x: the server did not redirect this download, it would drop the client after wwwdl done.
    // CL_DownloadsComplete then sends donedl, which works without a redirect.
    else if (downloading_url.empty()) {
    // CoD2x: End
        CL_AddReliableCommand("wwwdl done");
    }

    downloading_url.clear();
    cl_downloadingWWW = 0;
    CL_NextDownload();
}


/** Called when the DLL is unloaded, the thread would continue in the old code. */
void downloading_unload() {
    downloading_cancelHttps();
}


/** Called before the entry point to patch memory for downloading features. */
void downloading_patch() {
    patch_int32(0x0054e90c + 1, (unsigned int)EventListTimerHandler); // push EventListTimerHandler

    // Downloading screen
    patch_float(0x00539351 + 1, 0.35f); // font size (0.5f original)
    patch_float(0x005c4390, 210.0f);    // Y1 (210.0f original)
    patch_float(0x005c438c, 230.0f);    // Y2 (235.0f original)
    patch_float(0x005c4388, 250.0f);    // Y3 (260.0f original)

    patch_call(0x0040f87f, (unsigned int)CL_WWWDownload); // in CL_Frame
    patch_call(0x0040ded6, (unsigned int)CL_BeginDownload_Win32); // in CL_NextDownload
    patch_call(0x00414049, (unsigned int)DL_BeginDownload); // in CL_ParseWWWDownload

    patch_jump(0x0040cc95, (unsigned int)DL_CancelDownload); // tail call in CL_ClearStaticDownload
    patch_call(0x0040ce58, (unsigned int)DL_CancelDownload); // in CL_Disconnect
    patch_call(0x0040dcee, (unsigned int)DL_CancelDownload); // in CL_DownloadsComplete
    patch_call(0x0040df56, (unsigned int)DL_CancelDownload); // in CL_InitDownloads
    patch_call(0x00413fa7, (unsigned int)DL_CancelDownload); // in CL_ParseWWWDownload
    patch_call(0x0041405f, (unsigned int)DL_CancelDownload); // in CL_ParseWWWDownload
}
