/*
 * Sharing of language files - collects the language files of the installs of the players, so CoD2x can add the
 * languages it does not have yet (texts and fonts of the localized versions).
 *
 * Once per game start, on a background thread:
 *   1. Scan: the localized IWDs in main (localized_*.iwd, not the CoD2x one) are read - only their zip directory, no
 *      file is unpacked. The language files of each IWD are listed with the CRC32 and size the zip has for them:
 *        localizedstrings/<name>.str, fonts/<path>, materials/fonts/<name>, images/gamefonts<name>.iwi
 *   2. Manifest: the list (IWD names, file paths, CRC32, sizes - no content) with the game language and the language of
 *      Windows is sent to the master server (a translation in the english IWDs, like the czech 1.0 version, is told
 *      apart by its files and the language of Windows). A manifest that was already delivered (its CRC32 in the registry, COD2X_LANGUAGE_FILES) is not
 *      sent again.
 *   3. The master server answers with the files it does not have yet ("<crc32>-<size>").
 *   4. Upload: only those files are sent, in one zip - its entries are copied from the IWDs as they are (compressed),
 *      named "<iwd name>/<path>".
 *
 * Cvar cl_shareLanguageFiles 0 turns it off (archived, on by default). Nothing else from the install is sent.
 */

// http_client.h pulls in mongoose -> winsock2.h, which must be included before windows.h.
#include "../shared/http_client.h"
#include "../shared/json.h"
#include "../shared/error.h"

#include "language.h"
#include "language_share.h"

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "shared.h"
#include "main.h"
#include "version.h"
#include "logger.h"
#include "system.h"
#include "../shared/cod2_dvars.h"

#define LANGUAGE_SHARE_URL          "https://master.cod2x.me/api/telemetry/language-files"
//#define LANGUAGE_SHARE_URL        "http://localhost:8080/api/telemetry/language-files"
#define LANGUAGE_SHARE_TEST         0       // 1 = also in debug builds

#define LANGUAGE_SHARE_REG_PATH     "SOFTWARE\\Activision\\Call of Duty 2" // created by registry.cpp
#define LANGUAGE_SHARE_REG_VALUE    "COD2X_LANGUAGE_FILES"                 // CRC32 of the last delivered manifest
#define LANGUAGE_SHARE_MAX_FILE     (4 * 1024 * 1024)   // bigger files are not listed (gamefonts.iwi has 700 KB)
#define LANGUAGE_SHARE_MAX_UPLOAD   (8 * 1024 * 1024)   // max size of one upload, more files go into more uploads

dvar_t* cl_shareLanguageFiles = NULL;

static bool languageShare_started = false;


// ======================================================================================================
// Zip directory of an IWD
// ======================================================================================================

// Language file of an IWD
struct languageShareFile_t {
    std::string iwd;            // IWD file name, "localized_polish_iw00.iwd"
    std::string path;           // path in the IWD as the game reads it (the name of the local header), "fonts/polish/bigFont"
    uint32_t crc32;
    uint32_t size;              // uncompressed
    uint32_t compressedSize;
    uint16_t method;            // 0 stored, 8 deflate
    uint32_t localHeaderOffset;
    char key[24];               // "<crc32>-<size>"
};

static uint16_t LanguageShare_U16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t LanguageShare_U32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

static bool LanguageShare_StartsWith(const std::string& s, const char* prefix) {
    return _strnicmp(s.c_str(), prefix, strlen(prefix)) == 0;
}

static bool LanguageShare_EndsWith(const std::string& s, const char* suffix) {
    size_t len = strlen(suffix);
    return s.size() >= len && _stricmp(s.c_str() + s.size() - len, suffix) == 0;
}

// Language files: texts, fonts, font materials and font images
static bool LanguageShare_IsLanguageFile(const std::string& path) {
    return (LanguageShare_StartsWith(path, "localizedstrings/") && LanguageShare_EndsWith(path, ".str")) ||
           LanguageShare_StartsWith(path, "fonts/") ||
           LanguageShare_StartsWith(path, "materials/fonts/") ||
           (LanguageShare_StartsWith(path, "images/gamefonts") && LanguageShare_EndsWith(path, ".iwi"));
}

// Reads the language files of an IWD from its zip directory. The path is the name of the local header, like the
// game reads it - some IWDs have another name in the directory ("fonts^bigFont").
static void LanguageShare_ReadIwd(const char* filePath, const char* iwdName, std::vector<languageShareFile_t>& files) {
    FILE* f = fopen(filePath, "rb");
    if (!f)
        return;

    std::vector<uint8_t> dir;
    do {
        // End of central directory: the last 22 bytes + a comment of max 64 KB
        if (fseek(f, 0, SEEK_END) != 0) break;
        long fileSize = ftell(f);
        if (fileSize < 22) break;
        long tailSize = std::min<long>(fileSize, 22 + 0xFFFF);
        std::vector<uint8_t> tail(tailSize);
        if (fseek(f, fileSize - tailSize, SEEK_SET) != 0 || fread(tail.data(), 1, tailSize, f) != (size_t)tailSize) break;

        long eocd = -1;
        for (long i = tailSize - 22; i >= 0; i--) {
            if (LanguageShare_U32(&tail[i]) == 0x06054b50) { eocd = i; break; }
        }
        if (eocd < 0) break;

        uint16_t count = LanguageShare_U16(&tail[eocd + 10]);
        uint32_t dirSize = LanguageShare_U32(&tail[eocd + 12]);
        uint32_t dirOffset = LanguageShare_U32(&tail[eocd + 16]);
        if (dirSize > 16 * 1024 * 1024 || (long)dirOffset + (long)dirSize > fileSize) break;

        dir.resize(dirSize);
        if (fseek(f, dirOffset, SEEK_SET) != 0 || fread(dir.data(), 1, dirSize, f) != dirSize) break;

        size_t pos = 0;
        for (uint16_t n = 0; n < count && pos + 46 <= dir.size(); n++) {
            const uint8_t* e = &dir[pos];
            if (LanguageShare_U32(e) != 0x02014b50) break;
            uint16_t method = LanguageShare_U16(e + 10);
            uint32_t crc = LanguageShare_U32(e + 16);
            uint32_t compressedSize = LanguageShare_U32(e + 20);
            uint32_t size = LanguageShare_U32(e + 24);
            uint16_t nameLen = LanguageShare_U16(e + 28);
            uint16_t extraLen = LanguageShare_U16(e + 30);
            uint16_t commentLen = LanguageShare_U16(e + 32);
            uint32_t localOffset = LanguageShare_U32(e + 42);
            if (pos + 46 + nameLen > dir.size()) break;
            std::string dirName((const char*)e + 46, nameLen);
            pos += 46 + nameLen + extraLen + commentLen;

            // Quick filter with the directory name (a broken name still starts with the folder name)
            if (!LanguageShare_StartsWith(dirName, "localizedstrings") && !LanguageShare_StartsWith(dirName, "fonts") &&
                !LanguageShare_StartsWith(dirName, "materials") && !LanguageShare_StartsWith(dirName, "images"))
                continue;
            if (size == 0 || size > LANGUAGE_SHARE_MAX_FILE || (method != 0 && method != 8))
                continue;

            // The name of the local header
            uint8_t local[30];
            if (fseek(f, localOffset, SEEK_SET) != 0 || fread(local, 1, 30, f) != 30 || LanguageShare_U32(local) != 0x04034b50)
                continue;
            uint16_t localNameLen = LanguageShare_U16(local + 26);
            std::string path(localNameLen, '\0');
            if (localNameLen == 0 || fread(&path[0], 1, localNameLen, f) != localNameLen)
                continue;
            std::replace(path.begin(), path.end(), '\\', '/');

            if (!LanguageShare_IsLanguageFile(path))
                continue;

            languageShareFile_t file;
            file.iwd = iwdName;
            file.path = path;
            file.crc32 = crc;
            file.size = size;
            file.compressedSize = compressedSize;
            file.method = method;
            file.localHeaderOffset = localOffset;
            snprintf(file.key, sizeof(file.key), "%08x-%u", crc, size);
            files.push_back(file);
        }
    } while (0);

    fclose(f);
}

// Language files of the localized IWDs in main (sorted by IWD and path, so the manifest of an install is always the same)
static std::vector<languageShareFile_t> LanguageShare_Scan(const std::string& mainPath, std::vector<std::string>& iwds) {
    std::vector<languageShareFile_t> files;

    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA((mainPath + "\\localized_*.iwd").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return files;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        std::string name = data.cFileName;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("cod2x") != std::string::npos) // the CoD2x language IWD
            continue;
        iwds.push_back(name);
    } while (FindNextFileA(find, &data));
    FindClose(find);

    std::sort(iwds.begin(), iwds.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    for (const std::string& iwd : iwds)
        LanguageShare_ReadIwd((mainPath + "\\" + iwd).c_str(), iwd.c_str(), files);

    return files;
}


// ======================================================================================================
// Upload zip
// ======================================================================================================

static void LanguageShare_Put16(std::string& out, uint16_t v) { out += (char)(v & 0xFF); out += (char)(v >> 8); }
static void LanguageShare_Put32(std::string& out, uint32_t v) { LanguageShare_Put16(out, (uint16_t)(v & 0xFFFF)); LanguageShare_Put16(out, (uint16_t)(v >> 16)); }

// Compressed data of a file as it is in the IWD
static bool LanguageShare_ReadCompressed(const std::string& mainPath, const languageShareFile_t& file, std::string& out) {
    FILE* f = fopen((mainPath + "\\" + file.iwd).c_str(), "rb");
    if (!f)
        return false;
    bool ok = false;
    uint8_t local[30];
    if (fseek(f, file.localHeaderOffset, SEEK_SET) == 0 && fread(local, 1, 30, f) == 30 && LanguageShare_U32(local) == 0x04034b50) {
        long dataOffset = (long)file.localHeaderOffset + 30 + LanguageShare_U16(local + 26) + LanguageShare_U16(local + 28);
        out.resize(file.compressedSize);
        ok = fseek(f, dataOffset, SEEK_SET) == 0 && (file.compressedSize == 0 || fread(&out[0], 1, file.compressedSize, f) == file.compressedSize);
    }
    fclose(f);
    return ok;
}

// Zip with the files, entries named "<iwd>/<path>", the data copied as it is (stored or deflate)
static std::string LanguageShare_BuildZip(const std::string& mainPath, const std::vector<const languageShareFile_t*>& files) {
    std::string zip, dir;
    uint16_t count = 0;
    for (const languageShareFile_t* file : files) {
        std::string data;
        if (!LanguageShare_ReadCompressed(mainPath, *file, data))
            continue;
        std::string name = file->iwd + "/" + file->path;
        uint32_t offset = (uint32_t)zip.size();

        LanguageShare_Put32(zip, 0x04034b50);
        LanguageShare_Put16(zip, 20);                   // version needed
        LanguageShare_Put16(zip, 0);                    // flags
        LanguageShare_Put16(zip, file->method);
        LanguageShare_Put32(zip, 0);                    // time, date
        LanguageShare_Put32(zip, file->crc32);
        LanguageShare_Put32(zip, file->compressedSize);
        LanguageShare_Put32(zip, file->size);
        LanguageShare_Put16(zip, (uint16_t)name.size());
        LanguageShare_Put16(zip, 0);                    // extra
        zip += name;
        zip += data;

        LanguageShare_Put32(dir, 0x02014b50);
        LanguageShare_Put16(dir, 20);                   // version made by
        LanguageShare_Put16(dir, 20);                   // version needed
        LanguageShare_Put16(dir, 0);                    // flags
        LanguageShare_Put16(dir, file->method);
        LanguageShare_Put32(dir, 0);                    // time, date
        LanguageShare_Put32(dir, file->crc32);
        LanguageShare_Put32(dir, file->compressedSize);
        LanguageShare_Put32(dir, file->size);
        LanguageShare_Put16(dir, (uint16_t)name.size());
        LanguageShare_Put16(dir, 0);                    // extra
        LanguageShare_Put16(dir, 0);                    // comment
        LanguageShare_Put16(dir, 0);                    // disk
        LanguageShare_Put16(dir, 0);                    // internal attributes
        LanguageShare_Put32(dir, 0);                    // external attributes
        LanguageShare_Put32(dir, offset);
        dir += name;
        count++;
    }
    if (count == 0)
        return "";

    uint32_t dirOffset = (uint32_t)zip.size();
    zip += dir;
    LanguageShare_Put32(zip, 0x06054b50);
    LanguageShare_Put16(zip, 0);
    LanguageShare_Put16(zip, 0);
    LanguageShare_Put16(zip, count);
    LanguageShare_Put16(zip, count);
    LanguageShare_Put32(zip, (uint32_t)dir.size());
    LanguageShare_Put32(zip, dirOffset);
    LanguageShare_Put16(zip, 0);
    return zip;
}


// ======================================================================================================
// Sending
// ======================================================================================================

struct languageShareInfo_t {
    std::string mainPath;       // fs_basepath/main
    std::string language;       // loc_language name
    std::string systemLanguage; // language of Windows ("cs") - tells a translation in the english IWDs
    std::string installId;
    std::string hwid2;
};

static std::string LanguageShare_BuildManifest(const languageShareInfo_t& info, const std::vector<std::string>& iwds, const std::vector<languageShareFile_t>& files) {
    std::string json = "{";
    json += "\"version\":\"" + json_escape_string(APP_VERSION) + "\",";
    json += "\"installId\":\"" + json_escape_string(info.installId) + "\",";
    json += "\"hwid2\":\"" + json_escape_string(info.hwid2) + "\",";
    json += "\"language\":\"" + json_escape_string(info.language) + "\",";
    json += "\"systemLanguage\":\"" + json_escape_string(info.systemLanguage) + "\",";
    json += "\"iwds\":[";
    for (size_t i = 0; i < iwds.size(); i++)
        json += (i ? ",\"" : "\"") + json_escape_string(iwds[i]) + "\"";
    json += "],\"files\":[";
    for (size_t i = 0; i < files.size(); i++) {
        if (i) json += ",";
        json += "{\"iwd\":\"" + json_escape_string(files[i].iwd) + "\",\"path\":\"" + json_escape_string(files[i].path) +
                "\",\"key\":\"" + files[i].key + "\",\"size\":" + std::to_string(files[i].size) + "}";
    }
    json += "]}";
    return json;
}

// Synchronous POST, returns the HTTP status (0 = failed)
static int LanguageShare_Post(const std::string& url, const std::string& body, bool isJson, std::string& response) {
    HttpClient client;
    int status = 0;
    auto onDone = [&](const HttpClient::Response& res) { status = res.status; response = res.body; };
    auto onError = [&](const std::string& err) { logger_add("[LANGUAGE] %s: %s", url.c_str(), err.c_str()); };
    if (isJson)
        client.postJson(url.c_str(), body.c_str(), onDone, onError, 10000);
    else
        client.upload(url.c_str(), body.data(), body.size(), nullptr, onDone, onError, 60000);
    client.poll_max(isJson ? 15000 : 90000);
    return status;
}

// CRC32 of the last delivered manifest (hex), empty if none
static void LanguageShare_GetDigest(char* out, DWORD size) {
    out[0] = '\0';
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, LANGUAGE_SHARE_REG_PATH, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;
    DWORD type = 0;
    DWORD read = size - 1; // room for the terminator, a REG_SZ is not always terminated
    if (RegQueryValueExA(key, LANGUAGE_SHARE_REG_VALUE, NULL, &type, (LPBYTE)out, &read) != ERROR_SUCCESS || type != REG_SZ)
        read = 0;
    out[read] = '\0';
    RegCloseKey(key);
}

static void LanguageShare_SetDigest(const char* digest) {
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, LANGUAGE_SHARE_REG_PATH, 0, KEY_WRITE, &key) != ERROR_SUCCESS)
        return;
    RegSetValueExA(key, LANGUAGE_SHARE_REG_VALUE, 0, REG_SZ, (const BYTE*)digest, (DWORD)strlen(digest) + 1);
    RegCloseKey(key);
}

static void LanguageShare_Thread(languageShareInfo_t info) {
    std::vector<std::string> iwds;
    std::vector<languageShareFile_t> files = LanguageShare_Scan(info.mainPath, iwds);
    if (files.empty())
        return;

    std::string manifest = LanguageShare_BuildManifest(info, iwds, files);

    // The same manifest was already delivered
    char digest[16];
    snprintf(digest, sizeof(digest), "%08x", mg_crc32(0, manifest.data(), manifest.size()));
    char cached[16];
    LanguageShare_GetDigest(cached, sizeof(cached));
    if (strcmp(cached, digest) == 0)
        return;

    std::string response;
    int status = LanguageShare_Post(LANGUAGE_SHARE_URL, manifest, true, response);
    if (status < 200 || status >= 300) {
        logger_add("[LANGUAGE] Manifest of %d language files not sent (HTTP %d)", (int)files.size(), status);
        return;
    }

    // Files the master server wants - only the ones of the manifest, each once
    std::vector<std::string> wanted;
    json_get_str_array(response.c_str(), "$.wanted", &wanted);
    std::vector<const languageShareFile_t*> upload;
    for (const std::string& key : wanted) {
        for (const languageShareFile_t& file : files) {
            if (key == file.key) {
                upload.push_back(&file);
                break;
            }
        }
    }

    // Uploads, max LANGUAGE_SHARE_MAX_UPLOAD each
    bool ok = true;
    size_t uploaded = 0;
    for (size_t i = 0; i < upload.size() && ok;) {
        std::vector<const languageShareFile_t*> part;
        size_t partSize = 0;
        while (i < upload.size() && (part.empty() || partSize + upload[i]->compressedSize <= LANGUAGE_SHARE_MAX_UPLOAD)) {
            partSize += upload[i]->compressedSize;
            part.push_back(upload[i++]);
        }
        std::string zip = LanguageShare_BuildZip(info.mainPath, part);
        if (zip.empty())
            continue;
        status = LanguageShare_Post(std::string(LANGUAGE_SHARE_URL) + "/upload", zip, false, response);
        ok = status >= 200 && status < 300;
        if (ok)
            uploaded += part.size();
    }
    logger_add("[LANGUAGE] Manifest of %d language files sent, %d of %d wanted files uploaded", (int)files.size(), (int)uploaded, (int)upload.size());

    if (ok)
        LanguageShare_SetDigest(digest);
}


// ======================================================================================================

// Called every frame
void language_share_frame() {
    if (languageShare_started || !cl_shareLanguageFiles || !cl_shareLanguageFiles->value.boolean)
        return;
    languageShare_started = true;

    #if DEBUG && !LANGUAGE_SHARE_TEST
        return;
    #endif

    dvar_t* fs_basepath = Dvar_GetDvarByName("fs_basepath");
    dvar_t* loc_language = Dvar_GetDvarByName("loc_language");
    if (!fs_basepath)
        return;

    languageShareInfo_t info;
    ErrorPlatformInfo platform = error_getPlatformInfo();
    info.mainPath = std::string(fs_basepath->value.string) + "\\main";
    info.language = language_getName(loc_language ? loc_language->value.integer : 0);
    info.systemLanguage = SYS_LANGUAGE_CODE;
    info.installId = platform.installId;
    info.hwid2 = platform.hwid2;

    std::thread(LanguageShare_Thread, info).detach();
}

// Called after the main config is loaded
void language_share_init() {
    cl_shareLanguageFiles = Dvar_RegisterBool("cl_shareLanguageFiles", true, (enum dvarFlags_e)(DVAR_ARCHIVE));
}
