#include "hook.h"

#include <winsock2.h> // must be called before windows.h to fix warning about winsock2.h
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "shared.h"
#include "admin.h"
#include "registry.h"
#include "url_protocol.h"
#include "affinity.h"
#include "hotreload.h"
#include "exception.h"
#include "freeze.h"
#include "window.h"
#include "rinput.h"
#include "competitive.h"
#include "console.h"
#include "cgame.h"
#include "updater.h"
#include "hwid.h"
#include "radar.h"
#include "drawing.h"
#include "master_server.h"
#include "error.h"
#include "downloading.h"
#include "demo.h"
#include "vmix.h"
#include "debug.h"
#include "../shared/iwd.h"
#include "../shared/common.h"
#include "../shared/server.h"
#include "../shared/dvar.h"
#include "../shared/game.h"
#include "../shared/animation.h"
#include "../shared/cod2_dvars.h"
#include "../shared/gsc.h"
#include "../shared/match.h"
#include "../shared/weapons.h"

HMODULE hModule;
unsigned int gfx_module_addr;
bool hook_hotreload_init = false;


/**
 * CL_Frame
 * Is called in the main loop every frame after processing events for client
 */
void hook_CL_Frame() 
{
    int time;
    ASM( movr, time, "esi" );

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x0040f850, 0, ESI(time));
}

/**
 * Com_Frame
 * Is called in the main loop every frame.
 */
void hook_Com_Frame() 
{
    logger_add("Com_Frame starting...");

    #if DEBUG
        // First frame from newly loaded DLL, we need to initialize
        if (hook_hotreload_init) {
            hook_hotreload_init = false;
            
            hook_Com_Init("");

            hook_gfxDll();
        }

        // New DLL is requested to be loaded, unload the old one and load the new one
        if (hotreload_requested) {
            
            // Unloading
            hotreload_unload();
            debug_unload();
            common_unload();
            weapons_unload();
            radar_unload();
            demo_unload();

            hotreload_loadDLL();
            return;
        }
    #endif

    // Only for client
    if (dedicated->value.integer == 0) {
        affinity_frame();
        competitive_frame();
        cgame_frame();
    }

    // Shared & Server
    debug_frame();
    freeze_frame();
    updater_frame();
    hwid_frame();
    window_frame();
    gsc_frame();
    match_frame();
    registry_frame();      // called as last so other modules can handle version changes
    drawing_frame();
    iwd_frame();
    radar_frame();
    demo_frame();
    vmix_frame();

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x00434f70);

    logger_add("Com_Frame finished.");
}


/**
 * Patch the function that loads gfx_d3d_mp_x86_s.dll
 * Is called only is dedicated = 0
 */
int hook_gfxDll() {
    logger_add("Loading gfx_d3d_mp_x86_s.dll...");

    // Call the original function
	int ret = ((int ( *)())0x00464e80)();

    // Get address of gfx_d3d_mp_x86_s.dll module stored at 0x00d53e80
    gfx_module_addr = (unsigned int)(*(HMODULE*)0x00d53e80);
    if (gfx_module_addr == 0) {
        SHOW_ERROR("Failed to read module handle of gfx_d3d_mp_x86_s.dll.");
        ExitProcess(EXIT_FAILURE);
    }

    ///////////////////////////////////////////////////////////////////
    // Patch gfx_d3d_mp_x86_s.dll
    ///////////////////////////////////////////////////////////////////

    window_rendered();
    updater_renderer();

    // Fix LOD
    //r_lodScale = Dvar_RegisterFloat("r_lodScale", 1f, 1f, 4f, DVAR_CHEAT | DVAR_RENDERER);
    patch_float(gfx_module_addr + 0x000acb4 + 1, 1.0f); // r_lodScale default value,    originally 1.0f
    patch_float(gfx_module_addr + 0x000acaf + 1, 0.0f); // r_lodScale min value,        originally 1.0f
    patch_float(gfx_module_addr + 0x000acaa + 1, 1.0f); // r_lodScale max value,        originally 4.0f
    patch_int32(gfx_module_addr + 0x000aca5 + 1, (DVAR_ARCHIVE | DVAR_RENDERER)); // r_lodScale flags, originally DVAR_CHEAT | DVAR_RENDERER

    logger_add("File gfx_d3d_mp_x86_s.dll loaded successfully.");

    return ret;
}



/**
 * CL_Init
 *  - Is called in Com_Init after inicialization of:
 *    - common cvars (dedicated, net, version, com_maxfps, developer, timescale, shortversion, ...)
 *    - commands (exec, quit, wait, bind, ...)
 *    - network system
 *    - SV_Init
 *    - NET_Init
 *  - Is called only if dedicated = 0
 *  - Original function:
 *    - registers cvars like cl_*, cg_*, fx_*, sensitivity, name, ...
 *    - registers commands like vid_restart, connect, record, ...
 *    - calls auto-update server
 *    - init renderer (loads gfx_d3d_mp_x86_s.dll)
 */
void hook_CL_Init() {
    logger_add("CL_Init starting...");
    
    #if DEBUG
    hotreload_init();
    #endif

    // Client
    error_init();       // reporting errors and crashes
    hwid_init(); 
    window_init();      // depends on being called before gfx dll is loaded
    rinput_init();
    competitive_init();
    console_init();
    cgame_init();
    master_server_init();
    match_init_client();
    drawing_init();
    radar_init();
    demo_init();
    vmix_init();
    
    if (!DLL_HOTRELOAD) {
        ASM_CALL(RETURN_VOID, 0x00410a10);
    }

    logger_add("CL_Init finished.");
}


/**
 * SV_Init
 *  - Is called in Com_Init after inicialization of:
 *    - splash screen
 *    - parse command line arguments
 *    - common cvars (dedicated, net, version, com_maxfps, developer, timescale, shortversion, net_*, ...)
 *    - commands (exec, quit, wait, bind, ...)
 *    - file system initialization (loading iwd files, reading registry cdkey)
 *  - Original function registers cvars like sv_*
 *  - Network is not initialized yet!
 */
void hook_SV_Init() {
    logger_add("SV_Init starting...");

    // Shared & Server 
    freeze_init();
    weapons_init();
    common_init();
    server_init();
    dvar_init();
    updater_init();
    game_init();
    animation_init();
    match_init();
    iwd_init();

    if (!DLL_HOTRELOAD) {
        ASM_CALL(RETURN_VOID, 0x004596d0);
    }

    logger_add("SV_Init finished.");
}


/**
 * Com_Init
 * Is called in main function when the game is started. Is called only once on game start.
 */
void hook_Com_Init(const char* cmdline) {
    logger_add("Com_Init starting...");

    Com_Printf("Command line: '%s'\n", cmdline);

    exception_init();   // sending thru NET will work after Com_Init is called and updater is initialized
    debug_init();
    admin_init();      // Must be called before file system initialization
    registry_init();   // Must be called before cdkey registry reading
    url_protocol_init(); // Must be called before reading cmdline, so we can handle cod2x:// URL protocol
    gsc_init();

    // Call the original function
    if (!DLL_HOTRELOAD) {
        ASM_CALL(RETURN_VOID, 0x00434460, 1, PUSH(cmdline));
    } else {
        hook_SV_Init();
        if (dedicated->value.boolean == 0)
            hook_CL_Init();
    }

    affinity_init();
    common_printInfo();

    logger_add("Com_Init finished.");
}


/**
 * Patch the CoD2MP_s.exe executable
 */
bool hook_patch() {
    logger_init();
    logger_add("Patching CoD2MP_s.exe...");

    hModule = GetModuleHandle(NULL);
    if (hModule == NULL) {
        SHOW_ERROR("Failed to get module handle of current process.");
        return FALSE;
    }

    ///////////////////////////////////////////////////////////////////
    // Patch CoD2MP_s.exe
    ///////////////////////////////////////////////////////////////////

    // Patch function calls
    patch_call(0x00434a66, (unsigned int)hook_Com_Init);
    patch_call(0x00434860, (unsigned int)hook_SV_Init);
    patch_call(0x004348b4, (unsigned int)hook_CL_Init);
    patch_call(0x00435282, (unsigned int)hook_Com_Frame);
    patch_call(0x0043506a, (unsigned int)hook_CL_Frame);
    patch_call(0x004102b5, (unsigned int)hook_gfxDll);

    // Patch client side
    error_patch();
    freeze_patch();
    window_patch();
    rinput_patch();
    competitive_patch();
    console_patch();
    cgame_patch();
    updater_patch();
    master_server_patch();
    downloading_patch();
    drawing_patch();
    radar_patch();
    demo_patch();
    vmix_patch();

    weapons_patch();
    // Patch server side
    common_patch();
    server_patch();
    game_patch();
    dvar_patch();
    animation_patch();
    gsc_patch();
    iwd_patch();

    
    // Patch black screen / long loading on game startup
    // Caused by Windows Mixer loading
    // For some reason function mixerGetLineInfoA() returns a lot of connections, causing it to loop for a long time
    // Disable whole function registering microphone functionalities by placing return at the beginning
    patch_byte(0x004b8dd0, 0xC3);


    // Disable call to CL_VoicePacket, as voice chat is not working becaused the Windows Mixer fix above
    patch_nop(0x0040e94e, 5);


    // Turn off "Run in safe mode?" dialog
    // Showed when game crashes (file __CoD2MP_s is found)
    patch_nop(0x004664cd, 2);           // 7506 (jne 0x4664d5)  ->  9090 (nop nop)
    patch_jump(0x004664d3, 0x4664fc);   // 7e27 (jle 0x4664fc)  ->  eb27 (jmp 0x4664fc)


    // Turn off "Set Optimal Settings?" and "Recommended Settings Update" dialog
    patch_nop(0x004345c3, 5);           // e818f9ffff call sub_433ee0
    patch_nop(0x0042d1a7, 5);           // e8346d0000 call sub_433ee0


    // Change text in console -> CoD2 MP: 1.3>
    patch_string_ptr(0x004064c6 + 1, "CoD2x MP");
    patch_string_ptr(0x004064c1 + 1, APP_VERSION);
    patch_string_ptr(0x004064cb + 1, "%s: %s> ");



    // How many cvars to show in console ("Too many to show" info message)
    patch_byte(0x004065fb + 2, 60); // originally 24
    // Detailed dvar match, number of chars to show
    patch_byte(0x00406091 + 1, 40); // originally 24
    // Cvar match, number of chars to show
    patch_byte(0x00405c3c + 1, 40); // originally 24
    // Dvar value X offset in console
    patch_float(0x005c41a0, 340.0f); // originally 200.0f



    // Fix console not closing when fatal error occured for server
    patch_byte(0x0046853b, 0xeb); // je => jmp  0x74 => 0xeb


    // Hot-reloading in debug mode
    #if DEBUG
        // DLL was hot-reloaded, we need to call the init functions again
        if (DLL_HOTRELOAD) {
            hook_hotreload_init = true;
        }

        // Watch for mss32.dll change
        hotreload_watch_dll();
    #endif

    logger_add("Patched CoD2MP_s.exe successfully.");

    return TRUE;
}