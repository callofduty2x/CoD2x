#include "drawing.h"

#include "../shared/cod2_client.h"
#include "../shared/cod2_dvars.h"
#include "demo.h"
#include "radar.h"
#include "shared.h"

vec4_t colWhite			    = { 1, 1, 1, 1 };
vec4_t colBlack			    = { 0, 0, 0, 1 };
vec4_t colRed			    = { 1, 0, 0, 1 };
vec4_t colGreen			    = { 0, 1, 0, 1 };
vec4_t colBlue			    = { 0, 0, 1, 1 };
vec4_t colYellow		    = { 1, 1, 0, 1 };


dvar_t* cg_drawSpectatedPlayerName = NULL;
dvar_t* cg_drawCompass = NULL;
dvar_t* cg_hudCompassOffsetX = NULL;
dvar_t* cg_hudCompassOffsetY = NULL;
dvar_t* cg_debugBullets = NULL;


/**
 * Drawing of the text "following" and player name in top center of the screen when spectating.
 */
int CG_DrawFollow() {

    if (!cg_drawSpectatedPlayerName->value.boolean) {
        return 0;
    }

    int drawed = 0;
    ASM_CALL(RETURN(drawed), 0x004cba90);
    return drawed;
}

struct compass_hud_data {
    float x;
    float y;
    float w;
    float h;
    horizontalAlign_e horizontalAlign;
    verticalAlign_e verticalAlign;
};

/** Drawing of the rotating image of compass */
void CG_DrawPlayerCompass(void* shader, vec4_t* color) {
    compass_hud_data* data; ASM__movr(data, "esi");

    radar_draw();

    if (!cg_drawCompass->value.boolean)
        return;

    data->x += cg_hudCompassOffsetX->value.decimal;
    data->y += cg_hudCompassOffsetY->value.decimal;

    ASM_CALL(RETURN_VOID, 0x004c5400, 2, ESI(data), PUSH(shader), PUSH(color));
}

/** Drawing of the objectives on the compass. */
void CG_DrawPlayerCompassObjectives(compass_hud_data* data, vec4_t* color) {
    
    if (!cg_drawCompass->value.boolean)
        return;

    data->x += cg_hudCompassOffsetX->value.decimal;
    data->y += cg_hudCompassOffsetY->value.decimal;

    ASM_CALL(RETURN_VOID, 0x004c5620, 2, PUSH(data), PUSH(color));
}

/** Drawing of the players on the compass. */
void CG_DrawCompassFriendlies(compass_hud_data* data, vec4_t* color) {
    if (!cg_drawCompass->value.boolean)
        return;
    
    data->x += cg_hudCompassOffsetX->value.decimal;
    data->y += cg_hudCompassOffsetY->value.decimal;

    ASM_CALL(RETURN_VOID, 0x004dafe0, 2, PUSH(data), PUSH(color));
}

/** Draws the background for the compass. */
void __cdecl CG_DrawPlayerCompassBack(void* shader, vec4_t* color) {
    compass_hud_data* data; ASM__movr(data, "esi");

    if (!cg_drawCompass->value.boolean)
        return;

    data->x += cg_hudCompassOffsetX->value.decimal;
    data->y += cg_hudCompassOffsetY->value.decimal;

    ASM_CALL(RETURN_VOID, 0x004c5510, 2, ESI(data), PUSH(shader), PUSH(color));
}


void CG_DrawCrosshairNames() {
    ASM_CALL(RETURN_VOID, 0x004c97c0);
}


void CG_BulletHitEvent() {
    int32_t clientNum;
    int32_t sourceEntityNum;
    vec3_t* end;

    ASM__movr(clientNum, "eax");
    ASM__movr(sourceEntityNum, "ecx");
    ASM__movr(end, "esi");

    // CoD2x: Debug bullets
    if (cg_debugBullets->value.boolean) {
        Com_Printf("CG_BulletHitEvent called: clientNum=%d, sourceEntityNum=%d, end=(%.2f, %.2f, %.2f)\n", clientNum, sourceEntityNum, (*end)[0], (*end)[1], (*end)[2]);

        CL_AddDebugCrossPoint(*end, 3, colRed, 1000, 0, 0);

        vec3_t start;
        int result = CG_CalcMuzzlePoint(start, clientNum, sourceEntityNum);

        if (result) {
            CL_AddDebugLine(start, *end, colYellow, 1000, 0, 0);
        }
    }
    // CoD2x: End

    ASM_CALL(RETURN_VOID, 0x004d7a50, 0, EAX(clientNum), ECX(sourceEntityNum), ESI(end));
}


// Help web page removed, fixed crash when getting translations
void Sys_DirectXFatalError() {
    MessageBoxA(NULL, "DirectX(R) encountered an unrecoverable error.", "DirectX Error", MB_OK | MB_ICONERROR);
    ExitProcess(-1);
}


/** This function is called after all 2D drawing is done */
void drawing_end(int num) {

    //UI_DrawText("CoD2x Mod", INT_MAX, fontNormal, 10.0f, 50.0f, HORIZONTAL_ALIGN_LEFT, VERTICAL_ALIGN_TOP, 1.0f, colWhite, TEXT_STYLE_NORMAL);

    demo_drawing();

    uint32_t addr = *(uint32_t*)0x0068a2b8;
    ASM_CALL(RETURN_VOID, addr, 1, PUSH(num));
}



/** Called every frame on frame start. */
void drawing_frame() {
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void drawing_init() {
    cg_drawSpectatedPlayerName = Dvar_RegisterBool("cg_drawSpectatedPlayerName", true, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_drawCompass = Dvar_RegisterBool("cg_drawCompass", true, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudCompassOffsetX = Dvar_RegisterFloat("cg_hudCompassOffsetX", 0.0f, -640.0f, 640.0f, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    cg_hudCompassOffsetY = Dvar_RegisterFloat("cg_hudCompassOffsetY", 0.0f, -480.0f, 480.0f, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));

    cg_debugBullets = Dvar_RegisterBool("cg_debugBullets", false, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET | DVAR_CHEAT));
}

/** Called before the entry point is called. Used to patch the memory. */
void drawing_patch() {
    patch_call(0x004cbdce, (unsigned int)CG_DrawFollow);

    patch_call(0x004c6870, (unsigned int)CG_DrawPlayerCompass);
    patch_call(0x004c6884, (unsigned int)CG_DrawPlayerCompassBack);
    patch_call(0x004c6898, (unsigned int)CG_DrawPlayerCompassObjectives);
    patch_call(0x004c68e8, (unsigned int)CG_DrawCompassFriendlies);
    patch_call(0x004cbd36, (unsigned int)CG_DrawCrosshairNames);
    patch_call(0x004cbd6b, (unsigned int)CG_DrawCrosshairNames);

    patch_call(0x004d7bce, (unsigned int)CG_BulletHitEvent);

    // Make tracers visible also for 1st person view
    //patch_byte(0x004d7a91, 0x74); // Always jump
    //patch_byte(0x004d7a89, 0xeb); // Always jump

    // Improve DirectX error message
    patch_int32(0x0040fcf5 + 4, (unsigned int)Sys_DirectXFatalError);

    // Patch end view func
    patch_call(0x00414a8c, (unsigned int)drawing_end);
    patch_nop(0x00414a8c + 5, 1); // Nop rest of the call function, because its calling through pointer

}
