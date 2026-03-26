#include "cgame.h"

#include <windows.h>

#include "shared.h"
#include "../shared/cod2_client.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_cmd.h"
#include "../shared/animation.h"



#define clientState                   (*((clientState_e*)0x00609fe0))
#define sv_cheats                     (*((dvar_t**)0x00c5c5cc))
#define cg_thirdperson                (*((dvar_t**)0x014b5bdc))
#define cg_thirdPersonAngle           (*((dvar_t**)0x0166e024))
#define cg_thirdPersonRange           (*((dvar_t**)0x0166baa0))

dvar_t* cg_thirdPersonMode;

extern dvar_t* g_cod2x;

int cgame_clientStateLast = -1;
bool cgame_demoPlayingLast = false;
int cgame_developerLast = 0;


void Cmd_Increase_Decrease() {
    const char* cmd = Cmd_Argv(0);
    
    if (Cmd_Argc() != 2) {
        Com_Printf("%s <variablename> : increase value\n", cmd);
        return;
    }

    int sign = 1;
    if (strcmp(cmd, "decrease") == 0) {
        sign = -1;
    }

    const char* dvarName = Cmd_Argv(1);
	dvar_t* dvar = Dvar_GetDvarByName(dvarName);

    if (dvar == NULL) {
        Com_Printf("%s not found\n", dvarName);
        return;
    }

    if (dvar->type == DVAR_TYPE_INT) {
        Dvar_SetInt(dvar, dvar->value.integer + sign);
    } else if (dvar->type == DVAR_TYPE_FLOAT) {
        Dvar_SetFloat(dvar, dvar->value.decimal + sign);
    } else {
        Com_Printf("%s is not an int or float\n", dvarName);
    }
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void cgame_init() {

    // Register USERINFO cvar that is automatically appended to the client's userinfo string sent to the server
    Dvar_RegisterInt("protocol_cod2x", APP_VERSION_PROTOCOL, APP_VERSION_PROTOCOL, APP_VERSION_PROTOCOL, (enum dvarFlags_e)(DVAR_USERINFO | DVAR_ROM));
    
    // Add another mode to thirdperson
    cg_thirdPersonMode = Dvar_RegisterInt("cg_thirdPersonMode", 0, 0, 1, (enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));

    Cmd_AddCommand("increase", Cmd_Increase_Decrease);
    Cmd_AddCommand("decrease", Cmd_Increase_Decrease);
}


/** Called every frame on frame start. */
void cgame_frame() {

    if (clientState != cgame_clientStateLast) {
        logger_add("Client state changed from %d:%s to %d:%s", cgame_clientStateLast, get_client_state_name(cgame_clientStateLast), clientState, get_client_state_name(clientState));
        Com_DPrintf("Client state changed from %d:%s to %d:%s\n", cgame_clientStateLast, get_client_state_name(cgame_clientStateLast), clientState, get_client_state_name(clientState));
    }


    // Player disconnected from the server, reset the cvar
    if (g_cod2x->value.integer > 0 && clientState != cgame_clientStateLast && clientState <= CLIENT_STATE_CONNECTED) {
        Dvar_SetInt(g_cod2x, 0);
        g_cod2x->modified = true;
    }
    // Player just connected to 1.3 server (g_cod2x == 0)
    // Set the cvar modified so the text is printed in the console again below
    if (g_cod2x->value.integer == 0 && clientState != cgame_clientStateLast && clientState == CLIENT_STATE_ACTIVE && cgame_clientStateLast <= CLIENT_STATE_PRIMED) {
        g_cod2x->modified = true;
    }
    // Cvar changed (by server, init or disconenct), apply the appropriate bug fixes
    if (g_cod2x->modified) {
        g_cod2x->modified = false;

        Com_Printf("---------------------------------------------------------------------------------\n");
        if (g_cod2x->value.integer == 0) {
            Com_Printf("CoD2x: Changes turned off, using legacy CoD2 1.3\n");
        } else {
            Com_Printf("CoD2x: Changes turned on, using changes according to server version 1.4.%d.x\n", g_cod2x->value.integer);          
            if (g_cod2x->value.integer != APP_VERSION_PROTOCOL)
                Com_Printf("CoD2x: ^3Server is running older version 1.4.%d.x, your version is %s\n", g_cod2x->value.integer, APP_VERSION);
        }
        Com_Printf("---------------------------------------------------------------------------------\n");

        // Fix animation time from crouch to stand since version 1.4.3.x
        animation_changeFix(g_cod2x->value.integer >= 3);
    }


    // Demo playback state changed
    if (demo_isPlaying != cgame_demoPlayingLast) {
        dvar_t* developer = Dvar_GetDvarByName("developer");

        // Demo playback started
        if (demo_isPlaying) {
            // Enable cheats and developer mode for demo playback
            Dvar_SetBool(sv_cheats, true);
            cgame_developerLast = developer->value.integer;
            Dvar_SetInt(developer, 2);

        // Demo playback stopped
        } else {
            Dvar_SetInt(developer, cgame_developerLast);
        }
    }
    cgame_demoPlayingLast = demo_isPlaying;


    cgame_clientStateLast = clientState;
}

// .text:004020D0 const char *__usercall CL_GetConfigString@<eax>(int idx@<eax>)
const char* CL_GetConfigString(int idx) {
    const char* ret;
    ASM_CALL(RETURN(ret), 0x004020d0, 0, EAX(idx));

    return ret;
}

void CG_RegisterItems() {
    if (strlen(CL_GetConfigString(8)) >= 0x108u) {
        Com_Error(ERR_DROP, "CG_RegisterItems: overflowed");
    }

    // Call the original function
    ((void(*)())0x004d5270)();
}



void CG_TraceCapsule(trace_t *result, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int skipNumber, int mask) {
    ASM_CALL(RETURN_VOID, 0x004de690, 7, PUSH(result), PUSH(start), PUSH(mins), PUSH(maxs), PUSH(end), PUSH(skipNumber), PUSH(mask));
}

void CG_OffsetThirdPersonView( void ) {

    // Use the original function if not connected to cod2x server, unless the player is in the new third person mode
    if (g_cod2x->value.integer < 3 && cg_thirdPersonMode->value.integer != 1) {
        // Call the original function
        ((void(*)())0x004ce890)();
        return;
    }

	vec3_t forward, right, up;
	vec3_t view;
	vec3_t focusAngles;
	trace_t trace;
	static vec3_t mins = { -4, -4, -4 };
	static vec3_t maxs = { 4, 4, 4 };
	vec3_t focusPoint;
	float focusDist;

    cg.refdef.vieworg[2] += cg.predictedPlayerState.viewHeightCurrent;

	VectorCopy( cg.refdefViewAngles, focusAngles );

    if (cg.predictedPlayerState.pm_type > 5) {
        focusAngles[1] = cg.predictedPlayerState.stats[1];
        cg.refdefViewAngles[1] = cg.predictedPlayerState.stats[1];
    }

	if ( focusAngles[PITCH] > 45 ) {
		focusAngles[PITCH] = 45;        // don't go too far overhead
	}

	AngleVectors( focusAngles, forward, NULL, NULL );
	VectorMA( cg.refdef.vieworg, 512, forward, focusPoint );

	VectorCopy( cg.refdef.vieworg, view );
	view[2] += 8;
	cg.refdefViewAngles[PITCH] *= 0.5;
    cg.refdefViewAngles[YAW] -= cg_thirdPersonAngle->value.decimal;
        
	AngleVectors( cg.refdefViewAngles, forward, right, up );
	VectorMA( view, -cg_thirdPersonRange->value.decimal, forward, view );

    // CoD2x: New mode that rotates around head without collision
    if (cg_thirdPersonMode->value.integer == 1) {
        VectorCopy( view, cg.refdef.vieworg );
        cg.refdefViewAngles[PITCH] *= 1.2;
        return;
    }
    // CoD2x: end

	// trace a ray from the origin to the viewpoint to make sure the view isn't
	// in a solid block.  Use an 8 by 8 block to prevent the view from near clipping anything
	CG_TraceCapsule( &trace, cg.refdef.vieworg, mins, maxs, view, cg.predictedPlayerState.clientNum, 0x811 );

	if ( trace.fraction != 1.0 ) {

        vec3_t diff;   
        vec3_t endpos;
        VectorSubtract( view, cg.refdef.vieworg, diff );
        VectorMA( cg.refdef.vieworg, trace.fraction, diff, endpos );
		VectorCopy( endpos, view );

		view[2] += ( 1.0 - trace.fraction ) * 32;

		// try another trace to this position, because a tunnel may have the ceiling
		// close enogh that this is poking out
		CG_TraceCapsule( &trace, cg.refdef.vieworg, mins, maxs, view, cg.predictedPlayerState.clientNum, 0x811 );
        
        VectorSubtract( view, cg.refdef.vieworg, diff );
        VectorMA( cg.refdef.vieworg, trace.fraction, diff, endpos );
		VectorCopy( endpos, view );
	}

	VectorCopy( view, cg.refdef.vieworg );

	// select pitch to look at focus point from vieword
	VectorSubtract( focusPoint, cg.refdef.vieworg, focusPoint );
	focusDist = sqrt( focusPoint[0] * focusPoint[0] + focusPoint[1] * focusPoint[1] );
	if ( focusDist < 1 ) {
		focusDist = 1;  // should never happen
	}
    cg.refdefViewAngles[PITCH] = -180 / M_PI * atan2( focusPoint[2], focusDist );
}




/** Called before the entry point is called. Used to patch the memory. */
void cgame_patch() {

    patch_call(0x004cfb27, (unsigned int)CG_OffsetThirdPersonView);


    // Cvar "snaps" max value change from 30 to 40
    patch_int32(0x00411067 + 1, 40); // 00411067  bb1e000000         mov     ebx, 30

    // Cvar cl_maxpackets max value change from 100 to 125
    patch_int32(0x00410c64 + 1, 125); // 00410c64  bb64000000         mov     ebx, 100


    // Make "RECORDING %s" message smaller
    patch_float(0x004147e0 + 1, 0.2f); // 004147e0  68abaaaa3e push    0.333333343


    patch_jump(0x004d0c6b, (unsigned int)CG_RegisterItems);
    patch_call(0x004bff65, (unsigned int)CG_RegisterItems); 
}