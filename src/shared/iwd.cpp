#include "iwd.h"
#include "shared.h"

#include "http_client.h"
#if COD2X_WIN32
    #include <windows.h>
#endif
#include <dirent.h>
#include <sys/stat.h>   // stat, mkdir
#include <cstdio>       // fopen, fwrite, fclose
#include <cerrno>       // errno
#include <cstring>      // strerror

#include "cod2_common.h"
#include "cod2_dvars.h"
#include "cod2_shared.h"
#include "cod2_file.h"
#include "cod2_cmd.h"
#if COD2X_WIN32
    #include "cod2_client.h"
#endif


// Macros to declare and use embedded files
// Windows strip the underscore prefix from symbol names, Linux does not
#if COD2X_WIN32
    #define EMBEDDED_FILE_DECLARATIONS(filename) \
        extern const unsigned char binary_##filename##_start[]; \
        extern const unsigned char binary_##filename##_end[];
    #define EMBEDDED_FILE_SYMBOLS(filename) \
        binary_##filename##_start, binary_##filename##_end
#endif
#if COD2X_LINUX
    #define EMBEDDED_FILE_DECLARATIONS(filename) \
        extern const unsigned char _binary_##filename##_start[]; \
        extern const unsigned char _binary_##filename##_end[];
    #define EMBEDDED_FILE_SYMBOLS(filename) \
        _binary_##filename##_start, _binary_##filename##_end
#endif

// Define embedded files
// The file name depends on the name used in CMakeLists.txt - file name is used with non-alphanumeric characters replaced by '_'
extern "C" {
    EMBEDDED_FILE_DECLARATIONS(iw_CoD2x_01_iwd)
}

#define NUM_IW_IWDS 25 // Original
#define NUM_IW_COD2X_IWDS 1 // New CoD2x iwds

#define ZPAM_LATEST_VERSION "zpam406"
#define ZPAM_LATEST_MAPPACK "zpam_maps_v7"

extern dvar_t* g_cod2x;
dvar_t* com_writeConfig = NULL;


/**
 * Cleanup test zPAM files specified in a blacklist
 */
static void iwd_processZpamFiles() {

    // Exit if not server
    if (Dvar_GetInt("dedicated") == 0) {
        return;
    }

    const char* blacklist[] = {
        "zpam400_test1.iwd", 
        "zpam400_test2.iwd", 
        "zpam400_test3.iwd",
        "zpam400_test4.iwd",
        "zpam400_test5.iwd",
        "zpam400_test6.iwd",
        "zpam401.iwd",
        "zpam402.iwd",
        "zpam403.iwd",
        "zpam404.iwd",
        "zpam_maps_v6.iwd",
        "zpam405.iwd",
    };
    size_t blacklistSize = sizeof(blacklist) / sizeof(blacklist[0]);

    dvar_t* fs_basePath = Dvar_GetDvarByName("fs_basepath");
    if (!fs_basePath || !fs_basePath->value.string || fs_basePath->value.string[0] == '\0') {
        Com_Printf("zPAM cleanup: fs_basepath not set, skipping.\n");
        return;
    }

    const char* sep = WL("\\", "/");
    char mainDir[MAX_OSPATH * 2] = {0};
    snprintf(mainDir, sizeof(mainDir), "%s%smain", (const char*)fs_basePath->value.string, sep);

    DIR* dir = opendir(mainDir);
    if (!dir) return;

    bool downloadLatestZpam = false;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        const char* name = ent->d_name;

        // Skip dot entries
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        // Build full path and filter directories via stat
        char fullPath[MAX_OSPATH * 2] = {0};
        snprintf(fullPath, sizeof(fullPath), "%s%s%s", mainDir, sep, name);

        struct stat st;
        if (stat(fullPath, &st) == 0 && S_ISDIR(st.st_mode)) {
            continue;
        }

        // Check if the file is in the blacklist
        bool isBlacklisted = false;
        for (size_t i = 0; i < blacklistSize; ++i) {
            if (I_strnicmp(name, blacklist[i], strlen(blacklist[i])) == 0) {
                isBlacklisted = true;
                break;
            }
        }

        if (isBlacklisted) {
            if (remove(fullPath) == 0) {
                Com_Printf("Deleted old zPAM file: %s\n", fullPath);
                downloadLatestZpam = true;
            } else {
                Com_Error(ERR_FATAL, "Failed to delete old zPAM file: %s (errno=%d)\n", fullPath, errno);
            }
        }
    }
    closedir(dir);

    if (downloadLatestZpam) {
        Com_Printf("Downloading latest zPAM version: %s\n", ZPAM_LATEST_VERSION);

        auto downloadFile = [](const char* url, const char* outputPath) {
            HttpClient* client = new HttpClient();

            // Open file for writing
            FILE* file = fopen(outputPath, "wb");
            if (!file) {
                Com_Printf("Failed to open file for zPAM writing: %s\n", outputPath);
                delete client;
                return;
            }

            bool downloading = true;

            auto closeFile = [&file]() {
                if (file) {
                    fclose(file);
                    file = nullptr;
                }
            };

            client->downloadFile(url, 
                [file, url](const char* data, size_t length, size_t downloaded, size_t total) {
                    static uint64_t lastPrintTime = 0;
                    uint64_t currentTime = ticks_ms();

                    // Write received data chunk to file
                    if (file && data && length > 0) {
                        fwrite(data, 1, length, file);
                    }

                    // Show progress only if at least 1 second has passed since the last message
                    if (currentTime - lastPrintTime >= 1000) {
                        lastPrintTime = currentTime;

                        if (total > 0) {
                            int progress = (int)((double)downloaded / (double)total * 100.0);
                            Com_Printf("Downloading %s: %3d%%  %10zu / %10zu bytes\n", url, progress, downloaded, total);
                        } else {
                            Com_Printf("Downloading %s: %zu bytes\n", url, downloaded);
                        }
                    }
                },
                [&downloading, &closeFile, url](const HttpClient::Response& res) {
                    downloading = false;
                    closeFile();
                    if (res.status == 200 || res.status == 201) {
                        Com_Printf("Downloading %s: 100%% complete!\n", url);
                    } else {
                        Com_Printf("Downloading %s: zPAM download failed with status %d: %s\n", url, res.status, res.body.c_str());
                    }
                },
                [&downloading, &closeFile](const std::string& error) {
                    downloading = false;
                    closeFile();
                    Com_Printf("HTTP error while downloading IWD file: %s\n", error.c_str());
                },
                60000,  // 60 second overall timeout
                3000   // 3 second for initial connection
            );

            // Poll for events
            while (downloading) {
                client->poll(50);  // Poll every 50ms for better responsiveness
            }

            delete client;
        };

        // Download zPAM mod
        char outputPathzPAM[MAX_OSPATH * 2] = {0};
        snprintf(outputPathzPAM, sizeof(outputPathzPAM), "%s%s%s.iwd", mainDir, sep, ZPAM_LATEST_VERSION);
        char urlZpam[MAX_OSPATH * 2] = {0};
        snprintf(urlZpam, sizeof(urlZpam), "http://cod2x.me/zpam/main/%s.iwd", ZPAM_LATEST_VERSION);
        downloadFile(urlZpam, outputPathzPAM);

        // Check if we need to download the mappack
        char outputPathMappack[MAX_OSPATH * 2] = {0};
        snprintf(outputPathMappack, sizeof(outputPathMappack), "%s%s%s.iwd", mainDir, sep, ZPAM_LATEST_MAPPACK);

        // Check if the mappack file already exists
        struct stat st;
        if (stat(outputPathMappack, &st) != 0) {
            // If the file does not exist, download it
            char urlMappack[MAX_OSPATH * 2] = {0};
            snprintf(urlMappack, sizeof(urlMappack), "http://cod2x.me/zpam/main/%s.iwd", ZPAM_LATEST_MAPPACK);
            downloadFile(urlMappack, outputPathMappack);
        } else {
            Com_Printf("zPAM mappack already exists, skipping download: %s\n", outputPathMappack);
        }
    }
}


/**
 * Cleanup newer or testing CoD2x IWD files
 */
static void iwd_cleanupCoD2xIwdFiles() {
    dvar_t* fs_basePath = Dvar_GetDvarByName("fs_basepath");
    if (!fs_basePath || !fs_basePath->value.string || fs_basePath->value.string[0] == '\0') {
        Com_Printf("CoD2x IWD cleanup: fs_basepath not set, skipping.\n");
        return;
    }

    const char* sep = WL("\\", "/");
    char mainDir[MAX_OSPATH * 2] = {0};
    snprintf(mainDir, sizeof(mainDir), "%s%smain", (const char*)fs_basePath->value.string, sep);

    auto shouldDelete = [](const char* filename) -> bool {
        // Only consider IWD files
        const char* dot = strrchr(filename, '.');
        if (!dot || FS_FilenameCompare(dot, ".iwd") != 0) return false;

        // Must start with iw_CoD2x_
        const char* prefix = "iw_CoD2x_";
        size_t prefixLen = strlen(prefix);
        if (I_strnicmp(filename, prefix, prefixLen) != 0) return false;

        // Expect two digits after prefix
        if (!isdigit((unsigned char)filename[prefixLen]) || !isdigit((unsigned char)filename[prefixLen + 1])) {
            return false;
        }
        int num = (filename[prefixLen] - '0') * 10 + (filename[prefixLen + 1] - '0');

        // Delete higher-numbered IWDs (downgrade protection)
        if (num > NUM_IW_COD2X_IWDS) return true;

        // Delete testing versions
        // iw_CoD2x_xx_1.4.5.1-test.1.iwd  vs  iw_CoD2x_xx.iwd
        const char* versionPart = filename + prefixLen + 2; // after the two digits
        if (*versionPart != '.') {
            if (APP_VERSION_IS_TEST) {
                // Ignore current test version
                if (strstr(versionPart, APP_VERSION) == nullptr) {
                    return true; // if it does not contain current test version, delete it
                }
            } else {
                return true; // if it does not end with .iwd, delete it (it's a testing version)
            }
        }

        return false;
    };


    DIR* dir = opendir(mainDir);
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        const char* name = ent->d_name;

        // skip dot entries
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        // build full path and filter directories via stat
        char fullPath[MAX_OSPATH * 2] = {0};
        snprintf(fullPath, sizeof(fullPath), "%s%s%s", mainDir, sep, name);

        struct stat st;
        if (stat(fullPath, &st) == 0 && S_ISDIR(st.st_mode)) {
            continue;
        }

        if (shouldDelete(name)) {
            #if COD2X_WIN32 && DEBUG
                // Confirm on Windows
                if (IDYES != MessageBoxA(
                        NULL,
                        va("Found newer or testing CoD2x IWD that needs to be deleted:\n%s\n\nProceed with deletion?", fullPath),
                        "Confirm Deletion",
                        MB_ICONWARNING | MB_YESNO)) {
                    closedir(dir);
                    exit(0);
                    return;
                }
            #endif

            if (remove(fullPath) == 0) {
                Com_Printf("Deleted CoD2x IWD: %s\n", fullPath);
            } else {
                Com_Error(ERR_FATAL, "Failed to delete CoD2x IWD: %s (errno=%d)\n", fullPath, errno);
            }
        }
    }
    closedir(dir);

}


// Write embedded file if it doesn’t already exist
void iwd_extractFiles(const char* out_path, const unsigned char* start, const unsigned char* end) {
    size_t size = (size_t)(end - start);

    // Check if file already exists and has the correct size
    struct stat st;
    if (stat(out_path, &st) == 0) {
        if ((size_t)st.st_size == size) {
            return; // file already exists and size matches
        } else {
            // file exists but size does not match, delete it
            int result = remove(out_path);
            if (result != 0) {
                Com_Error(ERR_FATAL, "Error while extracting IWD file to '%s'. Error while deleting existing file: %s\n", out_path, strerror(errno));
                return;
            }
        }
    }

    FILE* f = fopen(out_path, "wb");
    if (!f) {
        Com_Error(ERR_FATAL, "Error while extracting IWD file to '%s'. Error while opening file: %s\n", out_path, strerror(errno));
        return;
    }

    size_t written = fwrite(start, 1, size, f);
    fclose(f);

    if (written != size) {
        Com_Error(ERR_FATAL, "Error while extracting IWD file to '%s'. Error while writing to file: %s\n", out_path, strerror(errno));
        return;
    }
}

/**
 * Extract embedded iwd file file into main folder
 */
void iwd_extractIwdFileToMain(const char* iwdFileName, const unsigned char* start, const unsigned char* end) {
    
    dvar_t* fs_basePath = Dvar_GetDvarByName("fs_basepath"); // installation directory of the game

    if (!fs_basePath || !fs_basePath->value.string || fs_basePath->value.string[0] == '\0') {
        Com_Error(ERR_FATAL, "Error while extracting IWD file '%s' into main folder. Dvar fs_basepath not found\n", iwdFileName);
        return;
    }

    const char* pathSeparator = WL("\\", "/");
    const char* testVersion = APP_VERSION_IS_TEST ? va("_%s", APP_VERSION) : "";
    // path/main/iw_CoD2x_01.iwd
    // path/main/iw_CoD2x_01_1.4.5.1-test.1.iwd - for test versions, file will be deleted
    const char* newFile = va("%s%smain%s%s%s.iwd", (const char*)fs_basePath->value.string, pathSeparator, pathSeparator, iwdFileName, testVersion);

    iwd_extractFiles(newFile, start, end);
}


/**
 * Is called when game needs to verify if particular iwd file is part of installation, not custom mod
 * Since we added new iw_CoD2x_01.iwd files, we need to make sure these files are accepted
 * iwd might be "main/zpam_maps_v4"
 * base might be "main"
 */
qboolean FS_iwIwd(char *iwd, const char *base, int cod2x_version)
{
	char *pszLoc;
	char szFile[MAX_QPATH];
	int i;

	for ( i = 0; i < NUM_IW_IWDS; ++i )
	{
        const char *filename = va("%s/iw_%02d", base, i); // check if it starts with this
		if ( !FS_FilenameCompare(iwd, filename) )
			return qtrue;
	}

    // CoD2x: New iwds
    int numCoD2xIwds = 0;
    switch (cod2x_version)
    {
        case 0: 
        case 1: 
        case 2: 
        case 3: 
        case 4: 
            numCoD2xIwds = 0; 
            break;

        // Since 1.4.5.1
        case 5: 
        default:
            numCoD2xIwds = 1; 
            break;
    }
    for ( i = 0; i < numCoD2xIwds; ++i )
    {
        const char *filename = va("%s/iw_CoD2x_%02d", base, i + 1); // check if it starts with this
        if ( !I_strnicmp(iwd, filename, strlen(filename)) )
            return qtrue;
    }
    // CoD2x: End

	pszLoc = strstr(iwd, "localized_");

	if ( pszLoc )
	{
		strcpy(szFile, iwd);
		szFile[pszLoc - iwd + 10] = 0;

		if ( !FS_FilenameCompare(szFile, va("%s/localized_", base)) )
		{
			strcpy(szFile, pszLoc + 10);
			I_strlwr(szFile);

			for ( i = 0; i < NUM_IW_IWDS; ++i )
			{
				if ( strstr(szFile, va("_iw%02d", i)) )
					return qtrue;
			}
		}
	}

	return qfalse;
}
qboolean FS_iwIwd_Win32() {
	char* iwd;  ASM( movr, iwd,  "edi" );
    char* base; ASM( movr, base, "ebx" );
    return FS_iwIwd(iwd, base, true);
}
qboolean FS_iwIwd_Linux(char *iwd, const char *base) {
    return FS_iwIwd(iwd, base, true);
}



#if COD2X_WIN32

// Windows only
// Called when the game needs to list files in a directory
char** Sys_ListFiles(char* extension, int32_t* numFiles, int32_t wantsubs) {
    // Load parameters from registers
    char* directory;
    char* filter;
    ASM( movr, directory, "eax" );
    ASM( movr, filter, "edx" );

    // Call the original function
    char** result;
    ASM_CALL(RETURN(result), 0x00463e70, 3, EAX(directory), EDX(filter), PUSH(extension), PUSH(numFiles), PUSH(wantsubs));
    // directory: D:\\CoD2x\\bin\\windows\\main
    // extension: "iwd"
    // result[0] = "iw_00.iwd"

    // When the game starts for the first time, load only the original IWD files
    // The main folder might contain mix of mods from different servers that might cause "iwd sum mismatch" errors when running the game
    // This will make sure these mods are not loaded at startup, but will be loaded when connecting to the game

    // No files in this folder (propably raw, devraw, etc)
    if (*numFiles <= 0) {
        return result;
    }

    // Read value via function to get value even if dvar is not registered yet
    int dedicatedValue = Dvar_GetInt("dedicated");

    // Server - load all IWD files, do not filter. 
    if (dedicatedValue > 0) {
        return result;
    }


    const char* fs_game = Dvar_GetString("fs_game");

    // Helper: check whether 'path' ends with 'suffix', treating '\\' and '/' as equivalent (case-insensitive)
    // The suffix must be preceded by a separator (or occupy the whole path).
    auto pathEndsWith = [](const char* path, const char* suffix) -> bool {
        if (!suffix || suffix[0] == '\0') return false;
        size_t pathLen = strlen(path);
        size_t suffixLen = strlen(suffix);
        if (pathLen < suffixLen) return false;
        const char* pathEnd = path + pathLen - suffixLen;
        // Must be at the start or preceded by a path separator
        if (pathEnd != path && *(pathEnd - 1) != '\\' && *(pathEnd - 1) != '/') return false;
        for (size_t k = 0; k < suffixLen; k++) {
            char pc = pathEnd[k]; if (pc == '\\') pc = '/';
            char sc = suffix[k];  if (sc == '\\') sc = '/';
            if (tolower((unsigned char)pc) != tolower((unsigned char)sc)) return false;
        }
        return true;
    };

    // Extract the folder name from directory.
    // First check whether directory corresponds to fs_game (which may contain slashes).
    // Fall back to the last path segment only when that check fails.
    const char* folder;
    if (fs_game && fs_game[0] != '\0' && pathEndsWith(directory, fs_game)) {
        folder = fs_game;
    } else {
        const char* lastSlash = nullptr;
        for (const char* p = directory; *p; ++p) {
            if (*p == '\\' || *p == '/') lastSlash = p;
        }
        folder = lastSlash ? lastSlash + 1 : directory;
    }

    // This is fs_game folder
    bool isFsGameFolder = fs_game && fs_game[0] != '\0' && pathEndsWith(directory, fs_game);


    {
        // The original function created list of loaded iwd files
        // We will remove some entries from this list via filtering
        int writeIndex = 0;
        int i;
        for (i = 0; i < *numFiles; i++) {
            //Com_Printf("File: %s\n", result[i]);

            // Is running listen server, this function is called with clientState == CLIENT_STATE_CHALLENGING
            // When disconnecting from listen server, sv_running will still be true, so we need to check clientState != DISCONNECTED
            bool isListenServer = (COD2X_WIN32 && sv_running && sv_running->value.boolean && clientState != CLIENT_STATE_DISCONNECTED);

            // We need to decide if we will load CoD2x IWD files or not
            // We might be connecting to older server that does not have newer CoD2x IWD files, causing impure client error because client has loaded files that server does not have
            // When connecting to server, g_cod2x is systeminfo variable, meaning it will get synced with the client (g_cod2x will be set on server's g_cod2x value)
            // For servers before 1.4.5.1, g_cod2x is not systeminfo cvar, meaning the value is 0
            // For servers since version 1.4.5.1, g_cod2x is systeminfo cvar, meaning the value is synced with the client on early stage of connection
            int cod2x_version = APP_VERSION_PROTOCOL;
            if (g_cod2x && COD2X_WIN32 && clientState >= CLIENT_STATE_CONNECTING && isListenServer == false) {
                cod2x_version = g_cod2x->value.integer; // 0 for 1.3 and <1.4.5.1 servers, 5 for 1.4.5.1 (and so on for new versions)
            }
            if (i == 0) {
                Com_Printf("Loading IWD files for CoD2x version: %d\n", cod2x_version);
            }

            // Remove ".iwd" extension from result[i]
            char iwIwdName[MAX_QPATH];
            char* dot = strrchr(result[i], '.');
            int len = dot ? (int)(dot - result[i]) : (int)strlen(result[i]);
            snprintf(iwIwdName, sizeof(iwIwdName), "/%.*s", len, result[i]);
            
            // Is file "iw_00" - "iw_15" or starts with "localized_"
            bool allowFileToBeLoaded = FS_iwIwd(iwIwdName, "", cod2x_version);


            // When connecting to server or replaying demo
            if (allowFileToBeLoaded == false && COD2X_WIN32 && isListenServer == false && clientState >= CLIENT_STATE_CONNECTING) {

                // When playing demo, load all IWD files in movie folder
                // This allows to load mod IWD files when playing demo
                if (demo_isPlaying && stricmp(folder, "movie") == 0) {                 
                    allowFileToBeLoaded = true;                  
                }
                // For other non-movie folders or when conecting to server, load only IWD files referenced by the server
                else {
                    const char* systeminfo = CL_GetConfigString(CS_SYSTEMINFO);
                    const char* sv_iwdNames = Info_ValueForKey(systeminfo, "sv_iwdNames"); // "zpam_maps_v4 zpam334 iw_16 iw_15 iw_14 iw_13 iw_12 iw_11 iw_10 iw_09 iw_08 iw_07 iw_06 iw_05 iw_04 iw_03 iw_02 iw_01".

                    Cmd_TokenizeString(sv_iwdNames);

                    int count = Cmd_Argc();
                    if (count > 1024)
                        count = 1024;
                    if (count > 0) {               
                        // Check if this iwd is in the list
                        for (int j = 0; j < count; j++) {
                            const char* demoIwdName = Cmd_Argv(j); // "zpam334"
                            // If demo iwd name starts with currently processed iwd file
                            // demoIwdName might be "zpam_maps_v4"
                            // result[i] might be "zpam_maps_v4.iwd" or "zpam_maps_v4.8bb28f35.iwd" (use both)
                            if (I_strnicmp(demoIwdName, result[i], strlen(demoIwdName)) == 0) {
                                Com_Printf("Required IWD from server/demo: %s.iwd\n", demoIwdName);
                                allowFileToBeLoaded = true;
                                break;
                            }
                        }
                    }
                }
            }

            // Listen server
            if (allowFileToBeLoaded == false && isListenServer) {
                
                // Helper lambda to check if zpam/zpam_maps_v IWD is latest, allowing suffix after version
                auto isLatestZpamIwd = [&](const char* prefix) -> bool {
                    int prefixLen = (int)strlen(prefix);
                    if (strnicmp(result[i], prefix, prefixLen) != 0) return false;
                    // Find version number after prefix
                    const char* verStart = result[i] + prefixLen;
                    // Check if first char is digit
                    if (!isdigit((unsigned char)verStart[0])) return false;
                    int currentVersion = atoi(verStart);
                    for (int j = 0; j < *numFiles; j++) {
                        if (j == i) continue;
                        if (strnicmp(result[j], prefix, prefixLen) == 0) {
                            const char* otherVerStart = result[j] + prefixLen;
                            int otherVersion = atoi(otherVerStart);
                            // If otherVersion > currentVersion, it's newer
                            if (otherVersion > currentVersion) {
                                return false;
                            }
                        }
                    }
                    return true;
                };

                // Allow latest "zpam[...].iwd" file
                if (isLatestZpamIwd("zpam")) {
                    Com_Printf("Allowed latest ZPAM IWD file: %s\n", result[i]);
                    allowFileToBeLoaded = true;
                }

                // Allow latest "zpam_maps_vX[...].iwd" file
                if (isLatestZpamIwd("zpam_maps_v")) {
                    Com_Printf("Allowed latest ZPAM_MAPS IWD file: %s\n", result[i]);
                    allowFileToBeLoaded = true;
                }
                
            }

            // Runned with fs_game, but not connecting to server yet, allow loading IWD files for that folder 
            if (allowFileToBeLoaded == false && isFsGameFolder) {
                allowFileToBeLoaded = true;
            }


            if (allowFileToBeLoaded) {
                // Swap the entries in the list
                // We could remove the item from the list, but the string is allocated by the original function and we cannot free it in this dll, because its using different memory manager
                if (writeIndex != i) {
                    char* temp = result[writeIndex];
                    result[writeIndex] = result[i];
                    result[i] = temp;
                }
                writeIndex++;
            } else {
                Com_Printf("Filtered out IWD file: %s\n", result[i]);
            }
        }
        *numFiles = writeIndex;
    }


    return result;
}


void CL_InitDownloads() {
    // Since we are filtering loaded IWD files for demo, game then thought some required IWD files were missing
    // Fix that by calling CL_DownloadsComplete directly
    if (demo_isPlaying) {
        // Call CL_DownloadsComplete
        ASM_CALL(RETURN_VOID, 0x0040dc20); 
        return;
    }

    // Call the original CL_InitDownloads function
    ASM_CALL(RETURN_VOID, 0x0040df10);
}



// Called on /disconnect
int CL_Disconnect_CMD_disconnect() {
    int cs = clientState;

    int ret;
    ASM_CALL(RETURN(ret), 0x0040cd10);

    // If player was connected to server, reset fs_game to main and restart FS
    if (cs >= CLIENT_STATE_CONNECTED)
    {
        Dvar_SetString(Dvar_GetDvarByName("fs_game"), "");
        FS_Restart(0);
        ASM_CALL(RETURN_VOID, 0x00415a00); // CL_ShutdownUI
    }

    return ret;
}

/**
 * Internal function to build OS path from base, game and qpath
 * base: "D:\CoD2x\bin\windows"
 * game: "main" / "modFolderName"
 * qpath: "players/default/config_mp.cfg" / "hunkusage.dat" / "iw_00.iwd"
 * ospath: output buffer
 */
void FS_BuildOSPath_Internal(const char* base, const char* game, char* qpath, char* ospath, int32_t thread, int read_write) {
    //Com_Printf("FS_BuildOSPath_Internal: R/W=%c, base='%s', game='%s', qpath='%s'\n", read_write ? 'W' : 'R', base, game, qpath);

    bool isConfig = com_playerProfile && stricmp(qpath, va("players/%s/config_mp.cfg", com_playerProfile->value.string)) == 0;
    
    // Always use "main" folder for config_mp.cfg
    if (isConfig) {
        //Com_Printf("FS_BuildOSPath_Internal: Changed game from '%s' to 'main'\n", game);
        game = "main"; // always use main for config_mp.cfg

        // Print debug write message
        if (read_write == 1) {
            Com_DPrintf("Writing to '%s/%s'\n", game, qpath);
        }
    }

    // Use only .iwd files from movie folder
    else if (stricmp(game, "movie") == 0) {
        const char* dot = strrchr(qpath, '.');
        if (!dot || FS_FilenameCompare(dot, ".iwd") != 0) {
            //Com_Printf("FS_BuildOSPath_Internal: Changed game from 'movie' to 'main' for demo\n");
            game = "main"; // use main for non-iwd files when playing demo
        }
    }

    ASM_CALL(RETURN_VOID, 0x00421c30, 4, EAX(base), PUSH(game), PUSH(qpath), PUSH(ospath), PUSH(thread));

    //Com_Printf("FS_BuildOSPath_Internal: Result: '%s'\n", ospath);

    return;
}

// Called in FS_FOpenFileWrite
void FS_BuildOSPath_Internal_FS_Write(const char* game, char* qpath, char* ospath, int32_t thread) {
    const char* base;  ASM( movr, base,  "eax" );
    return FS_BuildOSPath_Internal(base, game, qpath, ospath, thread, 1);
}

// Called in FS_FOpenFileRead
void FS_BuildOSPath_Internal_FS_Read(const char* game, char* qpath, char* ospath, int32_t thread) {
    const char* base;  ASM( movr, base,  "eax" );
    return FS_BuildOSPath_Internal(base, game, qpath, ospath, thread, 0);
}



bool com_writeConfigLastValue = false;

void Com_WriteConfigToFile() {
    char* filename; ASM( movr, filename, "eax" );

    // Use flag to disable writing into config after we write the cvar change itself
    if (com_writeConfig && !com_writeConfig->value.boolean) {
        if (com_writeConfigLastValue == false)
            return;
        com_writeConfigLastValue = false;
    } else {
        com_writeConfigLastValue = true;
    }

    ASM_CALL(RETURN_VOID, 0x00434a90, 0, EAX(filename));
}





// This function is called in FS_Startup after registering the original paths, we add new one from movie
void FS_AddCommands() {

    // Add "movie" folder to search path for IWD files
    if (demo_isPlaying) {
        const char* path = Dvar_GetString("fs_basepath");
        const char* dir = "movie";
        ASM_CALL(RETURN_VOID, 0x00424f20, 0, EBX(path), EDI(dir)); // FS_AddGameDirectory(const char *path, const char *dir)
    }

    // Original function
    ASM_CALL(RETURN_VOID, 0x0043bd30);
}

#endif // COD2X_WIN32



bool iwd_firstTime = true;

// Is called everytime FS system is restarted (every map change)
void FS_RegisterDvars() {
    ASM_CALL(RETURN_VOID, ADDR(0x00425110, 0x080a2c3c));

    if (iwd_firstTime) {
        iwd_firstTime = false;

        // We moved loading cvars like dedicated, com_, etc here to be able to detect if its server or not
        // Com_RegisterDvars()
        ASM_CALL(RETURN_VOID, ADDR(0x00434040, 0x08061d90));

        // Cleanup old CoD2x IWD files that are no longer needed
        iwd_cleanupCoD2xIwdFiles();

        iwd_processZpamFiles();

        // Extract embedded iw_CoD2x_01.iwd file into main folder
        iwd_extractIwdFileToMain("iw_CoD2x_01", EMBEDDED_FILE_SYMBOLS(iw_CoD2x_01_iwd));
    }
}

/** Called every frame on frame start. */
void iwd_frame() {
    #if COD2X_WIN32
        static int iwd_clientStateLast = -1;

        // Player disconnects (from the server, demo, etc)
        if (clientState != iwd_clientStateLast && clientState == CLIENT_STATE_DISCONNECTED) {

            // Fix crash when ui_joinGametype points to gametype that does not exists because of mod change
            if (Dvar_GetInt("ui_joinGametype") != 0)
                Dvar_SetInt(Dvar_GetDvarByName("ui_joinGametype"), 0);
        }

        iwd_clientStateLast = clientState;
    #endif
}


/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void iwd_init() {
    com_writeConfig = Dvar_RegisterBool("com_writeConfig", true, DVAR_ARCHIVE);
}


/** Called before the entry point is called. Used to patch the memory. */
void iwd_patch() {
    // Nop out the call to Com_RegisterDvars
    // Its called in FS_RegisterDvars so we can detect if its server or not
    patch_nop(ADDR(0x0043455d, 0x0806212e), 5); 

    patch_call(ADDR(0x00425284, 0x080a2dfc), (unsigned int)&FS_RegisterDvars);

    patch_call(ADDR(0x0043bb47, 0x080654fa), (unsigned int)&WL(FS_iwIwd_Win32, FS_iwIwd_Linux));  
    patch_call(ADDR(0x00455455, 0x0808ff2c), (unsigned int)&WL(FS_iwIwd_Win32, FS_iwIwd_Linux)); // 
    patch_call(ADDR(0x0045ab96, 0x08094fce), (unsigned int)&WL(FS_iwIwd_Win32, FS_iwIwd_Linux)); // SVC_Status
    patch_call(ADDR(0x0045b2e5, 0x08095822), (unsigned int)&WL(FS_iwIwd_Win32, FS_iwIwd_Linux)); // SVC_Info

    #if COD2X_WIN32
    patch_call(0x00424869, (unsigned int)Sys_ListFiles);
    patch_call(0x00413e4e, (unsigned int)CL_InitDownloads);
    patch_call(0x0040d5fb, (unsigned int)CL_Disconnect_CMD_disconnect);
    patch_call(0x004220fc, (unsigned int)FS_BuildOSPath_Internal_FS_Write); // FS_BuildOSPath_Internal in FS_FOpenFileWrite
    patch_call(0x00422283, (unsigned int)FS_BuildOSPath_Internal_FS_Write); // FS_BuildOSPath_Internal in FS_FOpenFileAppend
    patch_call(0x00422a2a, (unsigned int)FS_BuildOSPath_Internal_FS_Read); // FS_BuildOSPath_Internal in FS_FOpenFileRead
    patch_call(0x00422b2e, (unsigned int)FS_BuildOSPath_Internal_FS_Read); // FS_BuildOSPath_Internal in FS_FOpenFileRead
    patch_call(0x004229db, (unsigned int)FS_BuildOSPath_Internal_FS_Read); // FS_BuildOSPath_Internal in FS_FOpenFileRead
    patch_call(0x004227ec, (unsigned int)FS_BuildOSPath_Internal_FS_Read); // FS_BuildOSPath_Internal in FS_FOpenFileRead
    patch_call(0x0042572f, (unsigned int)FS_AddCommands); // in FS_Startup

    patch_call(0x00434b6a, (unsigned int)Com_WriteConfigToFile); // in Com_WriteConfigOnChange
    #endif
}