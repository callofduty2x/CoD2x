#ifndef UPDATER_HTTPS_H
#define UPDATER_HTTPS_H

#include <string>

// HTTPS update check (JSON over HTTPS), the main source of update checks. The legacy UDP request
// (getUpdateInfo2/3) is sent by the updater only as a fallback when the HTTPS check fails; the master
// server logs UDP requests from versions with the HTTPS updater, so HTTPS failures are visible there.
//
// The request runs on a background thread so it never blocks the caller. The result is handed
// back to the main thread via updater_https_pollResult(), which must be called every frame;
// all game state (cvars, dialogs, downloads) is changed only from the main thread.

// Request data. Only game/version/os decide the update; the rest is logged by the server
// (update_logs). Identity fields are only filled for a client, a dedicated server leaves them empty.
struct UpdaterHttpsRequest {
    std::string type;           // "client" or "server"
    std::string cdkeyHash;      // MD5 of the CD key
    std::string regid;          // install id
    bool        hasHwid = false;
    int         hwid = 0;       // short HWID
    std::string hwid2;          // MD5 HWID
    std::string hwid2Old;       // previous HWID2 when it changed
    bool        hasHwid2DiffCnt = false;
    int         hwid2DiffCnt = 0;
    std::string hwid2Diff;      // description of what changed in HWID2
    std::string systemInfo;     // short system info string
};

// Result of a finished update check.
struct UpdaterHttpsResult {
    bool        ok = false;     // false = request failed / rejected / unparsable (see 'error')
    std::string error;
    int         available = 0;  // 0 = none, 1 = optional update, 2 = forced update
    std::string url;            // URL of the file to download
    std::string version;        // version string of the available update
};

// Starts an update check in the background. Returns false if a check is already running.
bool updater_https_checkForUpdate(const UpdaterHttpsRequest& request);

// Main thread: returns true and fills 'out' once when a started check has finished.
bool updater_https_pollResult(UpdaterHttpsResult* out);

#endif // UPDATER_HTTPS_H
