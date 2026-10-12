#include "mouse.h"

#include "shared.h"
#include "../shared/cod2_player.h"
#include "../shared/cod2_dvars.h"
#include <math.h>
#include <cod2_client.h>


// ============================================================================
// Client globals mapped to fixed binary addresses
// ============================================================================

// Double-buffered mouse input accumulators (raw pixel deltas, integer)
#define cl_mouseDx          (*(int (*)[2])0x98fda8)
#define cl_mouseDy          (*(int (*)[2])0x98fdb0)
// Current write-buffer index (0 or 1); read from the opposite slot each frame
#define cl_mouseIndex       (*(int*)0x98fdb8)
// Per-frame sensitivity multiplier injected by cgame (e.g. reduced when zoomed)
#define cl_cgameSensitivity (*(float*)0x98fdd4)
// Optional cgame-imposed max angle change per millisecond; 0 = uncapped
#define cl_cgameYawSpeed    (*(float*)0x98fddc)
#define cl_cgamePitchSpeed  (*(float*)0x98fdd8)
// View angles written by mouse input
#define cl_viewanglesYaw    (*(float*)0x98fdf0)
#define cl_viewanglesPitch  (*(float*)0x98fdec)
// Frame delta in milliseconds
#define frame_msec          (*(unsigned int*)0x96b5ec)

// kbutton array base pointer; individual buttons accessed by offset within it
#define kb_ptr              (*(void**)0x5cdd1c)
#define kb_strafe_active    (*((byte*)kb_ptr + 0xb0))   // KB_STRAFE.active
#define kb_mlook_active     (*((byte*)kb_ptr + 0x114))  // KB_MLOOK.active

// Engine dvars already registered by the binary
#define m_filter            (*(dvar_t**)0x96b638)
#define sensitivity         (*(dvar_t**)0x68a3a8)
#define cl_mouseAccel       (*(dvar_t**)0x96b5f8)
#define cl_showMouseRate    (*(dvar_t**)0x96b5dc)
#define m_yaw               (*(dvar_t**)0x68a3b4)
#define m_pitch             (*(dvar_t**)0x68a3b8)
#define m_side              (*(dvar_t**)0x96b62c)
#define m_forward           (*(dvar_t**)0x96b5f0)
#define cl_freelook         (*(dvar_t**)0xb013ec)
#define cl_movementFlags    (*(unsigned int*)0x96b690)
#define cl_heavyWeaponFlags (*(unsigned int*)0x96b724)

#define cg_zoomTransition (*(float*)0x014ee190)

// Custom dvars added by CoD2x
static dvar_t* sensitivity_MG = NULL;    // multiplier applied when using heavy weapon


/*
=================
CL_MouseMove
=================
*/
void CL_MouseMove(usercmd_s* cmd)
{
    float mx, my;

    // Mouse smoothing: average the two most recent raw input samples.
    if (m_filter->value.boolean) {
        mx = (float)(cl_mouseDx[0] + cl_mouseDx[1]) * 0.5f;
        my = (float)(cl_mouseDy[0] + cl_mouseDy[1]) * 0.5f;
    } else {
        mx = (float)cl_mouseDx[cl_mouseIndex];
        my = (float)cl_mouseDy[cl_mouseIndex];
    }

    // Advance the double-buffer and zero out the upcoming write slot.
    cl_mouseIndex ^= 1;
    cl_mouseDx[cl_mouseIndex] = 0;
    cl_mouseDy[cl_mouseIndex] = 0;

    unsigned int msec = frame_msec;
    if (msec == 0)
        return;

    // Derive mouse speed (pixels/ms) and blend sensitivity with acceleration.
    float speed = sqrtf(mx * mx + my * my);
    float rate  = speed / (float)msec;
    float accelSensitivity = rate * cl_mouseAccel->value.decimal + sensitivity->value.decimal;

    // Scale by the cgame-provided multiplier (e.g. zoom factor).
    accelSensitivity *= cl_cgameSensitivity;

    if (rate != 0.0f && cl_showMouseRate->value.boolean) {
        Com_Printf("%f : %f\n", rate, accelSensitivity);
    }

    // Skip all view/movement updates when in a restricted player state.
    if (cl_movementFlags & 0x8000)
        return;

    // MG
    if (cl_heavyWeaponFlags & 0x300) {
        //mx *= 2.5f;
        //my *= 2.0f;
        
        // CoD2x: apply custom MG sensitivity multiplier on top of the regular acceleration
        float accelSensitivityMG = rate * cl_mouseAccel->value.decimal + sensitivity_MG->value.decimal;
        mx *= accelSensitivityMG * 2.5f;
        my *= accelSensitivityMG * 2.0f;
        // CoD2x: end
    } else {
        mx *= accelSensitivity;
        my *= accelSensitivity;
    }

    if (mx == 0.0f && my == 0.0f)
        return;

    // --- X axis (horizontal) ---
    if (kb_strafe_active) {
        // Strafe key: map horizontal mouse movement to cmd->rightmove.
        int rightmove = (int)cmd->rightmove + (int)(mx * m_side->value.decimal);
        cmd->rightmove = (char)(rightmove < -128 ? -128 : (rightmove > 127 ? 127 : rightmove));
    } else {
        // Free-look: rotate yaw, optionally capped to cgame's angular speed limit.
        float deltaYaw = mx * m_yaw->value.decimal;
        if (cl_cgameYawSpeed != 0.0f) {
            float maxDelta = (float)msec * cl_cgameYawSpeed * 0.001f;
            deltaYaw = fmaxf(-maxDelta, fminf(maxDelta, deltaYaw));
        }
        cl_viewanglesYaw -= deltaYaw;
    }

    // --- Y axis (vertical) ---
    if ((kb_mlook_active || cl_freelook->value.boolean) && !kb_strafe_active) {
        // Mouse-look: rotate pitch, optionally capped to cgame's angular speed limit.
        float deltaPitch = my * m_pitch->value.decimal;
        if (cl_cgamePitchSpeed != 0.0f) {
            float maxDelta = (float)msec * cl_cgamePitchSpeed * 0.001f;
            deltaPitch = fmaxf(-maxDelta, fminf(maxDelta, deltaPitch));
        }
        cl_viewanglesPitch += deltaPitch;
    } else {
        // No mouse-look: map vertical mouse movement to cmd->forwardmove.
        int forwardmove = (int)cmd->forwardmove - (int)(my * m_forward->value.decimal);
        cmd->forwardmove = (char)(forwardmove < -128 ? -128 : (forwardmove > 127 ? 127 : forwardmove));
    }
}


/** Called only once on game start after common initialization. */
void mouse_init()
{
    // Register custom sensitivity dvars
    sensitivity_MG = Dvar_RegisterFloat("sensitivity_MG", 1.0f, 0.01f, 100.0f, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));
}

/** Called before the entry point is called. Used to patch the memory. */
void mouse_patch()
{
    // Hook call at 0x00408B44 (inside CL_CreateCmd) -> CL_MouseMove (0x00408510)
    patch_call(0x00408B44, (unsigned int)CL_MouseMove);
}
