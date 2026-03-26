#ifndef COD2_FILE_H
#define COD2_FILE_H

#include "shared.h"
#include "assembly.h"

#define MAX_QPATH           64      // max length of a quake game pathname
#define MAX_OSPATH          256     // max length of a filesystem pathname

#define fs_homePath (*((dvar_t**)( ADDR(0x00b1e7e0, 0x0849fd44) )))
#define fs_gamedir ((char*)(ADDR(0x00b1a4a8, 0x08141080)))

// Read file in path
inline int FS_ReadFile(const char* path, void** buffer) {
    int ret;
    ASM_CALL(RETURN(ret), ADDR(0x00423240, 0x080a0a9c), 2, PUSH(path), PUSH(buffer));
    return ret;
}

// Write file in location fs_homepath + fs_game + filename
inline bool FS_WriteFile(const char* filename, const char* buffer, size_t size) {
    int ret;
    ASM_CALL(RETURN(ret), ADDR(0x00423340, 0x080a0ba8), 3, PUSH(filename), PUSH(buffer), PUSH(size));
    return ret;
}

// Delete file in location fs_homepath + fs_game + filename
inline bool FS_Delete(const char *filename)
{
    int ret;
	ASM_CALL(RETURN(ret), ADDR(0x00422e00, 0x080a03dc), WL(0, 1), WL(EAX, PUSH)(filename));
    return ret;
}

inline int FS_FileExists(const char *filename)
{
    int ret;
    ASM_CALL(RETURN(ret), ADDR(0x00421ea0, 0x080a0352), 1, PUSH(filename));
    return ret;
}

inline bool FS_FreeFile(void* buffer) {
    int ret;
    ASM_CALL(RETURN(ret), ADDR(0x004232f0, 0x0809ece0), 1, PUSH(buffer));
    return ret;
}

inline int FS_FilenameCompare(const char *s1, const char *s2)
{
    int ret;
	ASM_CALL(RETURN(ret), ADDR(0x00422300, 0x0809f32c), WL(0, 2), WL(EDX, PUSH)(s1), WL(EAX, PUSH)(s2));
    return ret;
}

inline void FS_Restart(int checksumFeed) {
    ASM_CALL(RETURN_VOID, ADDR(0x00425920, 0x080a337a), 1, PUSH(checksumFeed));
}

// 00421d30    int32_t FS_BuildOSPath(char* ospath @ eax, char* qpath @ ecx, void* game @ edx, char* base) 
inline void FS_BuildOSPath(char* ospath, const char* qpath, void* game, const char* base) {
    WL(
        ASM_CALL(RETURN_VOID, 0x00421d30, 1, EAX(ospath), ECX(qpath), EDX(game), PUSH(base)),
        ASM_CALL(RETURN_VOID, 0x0809eab0, 4, PUSH(ospath), PUSH(qpath), PUSH(game), PUSH(base))
    );
}

#endif