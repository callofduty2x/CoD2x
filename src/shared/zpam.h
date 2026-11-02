#ifndef ZPAM_H
#define ZPAM_H

#include <string>
#include <vector>
#include <cstddef>

// Automatic zPAM updates of dedicated servers.
//
// The master server sends a manifest in the update check response (see updater_https). New files
// are only added to main/ (the folder may be shared by more server instances), so installing a
// new version does not change any running server. Each process then decides on its own which
// version it loads: zpam_filterIwds() hides the other versions when the iwd files are listed.
// A server switches to the newest version on start, map change or full map_restart, unless a match
// is running. An empty server does not change the map on its own, so it restarts the map itself.
//
// Only servers with zpam<digits>.iwd in main/, without fs_game and with sv_wwwDownload + sv_wwwBaseURL
// are updated, otherwise players could not download the new version.
//
// main/zpam.json holds the version installed from the manifest and the blocked versions:
//   {"zpam":"zpam409.iwd","maps":"zpam_maps_v7.iwd","block":["zpam410.iwd"],"updated":1793000000}
// Without this file zPAM iwds are not touched at all.

struct ZpamManifestFile {
    std::string name;       // "zpam409.iwd" or "zpam_maps_v7.iwd"
    std::string url;
    size_t      size = 0;
    std::string sha256;     // lowercase hex
};

struct ZpamManifest {
    std::vector<ZpamManifestFile> files;
    std::vector<std::string>      block;    // versions that must not be loaded (rollback)
};

// True if this server would be updated (zPAM in main, no fs_game), sv_update 0 must not keep others checking
bool zpam_isUpdateCheckNeeded();
// Installed version for the update check ("zpam409.iwd"), empty if this server does not use zPAM
std::string zpam_getInstalled();

// Main thread: installs the manifest in the background
void zpam_applyManifest(const ZpamManifest& manifest);

// Client: if remoteName is a zPAM iwd from the download list ("main/zpam409.iwd"), returns the URL
// to download it from ("https://cod2x.me/zpam/main/zpam409.iwd"), empty for all other files.
std::string zpam_getDownloadUrl(const char* remoteName);

// Called when iwd files of a directory are listed. Moves the iwd files that should not be loaded
// to the end of the list and returns the new number of files.
int zpam_filterIwds(const char* directory, char** files, int numFiles);

void zpam_frame();
void zpam_init();

#endif // ZPAM_H
