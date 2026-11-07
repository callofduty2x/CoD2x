// http_download.h pulls in mongoose -> winsock2.h, which must be included before windows.h.
#include "http_download.h"
#include "json.h"

#include "zpam.h"
#include "shared.h"

#if COD2X_WIN32
    #include <windows.h>
#endif
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#include "cod2_common.h"
#include "cod2_dvars.h"
#include "cod2_cmd.h"
#include "cod2_shared.h"
#include "cod2_server.h"
#include "match.h"
#include "error.h"
#include "files.h"


#define ZPAM_PREFIX             "zpam"
#define ZPAM_MAPS_PREFIX        "zpam_maps_v"
#define ZPAM_WWW_BASE_URL       "https://cod2x.me/zpam"
#define ZPAM_STATE_FILE         "zpam.json"
#define ZPAM_IDLE_TIME_MS       60000                   // server must be empty for this long before switching
#define ZPAM_CLEANUP_AGE_SEC    (7 * 24 * 60 * 60)      // old versions are kept this long for other server instances

// Content of main/zpam.json
struct ZpamState {
    std::string zpam;
    std::string maps;
    std::vector<std::string> block;
    long long updated = 0;
};

static dvar_t* sv_updateZpam = NULL;

// Versions loaded by this process (other instances sharing main/ can have different ones)
static std::string zpam_loaded;
static std::string zpam_loadedMaps;

// This process does not run the newest version
static bool zpam_switchPending = false;
static uint64_t zpam_idleSince = 0;

// Background install. The result is written by the worker before Done and read by the main thread before Idle.
enum ZpamWorkerState { ZPAM_WORKER_IDLE, ZPAM_WORKER_RUNNING, ZPAM_WORKER_DONE };
static std::atomic<int> zpam_workerState(ZPAM_WORKER_IDLE);
static std::string zpam_workerError;
static std::string zpam_workerInstalled;


// "zpam409.iwd" with prefix "zpam" returns 409, -1 if the name has a different format
static int zpam_parseVersion(const char* name, const char* prefix) {
    size_t prefixLen = strlen(prefix);
    if (I_strnicmp(name, prefix, prefixLen) != 0)
        return -1;

    const char* p = name + prefixLen;
    if (!isdigit((unsigned char)*p))
        return -1;

    int version = atoi(p);
    while (isdigit((unsigned char)*p))
        p++;

    return Q_stricmp(p, ".iwd") == 0 ? version : -1;
}

static bool zpam_isIwdName(const char* name) {
    return zpam_parseVersion(name, ZPAM_PREFIX) >= 0 || zpam_parseVersion(name, ZPAM_MAPS_PREFIX) >= 0;
}

static bool zpam_isSameName(const std::string& a, const std::string& b) {
    return Q_stricmp(a.c_str(), b.c_str()) == 0;
}

static bool zpam_isInList(const std::string& name, const std::vector<std::string>& list) {
    for (const std::string& item : list) {
        if (zpam_isSameName(name, item))
            return true;
    }
    return false;
}

// Newest not blocked version from the list of file names, optionally only versions below 'below'
static std::string zpam_findNewest(const char* prefix, const std::vector<std::string>& names, const std::vector<std::string>& block, int below = INT_MAX) {
    std::string newest;
    int newestVersion = -1;
    for (const std::string& name : names) {
        int version = zpam_parseVersion(name.c_str(), prefix);
        if (version > newestVersion && version < below && !zpam_isInList(name, block)) {
            newest = name;
            newestVersion = version;
        }
    }
    return newest;
}

static std::string zpam_getMainDir() {
    dvar_t* fs_basePath = Dvar_GetDvarByName("fs_basepath");
    if (!fs_basePath || !fs_basePath->value.string || fs_basePath->value.string[0] == '\0')
        return "";
    return std::string((const char*)fs_basePath->value.string) + WL("\\", "/") + "main";
}

static std::string zpam_path(const std::string& dirPath, const std::string& name) {
    return dirPath + WL("\\", "/") + name;
}

static std::vector<std::string> zpam_listDirectory(const std::string& dirPath) {
    std::vector<std::string> names;
    DIR* dir = opendir(dirPath.c_str());
    if (!dir)
        return names;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        names.push_back(ent->d_name);
    }
    closedir(dir);
    return names;
}

static bool zpam_readState(const std::string& dirPath, ZpamState* state) {
    std::string path = zpam_path(dirPath, ZPAM_STATE_FILE);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;

    char content[8192];
    size_t size = fread(content, 1, sizeof(content) - 1, f);
    fclose(f);
    content[size] = '\0';

    char buf[256];
    if (json_get_str(content, "$.zpam", buf, sizeof(buf))) state->zpam = buf;
    if (json_get_str(content, "$.maps", buf, sizeof(buf))) state->maps = buf;

    double updated = 0;
    if (json_get_num(content, "$.updated", &updated)) state->updated = (long long)updated;

    json_get_str_array(content, "$.block", &state->block);

    return !state->zpam.empty();
}

// Atomically replaces the file, so other server instances see either the old or the new one
static bool zpam_replaceFile(const std::string& from, const std::string& to) {
    #if COD2X_WIN32
        return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    #endif
    #if COD2X_LINUX
        return rename(from.c_str(), to.c_str()) == 0;
    #endif
}

static bool zpam_writeState(const std::string& dirPath, const ZpamState& state) {
    std::string path = zpam_path(dirPath, ZPAM_STATE_FILE);
    std::string tempPath = path + ".tmp";

    FILE* f = fopen(tempPath.c_str(), "wb");
    if (!f)
        return false;

    fprintf(f, "{\"zpam\":\"%s\",\"maps\":\"%s\",\"block\":[", json_escape_string(state.zpam).c_str(), json_escape_string(state.maps).c_str());
    for (size_t i = 0; i < state.block.size(); i++) {
        fprintf(f, "%s\"%s\"", i > 0 ? "," : "", json_escape_string(state.block[i]).c_str());
    }
    fprintf(f, "],\"updated\":%lld}\n", state.updated);
    bool written = !ferror(f);
    if (fclose(f) != 0)
        written = false;

    // A cut file must not replace the good one, without it all versions would be loaded
    if (!written || !zpam_replaceFile(tempPath, path)) {
        remove(tempPath.c_str());
        return false;
    }
    return true;
}


// SHA-256 of the file as lowercase hex, empty if it can not be read
static std::string zpam_hashFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return "";

    mg_sha256_ctx sha;
    mg_sha256_init(&sha);

    unsigned char buffer[65536];
    size_t length;
    while ((length = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        mg_sha256_update(&sha, buffer, length);
    }
    fclose(f);

    unsigned char digest[32];
    mg_sha256_final(digest, &sha);

    char hex[65];
    mg_snprintf(hex, sizeof(hex), "%M", mg_print_hex, (int)sizeof(digest), digest);
    return hex;
}

// Downloads the file and verifies its size and hash. Runs on the background thread.
static bool zpam_downloadFile(const ZpamManifestFile& file, const std::string& outputPath, std::string* error) {
    HttpDownload download;
    std::string result;

    if (!download.download(file.url, outputPath, "PK"))
        result = file.name + ": " + download.getError();
    else if (download.getDownloaded() != file.size)
        result = file.name + ": received " + std::to_string(download.getDownloaded()) + " of " + std::to_string(file.size) + " bytes";
    else if (file.sha256 != zpam_hashFile(outputPath))
        result = file.name + ": hash mismatch";

    if (!result.empty()) {
        remove(outputPath.c_str());
        *error = result;
        return false;
    }
    return true;
}

// Removes versions that no server instance should need anymore. Runs on the background thread.
// Deleting can fail on Windows if another instance has the file open, it is tried again next time.
static void zpam_cleanup(const std::string& mainDir, const ZpamState& state, const std::string& loaded, const std::string& loadedMaps) {
    if (time(NULL) - state.updated < ZPAM_CLEANUP_AGE_SEC)
        return;

    std::vector<std::string> names = zpam_listDirectory(mainDir);

    std::vector<std::string> keep;
    keep.push_back(loaded);
    keep.push_back(loadedMaps);
    for (const char* prefix : { ZPAM_PREFIX, ZPAM_MAPS_PREFIX }) {
        std::string newest = zpam_findNewest(prefix, names, state.block);
        keep.push_back(newest);
        keep.push_back(zpam_findNewest(prefix, names, state.block, zpam_parseVersion(newest.c_str(), prefix)));
    }

    for (const std::string& name : names) {
        if (zpam_isIwdName(name.c_str()) && !zpam_isInList(name, keep))
            remove(zpam_path(mainDir, name).c_str());
    }
}

// Downloads the files missing in main/ next to their final name with the .part suffix
static bool zpam_downloadMissing(const std::string& mainDir, const ZpamManifest& manifest, std::vector<const ZpamManifestFile*>* downloaded, std::string* error) {
    for (const ZpamManifestFile& file : manifest.files) {
        struct stat st;
        if (stat(zpam_path(mainDir, file.name).c_str(), &st) == 0 && (size_t)st.st_size == file.size)
            continue;

        logger_add("[ZPAM] Downloading %s", file.url.c_str());
        if (!zpam_downloadFile(file, zpam_path(mainDir, file.name) + ".part", error))
            return false;
        downloaded->push_back(&file);
    }
    return true;
}

// Moves the downloaded files into main/. The mappack goes first, new zPAM might need new maps.
static bool zpam_moveDownloaded(const std::string& mainDir, const std::vector<const ZpamManifestFile*>& downloaded, std::string* installed, std::string* error) {
    for (const char* prefix : { ZPAM_MAPS_PREFIX, ZPAM_PREFIX }) {
        for (const ZpamManifestFile* file : downloaded) {
            if (zpam_parseVersion(file->name.c_str(), prefix) < 0)
                continue;

            std::string path = zpam_path(mainDir, file->name);
            if (!zpam_replaceFile(path + ".part", path)) {
                *error = "Failed to move " + file->name + " into main";
                return false;
            }
            *installed += (installed->empty() ? "" : ", ") + file->name;
        }
    }
    return true;
}

// Writes the state of the manifest. The file (and its time) is kept if nothing changed.
// The block list is written by zpam_saveBlock() before the install, it is only taken from the manifest
// when the file is created. A slow install of an older manifest must not overwrite a newer block list.
static bool zpam_saveState(const std::string& mainDir, const ZpamManifest& manifest, ZpamState* state, std::string* error) {
    for (const ZpamManifestFile& file : manifest.files) {
        if (zpam_parseVersion(file.name.c_str(), ZPAM_PREFIX) >= 0)      state->zpam = file.name;
        if (zpam_parseVersion(file.name.c_str(), ZPAM_MAPS_PREFIX) >= 0) state->maps = file.name;
    }

    ZpamState current;
    bool hasCurrent = zpam_readState(mainDir, &current);
    state->block = hasCurrent ? current.block : manifest.block;

    if (hasCurrent && current.zpam == state->zpam && current.maps == state->maps) {
        state->updated = current.updated;
        return true;
    }

    state->updated = time(NULL);
    if (!zpam_writeState(mainDir, *state)) {
        *error = "Failed to write " ZPAM_STATE_FILE;
        return false;
    }
    return true;
}

// Installs the manifest into main/, all or nothing. Runs on the background thread.
// New files are only added, so nothing changes for server instances that are already running.
static void zpam_install(const std::string& mainDir, const ZpamManifest& manifest, const std::string& loaded, const std::string& loadedMaps, std::string* installed, std::string* error) {
    FileLock lock = file_lockFolder(mainDir, "zpam");
    if (lock == FILE_LOCK_NONE)
        return; // other server instance is installing, the result is picked up by the next check

    std::vector<const ZpamManifestFile*> downloaded;
    bool ok = zpam_downloadMissing(mainDir, manifest, &downloaded, error)
        && zpam_moveDownloaded(mainDir, downloaded, installed, error);

    for (const ZpamManifestFile* file : downloaded) {
        remove((zpam_path(mainDir, file->name) + ".part").c_str());
    }

    ZpamState state;
    if (ok && zpam_saveState(mainDir, manifest, &state, error))
        zpam_cleanup(mainDir, state, loaded, loadedMaps);

    file_unlockFolder(lock);
}

static std::vector<std::string> zpam_sorted(std::vector<std::string> names) {
    std::sort(names.begin(), names.end());
    return names;
}

// Writes the block list to zpam.json right away on the main thread, before the CoD2x update from the same
// response is processed. A CoD2x downgrade takes effect on restart and the newer zPAM it can't run must be
// blocked by then, even if the install of the older zPAM fails. Returns false if the list could not be written.
// Sets 'changed' if the list was written.
static bool zpam_saveBlock(const std::string& mainDir, const std::vector<std::string>& block, bool* changed) {
    *changed = false;

    ZpamState state;
    if (!zpam_readState(mainDir, &state))
        return true; // zPAM in main is not managed yet, the first install creates the file with this list

    // The order of the list is not stable, rewriting the file would postpone the cleanup
    std::vector<std::string> sorted = zpam_sorted(block);
    if (zpam_sorted(state.block) == sorted)
        return true;

    FileLock lock = file_lockFolder(mainDir, "zpam");
    if (lock == FILE_LOCK_NONE)
        return false; // install is running (in this or other server instance), tried again on the next check

    // Read again under the lock, the other instance could have changed it in the meantime
    zpam_readState(mainDir, &state);
    state.block = sorted;
    state.updated = time(NULL);
    *changed = zpam_writeState(mainDir, state);

    file_unlockFolder(lock);

    if (*changed) {
        std::string list;
        for (const std::string& name : sorted) {
            list += (list.empty() ? "" : ", ") + name;
        }
        Com_Printf("zPAM: Blocked versions: %s\n", list.empty() ? "none" : list.c_str());
    }
    return *changed;
}


static bool zpam_isEnabled() {
    if (sv_updateZpam)
        return sv_updateZpam->value.boolean;

    // The dvar is not registered during the first FS startup, but it can be set from command line
    return Dvar_GetDvarByName("sv_updateZpam") == NULL || Dvar_GetInt("sv_updateZpam") != 0;
}

// Players get a new version only from a web server, downloading it from the game server is too slow
static bool zpam_isWwwDownloadEnabled() {
    dvar_t* sv_wwwDownload = Dvar_GetDvarByName("sv_wwwDownload");
    dvar_t* sv_wwwBaseURL = Dvar_GetDvarByName("sv_wwwBaseURL");
    return sv_wwwDownload && sv_wwwDownload->value.boolean
        && sv_wwwBaseURL && sv_wwwBaseURL->value.string && sv_wwwBaseURL->value.string[0];
}

// Server running its own mod does not use zPAM from main, even if the files are there
static bool zpam_isFsGameSet() {
    const char* fs_game = Dvar_GetString("fs_game");
    return fs_game && fs_game[0];
}

// Switching to a new version needs all of these, the zPAM from main is used and players can download it
static bool zpam_canAutoUpdate() {
    return zpam_isEnabled() && zpam_isWwwDownloadEnabled() && !zpam_isFsGameSet();
}

std::string zpam_getInstalled() {
    if (!zpam_loaded.empty())
        return zpam_loaded;
    return zpam_findNewest(ZPAM_PREFIX, zpam_listDirectory(zpam_getMainDir()), {});
}

bool zpam_isUpdateCheckNeeded() {
    return zpam_isEnabled() && !zpam_isFsGameSet() && !zpam_getInstalled().empty();
}


// Checks if this process loads the newest not blocked versions, otherwise it switches when possible
static void zpam_updateSwitchPending(const std::string& mainDir) {
    ZpamState state;
    if (!zpam_readState(mainDir, &state))
        return;

    std::vector<std::string> names = zpam_listDirectory(mainDir);
    std::string newest = zpam_findNewest(ZPAM_PREFIX, names, state.block);
    std::string newestMaps = zpam_findNewest(ZPAM_MAPS_PREFIX, names, state.block);
    // Empty if all versions are blocked, zpam_filterIwds() then keeps the files and there is nothing to switch to
    zpam_switchPending = (!newest.empty() && !zpam_isSameName(newest, zpam_loaded))
        || (!newestMaps.empty() && !zpam_isSameName(newestMaps, zpam_loadedMaps));
}

bool zpam_applyManifest(const ZpamManifest& manifest) {
    // A response without the manifest (or with a broken one) must not clear the block list
    if (Dvar_GetInt("dedicated") == 0 || manifest.files.empty())
        return true;

    // Only servers that already use zPAM are updated
    std::string mainDir = zpam_getMainDir();
    if (mainDir.empty() || !zpam_isUpdateCheckNeeded())
        return true;

    // Also without www download, the server can have a version from an earlier automatic update
    bool blockChanged;
    if (!zpam_saveBlock(mainDir, manifest.block, &blockChanged)) {
        Com_Printf("zPAM: Failed to save blocked versions to " ZPAM_STATE_FILE ", CoD2x update postponed\n");
        return false;
    }
    if (blockChanged)
        zpam_updateSwitchPending(mainDir);

    // The name from the response is used as a file name
    for (const ZpamManifestFile& file : manifest.files) {
        if (!zpam_isIwdName(file.name.c_str())) {
            Com_Printf("zPAM: Invalid file name in update info: %s\n", file.name.c_str());
            return true;
        }
    }

    if (!zpam_isWwwDownloadEnabled()) {
        static bool printed = false;
        if (!printed) {
            printed = true;
            Com_Printf("zPAM: Automatic update needs sv_wwwDownload 1 and sv_wwwBaseURL, update skipped\n");
        }
        return true;
    }

    int expected = ZPAM_WORKER_IDLE;
    if (!zpam_workerState.compare_exchange_strong(expected, ZPAM_WORKER_RUNNING))
        return true;

    std::string loaded = zpam_loaded;
    std::string loadedMaps = zpam_loadedMaps;

    std::thread([mainDir, manifest, loaded, loadedMaps]() {
        std::string installed, error;
        zpam_install(mainDir, manifest, loaded, loadedMaps, &installed, &error);

        zpam_workerError = error;
        zpam_workerInstalled = installed;
        zpam_workerState = ZPAM_WORKER_DONE;
    }).detach();

    return true;
}


// Picks the iwd file of one group that this process loads and hides the others
static void zpam_selectVersion(const char* prefix, std::string& loaded, bool canSwitch,
                               const std::vector<std::string>& names, const std::vector<std::string>& block, std::vector<bool>& hidden) {
    std::string newest = zpam_findNewest(prefix, names, block);
    if (newest.empty())
        return; // nothing to choose from

    std::string selected = newest;
    if (!loaded.empty() && !canSwitch && zpam_isInList(loaded, names))
        selected = loaded;

    if (!zpam_isSameName(selected, newest))
        zpam_switchPending = true;

    if (!zpam_isSameName(selected, loaded))
        Com_Printf("zPAM: Loading %s\n", selected.c_str());
    loaded = selected;

    for (size_t i = 0; i < names.size(); i++) {
        if (zpam_parseVersion(names[i].c_str(), prefix) >= 0 && !zpam_isSameName(names[i], selected))
            hidden[i] = true;
    }
}

static void zpam_rememberNewest(const char* prefix, std::string& loaded, const std::vector<std::string>& names) {
    std::string newest = zpam_findNewest(prefix, names, {});
    if (!newest.empty())
        loaded = newest;
}

static bool zpam_isMatchRunning() {
    return match.activated || match.loading || match.downloading;
}

static bool zpam_isServerIdle() {
    if (zpam_isMatchRunning())
        return false;

    for (int i = 0; i < sv_maxclients->value.integer; i++) {
        if (svs_clients[i].state != CS_FREE)
            return false;
    }
    return true;
}

std::string zpam_getDownloadUrl(const char* remoteName) {
    if (I_strnicmp(remoteName, "main/", 5) != 0 || !zpam_isIwdName(remoteName + 5))
        return "";

    return std::string(ZPAM_WWW_BASE_URL "/") + remoteName;
}

int zpam_filterIwds(const char* directory, char** files, int numFiles) {
    if (!zpam_isEnabled())
        return numFiles;

    // Only the main folder
    size_t len = strlen(directory);
    if (len < 5 || I_strnicmp(directory + len - 4, "main", 4) != 0 || (directory[len - 5] != '/' && directory[len - 5] != '\\'))
        return numFiles;

    std::vector<std::string> names(files, files + numFiles);

    // Without the state file all versions are loaded. The newest one is remembered, otherwise the
    // first install would look like an unknown version and could be switched during a match.
    ZpamState state;
    if (!zpam_readState(directory, &state)) {
        // fs_homepath/main of a server with the state file in fs_basepath/main, it must not change the selected version
        ZpamState managed;
        if (zpam_readState(zpam_getMainDir().c_str(), &managed))
            return numFiles;

        zpam_rememberNewest(ZPAM_PREFIX, zpam_loaded, names);
        zpam_rememberNewest(ZPAM_MAPS_PREFIX, zpam_loadedMaps, names);
        return numFiles;
    }

    std::vector<bool> hidden(numFiles, false);

    // Files are listed on server start, map change and full map_restart (not on round restarts).
    // Connected players download the new version as part of the map change.
    bool canSwitch = !zpam_isMatchRunning() && zpam_canAutoUpdate();
    zpam_switchPending = false;
    zpam_selectVersion(ZPAM_PREFIX, zpam_loaded, canSwitch, names, state.block, hidden);
    zpam_selectVersion(ZPAM_MAPS_PREFIX, zpam_loadedMaps, canSwitch, names, state.block, hidden);

    // The strings are allocated by the engine, so hidden entries are only moved to the end of the list
    int writeIndex = 0;
    for (int i = 0; i < numFiles; i++) {
        if (hidden[i]) {
            Com_Printf("zPAM: Ignoring %s\n", names[i].c_str());
            continue;
        }
        char* temp = files[writeIndex];
        files[writeIndex] = files[i];
        files[i] = temp;
        writeIndex++;
    }
    return writeIndex;
}


/** Called every frame. */
void zpam_frame() {
    if (!dedicated->value.boolean)
        return;

    if (zpam_workerState == ZPAM_WORKER_DONE) {
        std::string error = zpam_workerError;
        if (!error.empty()) {
            Com_Printf("zPAM: Update failed: %s\n", error.c_str());
            error_report(ERROR_KIND_ZPAM_UPDATE, "zPAM update failed: " + error);
        }
        if (!zpam_workerInstalled.empty())
            Com_Printf("zPAM: Installed %s\n", zpam_workerInstalled.c_str());
        zpam_workerState = ZPAM_WORKER_IDLE;

        // Also when other server instance installed it
        if (error.empty())
            zpam_updateSwitchPending(zpam_getMainDir());
    }

    if (!zpam_switchPending || !sv_running->value.boolean || !zpam_isServerIdle() || !zpam_canAutoUpdate()) {
        zpam_idleSince = 0;
        return;
    }

    // An empty server does not change the map on its own, so restart it to load the new version
    uint64_t now = ticks_ms();
    if (zpam_idleSince == 0) {
        zpam_idleSince = now;
    } else if (now - zpam_idleSince >= ZPAM_IDLE_TIME_MS) {
        zpam_idleSince = now;
        Com_Printf("zPAM: Server is empty, restarting the map to load the new version\n");
        Cbuf_AddText("map_restart\n");
    }
}


/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void zpam_init() {
    sv_updateZpam = Dvar_RegisterBool("sv_updateZpam", true, (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
}
