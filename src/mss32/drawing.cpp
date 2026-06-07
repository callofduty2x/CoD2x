#include "drawing.h"

#include "../shared/cod2_client.h"
#include "../shared/cod2_dvars.h"
#include "demo.h"
#include "radar.h"
#include "shared.h"

#include <string.h>
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

typedef char* (__cdecl *R_AddCmdDrawTextWithCursor_t)(const char* text, int maxChars, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style, int cursorPos, char cursor);
typedef char* (__cdecl *R_DrawText_t)(const char* text, int maxChars, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style);
typedef int (__cdecl *R_TextWidth_t)(const char* text, int maxChars, fontHandle_t* font);
typedef char* (__cdecl *R_DrawConsoleText_t)(const uint16_t* buffer, int count, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style);

#define GFX_REFAPI_R_TEXTWIDTH                 (*((R_TextWidth_t*)0x0068a314))
#define GFX_REFAPI_R_DRAWTEXT                  (*((R_DrawText_t*)0x0068a31c))
#define GFX_REFAPI_R_ADDCMDDRAWTEXTINSPACE    (*((R_DrawText_t*)0x0068a320))
#define GFX_REFAPI_R_DRAWCONSOLETEXT          (*((R_DrawConsoleText_t*)0x0068a328))
#define GFX_REFAPI_R_ADDCMDDRAWTEXTWITHCURSOR (*((R_AddCmdDrawTextWithCursor_t*)0x0068a32c))

static R_AddCmdDrawTextWithCursor_t original_R_AddCmdDrawTextWithCursor = NULL;
static R_DrawText_t                  original_R_DrawText                  = NULL;
static R_DrawText_t                  original_R_AddCmdDrawTextInSpace     = NULL;
static R_DrawConsoleText_t           original_R_DrawConsoleText           = NULL;
static R_TextWidth_t                 original_R_TextWidth                 = NULL;

char* __cdecl R_DrawText_Extended(const char* text, int maxChars, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style);
char* __cdecl R_AddCmdDrawTextInSpace_Extended(const char* text, int maxChars, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style);
char* __cdecl R_AddCmdDrawTextWithCursor_Extended(const char* text, int maxChars, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style, int cursorPos, char cursor);
char* __cdecl R_DrawConsoleText_Extended(const uint16_t* buffer, int count, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style);
int __cdecl R_TextWidth_Extended(const char* text, int maxChars, fontHandle_t* font);

static void TextColor_Copy(const float* src, float* dst)
{
    if (!src) {
        dst[0] = 1.0f;
        dst[1] = 1.0f;
        dst[2] = 1.0f;
        dst[3] = 1.0f;
        return;
    }

    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
}

static void TextColor_Set(float r, float g, float b, const float* baseColor, float* out)
{
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = baseColor ? baseColor[3] : 1.0f;
}

static bool TextColor_Lookup(char code, const float* baseColor, float* out)
{
    switch (code)
    {
        case '0': TextColor_Set(0.00f, 0.00f, 0.00f, baseColor, out); return true; // Black
        case '1': TextColor_Set(1.00f, 0.00f, 0.00f, baseColor, out); return true; // Red
        case '2': TextColor_Set(0.00f, 1.00f, 0.00f, baseColor, out); return true; // Green
        case '3': TextColor_Set(1.00f, 1.00f, 0.00f, baseColor, out); return true; // Yellow
        case '4': TextColor_Set(0.00f, 0.00f, 1.00f, baseColor, out); return true; // Blue
        case '5': TextColor_Set(0.00f, 1.00f, 1.00f, baseColor, out); return true; // Cyan
        case '6': TextColor_Set(1.00f, 0.00f, 1.00f, baseColor, out); return true; // Pink
        case '7':
            TextColor_Copy(colWhite, out);
            if (baseColor)
                out[3] = baseColor[3];
            return true;
        case '8': TextColor_Set(1.00f, 0.50f, 0.00f, baseColor, out); return true; // Orange
        case '9': TextColor_Set(0.50f, 0.50f, 0.50f, baseColor, out); return true; // Grey

        // Extended palette — letters only (a–z, 25 colors; tuned for visual separation)
        case 'a': TextColor_Set(0.85f, 0.75f, 0.15f, baseColor, out); return true; // Gold
        case 'b': TextColor_Set(0.00f, 0.55f, 0.55f, baseColor, out); return true; // Teal
        case 'c': TextColor_Set(0.55f, 0.10f, 0.95f, baseColor, out); return true; // Electric Violet
        case 'd': TextColor_Set(0.40f, 0.75f, 1.00f, baseColor, out); return true; // Sky Blue
        case 'e': TextColor_Set(0.60f, 0.15f, 0.55f, baseColor, out); return true; // Plum
        case 'f': TextColor_Set(0.75f, 0.90f, 1.00f, baseColor, out); return true; // Ice Blue
        case 'g': TextColor_Set(0.55f, 0.95f, 0.75f, baseColor, out); return true; // Mint
        case 'h': TextColor_Set(0.05f, 0.35f, 0.20f, baseColor, out); return true; // Pine
        case 'i': TextColor_Set(0.45f, 0.05f, 0.12f, baseColor, out); return true; // Maroon
        case 'j': TextColor_Set(0.75f, 0.08f, 0.20f, baseColor, out); return true; // Crimson
        case 'k': TextColor_Set(0.50f, 0.28f, 0.14f, baseColor, out); return true; // Chocolate
        case 'l': TextColor_Set(0.75f, 0.55f, 0.35f, baseColor, out); return true; // Tan
        case 'm': TextColor_Set(0.65f, 0.85f, 0.10f, baseColor, out); return true; // Chartreuse
        case 'n': TextColor_Set(0.95f, 0.70f, 0.55f, baseColor, out); return true; // Peach
        case 'o': TextColor_Set(0.95f, 0.93f, 0.82f, baseColor, out); return true; // Ivory
        case 'p': TextColor_Set(0.05f, 0.08f, 0.35f, baseColor, out); return true; // Midnight Blue
        case 'q': TextColor_Set(0.25f, 0.12f, 0.08f, baseColor, out); return true; // Espresso
        case 'r': TextColor_Set(0.80f, 0.45f, 0.20f, baseColor, out); return true; // Copper
        case 's': TextColor_Set(0.55f, 0.48f, 0.22f, baseColor, out); return true; // Olive Moss
        case 't': TextColor_Set(0.25f, 0.70f, 0.35f, baseColor, out); return true; // Fern
        case 'u': TextColor_Set(0.20f, 0.78f, 0.85f, baseColor, out); return true; // Aqua
        case 'v': TextColor_Set(0.92f, 0.15f, 0.10f, baseColor, out); return true; // Scarlet
        case 'w': TextColor_Set(0.72f, 0.74f, 0.78f, baseColor, out); return true; // Silver
        case 'y': TextColor_Set(0.00f, 0.72f, 0.45f, baseColor, out); return true; // Emerald
        case 'z': TextColor_Set(1.00f, 0.45f, 0.35f, baseColor, out); return true; // Coral
        default:
            return false;
    }
}

static void TextColor_FromColorNum(int colorNum, float out[4]);

static bool TextColor_IsCode(char code)
{
    float dummy[4];
    return TextColor_Lookup(code, colWhite, dummy);
}

struct TextSegmentState;

// Apply a ^X color escape. Extended palette codes always apply. Standard ^0..^9 only
// apply when the caller passed a negative color (vanilla inline-color mode) or when
// the string also uses extended palette codes (user-authored rich text).
static void TextColor_ApplyEscape(char code, int32_t allowDigitCodes, int* colorNum, float* currentRgb, bool richText)
{
    if (code >= '0' && code <= '9') {
        if (allowDigitCodes >= 0 && !richText)
            return;
        *colorNum = code - '0';
        TextColor_FromColorNum(*colorNum, currentRgb);
    } else {
        TextColor_Lookup(code, colWhite, currentRgb);
        *colorNum = 7;
    }
}

// Inline style codes (separate from color palette):
//   ^! = blink, ^# = rainbow, ^& = pulse, ^~ = reset all styles
static bool TextStyle_IsCode(char code)
{
    switch (code)
    {
        case '!':
        case '#':
        case '&':
        case '~':
            return true;
        default:
            return false;
    }
}

static bool Text_IsMarkupCode(char code)
{
    return TextColor_IsCode(code) || TextStyle_IsCode(code);
}

static bool Text_CaretIntroducesActiveMarkup(const char* caretPos)
{
    if (!caretPos || caretPos[0] != '^' || !caretPos[1])
        return false;
    if (caretPos[1] == '^')
        return false;
    return Text_IsMarkupCode(caretPos[1]);
}

static bool Text_ContainsActiveMarkup(const char* text)
{
    if (!text)
        return false;

    for (const char* p = text; *p; ++p)
    {
        if (Text_CaretIntroducesActiveMarkup(p))
            return true;
    }

    return false;
}

// True when the string contains a caret (needs our markup parser, not vanilla ^0-only).
static bool Text_TextHasCaret(const char* text)
{
    if (!text)
        return false;
    return strchr(text, '^') != NULL;
}

// Extended color = palette letters only; vanilla ^0..^9 are NOT extended.
static bool Text_IsExtendedColorCode(char code)
{
    if (code >= '0' && code <= '9')
        return false;
    return TextColor_IsCode(code);
}

// True only when the engine's native ^0..^9 handling is insufficient, i.e. the
// text uses extended palette colors or style/FX codes.
// Pure vanilla text (only ^0..^9 + plain text) is left to the original renderer,
// which matches stock CoD2 exactly and avoids re-implementing what the engine does.
static bool Text_NeedsExtendedHandling(const char* text)
{
    if (!text)
        return false;

    for (const char* p = text; *p; ++p)
    {
        if (p[0] != '^' || !p[1])
            continue;
        // A bare "^^" (literal caret) followed only by vanilla ^0..^9 / plain text is
        // left to the stock engine, which parses it exactly like vanilla CoD2:
        //   "^^5Sipex" -> literal '^' + ^5 color -> "^Sipex" (Sipex colored).
        // We only take over when an actual extended palette color or style/FX code
        // is present, since the engine cannot render those.
        if (TextStyle_IsCode(p[1]))
            return true;
        if (Text_IsExtendedColorCode(p[1]))
            return true;
    }

    return false;
}

static bool drawing_is_our_drawtext(R_DrawText_t fn)
{
    return fn == (R_DrawText_t)&R_DrawText_Extended;
}

static bool drawing_is_our_draw_in_space(R_DrawText_t fn)
{
    return fn == (R_DrawText_t)&R_AddCmdDrawTextInSpace_Extended;
}

static bool drawing_is_our_draw_console(R_DrawConsoleText_t fn)
{
    return fn == (R_DrawConsoleText_t)&R_DrawConsoleText_Extended;
}

static bool drawing_is_our_draw_with_cursor(R_AddCmdDrawTextWithCursor_t fn)
{
    return fn == (R_AddCmdDrawTextWithCursor_t)&R_AddCmdDrawTextWithCursor_Extended;
}

static bool drawing_is_our_textwidth(R_TextWidth_t fn)
{
    return fn == (R_TextWidth_t)&R_TextWidth_Extended;
}

static bool drawing_originals_valid()
{
    return original_R_DrawText && !drawing_is_our_drawtext(original_R_DrawText)
        && original_R_AddCmdDrawTextInSpace && !drawing_is_our_draw_in_space(original_R_AddCmdDrawTextInSpace)
        && original_R_DrawConsoleText && !drawing_is_our_draw_console(original_R_DrawConsoleText)
        && original_R_AddCmdDrawTextWithCursor && !drawing_is_our_draw_with_cursor(original_R_AddCmdDrawTextWithCursor)
        && original_R_TextWidth && !drawing_is_our_textwidth(original_R_TextWidth);
}

static void drawing_ensure_text_hooks();

struct TextSegmentState {
    float color[4];
    int style;
    bool rainbow;
    bool blink;
    bool pulse;
};

static void TextStyle_ApplyCode(char code, TextSegmentState* state)
{
    if (!state)
        return;

    switch (code)
    {
        case '!':
            state->blink = true;
            break;
        case '#':
            state->rainbow = true;
            break;
        case '&':
            state->pulse = true;
            break;
        case '~':
            state->style = TEXT_STYLE_NORMAL;
            state->rainbow = false;
            state->blink = false;
            state->pulse = false;
            break;
        default:
            break;
    }
}

static void TextColor_Rainbow(float alpha, float* out)
{
    DWORD ms = GetTickCount();
    float t = (float)(ms % 3000) / 3000.0f;
    float hue = t * 6.0f;
    int sector = (int)hue % 6;
    float frac = hue - (float)(int)hue;

    switch (sector)
    {
        case 0: out[0] = 1.0f; out[1] = frac;     out[2] = 0.0f; break;
        case 1: out[0] = 1.0f - frac; out[1] = 1.0f; out[2] = 0.0f; break;
        case 2: out[0] = 0.0f; out[1] = 1.0f; out[2] = frac;     break;
        case 3: out[0] = 0.0f; out[1] = 1.0f - frac; out[2] = 1.0f; break;
        case 4: out[0] = frac;     out[2] = 1.0f; out[1] = 0.0f; break;
        default: out[0] = 1.0f; out[1] = 0.0f; out[2] = 1.0f - frac; break;
    }

    out[3] = alpha;
}

static void TextSegmentState_Init(TextSegmentState* state, const float* baseColor, int baseStyle)
{
    TextColor_Copy(baseColor, state->color);
    state->style = baseStyle;
    state->rainbow = false;
    state->blink = false;
    state->pulse = false;
}

static int TextSegmentState_GetEngineStyle(const TextSegmentState* state)
{
    if (!state)
        return TEXT_STYLE_NORMAL;

    if (state->style == TEXT_STYLE_OUTLINESHADOWED ||
        state->style == TEXT_STYLE_OUTLINED ||
        state->style == TEXT_STYLE_SHADOWED ||
        state->style == TEXT_STYLE_SHADOWEDMORE)
        return state->style;

    return TEXT_STYLE_NORMAL;
}

static void TextSegmentState_FinalizeColor(const TextSegmentState* state, float* outColor)
{
    TextColor_Copy(state->color, outColor);
    if (state->rainbow)
        TextColor_Rainbow(outColor[3], outColor);

    DWORD ms = GetTickCount();
    float alpha = outColor[3];

    if (state->pulse) {
        float phase = (float)(ms % 2000) / 2000.0f;
        float wave = (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);
        alpha *= 0.55f + 0.45f * wave;
    }
    if (state->blink) {
        alpha *= ((ms / 400) % 2) ? 1.0f : 0.0f;
    }

    outColor[3] = alpha;
}

static bool TextSegmentState_Equal(const TextSegmentState* a, const TextSegmentState* b)
{
    return a->style == b->style &&
           a->rainbow == b->rainbow &&
           a->blink == b->blink &&
           a->pulse == b->pulse &&
           a->color[0] == b->color[0] &&
           a->color[1] == b->color[1] &&
           a->color[2] == b->color[2] &&
           a->color[3] == b->color[3];
}

static bool TextSegmentState_StyleFxEqual(const TextSegmentState* a, const TextSegmentState* b)
{
    return a->style == b->style &&
           a->rainbow == b->rainbow &&
           a->blink == b->blink &&
           a->pulse == b->pulse;
}

// Console-buffer control channels. Vanilla kill feed uses 10-12 (RGB) and 13-19
// (icon color, size, material — see CL_DeathMessagePrint / R_GetConsoleIcon).
// Never use 13-19 for CoD2x style/FX or R_GetConsoleString emits garbage (^C = hi 19).
#define CON_CTRL_STYLE   20
#define CON_CTRL_FX      21
#define FX_RAINBOW 1
#define FX_BLINK   2
#define FX_PULSE   4

static int g_extColorInstalled = 0;

static bool ConsoleBuffer_IsKillFeed(const uint16_t* buffer, int count)
{
    for (int i = 0; i < count; ++i) {
        uint8_t hi = (uint8_t)((buffer[i] >> 8) & 0xff);
        if (hi >= 13 && hi <= 19)
            return true;
    }
    return false;
}

static bool ConsoleBuffer_UsesExtendedEncoding(const uint16_t* buffer, int count)
{
    for (int i = 0; i < count; ++i) {
        uint8_t hi = (uint8_t)((buffer[i] >> 8) & 0xff);
        if ((hi >= 10 && hi <= 12) || hi == CON_CTRL_STYLE || hi == CON_CTRL_FX)
            return true;
    }
    return false;
}

static void drawing_set_vanilla_text_hooks(bool vanilla)
{
    if (vanilla) {
        if (original_R_TextWidth)
            patch_int32(0x0068a314, (int32_t)original_R_TextWidth);
        if (original_R_DrawText)
            patch_int32(0x0068a31c, (int32_t)original_R_DrawText);
        if (original_R_AddCmdDrawTextWithCursor)
            patch_int32(0x0068a32c, (int32_t)original_R_AddCmdDrawTextWithCursor);
        if (original_R_DrawConsoleText)
            patch_int32(0x0068a328, (int32_t)original_R_DrawConsoleText);
    } else if (g_extColorInstalled) {
        patch_int32(0x0068a314, (int32_t)&R_TextWidth_Extended);
        patch_int32(0x0068a31c, (int32_t)&R_DrawText_Extended);
        patch_int32(0x0068a32c, (int32_t)&R_AddCmdDrawTextWithCursor_Extended);
        patch_int32(0x0068a328, (int32_t)&R_DrawConsoleText_Extended);
    }
}

// Parse one ^ step (shared by display and edit fields):
//   ^X  = color/style when X is a code character; X is not shown
//   ^   = literal '^' (each caret in "^^" stays separate — no ^^ collapse)
static bool Text_ConsumeCaretMarkup(const char** p,
    int32_t allowDigitCodes,
    bool richText,
    int* colorNum,
    float* currentRgb,
    TextSegmentState* segState,
    char* literalOut,
    int* literalLen,
    int literalCap,
    bool applyMarkup = true)
{
    if (!p || !*p || **p != '^')
        return false;

    const char* s = *p;

    if (s[1] && Text_IsMarkupCode(s[1])) {
        if (applyMarkup) {
            char code = s[1];
            if (TextStyle_IsCode(code))
                TextStyle_ApplyCode(code, segState);
            else if (TextColor_IsCode(code)) {
                if (colorNum && currentRgb)
                    TextColor_ApplyEscape(code, allowDigitCodes, colorNum, currentRgb, richText);
                if (segState) {
                    if (currentRgb)
                        TextColor_Copy(currentRgb, segState->color);
                    else
                        TextColor_Lookup(code, colWhite, segState->color);
                    if (code == '7') {
                        segState->rainbow = false;
                        segState->blink = false;
                        segState->pulse = false;
                    }
                }
            }
        }
        *p = s + 2;
        return true;
    }

    if (literalOut && literalLen) {
        if (*literalLen < literalCap)
            literalOut[(*literalLen)++] = '^';
    }
    *p = s + 1;
    return true;
}

// Map raw edit-buffer cursor index to visible char index.
static int Text_MapRawCursorToVisibleInput(const char* text, int rawCursor)
{
    if (!text || rawCursor <= 0)
        return 0;

    int rawIdx = 0;
    int visIdx = 0;

    while (text[rawIdx] && rawIdx < rawCursor) {
        if (text[rawIdx] == '^') {
            const char* p = text + rawIdx;
            char litBuf[8];
            int litLen = 0;
            int dummyColor = 7;
            TextSegmentState dummyState;
            TextSegmentState_Init(&dummyState, colWhite, TEXT_STYLE_NORMAL);
            if (Text_ConsumeCaretMarkup(&p, -1, true, &dummyColor, dummyState.color, &dummyState,
                    litBuf, &litLen, (int)sizeof(litBuf))) {
                rawIdx = (int)(p - text);
                visIdx += litLen;
                continue;
            }
        }
        ++rawIdx;
        ++visIdx;
    }

    return visIdx;
}

static uint8_t TextColor_ToByte(float value)
{
    if (value <= 0.0f)
        return 0;
    if (value >= 1.0f)
        return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

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
#define cl_consoleActiveType           (*((int32_t*)0x00601790))
#define cl_consoleBufferBase           ((uint16_t*)0x005E1784)

#define cg_chatHeightDvar              (*((dvar_t**)0x014C3684))
#define cg_chatSayFadeTimeDvar         (*((dvar_t**)0x0166BB40))
#define cg_hudChatPositionDvar         (*((dvar_t**)0x014C362C))
#define cg_chatMessagesBase            ((char*)0x014EA9FC)
#define cg_chatRingWriteIndex          (*((int32_t*)0x014EB294))
#define cg_chatRingReadIndex           (*((int32_t*)0x014EB298))
#define cg_chatRingTimestamps          ((int32_t*)0x014EB274)
#define cg_chatBackgroundShader        (*((void**)0x014EB2A8))
#define cg_levelTime                   (*((int32_t*)0x01513C30))
#define cg_chatScreenScale             (*((float*)0x00C94C0C))
#define ui_smallFontDvar               (*((dvar_t**)0x019790E0))
#define ui_bigFontDvar                 (*((dvar_t**)0x0196FFFC))
#define ui_extraBigFontDvar            (*((dvar_t**)0x0196FFF4))

static bool Text_EscapeAmpDigitInLocalizeArgs(const char* src, char* dst, size_t dstCap)
{
    if (!src || !dst || dstCap == 0)
        return false;

    bool changed = false;
    bool hasArgDelim = strchr(src, 0x14) != NULL;
    bool inArgs = !hasArgDelim; // Fallback: plain string (no delimiters) -> sanitize whole text.
    size_t j = 0;

    for (size_t i = 0; src[i] && j < dstCap - 1; ++i) {
        if (!inArgs && src[i] == 0x14)
            inArgs = true;

        // Escapes exactly "&&1" in localization arguments so SEH_LocalizeTextMessage
        // does not treat a player name as the &&1 name/proxy placeholder.
        // Other placeholders (&&2, &&3, ...) are left untouched.
        // Reset BEFORE the first literal '&' so any active extended style (e.g. ^#)
        // does not bleed onto that first ampersand.
        if (inArgs && src[i] == '&' && src[i + 1] == '&' && src[i + 2] == '1' &&
            (src[i + 3] == '\0' || src[i + 3] < '0' || src[i + 3] > '9')) {
            const char* esc = "^7&^7&^7";
            for (int k = 0; esc[k] && j < dstCap - 1; ++k)
                dst[j++] = esc[k];
            ++i; // consume the second '&'
            changed = true;
            continue;
        }

        dst[j++] = src[i];
    }

    dst[j] = '\0';
    return changed;
}

static void* __cdecl Chat_SEH_LocalizeTextMessage_ChatSafe(char* a1, int a2, int a3)
{
    auto original = (void* (__cdecl*)(char*, int, int))0x004AB700;
    if (!a1 || !*a1)
        return original(a1, a2, a3);

    char sanitized[1024];
    bool changed = Text_EscapeAmpDigitInLocalizeArgs(a1, sanitized, sizeof(sanitized));

    if (changed)
        return original(sanitized, a2, a3);

    return original(a1, a2, a3);
}

static bool Text_HasResetBefore(const char* text, const char* pos)
{
    if (!text || !pos || pos < text)
        return false;

    if (pos - text >= 2 && pos[-2] == '^' && pos[-1] == '7')
        return true;
    if (pos - text >= 2 && pos[-2] == '^' && pos[-1] == '~')
        return true;
    if (pos - text >= 4 && pos[-4] == '^' && pos[-3] == '~' && pos[-2] == '^' && pos[-1] == '7')
        return true;

    return false;
}

// Normalize escaped-double markup in the first name-like token:
//   "^^kkName: ..." -> "^kName: ..."
//   "^^kkName"      -> "^kName"
// Keeps other patterns unchanged (e.g. "^5^5Name").
static bool Text_NormalizeEscapedDoubleMarkupInNameToken(char* text, size_t cap)
{
    if (!text || cap < 4)
        return false;

    const char* nameEnd = text;
    while (*nameEnd && *nameEnd != ' ' && *nameEnd != ':')
        ++nameEnd;
    if (nameEnd == text)
        return false;

    char rebuilt[272];
    if (cap > sizeof(rebuilt))
        cap = sizeof(rebuilt);

    char* out = rebuilt;
    char* outEnd = rebuilt + cap - 1;
    const char* in = text;
    bool changed = false;
    bool prevWasMarkup = false;
    char prevMarkupCode = 0;

    while (in < nameEnd && *in && out < outEnd) {
        char code = 0;
        bool haveMarkup = false;

        if ((in + 3) < nameEnd &&
            in[0] == '^' && in[1] == '^' &&
            Text_IsMarkupCode(in[2]) &&
            in[3] == in[2]) {
            code = in[2];
            in += 4;
            changed = true;
            haveMarkup = true;
        } else if ((in + 1) < nameEnd &&
                   in[0] == '^' &&
                   Text_IsMarkupCode(in[1])) {
            code = in[1];
            in += 2;
            haveMarkup = true;
        }

        if (haveMarkup) {
            // Collapse duplicated non-vanilla markup pairs (^k^k -> ^k).
            if ((code < '0' || code > '9') && prevWasMarkup && prevMarkupCode == code) {
                changed = true;
                continue;
            }
            if (out < outEnd) *out++ = '^';
            if (out < outEnd) *out++ = code;
            prevWasMarkup = true;
            prevMarkupCode = code;
            continue;
        }

        *out++ = *in++;
        prevWasMarkup = false;
        prevMarkupCode = 0;
    }

    while (*in && out < outEnd)
        *out++ = *in++;
    *out = '\0';

    if (!changed)
        return false;

    strncpy(text, rebuilt, cap - 1);
    text[cap - 1] = '\0';
    return true;
}

// Normalize escaped/doubled markup across the whole line:
//   "^^kkName"   -> "^kName"
//   "^k^kName"   -> "^kName"   (non-digit codes only)
// Used for system lines where the colored name is not the first token
// (e.g. "renamed to ^^kkName").
static bool Text_NormalizeEscapedDoubleMarkupGlobal(char* text, size_t cap)
{
    if (!text || cap < 4)
        return false;

    char rebuilt[1024];
    if (cap > sizeof(rebuilt))
        cap = sizeof(rebuilt);

    const char* in = text;
    char* out = rebuilt;
    char* outEnd = rebuilt + cap - 1;
    bool changed = false;
    bool prevWasMarkup = false;
    char prevMarkupCode = 0;

    while (*in && out < outEnd) {
        char code = 0;
        bool haveMarkup = false;

        if (in[0] == '^' && in[1] == '^' && in[2] && in[3] &&
            Text_IsMarkupCode(in[2]) && in[3] == in[2]) {
            code = in[2];
            in += 4;
            changed = true;
            haveMarkup = true;
        } else if (in[0] == '^' && in[1] && Text_IsMarkupCode(in[1])) {
            code = in[1];
            in += 2;
            haveMarkup = true;
        }

        if (haveMarkup) {
            if ((code < '0' || code > '9') && prevWasMarkup && prevMarkupCode == code) {
                changed = true;
                continue;
            }
            if (out + 1 >= outEnd)
                break;
            *out++ = '^';
            *out++ = code;
            prevWasMarkup = true;
            prevMarkupCode = code;
            continue;
        }

        *out++ = *in++;
        prevWasMarkup = false;
        prevMarkupCode = 0;
    }

    while (*in && out < outEnd)
        *out++ = *in++;
    *out = '\0';

    if (!changed)
        return false;

    strncpy(text, rebuilt, cap - 1);
    text[cap - 1] = '\0';
    return true;
}

// Chat ring-buffer still has vanilla-only consumers that only understand ^0..^9.
// For extended ^a..^z colors, emit a deterministic vanilla fallback digit so
// those paths still receive a stable color state, then apply the true extended
// code for our renderer.
static char Text_NearestVanillaDigitForCode(char code)
{
    // Do not use ^8/^9 (team-dynamic colors) for chat fallback.
    // Chat background should be stable and deterministic across teams/maps.
    // Mapping targets vanilla 0..7 only, tuned for visible parity.
    switch (code)
    {
        case 'a': return '3'; // Gold -> Yellow
        case 'b': return '5'; // Teal -> Cyan
        case 'c': return '6'; // Violet -> Pink
        case 'd': return '5'; // Sky -> Cyan
        case 'e': return '6'; // Plum -> Pink
        case 'f': return '5'; // Ice -> Cyan-ish (avoid white fallback)
        case 'g': return '2'; // Mint -> Green
        case 'h': return '2'; // Pine -> Green
        case 'i': return '1'; // Maroon -> Red
        case 'j': return '1'; // Crimson -> Red
        case 'k': return '1'; // Chocolate -> Red/Brown approximation
        case 'l': return '3'; // Tan -> Yellow
        case 'm': return '3'; // Chartreuse -> Yellow
        case 'n': return '3'; // Peach -> Yellow
        case 'o': return '3'; // Ivory -> Yellow-ish (visible over white)
        case 'p': return '4'; // Midnight -> Blue
        case 'q': return '0'; // Espresso -> Black
        case 'r': return '1'; // Copper -> Red
        case 's': return '2'; // Olive -> Green
        case 't': return '2'; // Fern -> Green
        case 'u': return '5'; // Aqua -> Cyan
        case 'v': return '1'; // Scarlet -> Red
        case 'w': return '7'; // Silver -> White
        case 'y': return '2'; // Emerald -> Green
        case 'z': return '1'; // Coral -> Red
        default:
            return '7';
    }
}

// Vanilla-style behavior: when the line starts with markup-colored name and then
// plain text begins, force a reset right at the first separator.
// Example: "^jPlayer connected" -> "^jPlayer^~^7 connected"
static bool Text_EnsureVanillaNameResetAtFirstSeparator(char* text, size_t cap)
{
    if (!text || cap < 8 || !Text_ContainsActiveMarkup(text))
        return false;

    const char* p = text;
    bool sawLeadingMarkup = false;
    while (p[0] == '^' && p[1] && Text_IsMarkupCode(p[1])) {
        sawLeadingMarkup = true;
        p += 2;
    }
    if (!sawLeadingMarkup)
        return false;

    const char* nameStart = p;
    while (*p && *p > ' ' && *p != ':')
        ++p;

    const char* sep = p;
    if (!sep || *sep != ' ')
        return false;
    if (sep == nameStart)
        return false;
    if ((size_t)(sep - nameStart) > 48) // keep scope close to player-name-like prefixes
        return false;
    if (Text_HasResetBefore(text, sep))
        return false;

    // Require actual trailing content to avoid rewriting one-word messages.
    const char* tail = sep + 1;
    while (*tail == ' ')
        ++tail;
    if (!*tail)
        return false;

    size_t tailLen = strlen(sep);
    size_t used = (size_t)(sep - text);
    if (used + 4 + tailLen + 1 > cap)
        return false;

    memmove((char*)sep + 4, sep, tailLen + 1);
    ((char*)sep)[0] = '^';
    ((char*)sep)[1] = '~';
    ((char*)sep)[2] = '^';
    ((char*)sep)[3] = '7';
    return true;
}

// write character into console buffer (equivalent to the disassembly)
void CL_WriteConsoleChar(uint32_t frameIndex, uint8_t ch, uint8_t colorNum)
{
    int frame = cl_consoleFrameCounter % cl_consoleTotalBuffers;
    uint32_t dst = frame * cl_consoleBufferSize + cl_consoleBufferPos;
    cl_consoleBufferBase[dst] = (uint16_t)((colorNum << 8) | ch);
    cl_consoleBufferPos++;
}

void CL_WriteConsoleColorControl(uint8_t colorChannel, uint8_t value)
{
    if (cl_consoleBufferPos >= cl_consoleBufferSize)
        return;

    int frame = cl_consoleFrameCounter % cl_consoleTotalBuffers;
    uint32_t dst = frame * cl_consoleBufferSize + cl_consoleBufferPos;
    cl_consoleBufferBase[dst] = (uint16_t)((colorChannel << 8) | value);
    cl_consoleBufferPos++;
}

void CL_WriteConsoleRgbColor(const float* color)
{
    CL_WriteConsoleColorControl(10, TextColor_ToByte(color[0]));
    CL_WriteConsoleColorControl(11, TextColor_ToByte(color[1]));
    CL_WriteConsoleColorControl(12, TextColor_ToByte(color[2]));
}

static void CL_WriteConsoleStyle(int style)
{
    CL_WriteConsoleColorControl(CON_CTRL_STYLE, (uint8_t)(style & 0xff));
}

static void CL_WriteConsoleFxFlags(const TextSegmentState* state)
{
    uint8_t flags = 0;
    if (state && state->rainbow) flags |= FX_RAINBOW;
    if (state && state->blink) flags |= FX_BLINK;
    if (state && state->pulse) flags |= FX_PULSE;
    CL_WriteConsoleColorControl(CON_CTRL_FX, flags);
}

// Wrapper around the EXE's Con_FlushLine (sub_404340) that finishes a console
// buffer line and copies it to the chat-HUD/notify ring buffers.
typedef int (__fastcall *Con_FlushLine_t)(int a1, int a2);
#define EXE_Con_FlushLine ((Con_FlushLine_t)0x00404340)

// Helper: returns RGB for a base color number (0-9). Used to emit RGB control codes
// for standard color codes too, so the chat-HUD wrapper can read them uniformly.
static bool TextColor_TryReadTeamColorDvar(const char* dvarName, float out[4])
{
    if (!dvarName || !out)
        return false;

    dvar_t* dvar = Dvar_GetDvarByName(dvarName);
    if (!dvar)
        return false;

    float parsed[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    Dvar_StringToColor(dvar, &parsed);
    TextColor_Copy(parsed, out);
    out[3] = 1.0f;
    return true;
}

static void TextColor_FromColorNum(int colorNum, float out[4])
{
    // Vanilla renderer resolves ^8/^9 dynamically to live team colors
    // (RB_LookupColor -> dword_11E1AC4 / dword_11E1AC0). Mirror that here
    // whenever we need RGB-decoded output in custom paths.
    if (colorNum == 8 && TextColor_TryReadTeamColorDvar("g_TeamColor_Allies", out))
        return;
    if (colorNum == 9 && TextColor_TryReadTeamColorDvar("g_TeamColor_Axis", out))
        return;

    char digit = '0' + (colorNum & 0x0f);
    if (colorNum < 0 || colorNum > 9 || !TextColor_Lookup(digit, colWhite, out)) {
        out[0] = 1.0f; out[1] = 1.0f; out[2] = 1.0f; out[3] = 1.0f;
    }
}

// Closely mirrors the disassembled function, with extended-color support unified
// through RGB control codes (channels 10/11/12) so that the chat-HUD wrapper can
// pick up arbitrary colors.
void CL_AddConsoleText(int32_t color)
{
    char* str; ASM__movr(str, "eax");
    if (!str)
        return;

    char lineScratch[1024];
    const char* parseStr = str;
    if (Text_ContainsActiveMarkup(str)) {
        strncpy(lineScratch, str, sizeof(lineScratch) - 1);
        lineScratch[sizeof(lineScratch) - 1] = '\0';
        Text_NormalizeEscapedDoubleMarkupGlobal(lineScratch, sizeof(lineScratch));
        Text_NormalizeEscapedDoubleMarkupInNameToken(lineScratch, sizeof(lineScratch));
        Text_EnsureVanillaNameResetAtFirstSeparator(lineScratch, sizeof(lineScratch));
        parseStr = lineScratch;
    }

    int32_t colorNum = color;
    char *p = (char*)parseStr;
    float currentRgb[4] = { 1, 1, 1, 1 };
    float lastEmittedRgb[4] = { -1, -1, -1, -1 };
    bool useRgbEncoding = false;
    TextSegmentState currentState;
    TextSegmentState lastEmittedState;
    TextSegmentState_Init(&currentState, currentRgb, TEXT_STYLE_NORMAL);
    TextSegmentState_Init(&lastEmittedState, currentRgb, TEXT_STYLE_NORMAL);
    lastEmittedState.color[0] = -1.0f;

    if (colorNum < 0)
        colorNum = 7;

    int32_t frameIndex = cl_consoleFrameCounter % cl_consoleTotalBuffers;

    // Initialize current color from the base colorNum.
    TextColor_FromColorNum(colorNum, currentRgb);
    TextColor_Copy(currentRgb, currentState.color);

    const bool richText = true;

    auto emitRgbIfChanged = [&]() {
        if (!useRgbEncoding)
            return;
        if (currentRgb[0] != lastEmittedRgb[0] ||
            currentRgb[1] != lastEmittedRgb[1] ||
            currentRgb[2] != lastEmittedRgb[2]) {
            CL_WriteConsoleRgbColor(currentRgb);
            lastEmittedRgb[0] = currentRgb[0];
            lastEmittedRgb[1] = currentRgb[1];
            lastEmittedRgb[2] = currentRgb[2];
        }
    };

    auto emitStyleIfChanged = [&]() {
        if (!TextSegmentState_StyleFxEqual(&currentState, &lastEmittedState)) {
            CL_WriteConsoleStyle(currentState.style);
            CL_WriteConsoleFxFlags(&currentState);
            lastEmittedState = currentState;
        }
    };

    // Load first char
    unsigned char ch1 = (unsigned char)*p;
    unsigned char ch2 = ch1;

    if (ch2) {
        while (cl_consoleBufferPos < cl_consoleBufferSize) {

            // ================================
            // Legacy "single-color" mode: still honor ^-markup so vanilla ^0-^9
            // and extended colors behave consistently in chat/console.
            // ================================
            if (!con_printDoubleColors->value.boolean) {
                if (ch1 == '^') {
                    char litBuf[8];
                    int litLen = 0;
                    const char* markupStart = p;
                    if (Text_ConsumeCaretMarkup((const char**)&p, color, true, &colorNum, currentRgb, &currentState,
                            litBuf, &litLen, (int)sizeof(litBuf))) {
                        if (markupStart[0] == '^' && markupStart[1] && Text_IsMarkupCode(markupStart[1])) {
                            char code = markupStart[1];
                            if (TextStyle_IsCode(code) || Text_IsExtendedColorCode(code))
                                useRgbEncoding = true;
                        }
                        for (int li = 0; li < litLen; ++li) {
                            emitRgbIfChanged();
                            emitStyleIfChanged();
                            CL_WriteConsoleChar((uint32_t)frameIndex, (uint8_t)litBuf[li], (uint8_t)colorNum);
                        }
                    } else {
                        ++p;
                        emitRgbIfChanged();
                        emitStyleIfChanged();
                        CL_WriteConsoleChar((uint32_t)frameIndex, ch2, (uint8_t)colorNum);
                    }

                    ch1 = (unsigned char)*p;
                    ch2 = ch1;
                    if (!ch2) break;
                    continue;
                } else {
                    ++p;
                    if (ch2 != '\n' && ch2 != '\r') {
                        emitRgbIfChanged();
                        emitStyleIfChanged();
                        CL_WriteConsoleChar((uint32_t)frameIndex, ch2, (uint8_t)colorNum);
                    }
                }
            }

            // ================================
            // PROCESS color/style codes (^X)
            // ================================
            else {
                if (ch1 == '^') {
                    char litBuf[8];
                    int litLen = 0;
                    const char* markupStart = p;
                    if (Text_ConsumeCaretMarkup((const char**)&p, color, richText, &colorNum, currentRgb, &currentState,
                            litBuf, &litLen, (int)sizeof(litBuf))) {
                        if (markupStart[0] == '^' && markupStart[1] && Text_IsMarkupCode(markupStart[1])) {
                            char code = markupStart[1];
                            if (TextStyle_IsCode(code) || Text_IsExtendedColorCode(code))
                                useRgbEncoding = true;
                        }
                        for (int li = 0; li < litLen; ++li) {
                            emitRgbIfChanged();
                            emitStyleIfChanged();
                            CL_WriteConsoleChar((uint32_t)frameIndex, (uint8_t)litBuf[li], (uint8_t)colorNum);
                        }
                    } else {
                        ++p;
                        emitRgbIfChanged();
                        emitStyleIfChanged();
                        CL_WriteConsoleChar((uint32_t)frameIndex, ch2, (uint8_t)colorNum);
                    }
                } else {
                    p++;

                    if (ch2 != '\n' && ch2 != '\r') {
                        emitRgbIfChanged();
                        emitStyleIfChanged();
                        CL_WriteConsoleChar((uint32_t)frameIndex, ch2, (uint8_t)colorNum);
                    }
                }
            }

            ch1 = (unsigned char)*p;
            ch2 = ch1;
            if (!ch2)
                break;
        }
    }
}

static void R_DrawPlainTextSegment(const char* text, int textLen, fontHandle_t* font, float* x, float y, float xScale, float yScale, const TextSegmentState* state)
{
    if (!original_R_DrawText || textLen <= 0 || !state)
        return;

    float drawColor[4];
    TextSegmentState_FinalizeColor(state, drawColor);
    int engineStyle = TextSegmentState_GetEngineStyle(state);
    R_TextWidth_t textWidth = original_R_TextWidth ? original_R_TextWidth : GFX_REFAPI_R_TEXTWIDTH;

    // The engine's R_DrawText still parses ^X color codes. This text is already
    // markup-stripped, so every '^' must render as a literal caret. The engine
    // only renders a caret verbatim when it sits at the end of the string or is
    // followed by another caret ("ab^^" -> "ab^^"); a caret directly before a
    // normal char ("^a") would be misread as a color code. So we cut the text
    // into pieces that each END in their caret run: "<non-caret chars><carets>".
    char piece[1024];
    int i = 0;
    while (i < textLen)
    {
        int start = i;
        while (i < textLen && text[i] != '^')
            ++i;
        while (i < textLen && text[i] == '^')
            ++i;

        int n = i - start;
        if (n >= (int)sizeof(piece)) {
            n = (int)sizeof(piece) - 1;
            i = start + n; // keep the remainder for the next iteration
        }
        memcpy(piece, text + start, n);
        piece[n] = '\0';

        original_R_DrawText(piece, n, font, *x, y, xScale, yScale, drawColor, engineStyle);
        if (textWidth)
            *x += (float)textWidth(piece, n, font) * xScale;
    }
}

int __cdecl R_TextWidth_Extended(const char* text, int maxChars, fontHandle_t* font)
{
    drawing_ensure_text_hooks();

    if (!original_R_TextWidth || !text)
        return 0;

    if (!Text_NeedsExtendedHandling(text))
        return original_R_TextWidth(text, maxChars, font);

    int maxVisibleChars = (maxChars < 0) ? 0x7fffffff : maxChars;
    int visible = 0;
    int width = 0;

    for (const char* p = text; *p && visible < maxVisibleChars; ) {
        if (*p == '^') {
            char litBuf[8];
            int litLen = 0;
            int dummyColor = 7;
            TextSegmentState dummyState;
            TextSegmentState_Init(&dummyState, colWhite, TEXT_STYLE_NORMAL);
            if (Text_ConsumeCaretMarkup(&p, -1, true, &dummyColor, dummyState.color, &dummyState,
                    litBuf, &litLen, (int)sizeof(litBuf), false)) {
                for (int i = 0; i < litLen && visible < maxVisibleChars; ++i) {
                    char ch[2] = { litBuf[i], '\0' };
                    width += original_R_TextWidth(ch, 1, font);
                    ++visible;
                }
                continue;
            }
        }

        char ch[2] = { *p++, '\0' };
        width += original_R_TextWidth(ch, 1, font);
        ++visible;
    }

    return width;
}

// Returns true if the text was processed with extended markup, false otherwise.
static bool R_DispatchExtendedText(const char* text, int maxChars, fontHandle_t* font, float* x, float y, float xScale, float yScale, const float* color, int style)
{
    if (!text || !x || !Text_TextHasCaret(text))
        return false;

    TextSegmentState segmentState;
    TextSegmentState_Init(&segmentState, color, style);

    const char* segmentStart = text;
    const char* p = text;
    int visibleChars = 0;
    int maxVisibleChars = (maxChars < 0) ? 0x7fffffff : maxChars;

    while (*p && visibleChars < maxVisibleChars)
    {
        if (*p == '^')
        {
            if (p > segmentStart)
                R_DrawPlainTextSegment(segmentStart, (int)(p - segmentStart), font, x, y, xScale, yScale, &segmentState);

            char litBuf[8];
            int litLen = 0;
            int dummyColor = 7;
            Text_ConsumeCaretMarkup(&p, -1, true, &dummyColor, segmentState.color, &segmentState,
                litBuf, &litLen, (int)sizeof(litBuf));

            if (litLen > 0) {
                if (visibleChars + litLen > maxVisibleChars)
                    litLen = maxVisibleChars - visibleChars;
                R_DrawPlainTextSegment(litBuf, litLen, font, x, y, xScale, yScale, &segmentState);
                visibleChars += litLen;
            }

            segmentStart = p;
            continue;
        }

        ++p;
        ++visibleChars;
    }

    R_DrawPlainTextSegment(segmentStart, (int)(p - segmentStart), font, x, y, xScale, yScale, &segmentState);
    return true;
}

// Reimplements sub_404690 (the EXE's "add wrapped text to console buffer" function)
// with extended-color-code awareness. Intended behavior:
//   - writes characters to word_5E1784[] using the same indexing as the original;
//   - passes the `a2` type parameter through to Con_FlushLine unchanged (type 4+
//     means "no chat-HUD/notify ring-buffer copy" — that's how the engine
//     distinguishes regular console prints from chat-visible ones, so we MUST
//     preserve it);
//   - flushes only on '\n' or buffer-fill — never auto-flushes on return so that
//     non-newline-terminated prints stay open until the caller is ready;
//   - supports both standard ^0..^9 and extended (e.g. ^j, ^*) color codes,
//     emitting RGB control bytes (channels 10/11/12) for true-color display.
// Calling convention matches the original: text in EAX, the rest on the stack.

int __cdecl Con_AddText_Extended(int a2, int a3, int a4, int a5)
{
    char* text; ASM__movr(text, "eax");
    if (!text) return 0;

    char lineScratch[1024];
    const char* parseText = text;
    // Apply only on notify/game-message types where name-color bleed is expected.
    // Type 3 covers kill/death/system messages; broader scope can rewrite user text unintentionally.
    if (text && (a2 == 1 || a2 == 2 || a2 == 3) && Text_ContainsActiveMarkup(text)) {
        strncpy(lineScratch, text, sizeof(lineScratch) - 1);
        lineScratch[sizeof(lineScratch) - 1] = '\0';
        Text_NormalizeEscapedDoubleMarkupGlobal(lineScratch, sizeof(lineScratch));
        Text_NormalizeEscapedDoubleMarkupInNameToken(lineScratch, sizeof(lineScratch));
        Text_EnsureVanillaNameResetAtFirstSeparator(lineScratch, sizeof(lineScratch));
        parseText = lineScratch;
    }

    // Match original Con_AddText: flush the previous line when the output type changes.
    if (a2 != cl_consoleActiveType && cl_consoleBufferPos > 0)
        EXE_Con_FlushLine(cl_consoleActiveType, a3);

    int colorNum = (a5 < 0) ? 7 : (a5 & 0xff);
    int frameIndex = cl_consoleFrameCounter % cl_consoleTotalBuffers;

    float currentRgb[4] = { 1, 1, 1, 1 };
    float lastEmittedRgb[4] = { -1, -1, -1, -1 };
    bool useRgbEncoding = false;
    TextSegmentState currentState;
    TextSegmentState lastEmittedState;
    TextSegmentState_Init(&currentState, currentRgb, TEXT_STYLE_NORMAL);
    TextSegmentState_Init(&lastEmittedState, currentRgb, TEXT_STYLE_NORMAL);
    lastEmittedState.color[0] = -1.0f;
    TextColor_FromColorNum(colorNum, currentRgb);
    TextColor_Copy(currentRgb, currentState.color);

    const bool richText = true;

    auto emitRgbIfChanged = [&]() {
        if (!useRgbEncoding)
            return;
        if (currentRgb[0] != lastEmittedRgb[0] ||
            currentRgb[1] != lastEmittedRgb[1] ||
            currentRgb[2] != lastEmittedRgb[2]) {
            CL_WriteConsoleRgbColor(currentRgb);
            lastEmittedRgb[0] = currentRgb[0];
            lastEmittedRgb[1] = currentRgb[1];
            lastEmittedRgb[2] = currentRgb[2];
        }
    };

    auto emitStyleIfChanged = [&]() {
        if (!TextSegmentState_StyleFxEqual(&currentState, &lastEmittedState)) {
            CL_WriteConsoleStyle(currentState.style);
            CL_WriteConsoleFxFlags(&currentState);
            lastEmittedState = currentState;
        }
    };

    for (char* p = (char*)parseText; *p; ) {
        unsigned char ch = (unsigned char)*p;

        if (ch == '^') {
            char litBuf[8];
            int litLen = 0;
            const char* markupStart = p;
            if (Text_ConsumeCaretMarkup((const char**)&p, a5, richText, &colorNum, currentRgb, &currentState,
                    litBuf, &litLen, (int)sizeof(litBuf))) {
                if (markupStart[0] == '^' && markupStart[1] && Text_IsMarkupCode(markupStart[1])) {
                    char code = markupStart[1];
                    if (TextStyle_IsCode(code) || Text_IsExtendedColorCode(code))
                        useRgbEncoding = true;
                }
                for (int li = 0; li < litLen; ++li) {
                    if (cl_consoleBufferPos >= cl_consoleBufferSize)
                        EXE_Con_FlushLine(a2, a3);
                    emitRgbIfChanged();
                    emitStyleIfChanged();
                    CL_WriteConsoleChar((uint32_t)frameIndex, (uint8_t)litBuf[li], (uint8_t)colorNum);
                }
                continue;
            }
        }

        if (ch == '\n') {
            EXE_Con_FlushLine(a2, a3);
            ++p;
            continue;
        }
        if (ch == '\r') { ++p; continue; }

        if (cl_consoleBufferPos >= cl_consoleBufferSize) {
            EXE_Con_FlushLine(a2, a3);
        }

        emitRgbIfChanged();
        emitStyleIfChanged();
        CL_WriteConsoleChar((uint32_t)frameIndex, ch, (uint8_t)colorNum);
        ++p;
    }

    // Match original Con_AddText LABEL_174: auto-flush buffered text on return.
    // iprintln / iprintlnbold rely on this to reach the center-screen notify HUD.
    if (cl_consoleBufferPos <= 0) {
        cl_consoleActiveType = a2;
    } else if (a2) {
        EXE_Con_FlushLine(a2, a3);
        cl_consoleActiveType = a2;
    } else {
        EXE_Con_FlushLine(0, a3);
        cl_consoleActiveType = 0;
    }

    return colorNum;
}

// Reimplements sub_4D0E10 (append a chat line to the HUD ring buffer).
// Extended colors in player names are stored literally; ^7 before ": " keeps
// the message body white while still allowing inline ^X codes in the message.
int __fastcall CG_AddChatMessage(int a1)
{
    char* src = (char*)a1;
    if (!src || !*src)
        return 0;

    char lineScratch[272];
    strncpy(lineScratch, src, sizeof(lineScratch) - 1);
    lineScratch[sizeof(lineScratch) - 1] = '\0';

    Text_NormalizeEscapedDoubleMarkupGlobal(lineScratch, sizeof(lineScratch));
    Text_NormalizeEscapedDoubleMarkupInNameToken(lineScratch, sizeof(lineScratch));

    src = lineScratch;

    int maxLines = cg_chatHeightDvar ? cg_chatHeightDvar->value.integer : 0;
    if (!maxLines || !cg_chatSayFadeTimeDvar || cg_chatSayFadeTimeDvar->value.integer <= 0)
    {
        cg_chatRingReadIndex = 0;
        cg_chatRingWriteIndex = 0;
        return 0;
    }

    int wrapColor = '7';
    char wrapExtended = 0;
    int lineLen = 0;
    int lastSpaceDst = 0;
    bool inNameToken = true;
    int dst = (int)(cg_chatMessagesBase + 271 * (cg_chatRingWriteIndex % maxLines));
    *(char*)dst = '\0';

    while (*src)
    {
        if (lineLen > 89)
        {
            if (lastSpaceDst)
            {
                src += lastSpaceDst - dst + 1;
                dst = lastSpaceDst;
            }
            *(char*)dst = '\0';
            lineLen = 0;
            cg_chatRingTimestamps[cg_chatRingWriteIndex++ % maxLines] = cg_levelTime;
            char* lineStart = cg_chatMessagesBase + 271 * (cg_chatRingWriteIndex % maxLines);
            *lineStart++ = '^';
            *lineStart = (char)wrapColor;
            dst = (int)(lineStart + 1);
            if (wrapExtended) {
                *(char*)dst++ = '^';
                *(char*)dst++ = wrapExtended;
            }
            lastSpaceDst = 0;
            inNameToken = false;
        }

        if (*src == '^' && src[1])
        {
            if (Text_IsMarkupCode(src[1])) {
                char code = src[1];
                if (inNameToken && Text_IsExtendedColorCode(code)) {
                    char fallback = Text_NearestVanillaDigitForCode(code);
                    *(char*)dst++ = '^';
                    *(char*)dst++ = fallback;
                    wrapColor = fallback;
                }
                *(char*)dst++ = *src++;
                *(char*)dst++ = *src++;
                // Vanilla behavior: color markup changes state but does not consume
                // visible line width for wrapping.
                if (code >= '0' && code <= '9') {
                    wrapColor = code;
                    wrapExtended = 0;
                } else if (Text_IsExtendedColorCode(code)) {
                    // Keep an exact extended color state for wrapped continuation lines.
                    wrapExtended = code;
                    if (!inNameToken)
                        wrapColor = Text_NearestVanillaDigitForCode(code);
                } else if (code == '~') {
                    wrapColor = '7';
                    wrapExtended = 0;
                }
                continue;
            }
        }

        if (*src == '^') {
            *(char*)dst++ = *src++;
            ++lineLen;
            continue;
        }

        if (*src == ' ')
            lastSpaceDst = dst;
        if (*src == ':')
            inNameToken = false;

        *(char*)dst++ = *src++;
        ++lineLen;
    }

    *(char*)dst = '\0';
    cg_chatRingTimestamps[cg_chatRingWriteIndex % maxLines] = cg_levelTime;
    int result = cg_chatRingWriteIndex + 1;
    int overflow = cg_chatRingWriteIndex++ + 1 - cg_chatRingReadIndex;
    if (overflow > maxLines)
    {
        result -= maxLines;
        cg_chatRingReadIndex = result;
    }
    return result;
}

static void Text_ResolveLeadingChatColor(const char* text, float out[4])
{
    TextColor_Copy(colWhite, out);
    if (!text || !*text)
        return;

    // Keep stock behavior for vanilla-only lines: background tint is decided by
    // the FIRST "^digit" at string start.
    if (text[0] == '^' && text[1] >= '0' && text[1] <= '9') {
        int colorNum = text[1] - '0';
        TextColor_FromColorNum(colorNum, out);
    }

    // Extended path: if any leading extended color exists, let it override the
    // vanilla fallback so background matches the final extended name color.
    TextSegmentState state;
    TextSegmentState_Init(&state, out, TEXT_STYLE_NORMAL);
    bool sawExtendedColor = false;

    const char* p = text;
    while (p[0] == '^' && p[1] && Text_IsMarkupCode(p[1])) {
        const char code = p[1];
        if (TextColor_IsCode(code)) {
            int colorNum = 7;
            TextColor_ApplyEscape(code, -1, &colorNum, state.color, true);
            if (Text_IsExtendedColorCode(code))
                sawExtendedColor = true;
        } else {
            TextStyle_ApplyCode(code, &state);
        }
        p += 2;
    }

    if (sawExtendedColor)
        TextColor_Copy(state.color, out);
}

static void Text_BuildVisibleChatText(const char* text, char* out, size_t outCap)
{
    if (!out || outCap == 0)
        return;
    out[0] = '\0';
    if (!text || !*text)
        return;

    const char* p = text;
    size_t j = 0;
    int dummyColor = 7;
    TextSegmentState dummyState;
    TextSegmentState_Init(&dummyState, colWhite, TEXT_STYLE_NORMAL);

    while (*p && j < outCap - 1) {
        if (*p == '^') {
            char litBuf[8];
            int litLen = 0;
            if (Text_ConsumeCaretMarkup(&p, -1, true, &dummyColor, dummyState.color, &dummyState,
                litBuf, &litLen, (int)sizeof(litBuf), false)) {
                for (int li = 0; li < litLen && j < outCap - 1; ++li)
                    out[j++] = litBuf[li];
                continue;
            }
        }

        out[j++] = *p++;
    }

    out[j] = '\0';
}

static fontHandle_t* Text_GetVanillaChatFont()
{
    // Mirror the stock CG_DrawChatMessages font pick logic (sub_4C7760 path):
    // effectiveScale = flt_C94C0C * 0.20833333f, then threshold-based handle select.
    float effectiveScale = cg_chatScreenScale * 0.20833333f;

    fontHandle_t* chosen = fontSmall;
    dvar_t* small = ui_smallFontDvar;
    dvar_t* big = ui_bigFontDvar;
    dvar_t* extraBig = ui_extraBigFontDvar;

    if (small && effectiveScale > small->value.decimal) {
        if (big && effectiveScale < big->value.decimal) {
            chosen = fontBig;
            if (extraBig && effectiveScale < extraBig->value.decimal)
                chosen = fontNormal;
        } else {
            chosen = fontExtraBigSmall;
        }
    }

    if (!chosen)
        chosen = fontSmall ? fontSmall : fontNormal;
    return chosen;
}

// Custom chat draw path: keep vanilla timing/placement, but compute background color
// from full leading markup (^a-^z / style prefixes) instead of digit-only ^0-^9.
int __cdecl CG_DrawChatMessages_Extended()
{
    const int maxLines = cg_chatHeightDvar ? cg_chatHeightDvar->value.integer : 0;
    if (maxLines <= 0)
        return 0;

    const int chatTime = (cg_chatSayFadeTimeDvar ? cg_chatSayFadeTimeDvar->value.integer : 0);
    if (chatTime <= 0) {
        cg_chatRingReadIndex = 0;
        cg_chatRingWriteIndex = 0;
        return 0;
    }

    if (!cg_hudChatPositionDvar || !cg_hudChatPositionDvar->value.vec2)
        return 0;

    float* hudPos = cg_hudChatPositionDvar->value.vec2;
    const int chatX = (int)hudPos[0];
    const int chatY = (int)hudPos[1];

    int readIdx = cg_chatRingReadIndex;
    const int writeIdx = cg_chatRingWriteIndex;
    if (readIdx == writeIdx)
        return 0;

    if (cg_levelTime - cg_chatRingTimestamps[readIdx % maxLines] > chatTime) {
        cg_chatRingReadIndex = readIdx + 1;
        readIdx = cg_chatRingReadIndex;
    }

    fontHandle_t* chatFont = Text_GetVanillaChatFont();
    if (!chatFont)
        return 0;

    void* chatBgShader = cg_chatBackgroundShader;
    if (!chatBgShader)
        chatBgShader = shaderWhite;

    for (int line = writeIdx - 1; line >= readIdx; --line) {
        const int slot = line % maxLines;
        float life = (float)chatTime - (float)(cg_levelTime - cg_chatRingTimestamps[slot]);
        if (life <= 0.0f)
            continue;

        float alpha = (life > 200.0f) ? 0.6f : (life / 200.0f) * 0.6f;
        if (alpha <= 0.0f)
            continue;

        const char* lineText = cg_chatMessagesBase + 271 * slot;
        if (!lineText || !*lineText)
            continue;

        vec4_t bgColor;
        Text_ResolveLeadingChatColor(lineText, bgColor);
        bgColor[0] *= 0.25f;
        bgColor[1] *= 0.25f;
        bgColor[2] *= 0.25f;
        bgColor[3] = alpha;

        const float y = (float)(chatY - 10 * (writeIdx - line));
        // Vanilla formula is still: UI_TextWidth(...) + 24.
        // For extended markup, measure the exact visible stream (same caret semantics as draw path).
        char visibleText[271];
        Text_BuildVisibleChatText(lineText, visibleText, sizeof(visibleText));
        const int textWidth = UI_TextWidth(visibleText, 0, chatFont, 0.20833333f);
        UI_DrawHandlePic(0.0f, y, (float)(textWidth + 24), 10.0f,
            HORIZONTAL_ALIGN_LEFT, VERTICAL_ALIGN_TOP, bgColor, chatBgShader);

        float textAlpha = (life > 200.0f) ? 1.0f : (life / 200.0f);
        vec4_t textColor = { 1.0f, 1.0f, 1.0f, textAlpha };
        UI_DrawText(lineText, 0x7FFFFFFF, chatFont, (float)chatX, y + 9.0f,
            HORIZONTAL_ALIGN_LEFT, VERTICAL_ALIGN_TOP, 0.20833333f, textColor, TEXT_STYLE_SHADOWED);
    }

    return 0;
}

char* __cdecl R_AddCmdDrawTextWithCursor_Extended(const char* text, int maxChars, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style, int cursorPos, char cursor)
{
    drawing_ensure_text_hooks();

    if (!drawing_originals_valid()) {
        R_AddCmdDrawTextWithCursor_t cur = GFX_REFAPI_R_ADDCMDDRAWTEXTWITHCURSOR;
        if (cur && !drawing_is_our_draw_with_cursor(cur))
            return cur(text, maxChars, font, x, y, xScale, yScale, color, style, cursorPos, cursor);
        return NULL;
    }

    // Keep edit-field behavior stock unless real extended markup is present.
    if (!text || !Text_NeedsExtendedHandling(text))
        return original_R_AddCmdDrawTextWithCursor(text, maxChars, font, x, y, xScale, yScale, color, style, cursorPos, cursor);

    R_TextWidth_t textWidth = original_R_TextWidth ? original_R_TextWidth : GFX_REFAPI_R_TEXTWIDTH;

    // Pass 1: build visible chars using edit-field rules (^X = color, ^ = literal, ^^ = two literals).
    struct VC { char ch; TextSegmentState state; };
    VC visible[1024];
    int visCount = 0;
    int maxVisible = (maxChars < 0) ? (int)(sizeof(visible) / sizeof(visible[0])) : maxChars;
    if (maxVisible > (int)(sizeof(visible) / sizeof(visible[0])))
        maxVisible = (int)(sizeof(visible) / sizeof(visible[0]));

    TextSegmentState curState;
    TextSegmentState_Init(&curState, color, style);

    for (const char* p = text; *p && visCount < maxVisible; ) {
        if (*p == '^') {
            char litBuf[8];
            int litLen = 0;
            int dummyColor = 7;
            if (Text_ConsumeCaretMarkup(&p, -1, true, &dummyColor, curState.color, &curState,
                    litBuf, &litLen, (int)sizeof(litBuf))) {
                for (int li = 0; li < litLen && visCount < maxVisible; ++li) {
                    visible[visCount].ch = litBuf[li];
                    visible[visCount].state = curState;
                    ++visCount;
                }
                continue;
            }
        }
        visible[visCount].ch = *p++;
        visible[visCount].state = curState;
        ++visCount;
    }

    int visCursorPos = Text_MapRawCursorToVisibleInput(text, cursorPos);

    float runX = x;
    int i = 0;
    while (i < visCount) {
        int j = i + 1;
        while (j < visCount && TextSegmentState_Equal(&visible[i].state, &visible[j].state))
            ++j;

        char run[1024];
        char drawRun[1024];
        int runLen = j - i;
        if (runLen >= (int)sizeof(run)) runLen = (int)sizeof(run) - 1;
        for (int k = 0; k < runLen; ++k)
            run[k] = visible[i + k].ch;
        // RB_DrawTextWithCursor draws '^' literally (only ^0..^9 change color; no ^^ collapse).
        // Visible chars are already markup-decoded — never double carets like R_DrawText.
        memcpy(drawRun, run, runLen);
        drawRun[runLen] = '\0';
        int drawLen = runLen;

        bool isLastRun = (j >= visCount);

        int relCursor = -1;
        char relCursorCh = 0;
        if (cursor != 0 || cursorPos >= 0) {
            if (visCursorPos >= i && visCursorPos < j) {
                relCursor = visCursorPos - i;
                relCursorCh = cursor;
            } else if (isLastRun && visCursorPos >= visCount) {
                relCursor = runLen;
                relCursorCh = cursor;
            }
        }

        float drawColor[4];
        TextSegmentState_FinalizeColor(&visible[i].state, drawColor);
        original_R_AddCmdDrawTextWithCursor(drawRun, drawLen, font, runX, y, xScale, yScale, drawColor, TextSegmentState_GetEngineStyle(&visible[i].state), relCursor, relCursorCh);

        if (textWidth)
            runX += (float)textWidth(drawRun, drawLen, font) * xScale;

        i = j;
    }

    return (char*)text;
}

char* __cdecl R_DrawText_Extended(const char* text, int maxChars, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style)
{
    drawing_ensure_text_hooks();

    if (!drawing_originals_valid()) {
        R_DrawText_t cur = GFX_REFAPI_R_DRAWTEXT;
        if (cur && !drawing_is_our_drawtext(cur))
            return cur(text, maxChars, font, x, y, xScale, yScale, color, style);
        return NULL;
    }

    // Only intercept text that actually needs extended handling. Pure vanilla
    // (^0..^9 / plain) is rendered by the engine itself, exactly like stock CoD2.
    if (!text || !Text_NeedsExtendedHandling(text))
        return original_R_DrawText(text, maxChars, font, x, y, xScale, yScale, color, style);

    float drawX = x;
    if (!R_DispatchExtendedText(text, maxChars, font, &drawX, y, xScale, yScale, color, style))
        return original_R_DrawText(text, maxChars, font, x, y, xScale, yScale, color, style);

    return (char*)text;
}

char* __cdecl R_AddCmdDrawTextInSpace_Extended(const char* text, int maxChars, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style)
{
    drawing_ensure_text_hooks();

    if (!drawing_originals_valid()) {
        R_DrawText_t cur = GFX_REFAPI_R_ADDCMDDRAWTEXTINSPACE;
        if (cur && !drawing_is_our_draw_in_space(cur))
            return cur(text, maxChars, font, x, y, xScale, yScale, color, style);
        return NULL;
    }

    // Only intercept text that actually needs extended handling (see R_DrawText_Extended).
    if (!text || !Text_NeedsExtendedHandling(text))
        return original_R_AddCmdDrawTextInSpace(text, maxChars, font, x, y, xScale, yScale, color, style);

    float drawX = x;
    if (!R_DispatchExtendedText(text, maxChars, font, &drawX, y, xScale, yScale, color, style))
        return original_R_AddCmdDrawTextInSpace(text, maxChars, font, x, y, xScale, yScale, color, style);

    return (char*)text;
}

// Walks the console-buffer (uint16_t entries: high=color/control, low=char) and
// renders it via R_DrawText, decoding our extended RGB control codes
// (channels 10/11/12) along the way. This makes the chat HUD and main console
// scrollback honor extended (non-^0..^9) colors written by CL_AddConsoleText.
char* __cdecl R_DrawConsoleText_Extended(const uint16_t* buffer, int count, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style)
{

    if (!buffer || count <= 0)
        return original_R_DrawConsoleText ? original_R_DrawConsoleText(buffer, count, font, x, y, xScale, yScale, color, style) : NULL;

    if (!drawing_originals_valid())
        return original_R_DrawConsoleText(buffer, count, font, x, y, xScale, yScale, color, style);

    // Kill-feed lines that use only vanilla encoding (color indices 0-9 plus
    // weapon-icon blocks, no extended RGB/style/FX) are left to the stock engine.
    // It resolves ^8/^9 to the LIVE team colors via RB_LookupColor and advances
    // past weapon icons with the exact glyph widths — both of which our custom
    // renderer can only approximate (it showed ^8 as a static yellow and let the
    // Keep kill-feed vanilla-rendered for exact icon spacing and team-color
    // behavior. If our extended style/FX control bytes (20/21) are present,
    // strip them first because stock renderer doesn't understand them.
    if (ConsoleBuffer_IsKillFeed(buffer, count)) {
        bool hasStyleFx = false;
        for (int i = 0; i < count; ++i) {
            uint8_t hi = (uint8_t)((buffer[i] >> 8) & 0xff);
            if (hi == CON_CTRL_STYLE || hi == CON_CTRL_FX) {
                hasStyleFx = true;
                break;
            }
        }

        drawing_set_vanilla_text_hooks(true);
        char* ret = NULL;
        if (!hasStyleFx) {
            ret = original_R_DrawConsoleText(buffer, count, font, x, y, xScale, yScale, color, style);
        } else {
            uint16_t sanitized[1024];
            int outCount = 0;
            for (int i = 0; i < count && outCount < (int)(sizeof(sanitized) / sizeof(sanitized[0])); ++i) {
                uint8_t hi = (uint8_t)((buffer[i] >> 8) & 0xff);
                if (hi == CON_CTRL_STYLE || hi == CON_CTRL_FX)
                    continue;
                sanitized[outCount++] = buffer[i];
            }
            ret = original_R_DrawConsoleText(sanitized, outCount, font, x, y, xScale, yScale, color, style);
        }
        drawing_set_vanilla_text_hooks(false);
        return ret;
    }

    if (!ConsoleBuffer_UsesExtendedEncoding(buffer, count))
        return original_R_DrawConsoleText(buffer, count, font, x, y, xScale, yScale, color, style);

    if (!original_R_DrawText) {
        return original_R_DrawConsoleText(buffer, count, font, x, y, xScale, yScale, color, style);
    }

    float baseColor[4];
    TextSegmentState segmentState;
    TextColor_Copy(color, baseColor);
    TextSegmentState_Init(&segmentState, color, style);

    char segment[1024];
    int segLen = 0;
    float segX = x;
    bool segColorSet = false;

    auto flush = [&]() {
        if (segLen <= 0) return;
        segment[segLen] = '\0';
        // Console-buffer chars are literal (markup was consumed at write time). Re-parsing
        // would misread e.g. literal '^' + "asd" as ^a (Gold) after ^^j / ^^0 style input.
        R_DrawPlainTextSegment(segment, segLen, font, &segX, y, xScale, yScale, &segmentState);
        segLen = 0;
    };

    for (int i = 0; i < count; ++i) {
        uint8_t hi = (uint8_t)((buffer[i] >> 8) & 0xff);
        uint8_t lo = (uint8_t)(buffer[i] & 0xff);

        if (hi == 10 || hi == 11 || hi == 12) {
            // RGB control code: channel 10=R, 11=G, 12=B.
            int idx = hi - 10;
            float v = (float)lo / 255.0f;
            if (segmentState.color[idx] != v) {
                flush();
                segmentState.color[idx] = v;
                segColorSet = true;
            }
            continue;
        }

        if (hi == CON_CTRL_STYLE) {
            if (lo == TEXT_STYLE_BLINK) {
                if (!segmentState.blink) {
                    flush();
                    segmentState.blink = true;
                }
            } else if (lo == TEXT_STYLE_PULSE) {
                if (!segmentState.pulse) {
                    flush();
                    segmentState.pulse = true;
                }
            } else if (segmentState.style != (int)lo) {
                flush();
                segmentState.style = (int)lo;
            }
            continue;
        }

        if (hi == CON_CTRL_FX) {
            bool newRainbow = (lo & FX_RAINBOW) != 0;
            bool newBlink = (lo & FX_BLINK) != 0;
            bool newPulse = (lo & FX_PULSE) != 0;
            if (segmentState.rainbow != newRainbow ||
                segmentState.blink != newBlink ||
                segmentState.pulse != newPulse) {
                flush();
                segmentState.rainbow = newRainbow;
                segmentState.blink = newBlink;
                segmentState.pulse = newPulse;
            }
            continue;
        }

        // Default Quake color index in high byte ('7' = ASCII 55): plain char.
        if (hi == 55 || hi == (uint8_t)'7') {
            if (lo == 0) continue;
            if (segLen + 1 >= (int)sizeof(segment)) flush();
            segment[segLen++] = (char)lo;
            continue;
        }

        // Standard color number in high byte (0-9) — only relevant if RGB was
        // never set (e.g. raw text written by code that didn't go through our
        // CL_AddConsoleText). For safety, map standard colors to their RGB.
        if (!segColorSet) {
            float wanted[4];
            TextColor_FromColorNum((int)hi, wanted);
            wanted[3] = baseColor[3];
            if (wanted[0] != segmentState.color[0] || wanted[1] != segmentState.color[1] ||
                wanted[2] != segmentState.color[2] || wanted[3] != segmentState.color[3]) {
                flush();
                TextColor_Copy(wanted, segmentState.color);
            }
        }

        // Append printable char (0 byte = end-of-string marker; ignore).
        if (lo == 0) continue;
        if (segLen + 1 >= (int)sizeof(segment)) flush();
        segment[segLen++] = (char)lo;
    }

    flush();
    return NULL;
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
    drawing_ensure_text_hooks();
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

static int g_extColorFrameCount = 0;

/** Installs (or re-installs) the extended-color wrappers in the renderer function table.
 *  Must run AFTER the EXE has copied refExport_t into the global table at 0x0068a1e8 —
 *  i.e. NOT inside hook_gfxDll (memcpy happens after the gfx loader returns), but on the
 *  first frame instead. Safe to call multiple times (e.g. after vid_restart). */
void drawing_install_text_hooks() {
    ++g_extColorFrameCount;

    R_DrawText_t                 curDrawText        = GFX_REFAPI_R_DRAWTEXT;
    R_DrawText_t                 curDrawInSpace     = GFX_REFAPI_R_ADDCMDDRAWTEXTINSPACE;
    R_DrawConsoleText_t          curDrawConsole     = GFX_REFAPI_R_DRAWCONSOLETEXT;
    R_AddCmdDrawTextWithCursor_t curDrawWithCursor  = GFX_REFAPI_R_ADDCMDDRAWTEXTWITHCURSOR;

    if (drawing_originals_valid()
        && GFX_REFAPI_R_TEXTWIDTH == (R_TextWidth_t)&R_TextWidth_Extended
        && curDrawText == (R_DrawText_t)&R_DrawText_Extended
        && curDrawInSpace == (R_DrawText_t)&R_AddCmdDrawTextInSpace_Extended
        && curDrawConsole == (R_DrawConsoleText_t)&R_DrawConsoleText_Extended
        && curDrawWithCursor == (R_AddCmdDrawTextWithCursor_t)&R_AddCmdDrawTextWithCursor_Extended) {
        g_extColorInstalled = 1;
        return;
    }

    if (g_extColorInstalled
        && (GFX_REFAPI_R_TEXTWIDTH != (R_TextWidth_t)&R_TextWidth_Extended
            || curDrawText != (R_DrawText_t)&R_DrawText_Extended
            || curDrawInSpace != (R_DrawText_t)&R_AddCmdDrawTextInSpace_Extended
            || curDrawConsole != (R_DrawConsoleText_t)&R_DrawConsoleText_Extended
            || curDrawWithCursor != (R_AddCmdDrawTextWithCursor_t)&R_AddCmdDrawTextWithCursor_Extended)) {
        original_R_TextWidth = NULL;
        original_R_DrawText = NULL;
        original_R_AddCmdDrawTextInSpace = NULL;
        original_R_DrawConsoleText = NULL;
        original_R_AddCmdDrawTextWithCursor = NULL;
        g_extColorInstalled = 0;
    }

    if (!curDrawText || !curDrawInSpace || !curDrawConsole || !curDrawWithCursor)
        return;

    R_TextWidth_t curTextWidth = GFX_REFAPI_R_TEXTWIDTH;
    if (curTextWidth && !drawing_is_our_textwidth(curTextWidth))
        original_R_TextWidth = curTextWidth;
    if (curDrawText && !drawing_is_our_drawtext(curDrawText))
        original_R_DrawText = curDrawText;
    if (curDrawInSpace && !drawing_is_our_draw_in_space(curDrawInSpace))
        original_R_AddCmdDrawTextInSpace = curDrawInSpace;
    if (curDrawConsole && !drawing_is_our_draw_console(curDrawConsole))
        original_R_DrawConsoleText = curDrawConsole;
    if (curDrawWithCursor && !drawing_is_our_draw_with_cursor(curDrawWithCursor))
        original_R_AddCmdDrawTextWithCursor = curDrawWithCursor;

    if (!drawing_originals_valid())
        return;

        g_extColorFrameCount,
        (void*)original_R_DrawText, (void*)original_R_AddCmdDrawTextInSpace,
        (void*)original_R_DrawConsoleText, (void*)original_R_AddCmdDrawTextWithCursor);

    patch_int32(0x0068a314, (int32_t)&R_TextWidth_Extended);
    patch_int32(0x0068a31c, (int32_t)&R_DrawText_Extended);
    patch_int32(0x0068a320, (int32_t)&R_AddCmdDrawTextInSpace_Extended);
    patch_int32(0x0068a328, (int32_t)&R_DrawConsoleText_Extended);
    patch_int32(0x0068a32c, (int32_t)&R_AddCmdDrawTextWithCursor_Extended);

    g_extColorInstalled = 1;
}

static void drawing_ensure_text_hooks()
{
    R_DrawText_t curDrawText = GFX_REFAPI_R_DRAWTEXT;
    R_TextWidth_t curTextWidth = GFX_REFAPI_R_TEXTWIDTH;
    if (!drawing_originals_valid()
        || curDrawText != (R_DrawText_t)&R_DrawText_Extended
        || curTextWidth != (R_TextWidth_t)&R_TextWidth_Extended) {
        drawing_install_text_hooks();
    }
}

/** Called after gfx_d3d_mp_x86_s.dll is loaded. The function table is NOT yet populated
 *  at this point — it is filled by a memcpy that happens AFTER the loader returns.
 *  We therefore defer the table patches to the first frame. */
void drawing_renderer() {

    original_R_DrawText                  = NULL;
    original_R_AddCmdDrawTextInSpace     = NULL;
    original_R_DrawConsoleText           = NULL;
    original_R_AddCmdDrawTextWithCursor  = NULL;
    original_R_TextWidth                 = NULL;
    g_extColorFrameCount = 0;
    g_extColorInstalled = 0;
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

    patch_call(0x004055cb, (unsigned int)CL_AddConsoleText);
    patch_call(0x00405726, (unsigned int)CL_AddConsoleText);

    // CoD2x: also redirect the function ITSELF (sub_405480 = original CL_AddConsoleText)
    // so that every direct call from anywhere in the EXE goes through the extended
    // color processing — not only the two named call sites above.
    patch_jump(0x00405480, (unsigned int)CL_AddConsoleText);

    // CoD2x: sub_404690 is a SECOND console-write function (Con_AddText, used for
    // server messages like "X Connected", "X Joined", with line-wrapping). It has
    // its own ^0..^9-only color parser. Replace its entry with our extended-color
    // implementation so chat-HUD/notify text honors ^j, ^*, ^c, etc. too.
    patch_jump(0x00404690, (unsigned int)Con_AddText_Extended);

    // CoD2x: sub_4D0E10 stores incoming chat lines in the HUD ring buffer.
    patch_jump(0x004D0E10, (unsigned int)CG_AddChatMessage);

    // CoD2x: custom chat draw path so extended name colors can drive exact
    // background tint (vanilla path only reads ^0..^9 at line start).
    patch_jump(0x004C7760, (unsigned int)CG_DrawChatMessages_Extended);

    // Chat/team-chat only: sanitize exactly &&1 inside localization arguments so
    // names like "&&1foo" don't consume the name/proxy placeholder slot.
    patch_call(0x004D1CBF, (unsigned int)Chat_SEH_LocalizeTextMessage_ChatSafe);
    patch_call(0x004D1D1C, (unsigned int)Chat_SEH_LocalizeTextMessage_ChatSafe);

    // Patch end view func
    patch_call(0x00414a8c, (unsigned int)drawing_end);
    patch_nop(0x00414a8c + 5, 1); // Nop rest of the call function, because its calling through pointer

}
