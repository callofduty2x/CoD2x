#include "competitive.h"

#include <windows.h>

#include "shared.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_client.h"


int competitive_clientStateLast = 0;

extern dvar_t* g_competitive;


void competitive_cmd_wait() {

    bool block = g_competitive->value.boolean;

    // Prevent blocking wait command untill connnected to the server
    if (COD2X_WIN32 && (clientState < CLIENT_STATE_PRIMED || demo_isPlaying)) {
        block = false;
    }

    if (block) {
        Com_Printf("Wait command is disabled by the server via competitive settings.\n");
        return;
    }
    ASM_CALL(RETURN_VOID, 0x00420a80);
}

/** Called every frame on frame start. */
void competitive_frame() {

    // Competitive settings - cvar that controls all other limits via one cvar
    if (g_competitive->modified) {
        g_competitive->modified = false;

        // If demo is playing, ignore competitive settings
        // The cvar will be controled by MOD (like zPAM) on the server, we don't want to limit things when replaying the demo
        if (demo_isPlaying) {
            Dvar_SetBool(g_competitive, false);
        }

        dvar_t* com_maxFps = Dvar_GetDvarByName("com_maxFps");
        dvar_t* rate = Dvar_GetDvarByName("rate");
        dvar_t* snaps = Dvar_GetDvarByName("snaps");
        dvar_t* cl_maxpackets = Dvar_GetDvarByName("cl_maxpackets");
        dvar_t* sc_shadows = Dvar_GetDvarByName("sc_enable");
        dvar_t* fx_sort = Dvar_GetDvarByName("fx_sort");
        dvar_t* mss_q3fs = Dvar_GetDvarByName("mss_q3fs");


        // Apply competitive settings
        if (g_competitive->value.boolean) {
            
            // FPS: set the upper and lower limits
            com_maxFps->limits.integer.min = 125;
            com_maxFps->limits.integer.max = 250;
            if (com_maxFps->value.integer < 125 || com_maxFps->value.integer > 250) {
                Dvar_SetInt(com_maxFps, 250); // it will also set the latched value
            }

            // Rate    
            rate->limits.integer.min = 25000;
            if (rate->value.integer != 25000) {
                Dvar_SetInt(rate, 25000);
            }
            // Snaps
            snaps->limits.integer.min = 40;
            if (snaps->value.integer != 40) {
                Dvar_SetInt(snaps, 40);
            }
            // cl_maxpackets
            cl_maxpackets->limits.integer.min = 125;
            if (cl_maxpackets->value.integer != 125) {
                Dvar_SetInt(cl_maxpackets, 125);
            }
            // Shadows          
            sc_shadows->type = DVAR_TYPE_INT;
            sc_shadows->limits.integer.min = 0;
            sc_shadows->limits.integer.max = 0;
            if (sc_shadows->value.integer != 0) {
                Dvar_SetInt(sc_shadows, 0);
            }
            // Fx_sort
            fx_sort->type = DVAR_TYPE_INT;
            fx_sort->limits.integer.min = 1;
            fx_sort->limits.integer.max = 1;
            if (fx_sort->value.integer != 1) {
                Dvar_SetInt(fx_sort, 1);
            }
            // mss_q3fs
            mss_q3fs->type = DVAR_TYPE_INT;
            mss_q3fs->limits.integer.min = 1;
            mss_q3fs->limits.integer.max = 1;
            if (mss_q3fs->value.integer != 1) {
                Dvar_SetInt(mss_q3fs, 1);
            }

        // Restore the default limits
        } else {

            // FPS
            com_maxFps->limits.integer.min = 0;
            com_maxFps->limits.integer.max = 1000;
            // Rate
            rate->limits.integer.min = 1000;
            // Snaps
            snaps->limits.integer.min = 1;
            // cl_maxpackets
            cl_maxpackets->limits.integer.min = 10;
            // Shadows
            sc_shadows->type = DVAR_TYPE_BOOL;
            sc_shadows->limits.integer.min = 0;
            // Fx_sort
            fx_sort->type = DVAR_TYPE_BOOL;
            fx_sort->limits.integer.min = 0;
            // mss_q3fs
            mss_q3fs->type = DVAR_TYPE_BOOL;
            mss_q3fs->limits.integer.min = 0;

        }
    }

    // Connections state changed
    if (clientState != competitive_clientStateLast) {
        //SHOW_ERROR("Client state changed from %d to %d", clientStateLast, clientState);
        competitive_clientStateLast = clientState;

        // If player is not connected to a server, reset the limit
        if (clientState == CLIENT_STATE_DISCONNECTED) 
        {
            if (g_competitive->value.boolean)
                Dvar_SetBool(g_competitive, false);
        }
    }
}


/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void competitive_init() {
    competitive_clientStateLast = clientState;
}


/** Called before the entry point is called. Used to patch the memory. */
void competitive_patch() {
    patch_int32(0x004344f9 + 1, (unsigned int)competitive_cmd_wait); // Cmd_AddCommand("wait", cmd_wait); in Com_Init
}