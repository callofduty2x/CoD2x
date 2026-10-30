#include "timer.h"

#include <windows.h>

#include "../shared/cod2_common.h"
#include "../shared/cod2_dvars.h"

// When enabled, the finest system timer resolution (max 0.5ms) is requested and Windows 11 is told
// to not ignore it. The FPS limiter in Com_Frame waits on timeGetTime(), so its accuracy depends on
// the timer resolution. Without this, stable 250 FPS on Windows 11 needed tools like ISLC.
dvar_t* com_timerResolution = NULL;

// Values are in 100ns units
#define TIMER_RESOLUTION_LIMIT 5000 // 0.5ms

// Not defined in older MinGW headers
#define TIMER_PROCESS_POWER_THROTTLING                       4 // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling
#define TIMER_POWER_THROTTLING_CURRENT_VERSION               1
#define TIMER_POWER_THROTTLING_EXECUTION_SPEED               0x1
#define TIMER_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION       0x4

typedef struct {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} timer_powerThrottlingState_t;

typedef LONG (NTAPI *NtQueryTimerResolution_t)(PULONG MinimumResolution, PULONG MaximumResolution, PULONG CurrentResolution);
typedef LONG (NTAPI *NtSetTimerResolution_t)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
typedef BOOL (WINAPI *SetProcessInformation_t)(HANDLE hProcess, int ProcessInformationClass, LPVOID ProcessInformation, DWORD ProcessInformationSize);


/**
 * Windows 11 may ignore timer resolution requests of a process when it decides to power-throttle it.
 * Opt out of it (and of EcoQoS), so the timer resolution requested below is always honored.
 */
static void timer_disablePowerThrottling() {
    // Available since Windows 8
    SetProcessInformation_t pSetProcessInformation = (SetProcessInformation_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
    if (pSetProcessInformation == NULL) {
        return;
    }

    timer_powerThrottlingState_t state;
    state.Version = TIMER_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = TIMER_POWER_THROTTLING_EXECUTION_SPEED | TIMER_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0; // 0 = throttling is disabled for the flags in ControlMask

    if (pSetProcessInformation(GetCurrentProcess(), TIMER_PROCESS_POWER_THROTTLING, &state, sizeof(state))) {
        Com_Printf("Timer: power throttling disabled.\n");
        return;
    }

    // IGNORE_TIMER_RESOLUTION is supported only since Windows 11, try again without it
    state.ControlMask = TIMER_POWER_THROTTLING_EXECUTION_SPEED;
    if (pSetProcessInformation(GetCurrentProcess(), TIMER_PROCESS_POWER_THROTTLING, &state, sizeof(state))) {
        Com_Printf("Timer: power throttling disabled (execution speed only).\n");
    } else {
        Com_Printf("Timer: error disabling power throttling: %lu\n", GetLastError());
    }
}

/**
 * Request the finest timer resolution supported by the system, but not finer than 0.5ms.
 * The game itself requests only 1ms via timeBeginPeriod(1).
 */
static void timer_setResolution() {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll == NULL) {
        return;
    }
    NtQueryTimerResolution_t pNtQueryTimerResolution = (NtQueryTimerResolution_t)GetProcAddress(ntdll, "NtQueryTimerResolution");
    NtSetTimerResolution_t pNtSetTimerResolution = (NtSetTimerResolution_t)GetProcAddress(ntdll, "NtSetTimerResolution");
    if (pNtQueryTimerResolution == NULL || pNtSetTimerResolution == NULL) {
        return;
    }

    ULONG minimum, maximum, current;
    if (pNtQueryTimerResolution(&minimum, &maximum, &current) != 0) {
        Com_Printf("Timer: error querying timer resolution.\n");
        return;
    }

    // 'maximum' is the finest (smallest) resolution
    ULONG desired = maximum < TIMER_RESOLUTION_LIMIT ? TIMER_RESOLUTION_LIMIT : maximum;

    if (pNtSetTimerResolution(desired, TRUE, &current) != 0) {
        Com_Printf("Timer: error setting timer resolution to %.4fms\n", desired / 10000.0);
        return;
    }

    Com_Printf("Timer: resolution set to %.4fms (supported %.4fms - %.4fms).\n", current / 10000.0, maximum / 10000.0, minimum / 10000.0);
}

/** Called only once on game start before common initialization. Used to initialize variables, cvars, etc. */
void timer_init() {
    com_timerResolution = Dvar_RegisterBool("com_timerResolution", true, (dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));

    if (com_timerResolution->value.boolean) {
        timer_disablePowerThrottling();
        timer_setResolution();
    }
}
