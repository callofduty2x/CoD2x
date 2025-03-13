#include <windows.h>
#include <stdio.h>
#include "../shared/cod2.h"

/**
 * @brief Sets the CPU affinity for the current process.
 * 
 * This function sets the CPU affinity mask for the current process, allowing it 
 * to run on all available CPU cores. The system information is retrieved using 
 * `GetSystemInfo` to determine the number of processors, and then the affinity 
 * mask is set using `SetProcessAffinityMask`.
 * 
 * @return 0 on success. If there is an error setting the affinity, the error will 
 *         be printed via `Com_Printf`.
 */
int set_affinity()
{
    // Get the system's CPU information
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);

    // Create a bitmask for all available CPUs
    DWORD_PTR mask = (1 << sysInfo.dwNumberOfProcessors) - 1;

    // Attempt to set the process affinity mask
    if (SetProcessAffinityMask(GetCurrentProcess(), mask)) {
        Com_Printf("[CPU Affinity] Affinity set for all available CPU cores.\n");
    } else {
        Com_Printf("[CPU Affinity] Error setting affinity: %lu\n", GetLastError());
    }

    return 0;
}
