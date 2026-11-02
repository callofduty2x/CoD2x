#ifndef FILES_H
#define FILES_H

#include <stdint.h>
#include <string>

// Helpers for instances of the game sharing one folder.

typedef intptr_t FileLock;
#define FILE_LOCK_NONE ((FileLock)-1)

// Locks the folder for this process, name separates locks of different features. Does not wait:
// returns FILE_LOCK_NONE if somebody else holds the lock. It is released by the system when the
// process ends, so a crashed server can't leave it locked.
FileLock file_lockFolder(const std::string& dirPath, const char* name);
void file_unlockFolder(FileLock lock);

// Text that changes when the file is replaced, empty if the file does not exist
std::string file_getStamp(const char* path);

// Removes "<directory>/<prefix>*<suffix>" not modified for maxAgeSeconds. A file in use by another
// instance is kept by the age, its process may run in another container and can't be looked up.
void file_removeOld(const char* directory, const char* prefix, const char* suffix, int maxAgeSeconds);

#endif
