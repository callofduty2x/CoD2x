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

static int cgame_clientStateLast = -1;
bool cgame_firstTime = true;


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

void Cmd_Smooth() {
    const char* cmd = Cmd_Argv(0);
    
    if (Cmd_Argc() != 2) {
        Com_Printf("%s <variablename> : increase value\n", cmd);
        return;
    }

    int value = atoi(Cmd_Argv(1));
	
    // 004ff88a  c7461432000000     mov     dword [esi+0x14 {entityState_s::pos.trDuration}], 0x32
    patch_int32(0x004ff88a + 3, value);
}





void BG_EvaluateTrajectory(trajectory_t* trajectory, int32_t serverTime, float* out_vec) {
    ASM_CALL(RETURN_VOID, 0x00513df0, 0, EAX(serverTime), EBX(trajectory), ECX(out_vec));
}

void CG_InterpolateEntityPosition() {
    centity_t* cent;
    ASM( movr, cent, "esi" );

    CL_AddDebugCrossPoint(cent->currentState.pos.trBase, 5, (float[]){1.0f, 0.0f, 0.0f, 1.0f}, 1, 0, 0);
    CL_AddDebugCrossPoint(cent->nextState.pos.trBase, 5, (float[]){0.0f, 1.0f, 0.0f, 1.0f}, 500, 0, 0);


    //if (g_cod2x->value.integer == 0 || 1) {
        //ASM_CALL(RETURN_VOID, 0x004cd8d0, 0, ESI(cent));

        // No interpolation
        /*cent->lerpOrigin[0] = cent->nextState.pos.trBase[0];
        cent->lerpOrigin[1] = cent->nextState.pos.trBase[1];
        cent->lerpOrigin[2] = cent->nextState.pos.trBase[2];

        cent->lerpAngles[0] = cent->nextState.apos.trBase[0];
        cent->lerpAngles[1] = cent->nextState.apos.trBase[1];
        cent->lerpAngles[2] = cent->nextState.apos.trBase[2];*/

        //004ffa3a  7472               je      0x4ffaae  to 004ffa3a  eb72               jmp     0x4ffaae
        

        //return;
    //} else {

    //004ffa3a  7472               je      0x4ffaae  to 004ffa3a  eb72               jmp     0x4ffaae

    //}

    float f = cg.frameInterpolation;

    // Evaluate trajectory positions
    vec3_t currentPos, nextPos;
    BG_EvaluateTrajectory(&cent->currentState.pos, cg.snap->serverTime, currentPos);
    BG_EvaluateTrajectory(&cent->nextState.pos, cg.nextSnap->serverTime, nextPos);

    // Interpolate positions
    for (int i = 0; i < 3; i++) {
        cent->lerpOrigin[i] = (nextPos[i] - currentPos[i]) * f + currentPos[i];
    }

    CL_AddDebugCrossPoint(cent->lerpOrigin, 5, (float[]){0.0f, 0.0f, 1.0f, 1.0f}, 500, 0, 0);

    Com_Printf("tr_duration: %d, f: %f\n", cent->currentState.pos.trDuration, f);

    // Evaluate trajectory angles
    vec3_t currentAngles, nextAngles;
    BG_EvaluateTrajectory(&cent->currentState.apos, cg.snap->serverTime, currentAngles);
    BG_EvaluateTrajectory(&cent->nextState.apos, cg.nextSnap->serverTime, nextAngles);

    // Interpolate angles
    for (int i = 0; i < 3; i++) {
        cent->lerpAngles[i] = LerpAngle(currentAngles[i], nextAngles[i], f);
    }

    // Special handling for players
    if (cent->nextState.eType == ET_PLAYER) {
        // Find player-specific data in memory
        clientInfo_t* ci = &cg.clientsInfo[cent->nextState.clientNum];

        ci->movementYaw = (float)LerpAngle(cent->currentState.angles2[1], cent->nextState.angles2[1], f);
        ci->playerAngles[0] = cent->lerpAngles[0];
        ci->playerAngles[1] = cent->lerpAngles[1];
        ci->playerAngles[2] = cent->lerpAngles[2];
        float leanf = cent->currentState.leanf;
        cent->lerpAngles[0] = 0;
        cent->lerpAngles[2] = 0;
        ci->lerpLean = LerpAngle(leanf, cent->nextState.leanf, f);
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

    Cmd_AddCommand("smooth", Cmd_Smooth);

    cgame_firstTime = false;
}


/** Called every frame on frame start. */
void cgame_frame() {

    if (clientState != cgame_clientStateLast) {
        Com_DPrintf("Client state changed from %d:%s to %d:%s\n", cgame_clientStateLast, get_client_state_name(cgame_clientStateLast), clientState, get_client_state_name(clientState));
    }

    // Cvar is not defined yet or player disconnected from the server
    if (g_cod2x != NULL) {

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
    }

    // Enable cheats when player disconnects from the server
    // It would allow to play demos without the need to do devmap
    if (clientState != cgame_clientStateLast && clientState == CLIENT_STATE_DISCONNECTED) {
        Dvar_SetBool(sv_cheats, true);
    }

    cgame_clientStateLast = clientState;
}


//00463e70  void* __stdcall Sys_ListFiles(char* directory @ eax, char* extension, char* filter @ edx, int32_t* numFiles, int32_t wantsubs)
char** Sys_ListFiles(char* extension, int32_t* numFiles, int32_t wantsubs) {
    // Load parameters from registers
    char* directory;
    char* filter;
    ASM( movr, directory, "eax" );
    ASM( movr, filter, "edx" );

    // Call the original function
    const void* original_func = (void*)(0x00463e70);
    char** result;
    ASM( push,     wantsubs         ); // 5nd argument                    
    ASM( push,     numFiles         ); // 4nd argument                    
    ASM( push,     extension        ); // 3nd argument                    
    ASM( mov,      "edx", filter    ); // 2st argument
    ASM( mov,      "eax", directory ); // 1st argument
    ASM( call,     original_func    );
    ASM( add_esp,  12               ); // Clean up the stack (3 argument × 4 bytes = 12)   
    ASM( movr,     result, "eax"    ); // Store the return value in the 'result' variable


    // When the game starts for the first time, load only the original IWD files
    // The main folder might contain mix of mods from different servers that might cause "iwd sum mismatch" errors when running the game
    // This will make sure these mods are not loaded at startup, but will be loaded when connecting to the game
    
    // TODO: turned off untill these errors are fixed:
    // - not working when started as server on windows
    // - not working when upper iwd names are used
    // - ui_joinGametype is set to latest gametype mode, but since mods are not allowed the range is not valid (aldo idk how it worked when mods were deleted manually)
    // in future version we should support list of allowed iwd names instead of block all

    /*if (firstTime) {
        int writeIndex = 0;
        for (int i = 0; i < *numFiles; i++) {
            //Com_Printf("File: %s\n", result[i]);
            // Check if file starts with "iw_00" - "iw_15" or starts with "localized_"
            if (strncmp(result[i], "iw_", 3) == 0 || strncmp(result[i], "localized_", 10) == 0) {
                result[writeIndex] = result[i];
                writeIndex++;
            }
        }
        *numFiles = writeIndex;
    }*/

    return result;
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

    patch_call(0x00424869, (unsigned int)Sys_ListFiles);

    patch_call(0x004cfb27, (unsigned int)CG_OffsetThirdPersonView);


    // Cvar "snaps" max value change from 30 to 40
    patch_int32(0x00411067 + 1, 1000); // 00411067  bb1e000000         mov     ebx, 30

    // Cvar cl_maxpackets max value change from 100 to 125
    patch_int32(0x00410c64 + 1, 125); // 00410c64  bb64000000         mov     ebx, 100


    // CG_InterpolateEntityPosition
    patch_call(0x004cda69, (unsigned int)CG_InterpolateEntityPosition);


    //patch_byte(0x004ffa3a, 0x74); // orig
    patch_byte(0x004ffa3a, 0xEB);

}