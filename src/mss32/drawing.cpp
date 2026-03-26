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
dvar_t* con_printDoubleColors = NULL;


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



#define cl_consoleFrameCounter         (*((int32_t*)0x00601784))
#define cl_consoleTotalBuffers         (*((int32_t*)0x00601798))
#define cl_consoleBufferSize           (*((uint32_t*)0x00601794))
#define cl_consoleBufferPos            (*((uint32_t*)0x00601788))
#define cl_consoleBufferBase           ((uint16_t*)0x005E1784)

// write character into console buffer (equivalent to the disassembly)
void CL_WriteConsoleChar(uint32_t frameIndex, uint8_t ch, uint8_t colorNum)
{
    int frame = cl_consoleFrameCounter % cl_consoleTotalBuffers;
    uint32_t dst = frame * cl_consoleBufferSize + cl_consoleBufferPos;
    cl_consoleBufferBase[dst] = (uint16_t)((colorNum << 8) | ch);
    cl_consoleBufferPos++;
}

// Fully replicates the disassembled function.
void CL_AddConsoleText(int32_t color)
{
    char* str; ASM__movr(str, "eax");

    int32_t colorNum = color;
    char *p = str;

    if (colorNum < 0)
        colorNum = 7;

    int32_t frameIndex = cl_consoleFrameCounter % cl_consoleTotalBuffers;

    // Load first char
    unsigned char ch1 = (unsigned char)*p;
    unsigned char ch2 = ch1;

    if (ch2) {
        // Loop while we still have room in the current buffer
        while (cl_consoleBufferPos < cl_consoleBufferSize) {

            // ================================
            // CoD2x: NEW BEHAVIOR - disable double colors
            // ================================
            if (!con_printDoubleColors->value.boolean) {
                if (ch1 == '^') {
                    // Count a run of carets
                    size_t caretCount = 0;
                    do { ++p; ++caretCount; } while (*p == '^');

                    // Skip up to the same number of following digits [0-9]
                    size_t digitCount = 0;
                    while (p[digitCount] >= '0' && p[digitCount] <= '9') {
                        ++digitCount;
                    }
                    size_t skipDigits = (digitCount < caretCount) ? digitCount : caretCount;
                    p += skipDigits;

                    // Load next char and continue without writing
                    ch1 = (unsigned char)*p;
                    ch2 = ch1;
                    if (!ch2) break;
                    continue;
                } else {
                    // Normal char path (CR/LF suppressed)
                    ++p;
                    if (ch2 != '\n' && ch2 != '\r') {
                        CL_WriteConsoleChar((uint32_t)frameIndex, ch2, (uint8_t)colorNum);
                    }
                }
            }

            // ================================
            // ORIGINAL BEHAVIOR - allow double colors
            // ================================
            else {
                if (ch1 != '^') {
                    // label_4054f1 path: consume one char and (unless CR/LF) write it
                    p++;

                    if (ch2 != '\n' && ch2 != '\r') {
                        CL_WriteConsoleChar((uint32_t)frameIndex, ch2, colorNum);
                    }
                } else {
                    // Caret found — check for a color digit
                    unsigned char next = (unsigned char)p[1];

                    // If invalid color escape, treat '^' as a normal char
                    if (!next || next == '^' || next < '0' || next > '9') {
                        p++;
                        if (ch2 != '\n' && ch2 != '\r') {
                            CL_WriteConsoleChar((uint32_t)frameIndex, ch2, colorNum);
                        }
                    } else {
                        // Valid "^digit"
                        if (color < 0) {
                            uint32_t c = (uint32_t)(next - '0');  // 0..9
                            if (c >= 10) c = 7;
                            colorNum = c;
                        }
                        // Skip both '^' and digit; don't write them
                        p += 2;
                    }
                }
            }

            // Next char
            ch1 = (unsigned char)*p;
            ch2 = ch1;
            if (!ch2)
                break;
        }
    }
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

    con_printDoubleColors = Dvar_RegisterBool("con_printDoubleColors", true, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
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
    patch_call(0x004d7bce, (unsigned int)CG_BulletHitEvent);

    // Make tracers visible also for 1st person view
    //patch_byte(0x004d7a91, 0x74); // Always jump
    //patch_byte(0x004d7a89, 0xeb); // Always jump

    // Improve DirectX error message
    patch_int32(0x0040fcf5 + 4, (unsigned int)Sys_DirectXFatalError);

    patch_call(0x004055cb, (unsigned int)CL_AddConsoleText);
    patch_call(0x00405726, (unsigned int)CL_AddConsoleText);

    // Patch end view func
    patch_call(0x00414a8c, (unsigned int)drawing_end);
    patch_nop(0x00414a8c + 5, 1); // Nop rest of the call function, because its calling through pointer

}
