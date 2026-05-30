#include "physics.h"

#include <math.h>

#include "shared.h"
#include "../shared/cod2_client.h"
#include "../shared/cod2_shared.h"

static bool physics_jumpBounceEnabled = false;

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

    if (!physics_jumpBounceEnabled)
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
    if (clientState < CLIENT_STATE_PRIMED)
    {
        physics_jumpBounceEnabled = false;
        return;
    }

    const char* systeminfo = CL_GetConfigString(CS_SYSTEMINFO);
    const char* jumpBounceEnable = Info_ValueForKey(systeminfo, "jump_bounceEnable");
    physics_jumpBounceEnabled = atoi(jumpBounceEnable) != 0;
}

void physics_patch()
{
    // PM_StepSlideMove: replace the CoD2 velocity clip with the CoD4-style
    // projection used by jump_bounceEnable servers.
    patch_call(0x00530ea5, (unsigned int)PM_ProjectVelocity_Win32);
}
