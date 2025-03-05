#include "common.h"
#include "shared.h"

#if COD2X_WIN32
    #include "../mss32/window.h"
    #include "../mss32/rinput.h"
    #include "../mss32/fps.h"
    #include "../mss32/game.h"
    #include "../mss32/discord.h"
    #include "../mss32/steam.h"
    #include "../mss32/screenshot.h"
    #include "../mss32/url_protocol.h"
    #include "../mss32/mbedtls.h"
#endif

#if COD2X_LINUX
    #include "../linux/updater.h"
#endif

#include "server.h"

//
// This file contains functions that:
//  - are used on both client and server side
//  - are used in windows and linux version
//

/**
 * Com_Init
 * Is called in main function when the game is started. Is called only once on game start.
 */
void __cdecl hook_Com_Init(char* cmdline) {

    Com_Printf("CMD: '%s'\n", cmdline);

    // Call the original function
	((void (__cdecl *)(char*))ADDR(0x00434460, 0x080620c0))(cmdline);

    Com_Printf("-----------------------------------\n");
    Com_Printf("CoD2x " APP_VERSION " loaded\n");
    Com_Printf("-----------------------------------\n");

    //start_discord_thread();
    screenshot_init();
    steam_init();
    //url_protocol_init();

    request_init();

    #if COD2X_LINUX
        updater_hook_Com_Init();
    #endif

    server_hook_init();
}



/**
 * Com_Init_Dvars
 * Is called in Com_Init to initialize dvars like dedicated, com_maxfps, developer, logfile, etc..
 * Is also called after all original dvars are registered
 */
void __cdecl hook_Com_Init_Dvars() {

    // Call the original function
	((void (__cdecl *)())ADDR(0x00434040, 0x08061d90))();

    #if COD2X_WIN32
        window_hook_init_cvars();

        rinput_hook_init_cvars();

        fps_hook_init_cvars();

        game_hook_init_cvars();
    #endif

    #if COD2X_LINUX
        updater_hook_Com_Init_Dvars();
    #endif

    server_hook_init_cvars();
}


/**
 * Com_Frame
 * Is called in the main loop every frame.
 */
void __cdecl hook_Com_Frame() {

    #if COD2X_WIN32
        fps_hook_frame();

        game_hook_frame();
    #endif

    // Call the original function
	((void (__cdecl *)())ADDR(0x00434f70, 0x080626f4))();
}





// Fix animation time from crouch to stand
// Need to be fixed both on client and server
// This function is called when g_cod2x cvar is changed on client side or always on server side
// https://github.com/voron00/CoD2rev_Server/blob/e788f339977d1d28980333fd0c0f18e40eafbc13/src/bgame/bg_animation_mp.cpp#L1717
void common_fix_clip_bug(bool enable) {

    // 1st view
    // prone to crouch  = 400ms
    // crouch to prone  = 400 total, 200ms first part going down
    // crouch to stand  = 200ms
    // stand to crouch  = 200ms

    if (enable) {
        patch_copy(ADDR(0x004f9956, 0x080d9e7a), (void*)"\x90\x90\x90\x90\x90\x90", 6); // 0ms
    } else {
        patch_copy(ADDR(0x004f9956, 0x080d9e7a), (void*)"\x81\xc2\x90\x01\x00\x00", 6); // 400ms original
    }
    
}




// Server side hooks
// The hooked functions are the same for both Windows and Linux
void common_hook()
{
    // Patch Com_Init
    patch_call(ADDR(0x00434a66, 0x0806233d), (unsigned int)hook_Com_Init);

    // Patch function called in Com_Init that initializes dvars like dedicated, com_maxfps, developer, logfile, etc..
    patch_call(ADDR(0x0043455d, 0x0806212e), (unsigned int)hook_Com_Init_Dvars);

    // Patch Com_Frame
    patch_call(ADDR(0x00435282, 0x0806281a), (unsigned int)hook_Com_Frame);



    // Print into console when the app is started -> "CoD2 MP 1.3 build win-x86 May  1 2006"
    patch_string_ptr(ADDR(0x00434467 + 1, 0x080620c6 + 4), __DATE__ " " __TIME__);          // originally win: "May  1 2006",  linux: "Jun 23 2006"
    patch_string_ptr(ADDR(0x0043446c + 1, 0x080620ce + 4), ADDR("win-x86", "linux-i386"));  // original
    patch_string_ptr(ADDR(0x00434471 + 1, 0x080620d6 + 4), APP_VERSION);             // originally "1.3"
    patch_string_ptr(ADDR(0x00434476 + 1, 0x080620de + 4), "CoD2 MP");                      // original
    patch_string_ptr(ADDR(0x0043447b + 1, 0x08062235 + 3), "%s %s build %s %s\n");          // original


    // Value of cvar /version   ->   "CoD2 MP 1.3 build pc_1.3_1_1 Mon May 01 2006 05:05:43PM linux-i386"
    patch_string_ptr(ADDR(0x004346de + 1, 0x08051e1e + 4), __DATE__ " " __TIME__);          // originally "Mon May 01 2006 05:05:43PM"
    patch_string_ptr(ADDR(0x004346e3 + 1, 0x08051e26 + 4), "by eyza");                      // originally "pc_1.3_1_1"
    patch_string_ptr(ADDR(0x004346f7 + 1, 0x08062219 + 4), ADDR("win-x86", "linux-i386"));  // original
    patch_string_ptr(ADDR(0x00434701 + 1, 0x08062225 + 4), APP_VERSION);             // originally "1.3"
    patch_string_ptr(ADDR(0x00434706 + 1, 0x0806222d + 4), "CoD2 MP");                      // original
    patch_string_ptr(ADDR(0x0043470b + 1, 0x08062235 + 3), "%s %s build %s %s");            // original


    // Value of cvar /shortversion   ->   "1.3"
    // Also visible in menu right bottom corner
    patch_string_ptr(ADDR(0x0043477c + 1, 0x08062281 + 4), APP_VERSION);             // originally "1.3"


    // Value of cvar /protocol   ->   "118"
    // Changing this cvar does not involve the protocol number used while connecting to server
    // It only affect server info when asking for /serverinfo or server notify master server
    #if COD2X_WIN32
        patch_byte(0x00459754 + 1, PROTOCOL_VERSION);
    #endif
    #if COD2X_LINUX
        patch_byte(0x08093b2c + 4, PROTOCOL_VERSION);
        patch_byte(0x08093b34 + 4, PROTOCOL_VERSION);
        patch_byte(0x08093b3c + 4, PROTOCOL_VERSION);
    #endif



    #if COD2X_WIN32
        window_hook();
        rinput_hook();
        fps_hook();
        game_hook();
    #endif
    #if COD2X_LINUX
        updater_hook();
    #endif

    // Hook server side functions
    server_hook();
}