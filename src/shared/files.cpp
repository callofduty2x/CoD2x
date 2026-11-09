#include "files.h"
#include "shared.h"

#if COD2X_WIN32
    #include <windows.h>
#endif
#if COD2X_LINUX
    #include <sys/file.h>   // flock
    #include <fcntl.h>      // open
    #include <unistd.h>     // close
#endif
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <time.h>


#if COD2X_WIN32
    FileLock file_lockFolder(const std::string& dirPath, const char* name) {
        // Mutex name can't contain backslashes, use hash of the folder path
        uint32_t hash = 2166136261u;
        for (char c : dirPath) {
            hash = (hash ^ (uint8_t)tolower((unsigned char)c)) * 16777619u;
        }
        char mutexName[64];
        snprintf(mutexName, sizeof(mutexName), "Global\\CoD2x_%s_%08x", name, hash);

        HANDLE mutex = CreateMutexA(NULL, FALSE, mutexName);
        if (!mutex)
            return FILE_LOCK_NONE;

        DWORD wait = WaitForSingleObject(mutex, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(mutex);
            return FILE_LOCK_NONE;
        }
        return (FileLock)mutex;
    }

    void file_unlockFolder(FileLock lock) {
        ReleaseMutex((HANDLE)lock);
        CloseHandle((HANDLE)lock);
    }
#endif
#if COD2X_LINUX
    FileLock file_lockFolder(const std::string& dirPath, const char* name) {
        (void)name; // each feature locks its own folder

        int fd = open(dirPath.c_str(), O_RDONLY);
        if (fd < 0)
            return FILE_LOCK_NONE;

        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            close(fd);
            return FILE_LOCK_NONE;
        }
        return fd;
    }

    void file_unlockFolder(FileLock lock) {
        close((int)lock); // also releases the lock
    }
#endif


std::string file_getStamp(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return "";
    return std::to_string((long long)st.st_mtime) + ":" + std::to_string((long long)st.st_size);
}


void file_removeOld(const char* directory, const char* prefix, const char* suffix, int maxAgeSeconds) {
    DIR* dir = opendir(directory);
    if (!dir)
        return;

    size_t prefixLen = strlen(prefix);
    size_t suffixLen = strlen(suffix);
    time_t now = time(NULL);

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        const char* name = ent->d_name;
        size_t len = strlen(name);
        if (len < prefixLen + suffixLen || strncmp(name, prefix, prefixLen) != 0 || strcmp(name + len - suffixLen, suffix) != 0)
            continue;

        std::string path = std::string(directory) + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && now > st.st_mtime && now - st.st_mtime > maxAgeSeconds)
            remove(path.c_str());
    }
    closedir(dir);
}
