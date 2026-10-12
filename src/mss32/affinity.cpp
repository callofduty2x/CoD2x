#include "affinity.h"

#include <windows.h>

#include "../shared/cod2_common.h"
#include "../shared/cod2_dvars.h"

// When enabled, the process affinity is set to use all available CPU cores.
dvar_t* com_autoaffinity = NULL;


static void affinity_setAllCores() {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);

    // Build a mask with one bit per core. Guard against shifting by the full width
    // of DWORD_PTR (undefined behaviour) on machines with >= 32 logical processors.
    DWORD_PTR mask;
    if (sysInfo.dwNumberOfProcessors >= sizeof(DWORD_PTR) * 8) {
        mask = ~(DWORD_PTR)0;
    } else {
        mask = ((DWORD_PTR)1 << sysInfo.dwNumberOfProcessors) - 1;
    }

    if (SetProcessAffinityMask(GetCurrentProcess(), mask)) {
        Com_Printf("CPU Affinity: set to all %lu cores (mask 0x%08X).\n", sysInfo.dwNumberOfProcessors, mask);
    } else {
        Com_Printf("CPU Affinity: error setting affinity: %lu\n", GetLastError());
    }
}

/** Called only once on game start before common initialization. Used to initialize variables, cvars, etc. */
void affinity_init() {
    com_autoaffinity = Dvar_RegisterBool("com_autoaffinity", true, (dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));

    if (com_autoaffinity->value.boolean) {
        affinity_setAllCores();
    }
}
