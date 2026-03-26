#include "hook.h"

#include <sys/mman.h> // mprotect

#include "shared.h"
#include "../shared/iwd.h"
#include "../shared/cod2_common.h"
#include "../shared/common.h"
#include "../shared/weapons.h"
#include "../shared/server.h"
#include "../shared/dvar.h"
#include "../shared/game.h"
#include "../shared/animation.h"
#include "../shared/gsc.h"
#include "../shared/match.h"
#include "updater.h"


/**
 * Com_Frame
 * Is called in the main loop every frame.
 */
void __cdecl hook_Com_Frame() {

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x080626f4);

    gsc_frame();
    match_frame();
    iwd_frame();
}


/**
 * SV_Init
 *  - Is called in Com_Init after inicialization of:
 *    - common cvars (dedicated, net, version, com_maxfps, developer, timescale, shortversion, net_*...)
 *    - commands (exec, quit, wait, bind, ...)
 *  - Original function registers cvars like sv_*
 *  - Network is not initialized yet!
 */
void hook_SV_Init() {

    // Shared & Server
    common_init();
    weapons_init();
    server_init();
    dvar_init();
    updater_init();
    game_init();
    animation_init();
    match_init();
    iwd_init();

    ASM_CALL(RETURN_VOID, 0x08093adc);
}


/**
 * Com_Init
 * Is called in main function when the game is started. Is called only once on game start.
 */
void __cdecl hook_Com_Init(char* cmdline) {

    Com_Printf("Command line: '%s'\n", cmdline);
    
    // Call the original function
    ASM_CALL(RETURN_VOID, 0x080620c0, 1, PUSH(cmdline));

    gsc_init();
    updater_checkForUpdate(); // depends on dedicated and network system
    common_printInfo();
}


/**
 * Patch the cod2_lnxded.so executable
 */
bool hook_patch() {

    // allow to write in executable memory
	mprotect((void *)0x08048000, 0x134000, PROT_READ | PROT_WRITE | PROT_EXEC);


    // Patch function calls
    patch_call(0x0806233d, (unsigned int)hook_Com_Init);
    patch_call(0x080622ca, (unsigned int)hook_SV_Init);
    patch_call(0x0806281a, (unsigned int)hook_Com_Frame);


    weapons_patch();
    common_patch();
    server_patch();
    game_patch();
    dvar_patch();
    updater_patch();
    animation_patch();
    gsc_patch();
    match_patch();
    iwd_patch();

    return true;
}