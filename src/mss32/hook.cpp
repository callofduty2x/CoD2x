#include "hook.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "shared.h"
#include "exception.h"
#include "freeze.h"
#include "window.h"
#include "rinput.h"
#include "fps.h"
#include "game.h"
#include "updater.h"
#include "../shared/common.h"
#include "../shared/server.h"
#include "hwid.h"


HMODULE hModule;
unsigned int gfx_module_addr;


/**
 * Patch the function that loads gfx_d3d_mp_x86_s.dll
 * Is called only is dedicated = 0
 */
int __cdecl hook_gfxDll() {
    // Call the original function
	int ret = ((int (__cdecl *)())0x00464e80)();

    // Get address of gfx_d3d_mp_x86_s.dll module stored at 0x00d53e80
    gfx_module_addr = (unsigned int)(*(HMODULE*)0x00d53e80);
    if (gfx_module_addr == 0) {
        SHOW_ERROR("Failed to read module handle of gfx_d3d_mp_x86_s.dll.");
        ExitProcess(EXIT_FAILURE);
    }

    ///////////////////////////////////////////////////////////////////
    // Patch gfx_d3d_mp_x86_s.dll
    ///////////////////////////////////////////////////////////////////

    window_hook_rendered();

    return ret;
}


/**
 * Com_Init
 * Is called in main function when the game is started. Is called only once on game start.
 */
void __cdecl hook_Com_Init(char* cmdline) {

    exception_init();
    freeze_init();
    window_init();
    rinput_init();
    fps_init();
    game_init();
    server_init();
    hwid_init();

    // Call the original function
	((void (__cdecl *)(char*))0x00434460)(cmdline);

    common_init();
}


/**
 * Com_Frame
 * Is called in the main loop every frame.
 */
void __cdecl hook_Com_Frame() {

    freeze_frame();
    fps_frame();
    game_frame();

    // Call the original function
	((void (__cdecl *)())0x00434f70)();
}




/**
 * Patch the CoD2MP_s.exe executable
 */
bool hook_patch() {
    hModule = GetModuleHandle(NULL);
    if (hModule == NULL) {
        SHOW_ERROR("Failed to get module handle of current process.");
        return FALSE;
    }

    ///////////////////////////////////////////////////////////////////
    // Patch CoD2MP_s.exe
    ///////////////////////////////////////////////////////////////////

    // Patch Com_Init
    patch_call(0x00434a66, (unsigned int)hook_Com_Init);
    // Patch Com_Frame
    patch_call(0x00435282, (unsigned int)hook_Com_Frame);
    // Patch function that loads gfx_d3d_mp_x86_s.dll
    patch_call(0x004102b5, (unsigned int)hook_gfxDll);

    // Patch client side
    window_patch();
    rinput_patch();
    fps_patch();
    game_patch();
    updater_patch();

    // Patch server side
    common_patch();
    server_patch();


    
    // Patch black screen / long loading on game startup
    // Caused by Windows Mixer loading
    // For some reason function mixerGetLineInfoA() returns a lot of connections, causing it to loop for a long time
    // Disable whole function registering microphone functionalities by placing return at the beginning
    patch_byte(0x004b8dd0, 0xC3);


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


    // Improve error message when too many dvars are registered
    patch_string_ptr(0x00437e0f + 1, "Error while registering cvar '%s'.\nUnable to create more than %i dvars.\n\n"
        "There is too many cvars in your config!\nClean your config from unused dvars and try again.\n\n"
        "Normal config should contains no more than 400 lines of dvars. Compare your config with a default one to find the differences.");

    return TRUE;
}