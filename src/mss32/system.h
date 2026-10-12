#ifndef SYSTEM_H
#define SYSTEM_H

extern char SYS_WIN_BUILD[64];
extern char SYS_WIN_EDITION[64];
extern char SYS_WIN_DISPLAY_VERSION[32];
extern char SYS_WINE_VERSION[64];
extern char SYS_WINE_BUILD[64];
extern char SYS_VERSION_INFO[256];
extern char SYS_VM_NAME[64];
extern char SYS_COMPATIBILITY_FLAGS[64];
extern char SYS_LANGUAGE_CODE[8];
extern char SYS_VIRTUALIZATION[128];
extern bool SYS_IS_ELEVATED;

void system_getInfo();

#endif