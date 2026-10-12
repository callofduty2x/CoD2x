#include "module_detection.h"

#include <windows.h>
#include <tlhelp32.h>
#include <string.h>
#include <cstdio>

#include "../shared/logger.h"

#define MODULE_DETECTION_DELAYED_SCAN_MS 5000

enum ModuleDetectionType {
    MODULE_DETECTION_INCOMPATIBLE,  // known software that causes crashes/issues
    MODULE_DETECTION_CHEAT,         // cheat tools — generic error, no details
};

struct ModuleDetectionKnownModule {
    const char* moduleName;
    ModuleDetectionType type;
    const char* displayName;  // used for INCOMPATIBLE only
    const char* advice;       // used for INCOMPATIBLE only
};

static const ModuleDetectionKnownModule module_detection_knownModules[] = {
    // Incompatible software
    {
        "RTSSHooks.dll",
        MODULE_DETECTION_INCOMPATIBLE,
        "RivaTuner Statistics Server (RTSS / MSI Afterburner)",
        "Please turn off RTSS and MSI Afterburner before launching the game."
    },
    {
        "RTSSVHooksLoader.dll",
        MODULE_DETECTION_INCOMPATIBLE,
        "RivaTuner Statistics Server (RTSS / MSI Afterburner)",
        "Please turn off RTSS and MSI Afterburner before launching the game."
    },
    {
        "xfire_toucan_46139.dll",
        MODULE_DETECTION_INCOMPATIBLE,
        "Xfire",
        "Please turn off Xfire before launching the game."
    },

    // Very simple cheat detection
    // Not meant to be serious anticheat, just a very basic detection to prevent users from using known cheats
    // If you are hacker, patch here :)

    // Cheats
    { "CoD2.dll",                MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD 2.dll",               MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2 InfHook.dll",        MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2Chams Glas.dll",      MODULE_DETECTION_CHEAT, NULL, NULL },
    { "PhunKeh.dll",             MODULE_DETECTION_CHEAT, NULL, NULL },

    // Aimbot / triggerbot
    { "CoD2 AimBot.dll",         MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2Aimbot.dll",          MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2_Aimbot.dll",         MODULE_DETECTION_CHEAT, NULL, NULL },
    { "AimBot.dll",              MODULE_DETECTION_CHEAT, NULL, NULL },
    { "aimbot.dll",              MODULE_DETECTION_CHEAT, NULL, NULL },
    { "TriggerBot.dll",          MODULE_DETECTION_CHEAT, NULL, NULL },
    { "triggerbot.dll",          MODULE_DETECTION_CHEAT, NULL, NULL },
    { "AutoShoot.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "AutoAim.dll",             MODULE_DETECTION_CHEAT, NULL, NULL },
    { "iaimbot.dll",             MODULE_DETECTION_CHEAT, NULL, NULL },

    // Wallhack / ESP / chams
    { "CoD2 WallHack.dll",       MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2WallHack.dll",        MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2_WallHack.dll",       MODULE_DETECTION_CHEAT, NULL, NULL },
    { "WallHack.dll",            MODULE_DETECTION_CHEAT, NULL, NULL },
    { "wallhack.dll",            MODULE_DETECTION_CHEAT, NULL, NULL },
    { "wh.dll",                  MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2 ESP.dll",            MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2ESP.dll",             MODULE_DETECTION_CHEAT, NULL, NULL },
    { "esp.dll",                 MODULE_DETECTION_CHEAT, NULL, NULL },
    { "ESP.dll",                 MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2Chams.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "chams.dll",               MODULE_DETECTION_CHEAT, NULL, NULL },

    // Multihack / trainer / all-in-one
    { "CoD2 MultiHack.dll",      MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2MultiHack.dll",       MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2Multi.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2 Trainer.dll",        MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2Trainer.dll",         MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2Hack.dll",            MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2 Hack.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2Cheat.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "omnihack.dll",            MODULE_DETECTION_CHEAT, NULL, NULL },
    { "multihack.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "hack.dll",                MODULE_DETECTION_CHEAT, NULL, NULL },
    { "hax.dll",                 MODULE_DETECTION_CHEAT, NULL, NULL },
    { "cheat.dll",               MODULE_DETECTION_CHEAT, NULL, NULL },
    { "trainer.dll",             MODULE_DETECTION_CHEAT, NULL, NULL },

    // Speed / movement hacks
    { "CoD2 SpeedHack.dll",      MODULE_DETECTION_CHEAT, NULL, NULL },
    { "SpeedHack.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "speedhack.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "BunnyHop.dll",            MODULE_DETECTION_CHEAT, NULL, NULL },
    { "bunnyhop.dll",            MODULE_DETECTION_CHEAT, NULL, NULL },
    { "bhop.dll",                MODULE_DETECTION_CHEAT, NULL, NULL },
    { "SpinBot.dll",             MODULE_DETECTION_CHEAT, NULL, NULL },
    { "spinbot.dll",             MODULE_DETECTION_CHEAT, NULL, NULL },
    { "SuperJump.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "noclip.dll",              MODULE_DETECTION_CHEAT, NULL, NULL },

    // No recoil / no spread
    { "NoRecoil.dll",            MODULE_DETECTION_CHEAT, NULL, NULL },
    { "norecoil.dll",            MODULE_DETECTION_CHEAT, NULL, NULL },
    { "NoSpread.dll",            MODULE_DETECTION_CHEAT, NULL, NULL },
    { "nospread.dll",            MODULE_DETECTION_CHEAT, NULL, NULL },
    { "nospread_cod2.dll",       MODULE_DETECTION_CHEAT, NULL, NULL },
    { "RapidFire.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "rapidfire.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },

    // Radar / minimap hacks
    { "CoD2Radar.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2 Radar.dll",          MODULE_DETECTION_CHEAT, NULL, NULL },

    // D3D / OpenGL hook-based cheats
    { "d3dhook.dll",             MODULE_DETECTION_CHEAT, NULL, NULL },
    { "d3d_hook.dll",            MODULE_DETECTION_CHEAT, NULL, NULL },
    { "d3d9hack.dll",            MODULE_DETECTION_CHEAT, NULL, NULL },

    // Godmode / infinite ammo
    { "GodMode.dll",                    MODULE_DETECTION_CHEAT, NULL, NULL },
    { "godmode.dll",                    MODULE_DETECTION_CHEAT, NULL, NULL },
    { "InfiniteAmmo.dll",               MODULE_DETECTION_CHEAT, NULL, NULL },
    { "infiniteammo.dll",               MODULE_DETECTION_CHEAT, NULL, NULL },

    // CoD2-adapted names from known CoD-series cheat tools
    { "CoD2 Aim.dll",                   MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2bot.dll",                    MODULE_DETECTION_CHEAT, NULL, NULL },
    { "cod2bot.dll",                    MODULE_DETECTION_CHEAT, NULL, NULL },
    { "cod2Bot.dll",                    MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2D3D.dll",                    MODULE_DETECTION_CHEAT, NULL, NULL },
    { "cod2Hook.dll",                   MODULE_DETECTION_CHEAT, NULL, NULL },
    { "COD2HOOK.dll",                   MODULE_DETECTION_CHEAT, NULL, NULL },
    { "COD2HOOK1.3.dll",                MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CoD2multihack.dll",              MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Cod2multihack.dll",              MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Cod 2 multi hack.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Cod 2 1.3 multi hack.dll",       MODULE_DETECTION_CHEAT, NULL, NULL },
    { "cod2vision.dll",                 MODULE_DETECTION_CHEAT, NULL, NULL },
    { "cod2WH.dll",                     MODULE_DETECTION_CHEAT, NULL, NULL },
    { "cod2_espz.dll",                  MODULE_DETECTION_CHEAT, NULL, NULL },
    { "COD2_HOOK.dll",                  MODULE_DETECTION_CHEAT, NULL, NULL },
    { "BliksCoD2.dll",                  MODULE_DETECTION_CHEAT, NULL, NULL },
    { "BliksCoD2 2.0.dll",              MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Daves_CoD2_D3D_Hack.dll",        MODULE_DETECTION_CHEAT, NULL, NULL },
    { "jCod2.dll",                      MODULE_DETECTION_CHEAT, NULL, NULL },
    { "oshicod2v2.dll",                 MODULE_DETECTION_CHEAT, NULL, NULL },
    { "oshicod2v3.dll",                 MODULE_DETECTION_CHEAT, NULL, NULL },
    { "oshicod2v5.dll",                 MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Public cod2bot.dll",             MODULE_DETECTION_CHEAT, NULL, NULL },
    { "qtcod2.dll",                     MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Salt3r-Cod2RadarEnemies.dll",    MODULE_DETECTION_CHEAT, NULL, NULL },
    { "SchoolHackCoD2.dll",             MODULE_DETECTION_CHEAT, NULL, NULL },
    { "SyC CoD2BoT.dll",               MODULE_DETECTION_CHEAT, NULL, NULL },
    { "UH-Cod2.dll",                    MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Vossys_Cod2.dll",                MODULE_DETECTION_CHEAT, NULL, NULL },

    // Generic DLL injectors and hack bases (game-agnostic, commonly injected)
    { "500mg 1.0.0.dll",                MODULE_DETECTION_CHEAT, NULL, NULL },
    { "BigTex.dll",                     MODULE_DETECTION_CHEAT, NULL, NULL },
    { "BlankSS.dll",                    MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Buffalo.dll",                    MODULE_DETECTION_CHEAT, NULL, NULL },
    { "CodeDemonD3Dbase.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Codehook.dll",                   MODULE_DETECTION_CHEAT, NULL, NULL },
    { "codhck.dll",                     MODULE_DETECTION_CHEAT, NULL, NULL },
    { "ColorModel.dll",                 MODULE_DETECTION_CHEAT, NULL, NULL },
    { "D3DReleasebyWieter20.dll",       MODULE_DETECTION_CHEAT, NULL, NULL },
    { "D3D_cod-hacks.3dn.ru.dll",       MODULE_DETECTION_CHEAT, NULL, NULL },
    { "DBEPut.dll",                     MODULE_DETECTION_CHEAT, NULL, NULL },
    { "decafCoffee.dll",                MODULE_DETECTION_CHEAT, NULL, NULL },
    { "deceptivegames.dll",             MODULE_DETECTION_CHEAT, NULL, NULL },
    { "ellu.dll",                       MODULE_DETECTION_CHEAT, NULL, NULL },
    { "EvilHookv1.dll",                 MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Evolution.dll",                  MODULE_DETECTION_CHEAT, NULL, NULL },
    { "FisH4.dll",                      MODULE_DETECTION_CHEAT, NULL, NULL },
    { "GameHacks.dll",                  MODULE_DETECTION_CHEAT, NULL, NULL },
    { "h3he.dll",                       MODULE_DETECTION_CHEAT, NULL, NULL },
    { "h3heV2.dll",                     MODULE_DETECTION_CHEAT, NULL, NULL },
    { "iBase.dll",                      MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Injection_Utility.dll",          MODULE_DETECTION_CHEAT, NULL, NULL },
    { "lightHook.dll",                  MODULE_DETECTION_CHEAT, NULL, NULL },
    { "lolpenis.dll",                   MODULE_DETECTION_CHEAT, NULL, NULL },
    { "n7chams08.dll",                  MODULE_DETECTION_CHEAT, NULL, NULL },
    { "n7chams09.dll",                  MODULE_DETECTION_CHEAT, NULL, NULL },
    { "ObsidainHook.dll",               MODULE_DETECTION_CHEAT, NULL, NULL },
    { "ObsidianHook.dll",               MODULE_DETECTION_CHEAT, NULL, NULL },
    { "PBSS.dll",                       MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Robbin237's Hack V4.dll",        MODULE_DETECTION_CHEAT, NULL, NULL },
    { "selfHOOK.dll",                   MODULE_DETECTION_CHEAT, NULL, NULL },
    { "SiBot.dll",                      MODULE_DETECTION_CHEAT, NULL, NULL },
    { "SimpleNameTags.dll",             MODULE_DETECTION_CHEAT, NULL, NULL },
    { "sirhookcod.dll",                 MODULE_DETECTION_CHEAT, NULL, NULL },
    { "syc cod.dll",                    MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Symphony_hook.dll",              MODULE_DETECTION_CHEAT, NULL, NULL },
    { "the hack.dll",                   MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Thec0re.dll",                    MODULE_DETECTION_CHEAT, NULL, NULL },
    { "tobias.dll",                     MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Uamb.dll",                       MODULE_DETECTION_CHEAT, NULL, NULL },
    { "UC-forum Release.dll",           MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Ucreleasebywieter20.dll",        MODULE_DETECTION_CHEAT, NULL, NULL },
    { "utility_public.dll",             MODULE_DETECTION_CHEAT, NULL, NULL },
    { "u_pbss.dll",                     MODULE_DETECTION_CHEAT, NULL, NULL },
    { "VtableD3DBase.dll",              MODULE_DETECTION_CHEAT, NULL, NULL },
    { "xfire.dll",                      MODULE_DETECTION_CHEAT, NULL, NULL },
    { "Y-Hack v1.dll",                  MODULE_DETECTION_CHEAT, NULL, NULL },
    { "[EncoRe]CvarUnlocker.dll",       MODULE_DETECTION_CHEAT, NULL, NULL },
    { "[undetected] ESP Boxes.dll",     MODULE_DETECTION_CHEAT, NULL, NULL },
};

static bool module_detection_initialized = false;
static bool module_detection_violationDetected = false;
static bool module_detection_incompatibleWarned = false;
static bool module_detection_delayedScanDone = false;
static DWORD module_detection_delayedScanTime = 0;

static const ModuleDetectionKnownModule* module_detection_findKnownModule(const char* moduleName) {
    for (size_t i = 0; i < sizeof(module_detection_knownModules) / sizeof(module_detection_knownModules[0]); ++i) {
        if (_stricmp(moduleName, module_detection_knownModules[i].moduleName) == 0)
            return &module_detection_knownModules[i];
    }

    return NULL;
}

static void module_detection_triggerViolation(const char* moduleName, const ModuleDetectionKnownModule* known) {
    if (known->type == MODULE_DETECTION_CHEAT) {
        if (module_detection_violationDetected)
            return;
        module_detection_violationDetected = true;

        //logger_add("Cheat module detected: %s", moduleName);

        MessageBoxA(
            NULL,
            "A cheat module was detected, the game will now exit.",
            "Cheat software detected",
            MB_ICONERROR | MB_OK | MB_TOPMOST
        );
        ExitProcess(EXIT_FAILURE);
    } else {
        if (module_detection_incompatibleWarned)
            return;
        module_detection_incompatibleWarned = true;

        //logger_add("Incompatible module detected: %s", moduleName);

        char msg[1024];
        snprintf(msg, sizeof(msg),
            "%s is loaded and may cause crashes or other issues with Call of Duty 2.\n\n%s",
            known->displayName,
            known->advice
        );
        MessageBoxA(NULL, msg, "Incompatible software detected", MB_ICONWARNING | MB_OK | MB_TOPMOST);
    }
}

static void module_detection_scanModules() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return;

    MODULEENTRY32 me = {};
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            const ModuleDetectionKnownModule* known = module_detection_findKnownModule(me.szModule);
            if (known) {
                module_detection_triggerViolation(me.szModule, known);
                // CHEAT calls ExitProcess; INCOMPATIBLE only warns — keep scanning for cheats
                if (known->type == MODULE_DETECTION_CHEAT)
                    break;
            }
        } while (Module32Next(snap, &me));
    }

    CloseHandle(snap);
}

void module_detection_init() {
    module_detection_initialized = true;
    module_detection_violationDetected = false;
    module_detection_incompatibleWarned = false;
    module_detection_delayedScanDone = false;
    module_detection_delayedScanTime = GetTickCount() + MODULE_DETECTION_DELAYED_SCAN_MS;

    module_detection_scanModules();
}

void module_detection_frame() {
    if (!module_detection_initialized || module_detection_delayedScanDone)
        return;

    DWORD now = GetTickCount();
    if ((LONG)(now - module_detection_delayedScanTime) < 0)
        return;

    module_detection_delayedScanDone = true;
    module_detection_scanModules();
}