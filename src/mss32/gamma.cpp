#include "gamma.h"

#include <windows.h>
#include <string.h>

#include "shared.h"
#include "registry.h"
#include "../shared/cod2_dvars.h"

#define r_fullscreen                (*((dvar_t**)0x00d77128))
#define win_hwnd                    (*((HWND*)0x00d7713c))

/*
 * Windowed gamma is controlled from frame start instead of directly from window messages.
 * window.cpp only calls gamma_requestSync() for events such as WM_MOVE or WM_ACTIVATE,
 * while gamma_restore() is reserved for immediate teardown paths such as deactivation,
 * window destroy, and crash handling.
 *
 * Original ramps are stored under HKLM\SOFTWARE\Activision\Call of Duty 2 using
 * monitor-specific values derived from the device name. Example for "\\.\DISPLAY1":
 * COD2X_GAMMA_5C5C2E5C444953504C415931_DEVICE
 * COD2X_GAMMA_5C5C2E5C444953504C415931_RAMP
 */

struct GammaMonitorState {
    HDC dc;
    char deviceName[32];
    WORD originalRamp[3][256];
    bool customRampApplied;
    bool originalRampSaved;
};

static GammaMonitorState gamma_monitor = {};
static float gamma_previous = 1.0f;
static bool gamma_runtimeReady = false;
static bool gamma_savedRecoveryComplete = false;
static bool gamma_syncRequested = false;
static bool gamma_applyWarningShown = false;
static bool gamma_persistenceWarningShown = false;


static void gamma_updateRequestedGamma()
{
    dvar_t* gammaCvar = Dvar_GetDvarByName("r_gamma");
    if (gammaCvar == NULL)
    {
        return;
    }

    if (gammaCvar->modified)
    {
        gammaCvar->modified = false;
    }

    float gamma = gammaCvar->value.decimal;
    if (gamma != gamma_previous)
    {
        gamma_previous = gamma;
        gamma_requestSync();
    }
}


static void gamma_warnPersistenceFailure()
{
    logger_add("Failed to persist original gamma ramp, gamma change was skipped");

    if (gamma_persistenceWarningShown)
        return;

    gamma_persistenceWarningShown = true;
    Com_Printf("CoD2x: ^3Could not save original monitor gamma for crash recovery. Gamma change was skipped to keep the display restorable.^7\n");
}


static void gamma_warnApplyFailure(const char* deviceName)
{
    char message[128] = { 0 };
    snprintf(message, sizeof(message), "Failed to apply gamma for %s", deviceName);
    logger_add(message);

    if (gamma_applyWarningShown)
        return;

    gamma_applyWarningShown = true;
    Com_Printf("CoD2x: ^3Could not apply monitor gamma. The display was left unchanged.^7\n");
}


static void gamma_createRamp(float gamma, WORD ramp[3][256])
{
    for (int i = 0; i < 256; i++)
    {
        double normalized = i / 255.0;
        double value = pow(normalized, 1.0 / gamma);
        int rampValue = (int)(value * 65535.0 + 0.5);
        if (rampValue > 65535)
            rampValue = 65535;
        ramp[0][i] = ramp[1][i] = ramp[2][i] = (WORD)rampValue;
    }
}


/* Marks gamma state as dirty so the next frame can re-evaluate the active monitor. */
void gamma_requestSync()
{
    gamma_syncRequested = true;
}


static void gamma_releaseMonitor()
{
    if (gamma_monitor.dc)
    {
        DeleteDC(gamma_monitor.dc);
        gamma_monitor.dc = NULL;
    }

    gamma_monitor.deviceName[0] = '\0';
    gamma_monitor.customRampApplied = false;
    gamma_monitor.originalRampSaved = false;
}


static bool gamma_getWindowMonitorDeviceName(HWND hWnd, char* deviceName, size_t deviceNameSize)
{
    if (hWnd == NULL || deviceName == NULL || deviceNameSize == 0)
    {
        return false;
    }

    HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEX mi;
    ZeroMemory(&mi, sizeof(MONITORINFOEX));
    mi.cbSize = sizeof(MONITORINFOEX);
    if (!GetMonitorInfo(hMonitor, &mi))
    {
        return false;
    }

    strncpy(deviceName, mi.szDevice, deviceNameSize - 1);
    deviceName[deviceNameSize - 1] = '\0';
    return true;
}


static bool gamma_restoreSavedRampForDevice(const char* deviceName)
{
    if (deviceName == NULL)
    {
        return false;
    }

    WORD savedRamp[3][256] = { 0 };
    if (!registry_gamma_getSaved(deviceName, savedRamp))
    {
        return false;
    }

    HDC restoreDC = CreateDC(NULL, deviceName, NULL, NULL);
    if (!restoreDC)
    {
        return false;
    }

    bool restored = SetDeviceGammaRamp(restoreDC, savedRamp) == TRUE;
    DeleteDC(restoreDC);

    if (!restored)
    {
        return false;
    }

    registry_gamma_clear(deviceName);

    if (gamma_monitor.dc && strcmp(gamma_monitor.deviceName, deviceName) == 0)
    {
        memcpy(gamma_monitor.originalRamp, savedRamp, sizeof(gamma_monitor.originalRamp));
        gamma_monitor.customRampApplied = false;
        gamma_monitor.originalRampSaved = false;
    }

    char message[128] = { 0 };
    snprintf(message, sizeof(message), "Restored saved gamma for %s", deviceName);
    logger_add(message);
    return true;
}


static bool gamma_restoreSavedRampCallback(const char* deviceName, const WORD ramp[3][256], void* context)
{
    struct GammaRestoreBatch {
        char restoredDevices[16][32];
        int restoredCount;
    };
    GammaRestoreBatch* batch = (GammaRestoreBatch*)context;

    HDC restoreDC = CreateDC(NULL, deviceName, NULL, NULL);
    if (!restoreDC)
    {
        return true;
    }

    bool restored = SetDeviceGammaRamp(restoreDC, (void*)ramp) == TRUE;
    DeleteDC(restoreDC);
    if (!restored)
    {
        return true;
    }

    if (batch != NULL && batch->restoredCount < 16)
    {
        strncpy(batch->restoredDevices[batch->restoredCount], deviceName, sizeof(batch->restoredDevices[batch->restoredCount]) - 1);
        batch->restoredDevices[batch->restoredCount][sizeof(batch->restoredDevices[batch->restoredCount]) - 1] = '\0';
        batch->restoredCount++;
    }

    if (gamma_monitor.dc && strcmp(gamma_monitor.deviceName, deviceName) == 0)
    {
        memcpy(gamma_monitor.originalRamp, ramp, sizeof(gamma_monitor.originalRamp));
        gamma_monitor.customRampApplied = false;
        gamma_monitor.originalRampSaved = false;
    }

    char message[128] = { 0 };
    snprintf(message, sizeof(message), "Restored saved gamma for %s", deviceName);
    logger_add(message);
    return true;
}


static void gamma_restoreAllSavedRamps()
{
    struct GammaRestoreBatch {
        char restoredDevices[16][32];
        int restoredCount;
    } batch = {};

    registry_gamma_forEachSaved(gamma_restoreSavedRampCallback, &batch);

    for (int i = 0; i < batch.restoredCount; ++i)
    {
        registry_gamma_clear(batch.restoredDevices[i]);
    }

    gamma_savedRecoveryComplete = true;
}


static bool gamma_prepareMonitorForWindowedGamma(const char* deviceName)
{
    gamma_monitor.dc = CreateDC(NULL, deviceName, NULL, NULL);
    if (!gamma_monitor.dc)
    {
        return false;
    }

    strncpy(gamma_monitor.deviceName, deviceName, sizeof(gamma_monitor.deviceName) - 1);
    gamma_monitor.deviceName[sizeof(gamma_monitor.deviceName) - 1] = '\0';

    gamma_monitor.originalRampSaved = registry_gamma_getSaved(deviceName, gamma_monitor.originalRamp);
    if (!gamma_monitor.originalRampSaved)
    {
        if (!GetDeviceGammaRamp(gamma_monitor.dc, gamma_monitor.originalRamp))
        {
            gamma_releaseMonitor();
            return false;
        }

        gamma_monitor.originalRampSaved = registry_gamma_save(deviceName, gamma_monitor.originalRamp);
        if (!gamma_monitor.originalRampSaved)
        {
            gamma_warnPersistenceFailure();
            gamma_releaseMonitor();
            return false;
        }
    }

    return true;
}


static bool gamma_applyToTrackedMonitor(float gamma)
{
    if (!gamma_monitor.dc)
    {
        return false;
    }

    WORD newRamp[3][256] = { 0 };
    gamma_createRamp(gamma, newRamp);
    if (!SetDeviceGammaRamp(gamma_monitor.dc, newRamp))
    {
        gamma_warnApplyFailure(gamma_monitor.deviceName);
        return false;
    }

    gamma_monitor.customRampApplied = true;
    return true;
}


static void gamma_syncWindowedMode()
{
    if (!gamma_runtimeReady)
    {
        return;
    }

    HWND hWnd = win_hwnd;
    dvar_t* gammaCvar = Dvar_GetDvarByName("r_gamma");
    if (gammaCvar == NULL)
    {
        return;
    }

    float gamma = gammaCvar->value.decimal;
    if (r_fullscreen->value.boolean || hWnd == NULL)
    {
        if (hWnd != NULL)
        {
            char currentDeviceName[32] = "";
            if (gamma_getWindowMonitorDeviceName(hWnd, currentDeviceName, sizeof(currentDeviceName)))
            {
                gamma_restoreSavedRampForDevice(currentDeviceName);
            }
        }

        gamma_restore();
        return;
    }

    char windowDeviceName[32] = "";
    if (!gamma_getWindowMonitorDeviceName(hWnd, windowDeviceName, sizeof(windowDeviceName)))
    {
        return;
    }

    if (gamma == 1.0f)
    {
        gamma_restoreSavedRampForDevice(windowDeviceName);
        gamma_restore();
        return;
    }

    if (gamma_monitor.dc && strcmp(gamma_monitor.deviceName, windowDeviceName) != 0)
    {
        gamma_restore();
    }

    if (!gamma_monitor.dc)
    {
        if (!gamma_prepareMonitorForWindowedGamma(windowDeviceName))
        {
            return;
        }
    }

    if (!gamma_applyToTrackedMonitor(gamma))
    {
        gamma_restore();
    }
}


/*
 * First-frame and per-frame entry point.
 * Restores any crash-leftover ramps once, then applies pending monitor or r_gamma changes.
 */
void gamma_onFrameStart()
{
    if (dedicated->value.integer != 0)
    {
        return;
    }

    if (!gamma_runtimeReady)
    {
        gamma_runtimeReady = true;

        if (!gamma_savedRecoveryComplete)
        {
            gamma_restoreAllSavedRamps();
        }

        gamma_requestSync();
    }

    gamma_updateRequestedGamma();

    if (!gamma_syncRequested)
    {
        return;
    }

    gamma_syncRequested = false;
    gamma_syncWindowedMode();
}


/* Restores the tracked monitor immediately and clears persisted backup data on success. */
void gamma_restore()
{
    bool displayMatchesOriginal = !gamma_monitor.customRampApplied;

    if (gamma_monitor.dc && gamma_monitor.customRampApplied)
    {
        displayMatchesOriginal = SetDeviceGammaRamp(gamma_monitor.dc, gamma_monitor.originalRamp) == TRUE;
    }

    if (displayMatchesOriginal && gamma_monitor.originalRampSaved)
    {
        registry_gamma_clear(gamma_monitor.deviceName);
    }

    gamma_syncRequested = false;
    gamma_releaseMonitor();
}