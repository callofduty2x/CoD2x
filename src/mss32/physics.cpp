#include "physics.h"

#include <math.h>

#include "shared.h"
#include "../shared/cod2_dvars.h"

static dvar_t* jump_bounceEnable = NULL;
static bool physics_wasListenServerRunning = false;

static bool physics_isListenServerRunning()
{
    return dedicated && dedicated->value.integer == 0 && sv_running && sv_running->value.boolean;
}

static void physics_setJumpBounceWriteProtected(bool writeProtected)
{
    dvarFlags_e writeProtectFlag = DEBUG_RELEASE(DVAR_CHEAT, DVAR_NOWRITE);

    if (!jump_bounceEnable)
    {
        return;
    }

    if (writeProtected)
    {
        jump_bounceEnable->flags = (dvarFlags_e)(jump_bounceEnable->flags | writeProtectFlag);
    }
    else
    {
        jump_bounceEnable->flags = (dvarFlags_e)(jump_bounceEnable->flags & ~writeProtectFlag);
    }
}

static bool physics_isJumpBounceEnabled()
{
    return jump_bounceEnable && jump_bounceEnable->value.boolean;
}

static void PM_ClipVelocity_Win32(const float* velIn, const float* normal, float* velOut)
{
    ASM_CALL(RETURN_VOID, 0x00515430, 0, ECX(velIn), EAX(normal), EDX(velOut));
}

static void PM_ProjectVelocity(const float* velIn, const float* normal, float* velOut)
{
    float lengthSq2D;
    float adjusted;
    float newZ;
    float lengthScale;

    if (!physics_isJumpBounceEnabled())
    {
        PM_ClipVelocity_Win32(velIn, normal, velOut);
        return;
    }

    lengthSq2D = (float)(velIn[0] * velIn[0]) + (float)(velIn[1] * velIn[1]);

    if (fabsf(normal[2]) < 0.001f || lengthSq2D == 0.0f)
    {
        velOut[0] = velIn[0];
        velOut[1] = velIn[1];
        velOut[2] = velIn[2];
    }
    else
    {
        newZ = (float)-(float)((float)(velIn[0] * normal[0]) + (float)(velIn[1] * normal[1])) / normal[2];
        adjusted = velIn[1];
        lengthScale = sqrtf((float)((float)(velIn[2] * velIn[2]) + lengthSq2D) / (float)((float)(newZ * newZ) + lengthSq2D));

        if (lengthScale < 1.0f || newZ < 0.0f || velIn[2] > 0.0f)
        {
            velOut[0] = lengthScale * velIn[0];
            velOut[1] = lengthScale * adjusted;
            velOut[2] = lengthScale * newZ;
        }
    }
}

void PM_ProjectVelocity_Win32()
{
    const float* velIn;
    const float* normal;
    float* velOut;

    ASM( movr, velIn, "ecx" );
    ASM( movr, normal, "eax" );
    ASM( movr, velOut, "edx" );

    PM_ProjectVelocity(velIn, normal, velOut);
}

void physics_frame()
{
    bool listenServerRunning = physics_isListenServerRunning();

    if (physics_wasListenServerRunning && !listenServerRunning && jump_bounceEnable)
    {
        Dvar_SetBool(jump_bounceEnable, false);
    }

    physics_setJumpBounceWriteProtected(!listenServerRunning);
    physics_wasListenServerRunning = listenServerRunning;
}

void physics_init()
{
    jump_bounceEnable = Dvar_RegisterBool(
        "jump_bounceEnable",
        false,
        (dvarFlags_e)(DEBUG_RELEASE(DVAR_CHEAT, DVAR_NOWRITE) | DVAR_SYSTEMINFO | DVAR_CHANGEABLE_RESET)
    );
}

void physics_patch()
{
    // PM_StepSlideMove: replace the CoD2 velocity clip with the CoD4-style
    // projection controlled by the synced jump_bounceEnable dvar.
    patch_call(0x00530ea5, (unsigned int)PM_ProjectVelocity_Win32);
}
