/*
 * CoD2x text codes - colors, rainbow, fonts and background in player names and other texts.
 *
 * A digit after a prefix is always a color, a symbol picks the style of the prefix, '-' ends it:
 *   ^0 .. ^9       vanilla colors (^7 = default color, ^8 = allies, ^9 = axis team color)
 *   $0 .. $9       dark shade of a color family
 *   #0 .. #9       light shade
 *   @0 .. @9       deep shade
 *                  families: 0 brown, 1 red, 2 green, 3 yellow, 4 blue, 5 cyan, 6 pink, 7 grey, 8 orange, 9 violet
 *   $* $-          animation: rainbow on / off
 *   @+ @= @-       font: bold / console (mono) / default
 *   #_ #-          background: on / off; the current color becomes a semi-transparent bar under the
 *                  characters and the text goes back to the color it had before ("@4#_Name" = navy bar
 *                  with the default color, "^8E^1#_Y" in a chat line = red bar with the allies color)
 *   ^7             also ends the rainbow, the font and the background
 *
 * Player names are colored only on the scoreboard. Everywhere else - killfeed, chat, name over the player,
 * "You killed ...", "Following ...", game messages - all codes are removed from the name (strip_text_codes),
 * so it shows in the team or default color. A double code ("^^11", "$$11", ...) works like the single one
 * on the scoreboard, older names made for the killfeed keep their colors there.
 * To show a code as text, put another code between its characters: "$^75" shows "$5".
 * All of this is used in the menus and on servers since 1.4.7.x (TEXT_PROTOCOL). On older and vanilla servers only "^0" .. "^9"
 * are codes and names are shown like vanilla, so their texts look as they were made ("Rank #1", "$5 per map").
 * ('%' can not be a prefix, the network layer turns every '%' into '.' in MSG_ReadString.)
 *
 * Implementation:
 *   - renderer      R_TextWidth, R_DrawText, R_AddCmdDrawTextWithCursor, R_ConsoleTextWidth and
 *                   R_DrawConsoleText are replaced in the renderer function table gfx_funcs
 *   - console       CL_ConsolePrint_AddLine stores the codes as the color byte of each character
 *   - killfeed      CL_DeathMessagePrint adds the weapon icon and the extra icons the mod passed to obituary(),
 *                   CG_DrawGameMessages moves and scales the game messages (cg_gameMessage*)
 *   - names         CL_AddDeathMessageText (killfeed), CG_AddToTeamChat (chat), CG_DrawCrosshairNames,
 *                   CG_Obituary, CG_DrawFollow and the rename message strip the codes from the name
 *   - chat          CG_AddToTeamChat keeps the codes of the message, CG_DrawChatMessages colors the background
 *   - server        G_Say, Scr_ConstructMessageString (iprintln, ...) and SV_DropClient strip the codes
 *                   from player names (see server.cpp)
 */

#include "text.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "../shared/cod2_client.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_shared.h"
#include "drawing.h"
#include "shared.h"


dvar_t* cg_gameMessageOffsetX = NULL;
dvar_t* cg_gameMessageOffsetY = NULL;
dvar_t* cg_gameMessageScale = NULL;
extern bool cgame_hideServerIp;
extern dvar_t* g_cod2x;

// Protocol of the server that introduced the CoD2x codes and name rules (1.4.7.x)
#define TEXT_PROTOCOL 7

// The CoD2x codes ($N #N @N, styles) and name rules (names without codes, "^^11" on the scoreboard) are used in the menus
// and on servers since TEXT_PROTOCOL (g_cod2x). Texts of older servers were not made for them ("Rank #1", "$5 per map"),
// so there only the vanilla "^N" colors are read and names are cleaned like vanilla. Updated every frame in text_frame.
static bool text_cod2xCodes = true;



// ======================================================================================================
// Engine
// ======================================================================================================

#define cl_consoleBufferBase           ((uint16_t*)0x005e1784)
#define cl_consoleFrameCounter         (*((int32_t*)0x00601784))
#define cl_consoleBufferPos            (*((uint32_t*)0x00601788))
#define cl_consoleActiveType           (*((int32_t*)0x00601790))
#define cl_consoleBufferSize           (*((uint32_t*)0x00601794))
#define cl_consoleTotalBuffers         (*((int32_t*)0x00601798))
#define cl_noprint                     (*((dvar_t**)0x0096b628))
#define con_initialized                (*((int32_t*)0x005e1780))
#define con_gamemessagetime            (*((dvar_t**)0x005e1334))
#define notify_buffer                  (*((void**)0x006017b0)) // message windows, the first one is the game messages

// Killfeed icons, see CL_RegisterHudMsgIconMaterial
#define hudMsgIconMaterials            ((const char**)0x005e1340)
#define registeredIconMaterialCount    (*((int32_t*)0x005e1328))
#define HUD_MSG_ICON_MATERIALS_MAX     256 // the icon cell holds 8 bits

// Kill icon of a weapon definition (bg_weaponDefs)
struct killIconWeaponDef_t {
    char padding[0x34c];
    const char* killIcon;
    int32_t killIconRatio;  // 0 = 1:1, else 2:1 (twice as wide)
    int32_t flipKillIcon;   // horizontally flipped
};
#define bg_weaponDefs                  ((killIconWeaponDef_t**)0x01728200)
#define bg_numWeapons                  (*((int32_t*)0x019a1c74)) // weapons 1 .. bg_numWeapons

#define cg_gameMessageWidth            (*((dvar_t**)0x0166ba9c))
#define cg_hudCompassSize              (*((dvar_t**)0x0166bb34))
#define menu_hud_compass_y             (*((float*)0x014eba90))
#define menu_hud_compass_h             (*((float*)0x014eba8c))
#define cg_showScores                  (*((int32_t*)0x015195b4))
#define cg_scoreFadeTime               (*((int32_t*)0x015195b8))

#define cg_chatHeightDvar              (*((dvar_t**)0x014c3684))
#define cg_hudChatPositionDvar         (*((dvar_t**)0x014c362c))
#define cg_chatSayFadeTimeDvar         (*((dvar_t**)0x0166bb40))
#define cg_subtitleCharHeight          (*((dvar_t**)0x0166bb88))
#define cg_chatMessagesBase            ((char*)0x014ea9fc)
#define cg_chatRingTimestamps          ((int32_t*)0x014eb274)
#define cg_chatRingWriteIndex          (*((int32_t*)0x014eb294))
#define cg_chatRingReadIndex           (*((int32_t*)0x014eb298))
#define cg_chatBackgroundShader        (*((void**)0x014eb2a8))
#define CHAT_MESSAGE_SIZE              271

#define loc_language                   (*((dvar_t**)0x011972ec))
#define loc_isMultiByteLanguage        (*((int32_t*)0x011972f8))

// Renderer functions, copied by the engine into the table gfx_funcs (0x0068a1e8) after the renderer is loaded
typedef void  (__cdecl *R_AddCmdDrawStretchPic_t)(float x, float y, float w, float h, float s0, float t0, float s1, float t1, const float* color, void* material);
typedef float (__cdecl *R_NormalizedTextScale_t)(fontHandle_t* font, float scale);
typedef int   (__cdecl *R_TextWidth_t)(const char* text, int maxChars, fontHandle_t* font);
typedef int   (__cdecl *R_TextHeight_t)(fontHandle_t* font);
typedef void  (__cdecl *R_DrawText_t)(const char* text, int maxChars, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style);
typedef int   (__cdecl *R_ConsoleTextWidth_t)(const uint16_t* text, int textLen, fontHandle_t* font);
typedef void  (__cdecl *R_DrawConsoleText_t)(const uint16_t* text, int textLen, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style);
typedef void  (__cdecl *R_AddCmdDrawTextWithCursor_t)(const char* text, int maxChars, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style, int cursorPos, char cursor);

#define gfx_funcs_R_AddCmdDrawStretchPic     (*((R_AddCmdDrawStretchPic_t*)0x0068a294))
#define gfx_funcs_R_NormalizedTextScale      (*((R_NormalizedTextScale_t*)0x0068a310))
#define gfx_funcs_R_TextWidth                (*((R_TextWidth_t*)0x0068a314))
#define gfx_funcs_R_TextHeight               (*((R_TextHeight_t*)0x0068a318))
#define gfx_funcs_R_DrawText                 (*((R_DrawText_t*)0x0068a31c))
#define gfx_funcs_R_ConsoleTextWidth         (*((R_ConsoleTextWidth_t*)0x0068a324))
#define gfx_funcs_R_DrawConsoleText          (*((R_DrawConsoleText_t*)0x0068a328))
#define gfx_funcs_R_AddCmdDrawTextWithCursor (*((R_AddCmdDrawTextWithCursor_t*)0x0068a32c))

// Original renderer functions, saved when ours are put into gfx_funcs
static struct {
    R_TextWidth_t R_TextWidth;
    R_DrawText_t R_DrawText;
    R_ConsoleTextWidth_t R_ConsoleTextWidth;
    R_DrawConsoleText_t R_DrawConsoleText;
    R_AddCmdDrawTextWithCursor_t R_AddCmdDrawTextWithCursor;
} text_gfx;

// ColorIndex - inlined in the Windows build (a function in the Mac build)
static inline int ColorIndex(char c) {
    if (c < '0' || c > '9')
        return 7;
    return c - '0';
}



// ======================================================================================================
// Codes
// ======================================================================================================

// Extended colors: the prefix picks the shade, the digit picks the color family.
// Each one is kept as one code character: $0-$9 -> 'a'-'j', #0-#9 -> 'k'-'t', @0-@9 -> 'A'-'J'.
static const float text_extendedColors[30][3] = {
    // $0..$9 dark
    { 0.25f, 0.13f, 0.07f }, // $0 espresso
    { 0.45f, 0.05f, 0.10f }, // $1 maroon
    { 0.05f, 0.35f, 0.15f }, // $2 pine
    { 0.50f, 0.47f, 0.10f }, // $3 olive
    { 0.05f, 0.10f, 0.40f }, // $4 navy
    { 0.00f, 0.45f, 0.45f }, // $5 teal
    { 0.45f, 0.10f, 0.40f }, // $6 plum
    { 0.30f, 0.30f, 0.30f }, // $7 dark grey
    { 0.60f, 0.25f, 0.05f }, // $8 rust
    { 0.25f, 0.10f, 0.50f }, // $9 indigo
    // #0..#9 light
    { 0.80f, 0.62f, 0.42f }, // #0 tan
    { 1.00f, 0.50f, 0.45f }, // #1 coral
    { 0.65f, 0.95f, 0.20f }, // #2 lime
    { 1.00f, 0.95f, 0.65f }, // #3 cream
    { 0.45f, 0.70f, 1.00f }, // #4 sky
    { 0.70f, 0.95f, 1.00f }, // #5 ice
    { 1.00f, 0.65f, 0.85f }, // #6 rose
    { 0.80f, 0.80f, 0.80f }, // #7 light grey
    { 1.00f, 0.75f, 0.55f }, // #8 peach
    { 0.75f, 0.65f, 1.00f }, // #9 lavender
    // @0..@9 deep
    { 0.55f, 0.30f, 0.12f }, // @0 chocolate
    { 0.80f, 0.05f, 0.20f }, // @1 crimson
    { 0.00f, 0.72f, 0.40f }, // @2 emerald
    { 0.95f, 0.75f, 0.10f }, // @3 gold
    { 0.15f, 0.35f, 0.95f }, // @4 royal blue
    { 0.10f, 0.80f, 0.85f }, // @5 aqua
    { 0.95f, 0.15f, 0.60f }, // @6 hot pink
    { 0.50f, 0.50f, 0.50f }, // @7 grey
    { 1.00f, 0.50f, 0.00f }, // @8 orange
    { 0.55f, 0.15f, 0.95f }, // @9 violet
};

static const char text_extendedPrefixes[3] = { '$', '#', '@' };

// Index 0..29 into text_extendedColors of an extended code character, or -1.
static int Text_ExtendedIndex(char code)
{
    if (code >= 'a' && code <= 't')
        return code - 'a';
    if (code >= 'A' && code <= 'J')
        return 20 + (code - 'A');
    return -1;
}

// Extended code character of index 0..29.
static char Text_ExtendedCode(int index)
{
    return index < 20 ? (char)('a' + index) : (char)('A' + index - 20);
}

// Style codes: the prefix picks the kind of style, the symbol what it does. Each one is kept as one code
// character, so the callers can work with the code alone.
static const struct {
    char prefix;
    char symbol;
    char code;
} text_styleCodes[] = {
    { '$', '*', '*' }, // rainbow on
    { '$', '-', '!' }, // rainbow off
    { '@', '+', '+' }, // bold font
    { '@', '=', '=' }, // console (mono) font
    { '@', '-', '-' }, // default font
    { '#', '_', '_' }, // background on
    { '#', '-', '.' }, // background off
};

// Code character of the style code "<prefix><symbol>", or 0.
static char Text_StyleCode(char prefix, char symbol)
{
    for (const auto& style : text_styleCodes) {
        if (style.prefix == prefix && style.symbol == symbol)
            return style.code;
    }
    return 0;
}

static bool Text_IsStyleCode(char code)
{
    for (const auto& style : text_styleCodes) {
        if (style.code == code)
            return true;
    }
    return false;
}

// Code at p. Returns its length (0 = no code, the character is shown as it is) and the code character:
//   ^0 .. ^9         vanilla color                               2 chars, code '0'..'9'
//   $N #N @N         extended color                              2 chars, code 'a'..'t', 'A'..'J'
//   $* $- @+ ...     style, see text_styleCodes                  2 chars
//   $$NN ##NN @@NN   double extended color, the same as $N ...   4 chars
//   $$** @@++ ...    double style                                4 chars
// (A vanilla double color "^^11" is not one code: "^" + "^1" + "1", see Text_ResolveDoubleColors.)
static int Text_ParseCode(const char* p, char* code)
{
    if (!p[0] || !p[1])
        return 0;

    if (p[0] == '^') {
        if (p[1] < '0' || p[1] > '9')
            return 0;
        *code = p[1];
        return 2;
    }

    // Older server: '$', '#' and '@' are plain characters, like vanilla
    if (!text_cod2xCodes)
        return 0;

    int shade;
    if (p[0] == '$')
        shade = 0;
    else if (p[0] == '#')
        shade = 1;
    else if (p[0] == '@')
        shade = 2;
    else
        return 0;

    if (p[1] == p[0] && p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9') {
        *code = Text_ExtendedCode(shade * 10 + (p[3] - '0'));
        return 4;
    }
    if (p[1] >= '0' && p[1] <= '9') {
        *code = Text_ExtendedCode(shade * 10 + (p[1] - '0'));
        return 2;
    }

    if (p[1] == p[0] && p[2] && p[3] == p[2] && Text_StyleCode(p[0], p[2])) {
        *code = Text_StyleCode(p[0], p[2]);
        return 4;
    }
    if (Text_StyleCode(p[0], p[1])) {
        *code = Text_StyleCode(p[0], p[1]);
        return 2;
    }
    return 0;
}

// True if the text has a CoD2x code. Text with only vanilla "^N" codes is left to the engine.
static bool Text_HasCoD2xCodes(const char* text)
{
    char code;
    for (const char* p = text; *p; ++p) {
        if (p[0] != '^' && Text_ParseCode(p, &code))
            return true;
    }
    return false;
}

// Writes the 2 characters of a color code ("^1", "$5") into out.
static void Text_ColorCodeToString(char code, char out[2])
{
    int index = Text_ExtendedIndex(code);
    if (index < 0) {
        out[0] = '^';
        out[1] = code;
    } else {
        out[0] = text_extendedPrefixes[index / 10];
        out[1] = (char)('0' + index % 10);
    }
}

// Color of a color code. "^7" is the base color of the text, every color keeps its alpha.
static void Text_GetColor(char code, const float* baseColor, float* out)
{
    if (code == '7') {
        Vector4Copy(baseColor, out);
        return;
    }

    int index = Text_ExtendedIndex(code);
    if (index >= 0) {
        out[0] = text_extendedColors[index][0];
        out[1] = text_extendedColors[index][1];
        out[2] = text_extendedColors[index][2];
    } else {
        CL_LookupColor(code, out); // also ^8 / ^9 team colors
    }
    out[3] = baseColor[3];
}

// Vanilla double colors like "^^11Name" were made for the killfeed and chat, which stripped one level of
// codes ("^^11" -> "^1"). On the scoreboard vanilla shows them as "^1Name"; CoD2x resolves them the same
// way, so they color the name there. Returns text if nothing changed. Older servers keep the vanilla look.
static const char* Text_ResolveDoubleColors(const char* text, char* buffer, size_t size)
{
    if (!text_cod2xCodes || !strstr(text, "^^"))
        return text;

    bool changed = false;
    size_t len = 0;
    for (const char* p = text; *p; ) {
        if (len + 2 >= size)
            return text; // too long, draw it as it is
        if (p[0] == '^' && p[1] == '^' && p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9') {
            buffer[len++] = '^';
            buffer[len++] = p[3];
            p += 4;
            changed = true;
            continue;
        }
        buffer[len++] = *p++;
    }
    buffer[len] = '\0';

    return changed ? buffer : text;
}



// ======================================================================================================
// Fonts and text style
// ======================================================================================================

enum textFont_e {
    TEXT_FONT_DEFAULT,  // "$-" the font the text is drawn with
    TEXT_FONT_BOLD,     // "$+" boldFont
    TEXT_FONT_MONO,     // "$=" consoleFont
};

// Font to draw CoD2x text with. Only the typeface changes, not the size: *scale makes the font as
// high as baseFont (multiply xScale, yScale and measured widths by it).
//
// The fonts are bitmaps of a fixed pixel height (small 12, normal 16, big 24, extraBig 32, bold 30,
// console 16). For regular text the engine picks small / normal / big / extraBig by the screen size of
// the text and the cvars ui_smallFont, ui_bigFont, ui_extraBigFont (UI_GetFontHandle), and
// R_NormalizedTextScale scales any of them to the same height. CoD2x text does not follow these cvars:
// regular text is always drawn from extraBigFont, so it looks the same for every player (and matches the
// 30 px boldFont). A font the caller picked on purpose (console, bold) is kept.
static fontHandle_t* TextFont_Get(int font, fontHandle_t* baseFont, float* scale)
{
    *scale = 1.0f;

    fontHandle_t* resolved = NULL;
    if (font == TEXT_FONT_BOLD)
        resolved = fontNormalBold;
    else if (font == TEXT_FONT_MONO)
        resolved = fontConsole;
    else if (baseFont == fontSmall || baseFont == fontNormal || baseFont == fontBig)
        resolved = fontExtraBig;

    if (!resolved || !baseFont || resolved == baseFont)
        return baseFont;

    int baseHeight = gfx_funcs_R_TextHeight(baseFont);
    int height = gfx_funcs_R_TextHeight(resolved);
    if (baseHeight <= 0 || height <= 0)
        return baseFont;

    *scale = (float)baseHeight / (float)height;
    return resolved;
}

// Current color, rainbow, font and background while a text is drawn
struct textStyle_t {
    float color[4];
    float previousColor[4]; // the color before the last color code, the text goes back to it with "#_"
    bool rainbow;
    int font; // textFont_e
    bool background;
    float backgroundColor[4];
};

static void TextStyle_Init(textStyle_t* style, const float* baseColor)
{
    Vector4Copy(baseColor, style->color);
    Vector4Copy(baseColor, style->previousColor);
    style->rainbow = false;
    style->font = TEXT_FONT_DEFAULT;
    style->background = false;
}

// Sets a new text color, the current one is kept for "#_"
static void TextStyle_SetColor(textStyle_t* style, const float* color)
{
    Vector4Copy(style->color, style->previousColor);
    Vector4Copy(color, style->color);
}

static void TextStyle_ApplyCode(textStyle_t* style, char code, const float* baseColor)
{
    switch (code)
    {
        case '*': style->rainbow = true; break;
        case '!': style->rainbow = false; break;
        case '+': style->font = TEXT_FONT_BOLD; break;
        case '=': style->font = TEXT_FONT_MONO; break;
        case '-': style->font = TEXT_FONT_DEFAULT; break;
        case '_':
            // The current color becomes the background, the text goes back to the color it had before
            // (in a chat line "^8Name^1#_..." the allies team color, in a name "@4#_Name" the default color)
            style->background = true;
            Vector4Copy(style->color, style->backgroundColor);
            style->backgroundColor[3] = baseColor[3] * 0.5f;
            Vector4Copy(style->previousColor, style->color);
            break;
        case '.': style->background = false; break;
        default: {
            if (code == '7') { // "^7" ends the styles too (G_Say puts it after the player name)
                TextStyle_Init(style, baseColor);
                break;
            }
            float color[4];
            Text_GetColor(code, baseColor, color);
            TextStyle_SetColor(style, color);
            break;
        }
    }
}

static void TextStyle_GetDrawColor(const textStyle_t* style, float* out)
{
    Vector4Copy(style->color, out);
    if (!style->rainbow)
        return;

    // 0..6, one slow cycle in 10 seconds so the rainbow does not draw too much attention
    float hue = (float)(GetTickCount() % 10000) * (6.0f / 10000.0f);
    int sector = (int)hue;
    float frac = hue - (float)sector;
    switch (sector)
    {
        case 0:  out[0] = 1.0f;        out[1] = frac;        out[2] = 0.0f;        break;
        case 1:  out[0] = 1.0f - frac; out[1] = 1.0f;        out[2] = 0.0f;        break;
        case 2:  out[0] = 0.0f;        out[1] = 1.0f;        out[2] = frac;        break;
        case 3:  out[0] = 0.0f;        out[1] = 1.0f - frac; out[2] = 1.0f;        break;
        case 4:  out[0] = frac;        out[1] = 0.0f;        out[2] = 1.0f;        break;
        default: out[0] = 1.0f;        out[1] = 0.0f;        out[2] = 1.0f - frac; break;
    }
}



// ======================================================================================================
// Renderer
// ======================================================================================================

// Copies the next piece of text from start into piece and returns its length. The engine still reads
// "^0".."^9" as a color code, so a caret in front of a digit ends a piece.
static int Text_NextPiece(const char* text, int len, int start, char* piece, int size)
{
    int pieceLen = 0;
    while (start + pieceLen < len && pieceLen < size - 1) {
        char c = text[start + pieceLen];
        piece[pieceLen++] = c;
        char next = (start + pieceLen < len) ? text[start + pieceLen] : '\0';
        if (c == '^' && next >= '0' && next <= '9')
            break;
    }
    piece[pieceLen] = '\0';
    return pieceLen;
}

// Width of text without codes drawn in one style, in pixels of font at scale 1 (multiply by xScale).
static float Text_LiteralWidth(const char* text, int len, fontHandle_t* font, const textStyle_t* textStyle)
{
    float fontScale;
    fontHandle_t* drawFont = TextFont_Get(textStyle->font, font, &fontScale);

    float width = 0.0f;
    char piece[256];
    for (int start = 0; start < len; ) {
        int pieceLen = Text_NextPiece(text, len, start, piece, sizeof(piece));
        width += (float)text_gfx.R_TextWidth(piece, 0, drawFont) * fontScale;
        start += pieceLen;
    }
    return width;
}

// Draws text without codes in one style and moves x behind it. With cursorPos >= 0 the cursor is drawn
// in front of that character (at the end if cursorPos == len).
static void Text_DrawLiteral(const char* text, int len, fontHandle_t* font, float* x, float y, float xScale, float yScale, const textStyle_t* textStyle, int style, int cursorPos = -1, char cursor = 0)
{
    if (len <= 0 && cursorPos < 0)
        return;

    float color[4];
    TextStyle_GetDrawColor(textStyle, color);

    // Background: a bar from the top of the capitals to just below the baseline (the glyphs of every
    // font reach about 0.85 of the font height above the baseline)
    if (textStyle->background && len > 0) {
        float width = Text_LiteralWidth(text, len, font, textStyle) * xScale;
        float height = (float)gfx_funcs_R_TextHeight(font) * yScale;
        gfx_funcs_R_AddCmdDrawStretchPic(*x, y - height * 0.88f, width, height * 0.98f, 0.0f, 0.0f, 1.0f, 1.0f, textStyle->backgroundColor, shaderWhite);
    }

    float fontScale;
    font = TextFont_Get(textStyle->font, font, &fontScale);
    xScale *= fontScale;
    yScale *= fontScale;

    int start = 0;
    do {
        char piece[256];
        int pieceLen = Text_NextPiece(text, len, start, piece, sizeof(piece));

        int end = start + pieceLen;
        if (cursorPos >= start && (cursorPos < end || (end == len && cursorPos == len)))
            text_gfx.R_AddCmdDrawTextWithCursor(piece, INT_MAX, font, *x, y, xScale, yScale, color, style, cursorPos - start, cursor);
        else
            text_gfx.R_DrawText(piece, INT_MAX, font, *x, y, xScale, yScale, color, style);

        *x += (float)text_gfx.R_TextWidth(piece, 0, font) * xScale;
        start = end;
    } while (start < len);
}

// Draws text with CoD2x codes. With cursorPos >= 0 (index in text, codes included, like the engine)
// the cursor is drawn too.
static void Text_Draw(const char* text, int maxChars, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style, int cursorPos = -1, char cursor = 0)
{
    textStyle_t textStyle;
    TextStyle_Init(&textStyle, color);

    int charsLeft = (maxChars < 0) ? INT_MAX : maxChars;
    const float lineX = x;
    const char* codesStart = text;  // codes in front of the segment
    const char* segment = text;     // characters drawn in one style
    const char* p = text;

    // A cursor on a code is drawn in front of the next character
    auto drawSegment = [&](bool last) {
        int segmentCursor = -1;
        if (cursorPos >= 0) {
            int from = (int)(codesStart - text);
            int to = (int)(p - text);
            if (cursorPos >= from && (cursorPos < to || (last && cursorPos == to)))
                segmentCursor = (cursorPos > (int)(segment - text)) ? cursorPos - (int)(segment - text) : 0;
        }
        Text_DrawLiteral(segment, (int)(p - segment), font, &x, y, xScale, yScale, &textStyle, style, segmentCursor, cursor);
    };

    while (*p && charsLeft > 0) {
        char code;
        int codeLen = Text_ParseCode(p, &code);
        if (!codeLen && *p != '\n' && *p != '\r') {
            ++p;
            --charsLeft;
            continue;
        }

        if (p > segment) {
            drawSegment(false);
            codesStart = p;
        }

        if (codeLen) {
            TextStyle_ApplyCode(&textStyle, code, color);
            p += codeLen;
        } else {
            // New line like the engine (RB_DrawTextWithCursor): back to the start x, '\n' also one line down
            x = lineX;
            if (*p == '\n')
                y += (float)gfx_funcs_R_TextHeight(font) * yScale;
            ++p;
        }
        segment = p;
    }

    drawSegment(true);
}

// Width of text with CoD2x codes. Like the engine: maxChars <= 0 measures all, '\n' starts a new line,
// the widest line is returned. Every character is measured in the font it is drawn with (see Text_DrawLiteral).
static int Text_Width(const char* text, int maxChars, fontHandle_t* font)
{
    textStyle_t textStyle;
    TextStyle_Init(&textStyle, colWhite);

    int charsLeft = (maxChars <= 0) ? INT_MAX : maxChars;
    float width = 0.0f;
    float maxWidth = 0.0f;

    for (const char* p = text; *p && charsLeft > 0; ) {
        char code;
        int codeLen = Text_ParseCode(p, &code);
        if (codeLen) {
            TextStyle_ApplyCode(&textStyle, code, colWhite);
            p += codeLen;
            continue;
        }

        if (*p == '\n') {
            if (width > maxWidth)
                maxWidth = width;
            width = 0.0f;
            ++p;
            continue;
        }

        float fontScale;
        fontHandle_t* charFont = TextFont_Get(textStyle.font, font, &fontScale);
        char c[2] = { *p, '\0' };
        width += (float)text_gfx.R_TextWidth(c, 1, charFont) * fontScale;
        ++p;
        --charsLeft;
    }

    if (width > maxWidth)
        maxWidth = width;
    return (int)(maxWidth + 0.5f);
}

// ======================================================================================================
// Hidden IP addresses (cl_hideServerIp)
// ======================================================================================================

// Every text drawn by the renderer replacements below goes through here, so with cl_hideServerIp (or
// sv_hideServerIp of the connected server, see cgame_hideServerIp) the IP addresses are hidden everywhere
// on screen - console, game messages, menus, edit fields. The digits are
// replaced by '*' and the length stays the same, so cursor positions and the layout do not change, and the
// original text comes back when the cvar is turned off (the console buffer itself is not changed).
//   - IPv4 "a.b.c.d" with octets 0..255 and the port - "94.199.215.103:10405" is drawn as
//     "**.***.***.***:*****". Four single-digit numbers ("1.4.6.9") are a version, not an address, and stay.
//   - the address of the connect command, in every command of the line ("]\connect host" in the console, the
//     console input line, "connect 94.199.215.103:10405; password abc") - only the address up to the next
//     space or ';' is hidden, so a host name or a partly typed address does not leak either; the other
//     commands of the line stay visible.

static inline bool Text_IsDigit(char c) { return c >= '0' && c <= '9'; }
static inline bool Text_IsAlnum(char c) { return Text_IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Length of an IPv4 address at text[start], or 0. Single-digit-only addresses are versions, see above.
static int Text_IpAddressLength(const char* text, int len, int start)
{
    int p = start;
    bool multiDigit = false;
    for (int octet = 0; octet < 4; ++octet) {
        int octetStart = p;
        int value = 0;
        while (p < len && Text_IsDigit(text[p]) && p - octetStart < 3)
            value = value * 10 + (text[p++] - '0');
        if (p == octetStart || value > 255 || (p < len && Text_IsDigit(text[p])))
            return 0;
        if (p - octetStart > 1)
            multiDigit = true;
        if (octet < 3) {
            if (p >= len || text[p] != '.')
                return 0;
            ++p;
        }
    }
    if (p < len && (Text_IsAlnum(text[p]) || (text[p] == '.' && p + 1 < len && Text_IsDigit(text[p + 1]))))
        return 0;
    if (!multiDigit)
        return 0;

    // Port
    if (p + 1 < len && text[p] == ':' && Text_IsDigit(text[p + 1])) {
        ++p;
        while (p < len && Text_IsDigit(text[p]))
            ++p;
    }

    return p - start;
}

// Masks the IP addresses in a writable text of len characters; returns true if anything changed.
static bool Text_MaskIpAddresses(char* text, int len)
{
    bool changed = false;

    // The address of the connect command - at the start of the line and after every ';'
    for (int p = 0; p < len; ) {
        while (p < len && (text[p] == ']' || text[p] == '/' || text[p] == '\\' || text[p] == ' '))
            ++p;
        if (len - p >= 8 && strnicmp(text + p, "connect ", 8) == 0) {
            for (p += 8; p < len && text[p] == ' '; ++p)
                ;
            for (; p < len && text[p] != ' ' && text[p] != ';'; ++p) {
                if (Text_IsAlnum(text[p])) {
                    text[p] = '*';
                    changed = true;
                }
            }
        }
        while (p < len && text[p] != ';')
            ++p;
        ++p; // skip ';'
    }

    char prev = ' '; // character before text[i], a code counts as a separator
    for (int i = 0; i < len; ) {
        // Color and style codes ("^1", "$1", "#1", "@1") are not part of an address
        if ((text[i] == '^' || text[i] == '$' || text[i] == '#' || text[i] == '@') && i + 1 < len) {
            i += 2;
            prev = ' ';
            continue;
        }
        bool separated = !Text_IsAlnum(prev) && prev != '.';
        int ipLen = (separated && Text_IsDigit(text[i])) ? Text_IpAddressLength(text, len, i) : 0;
        if (ipLen == 0) {
            prev = text[i++];
            continue;
        }
        prev = text[i + ipLen - 1];
        for (int j = i; j < i + ipLen; ++j) {
            if (Text_IsDigit(text[j]))
                text[j] = '*';
        }
        i += ipLen;
        changed = true;
    }
    return changed;
}

// Text with hidden IP addresses if IP addresses are hidden (cgame_hideServerIp). Returns text if nothing changed.
static const char* Text_HideIpAddresses(const char* text, char* buffer, size_t size)
{
    if (!cgame_hideServerIp)
        return text;

    size_t len = strlen(text);
    if (len >= size)
        return text;
    memcpy(buffer, text, len + 1);

    return Text_MaskIpAddresses(buffer, (int)len) ? buffer : text;
}

// The same for console cells (see the console buffer cells below) - only the characters are masked,
// killfeed control cells (channels 10..19) separate them. Returns text if nothing changed.
static const uint16_t* Text_HideIpAddressesInCells(const uint16_t* text, int textLen, uint16_t* buffer, int size)
{
    if (!cgame_hideServerIp || textLen > size)
        return text;

    char chars[1024];
    if (textLen > (int)sizeof(chars))
        return text;
    for (int i = 0; i < textLen; ++i) {
        int hi = (text[i] >> 8) & 0xff;
        chars[i] = (hi >= 10 && hi < 0x40) ? '\x01' : (char)(text[i] & 0xff);
    }

    if (!Text_MaskIpAddresses(chars, textLen))
        return text;

    for (int i = 0; i < textLen; ++i) {
        buffer[i] = text[i];
        if (chars[i] == '*' && (char)(text[i] & 0xff) != '*')
            buffer[i] = (text[i] & 0xff00) | '*';
    }
    return buffer;
}


// 1000bf80 (gfx)    int32_t R_TextWidth(char* text, int32_t maxChars, void* font)
static int R_TextWidth(const char* text, int maxChars, fontHandle_t* font)
{
    // CoD2x: vanilla double colors "^^11" count like "^1", text with CoD2x codes is measured by CoD2x,
    // IP addresses are hidden with cl_hideServerIp
    char resolved[1024];
    text = Text_ResolveDoubleColors(text, resolved, sizeof(resolved));

    char hidden[1024];
    text = Text_HideIpAddresses(text, hidden, sizeof(hidden));

    if (Text_HasCoD2xCodes(text))
        return Text_Width(text, maxChars, font);
    // CoD2x: End

    return text_gfx.R_TextWidth(text, maxChars, font);
}

// 1000c030 (gfx)    char* R_DrawText(char* text, int32_t maxChars, void* font, float x, float y, float xScale, float yScale, float* color, int32_t style)
static void R_DrawText(const char* text, int maxChars, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style)
{
    // CoD2x: vanilla double colors "^^11" draw like "^1", text with CoD2x codes is drawn by CoD2x,
    // IP addresses are hidden with cl_hideServerIp
    char resolved[1024];
    text = Text_ResolveDoubleColors(text, resolved, sizeof(resolved));

    char hidden[1024];
    text = Text_HideIpAddresses(text, hidden, sizeof(hidden));

    if (Text_HasCoD2xCodes(text)) {
        Text_Draw(text, maxChars, font, x, y, xScale, yScale, color, style);
        return;
    }
    // CoD2x: End

    text_gfx.R_DrawText(text, maxChars, font, x, y, xScale, yScale, color, style);
}

// 10021700 (gfx)    char* R_AddCmdDrawTextWithCursor(char* text, int32_t maxChars, void* font, float x, float y, float xScale, float yScale, float* color, int32_t style, int32_t cursorPos, char cursor)
// Text of edit fields (console, menus).
static void R_AddCmdDrawTextWithCursor(const char* text, int maxChars, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style, int cursorPos, char cursor)
{
    // CoD2x: text with CoD2x codes is drawn by CoD2x, IP addresses are hidden with cl_hideServerIp
    char hidden[1024];
    text = Text_HideIpAddresses(text, hidden, sizeof(hidden));

    if (Text_HasCoD2xCodes(text)) {
        Text_Draw(text, maxChars, font, x, y, xScale, yScale, color, style, cursorPos, cursor);
        return;
    }
    // CoD2x: End

    text_gfx.R_AddCmdDrawTextWithCursor(text, maxChars, font, x, y, xScale, yScale, color, style, cursorPos, cursor);
}


// Console buffer cell: high byte = color byte or control channel, low byte = character or value.
// Vanilla:
//   0..9       color of the character
//   10..12     R, G, B of the following killfeed name (team color, CL_DeathMessagePrint)
//   13..19     weapon icon: 13..15 color, 16 width, 17 height, 19 flipped width, 18 material (ends the icon)
// CoD2x:
//   0x40..     extended color byte, see Text_EncodeColorByte
// Vanilla R_GetConsoleString writes every color byte as "^" + (byte + '0'), so lines with CoD2x cells
// are drawn and measured by CoD2x. Killfeed lines have no CoD2x cells (names are without codes), the
// engine draws them.

// Extended color byte (CL_ConsolePrint_AddLine): CON_COLOR_EXT_BASE + colorIndex + CON_COLOR_EXT_COLORS * fx
//   colorIndex   0..9 digits, 10..39 extended colors (Text_ExtendedIndex + 10)
//   fx           0 or CON_FX_RAINBOW (the color underneath stays for "$!")
#define CON_COLOR_EXT_BASE          0x40
#define CON_COLOR_EXT_COLORS        40
#define CON_FX_RAINBOW              1
#define CON_FX_COUNT                2

static int Text_EncodeColorByte(int colorIndex, int fx)
{
    if (fx == 0 && colorIndex <= 9)
        return colorIndex;
    return CON_COLOR_EXT_BASE + colorIndex + CON_COLOR_EXT_COLORS * fx;
}

static bool Text_DecodeColorByte(int value, int* colorIndex, int* fx)
{
    if (value >= 0 && value <= 9) {
        *colorIndex = value;
        *fx = 0;
        return true;
    }
    value -= CON_COLOR_EXT_BASE;
    if (value < 0 || value >= CON_COLOR_EXT_COLORS * CON_FX_COUNT)
        return false;
    *colorIndex = value % CON_COLOR_EXT_COLORS;
    *fx = value / CON_COLOR_EXT_COLORS;
    return true;
}

// Text style of a color byte
static void Text_ApplyColorByte(textStyle_t* textStyle, int value, const float* baseColor)
{
    int colorIndex, fx;
    if (!Text_DecodeColorByte(value, &colorIndex, &fx)) {
        colorIndex = 7;
        fx = 0;
    }

    TextStyle_Init(textStyle, baseColor);
    TextStyle_ApplyCode(textStyle, colorIndex <= 9 ? (char)('0' + colorIndex) : Text_ExtendedCode(colorIndex - 10), baseColor);
    textStyle->rainbow = (fx & CON_FX_RAINBOW) != 0;
}

// True if a console line has CoD2x cells (extended color bytes).
static bool Text_HasCoD2xCells(const uint16_t* text, int textLen)
{
    for (int i = 0; i < textLen; ++i) {
        int hi = (text[i] >> 8) & 0xff;
        if (hi >= CON_COLOR_EXT_BASE)
            return true;
    }
    return false;
}

// Collects the characters of console cells and draws them in one style, or only moves x behind them
struct textSegment_t {
    char text[1024];
    int len;
    float x;
    bool measure;
};

static void TextSegment_Add(textSegment_t* segment, char c)
{
    if (segment->len < (int)sizeof(segment->text) - 1)
        segment->text[segment->len++] = c;
}

static void TextSegment_Draw(textSegment_t* segment, fontHandle_t* font, float y, float xScale, float yScale, const textStyle_t* textStyle, int style)
{
    if (segment->measure)
        segment->x += Text_LiteralWidth(segment->text, segment->len, font, textStyle) * xScale;
    else
        Text_DrawLiteral(segment->text, segment->len, font, &segment->x, y, xScale, yScale, textStyle, style);
    segment->len = 0;
}

// Console or game message line written by CL_ConsolePrint_AddLine: the color byte styles each character.
// With measure nothing is drawn. Returns x behind the line.
static float Text_DrawConsoleLine(const uint16_t* text, int textLen, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style, bool measure)
{
    textStyle_t textStyle;
    TextStyle_Init(&textStyle, color);

    textSegment_t segment;
    segment.len = 0;
    segment.x = x;
    segment.measure = measure;
    int colorByte = -1;

    for (int i = 0; i < textLen; ++i) {
        int hi = (text[i] >> 8) & 0xff;
        char lo = (char)(text[i] & 0xff);
        if (!lo)
            continue;

        if (hi != colorByte) {
            TextSegment_Draw(&segment, font, y, xScale, yScale, &textStyle, style);
            Text_ApplyColorByte(&textStyle, hi, color);
            colorByte = hi;
        }
        TextSegment_Add(&segment, lo);
    }

    TextSegment_Draw(&segment, font, y, xScale, yScale, &textStyle, style);
    return segment.x;
}

// 1000c3b0 (gfx)    int32_t R_ConsoleTextWidth(uint16_t* text, int32_t textLen, void* font)
// Width of a console line, used to center the bold game messages (iprintlnbold).
static int R_ConsoleTextWidth(const uint16_t* text, int textLen, fontHandle_t* font)
{
    // CoD2x: lines with CoD2x cells are measured like Text_DrawConsoleLine draws them,
    // IP addresses are hidden with cl_hideServerIp
    uint16_t hidden[1024];
    text = Text_HideIpAddressesInCells(text, textLen, hidden, 1024);

    if (Text_HasCoD2xCells(text, textLen))
        return (int)Text_DrawConsoleLine(text, textLen, font, 0.0f, 0.0f, 1.0f, 1.0f, colWhite, 0, true);
    // CoD2x: End

    return text_gfx.R_ConsoleTextWidth(text, textLen, font);
}

// 1000c4d0 (gfx)    void R_DrawConsoleText(uint16_t* text, int32_t textLen, void* font, float x, float y, float xScale, float yScale, float* color, int32_t style)
// Lines of the console, game messages and killfeed.
static void R_DrawConsoleText(const uint16_t* text, int textLen, fontHandle_t* font, float x, float y, float xScale, float yScale, const float* color, int style)
{
    // CoD2x: lines with CoD2x cells are drawn by CoD2x, IP addresses are hidden with cl_hideServerIp
    uint16_t hidden[1024];
    text = Text_HideIpAddressesInCells(text, textLen, hidden, 1024);

    if (Text_HasCoD2xCells(text, textLen)) {
        Text_DrawConsoleLine(text, textLen, font, x, y, xScale, yScale, color, style, false);
        return;
    }
    // CoD2x: End

    text_gfx.R_DrawConsoleText(text, textLen, font, x, y, xScale, yScale, color, style);
}



// ======================================================================================================
// Console and game messages
// ======================================================================================================

// Code at text in CL_ConsolePrint_AddLine: updates the color byte (see Text_EncodeColorByte) and returns
// the code length, or 0 if the character is printed as it is. Font and background codes are skipped,
// the console has one font and no background.
static int Text_ParseConsoleCode(const char* text, uint32_t* color)
{
    char code;
    int codeLen = Text_ParseCode(text, &code);
    if (!codeLen)
        return 0;

    int colorIndex, fx;
    if (!Text_DecodeColorByte(*color & 0xff, &colorIndex, &fx)) {
        colorIndex = 7;
        fx = 0;
    }

    if (code == '7') { // ends the rainbow too
        colorIndex = 7;
        fx = 0;
    } else if (code >= '0' && code <= '9') {
        colorIndex = code - '0';
    } else if (Text_ExtendedIndex(code) >= 0) {
        colorIndex = 10 + Text_ExtendedIndex(code);
    } else if (code == '*') {
        fx |= CON_FX_RAINBOW;
    } else if (code == '!') {
        fx &= ~CON_FX_RAINBOW;
    } else if (code == '_') { // the color goes to the background (not drawn in the console)
        colorIndex = 7;
    }

    *color = Text_EncodeColorByte(colorIndex, fx);
    return codeLen;
}

// Reads one character, 2 bytes in double-byte languages (inlined 3x in CL_ConsolePrint_AddLine).
static uint32_t SEH_ReadCharFromString(const char** text)
{
    const unsigned char* s = (const unsigned char*)*text;
    uint32_t b0 = s[0];
    uint32_t b1 = s[1];

    if (loc_isMultiByteLanguage) {
        bool doubleByte = false;
        switch ((uint32_t)loc_language->value.integer - 8) {
            case 0:
                doubleByte = b0 >= 0xb0 && b0 <= 0xc8 && b1 > 0xa0 && b1 < 0xff;
                break;
            case 1:
                doubleByte = ((b0 >= 0xa1 && b0 <= 0xc6) || (b0 >= 0xc9 && b0 <= 0xf9)) &&
                             ((b1 >= 0x40 && b1 <= 0x7e) || (b1 >= 0xa1 && b1 <= 0xfe));
                break;
            case 2:
                doubleByte = ((b0 >= 0x81 && b0 <= 0x9f) || (b0 >= 0xe0 && b0 <= 0xef)) &&
                             ((b1 >= 0x40 && b1 <= 0x7e) || (b1 >= 0x80 && b1 <= 0xfc));
                break;
            case 3:
                doubleByte = b0 >= 0x81 && b0 <= 0xfe && b1 > 0x40 && b1 < 0xff;
                break;
        }
        if (doubleByte) {
            *text += 2;
            return (b0 << 8) + b1;
        }
    }

    *text += 1;
    return b0;
}

/**
 * Adds one print to the console buffer and wraps it into lines.
 * Channel 0 = console, 1 = game messages, 2 = bold game messages, 3 = subtitles, 4 = console only.
 * Returns the last color, so the caller can continue with it on the next line.
 */
uint32_t CL_ConsolePrint_AddLine(const char* txt, int32_t channel, int32_t duration, int32_t messageWidth, uint32_t color)
{
    int32_t lineLimit = cl_consoleBufferSize;
    if (channel != 0 && channel != 4 && lineLimit > 78)
        lineLimit = 78;

    if (messageWidth <= 0 || messageWidth > lineLimit)
        messageWidth = lineLimit;

    if (channel != cl_consoleActiveType && (int32_t)cl_consoleBufferPos > 0)
        Con_Linefeed(cl_consoleActiveType, duration);

    bool lineBroken = false;

    if (loc_isMultiByteLanguage && (channel == 1 || channel == 2 || channel == 3))
    {
        // Wrap by pixel width
        float charHeight = (channel == 3) ? (float)cg_subtitleCharHeight->value.integer : 12.0f;
        float scale = charHeight * (1.0f / 48.0f);
        fontHandle_t* font = UI_GetFontHandle(channel == 2 ? 4 : 0, scale);
        float normalizedScale = gfx_funcs_R_NormalizedTextScale(font, scale);
        int32_t lineWidth = 0;

        const char* p = txt;
        while (*p)
        {
            const char* next = p;
            uint32_t ch = SEH_ReadCharFromString(&next);

            // CoD2x: extended color and style codes
            /*
            if (ch == '^' && next[0] && next[0] != '^' && next[0] >= '0' && next[0] <= '9') {
                color = ColorIndex(next[0]);
                p = next + 1;
                continue;
            }
            */
            if (ch <= 0xff) {
                int32_t codeLen = Text_ParseConsoleCode(p, &color);
                if (codeLen) {
                    p += codeLen;
                    continue;
                }
            }
            // CoD2x: End

            // Break the line before a word that would not fit
            int32_t pos = cl_consoleBufferPos;
            int32_t width = lineWidth;
            const char* q = p;
            while (true) {
                int32_t charWidth = gfx_funcs_R_TextWidth(q, 1, font);
                if (SEH_ReadCharFromString(&q) <= ' ')
                    break;
                width += charWidth;
                pos++;
                if (pos > (int32_t)cl_consoleBufferSize || (double)messageWidth * 6.5 < (double)width * normalizedScale) {
                    Con_Linefeed(channel, duration);
                    lineBroken = true;
                    lineWidth = 0;
                    break;
                }
                if (loc_isMultiByteLanguage)
                    break;
            }

            int32_t charWidth = gfx_funcs_R_TextWidth(p, 1, font);
            ch = SEH_ReadCharFromString(&p);

            if (ch == '\n') {
                Con_Linefeed(channel, duration);
                lineWidth = 0;
            } else if (ch == '\r') {
                cl_consoleBufferPos = 0;
                lineWidth = 0;
            } else if (cl_consoleBufferPos != 0 || ch != ' ' || !lineBroken) {
                int32_t frame = cl_consoleFrameCounter % cl_consoleTotalBuffers;
                if (ch > 0xff) {
                    if (cl_consoleBufferPos + 1 >= cl_consoleBufferSize) {
                        Con_Linefeed(channel, duration);
                        lineBroken = true;
                        lineWidth = 0;
                        frame = cl_consoleFrameCounter % cl_consoleTotalBuffers;
                    }
                    cl_consoleBufferBase[frame * cl_consoleBufferSize + cl_consoleBufferPos] = (uint16_t)(((color & 0xff) << 8) | (ch >> 8));
                    cl_consoleBufferPos++;
                    ch &= 0xff;
                }
                cl_consoleBufferBase[frame * cl_consoleBufferSize + cl_consoleBufferPos] = (uint16_t)(((color & 0xff) << 8) | ch);
                cl_consoleBufferPos++;
                lineWidth += charWidth;
                if (cl_consoleBufferPos >= cl_consoleBufferSize) {
                    Con_Linefeed(channel, duration);
                    lineBroken = true;
                    lineWidth = 0;
                }
            }
        }
    }
    else
    {
        // Wrap by number of characters
        int32_t wrapAt = messageWidth;
        if (channel == 1 || channel == 2) {
            // Split long game messages into lines of similar length
            int32_t len = SEH_PrintStrlen(txt);
            if (len > messageWidth) {
                int32_t lines = (int32_t)ceilf((float)len / (float)messageWidth);
                wrapAt = (int32_t)((float)len / (float)lines);
            }
        }

        const char* p = txt;
        uint32_t ch = (unsigned char)*p;
        while (ch)
        {
            // CoD2x: extended color and style codes
            /*
            if (p[0] == '^' && p[1] && p[1] != '^' && p[1] >= '0' && p[1] <= '9') {
                color = ColorIndex(p[1]);
                p += 2;
                ch = (unsigned char)*p;
                continue;
            }
            */
            int32_t codeLen = Text_ParseConsoleCode(p, &color);
            if (codeLen) {
                p += codeLen;
                ch = (unsigned char)*p;
                continue;
            }
            // CoD2x: End

            // Break the line before a word that would not fit
            int32_t wordLen = 0;
            while (wordLen < messageWidth && (unsigned char)p[wordLen] > ' ')
                wordLen++;
            if (wordLen != messageWidth && (int32_t)cl_consoleBufferPos + wordLen > messageWidth) {
                Con_Linefeed(channel, duration);
                lineBroken = true;
            }

            p++;

            if (ch == '\n') {
                Con_Linefeed(channel, duration);
            } else if (ch == '\r') {
                cl_consoleBufferPos = 0;
            } else if (cl_consoleBufferPos != 0 || ch != ' ' || !lineBroken) {
                int32_t frame = cl_consoleFrameCounter % cl_consoleTotalBuffers;
                cl_consoleBufferBase[frame * cl_consoleBufferSize + cl_consoleBufferPos] = (uint16_t)(((color & 0xff) << 8) | ch);
                cl_consoleBufferPos++;
                if ((int32_t)cl_consoleBufferPos >= messageWidth || ((int32_t)cl_consoleBufferPos >= wrapAt && ch == ' ')) {
                    Con_Linefeed(channel, duration);
                    lineBroken = true;
                }
            }

            ch = (unsigned char)*p;
        }
    }

    if ((int32_t)cl_consoleBufferPos > 0) {
        if (channel)
            Con_Linefeed(channel, duration);
        else
            Con_UpdateNotifyLine(0, 0, duration); // console line stays open for the next print
    }
    cl_consoleActiveType = channel;

    return color;
}

// 00404690    uint32_t CL_ConsolePrint_AddLine(char* txt @ eax, int32_t channel, int32_t duration, int32_t messageWidth, uint32_t color)
uint32_t CL_ConsolePrint_AddLine_Win32(int32_t channel, int32_t duration, int32_t messageWidth, uint32_t color)
{
    char* txt;
    ASM( movr, txt, "eax" );

    return CL_ConsolePrint_AddLine(txt, channel, duration, messageWidth, color);
}



// ======================================================================================================
// Killfeed
// ======================================================================================================

/**
 * Writes a killfeed player name into the console buffer (called only by CL_DeathMessagePrint).
 * With color >= 0 the "^N" codes are stripped without being applied, so the name shows in the team
 * color (set by the cells in front of it).
 */
void CL_AddDeathMessageText(const char* str, int32_t color)
{
    // CoD2x: player names are colored only on the scoreboard - also the double codes ("^^11" would leave
    // "^1" in the name) and the CoD2x codes are removed. Older servers: vanilla, only the "^N" codes are skipped below.
    char cleanName[256];
    if (color >= 0 && text_cod2xCodes) {
        strip_text_codes(str, cleanName, sizeof(cleanName));
        str = cleanName;
    }
    // CoD2x: End

    int32_t colorNum = color;
    if (colorNum < 0)
        colorNum = ColorIndex('7');

    int32_t frame = cl_consoleFrameCounter % cl_consoleTotalBuffers;

    const char* p = str;
    while (*p && cl_consoleBufferPos < cl_consoleBufferSize)
    {
        if (p[0] == '^' && p[1] && p[1] != '^' && p[1] >= '0' && p[1] <= '9') {
            if (color < 0)
                colorNum = ColorIndex(p[1]);
            p += 2;
        } else {
            unsigned char ch = (unsigned char)*p;
            if (ch != '\n' && ch != '\r') {
                cl_consoleBufferBase[frame * cl_consoleBufferSize + cl_consoleBufferPos] = (uint16_t)(((colorNum & 0xff) << 8) | ch);
                cl_consoleBufferPos++;
            }
            ++p;
        }
    }
}

// 00405480    void CL_AddDeathMessageText(char* str @ eax, int32_t color)
void CL_AddDeathMessageText_Win32(int32_t color)
{
    char* str;
    ASM( movr, str, "eax" );

    CL_AddDeathMessageText(str, color);
}

// Entity state of the obituary CG_Obituary is printing, read by CL_DeathMessagePrint
static const entityState_t* text_obituaryState = NULL;

// Index of a killfeed icon in hudMsgIconMaterials. The weapon icons are added when the weapon is loaded,
// the extra icons of obituary() are added here (the renderer loads the material by the name when drawing).
// 0 when the table is full, like vanilla for an unknown icon.
static int Text_KillfeedIconIndex(const char* name)
{
    for (int i = 0; i < registeredIconMaterialCount; i++) {
        if (!strcmp(hudMsgIconMaterials[i], name))
            return i;
    }
    if (registeredIconMaterialCount >= HUD_MSG_ICON_MATERIALS_MAX)
        return 0;

    CL_RegisterHudMsgIconMaterial(name);
    return registeredIconMaterialCount - 1;
}

// Writes a killfeed icon into the console buffer like CL_DeathMessagePrint: color (13-15), width (16, or 19 when
// flipped), height (17) - in 1/32 of the font height - and the icon index (18, ends the icon).
static void Text_AddKillfeedIcon(const char* iconName, float iconWidth, float iconHeight, const float* iconColor, bool horzFlipIcon)
{
    CL_AddConsoleInfoColor(13, iconColor);

    int32_t frame = cl_consoleFrameCounter % cl_consoleTotalBuffers;
    uint16_t* line = cl_consoleBufferBase + frame * cl_consoleBufferSize;
    line[cl_consoleBufferPos++] = (uint16_t)((int32_t)(iconWidth * 32.0f) | (horzFlipIcon ? 0x1300 : 0x1000));
    line[cl_consoleBufferPos++] = (uint16_t)((int32_t)(iconHeight * 32.0f) | 0x1100);
    line[cl_consoleBufferPos++] = (uint16_t)(Text_KillfeedIconIndex(iconName) | 0x1200);
}

#define KILLFEED_ICON_CELLS 6 // cells of one icon, see Text_AddKillfeedIcon

// The icons between the names (see Obituary_SetExtendedIcons in gsc.cpp). Only when the mod passed extra icons
// to obituary(): the weapon icon - also with a headshot or melee, where vanilla shows the icon of the means of
// death - and the extra icons behind it. Otherwise (old mods, servers without CoD2x) only the vanilla icon.
static void Text_AddKillfeedIcons(const char* iconName, float iconWidth, float iconHeight, const float* iconColor, bool horzFlipIcon, int32_t lineLimit)
{
    const entityState_t* es = text_obituaryState;

    // The fields are set only by servers since TEXT_PROTOCOL (g_cod2x), older servers might leave something else in them
    if (!es || !es->obituaryIcons || g_cod2x->value.integer < TEXT_PROTOCOL) {
        Text_AddKillfeedIcon(iconName, iconWidth, iconHeight, iconColor, horzFlipIcon);
        return;
    }

    // The weapon icon, the vanilla icon if the weapon has none (falling, trigger hurt, ...)
    if (es->weapon > 0 && es->weapon <= bg_numWeapons) {
        const killIconWeaponDef_t* weaponDef = bg_weaponDefs[es->weapon];
        if (weaponDef && weaponDef->killIcon && weaponDef->killIcon[0]) {
            iconName = weaponDef->killIcon;
            iconWidth = weaponDef->killIconRatio ? iconHeight * 2.0f : iconHeight;
            horzFlipIcon = weaponDef->flipKillIcon != 0;
        }
    }
    Text_AddKillfeedIcon(iconName, iconWidth, iconHeight, iconColor, horzFlipIcon);

    for (int i = 0; i < 4; i++) {
        int shaderIndex = (es->obituaryIcons >> (8 * i)) & 0xff;
        if (!shaderIndex)
            continue;
        const char* shader = CL_GetConfigString(CS_SHADERS + shaderIndex);
        if (!shader[0] || !strcmp(shader, iconName))
            continue;
        // Keep room for the space and the victim color behind the icons
        if ((int32_t)cl_consoleBufferPos + KILLFEED_ICON_CELLS + 4 >= lineLimit)
            break;
        Text_AddKillfeedIcon(shader, iconHeight, iconHeight, iconColor, false);
    }
}

// Number of characters the current console line takes on screen, an icon as wide as its characters
// (a character is about half of the font height, the icon width is in 1/32 of the font height)
static int Text_KillfeedLineWidth()
{
    int32_t frame = cl_consoleFrameCounter % cl_consoleTotalBuffers;
    const uint16_t* line = cl_consoleBufferBase + frame * cl_consoleBufferSize;

    int width = 0;
    for (uint32_t i = 0; i < cl_consoleBufferPos; i++) {
        int hi = (line[i] >> 8) & 0xff;
        int lo = line[i] & 0xff;
        if (hi < 10 || hi >= CON_COLOR_EXT_BASE)
            width++;
        else if (hi == 16 || hi == 19)
            width += (lo + 15) / 16;
    }
    return width;
}

/**
 * Adds a killfeed line to the game messages: attacker name in the attacker color, the icon, victim name in the
 * victim color. The victim name goes to the next line if it does not fit.
 */
void CL_DeathMessagePrint(const char* attackerName, const float* attackerColor, const char* victimName, const float* victimColor, const char* iconName, float iconWidth, float iconHeight, const float* iconColor, bool horzFlipIcon)
{
    int32_t lineLimit = cl_consoleBufferSize;

    // CoD2x: a line of the game messages keeps only 78 cells (like CL_ConsolePrint_AddLine), the victim name goes to
    // the next line instead of being cut off when the extra icons make the line longer (older servers: vanilla)
    if (lineLimit > 78 && text_cod2xCodes)
        lineLimit = 78;
    // CoD2x: End

    if (cl_noprint && cl_noprint->value.boolean)
        return;

    if (!con_initialized)
        Con_OneTimeInit();

    int32_t duration = (int32_t)floorf(con_gamemessagetime->value.decimal * 1000.0f + 0.5f);
    if (duration < 0)
        duration = 0;

    if ((int32_t)cl_consoleBufferPos > 0)
        Con_Linefeed(cl_consoleActiveType, duration);

    if (*attackerName) {
        CL_AddConsoleInfoColor(10, attackerColor);
        CL_AddDeathMessageText(attackerName, ColorIndex('7'));
        CL_AddDeathMessageText(" ", ColorIndex('7'));
    }

    // CoD2x: the weapon icon and the extra icons when the mod passed them to obituary()
    /*
    CL_AddConsoleInfoColor(13, iconColor);
    ...width, height and icon index cells
    */
    Text_AddKillfeedIcons(iconName, iconWidth, iconHeight, iconColor, horzFlipIcon, lineLimit);
    // CoD2x: End

    CL_AddDeathMessageText(" ", ColorIndex('7'));
    CL_AddConsoleInfoColor(10, victimColor);

    // CoD2x: the name is measured without codes (CL_AddDeathMessageText removes them), it also goes to the next
    // line when the line would be wider than cg_gameMessageWidth. Older servers: vanilla.
    /*
    if ((int32_t)(strlen(victimName) + cl_consoleBufferPos + 3) >= lineLimit) {
    */
    char cleanVictimName[256];
    bool wrap;
    if (text_cod2xCodes) {
        strip_text_codes(victimName, cleanVictimName, sizeof(cleanVictimName));
        victimName = cleanVictimName;
        int32_t victimLen = (int32_t)strlen(victimName);
        wrap = victimLen + (int32_t)cl_consoleBufferPos + 3 >= lineLimit ||
               Text_KillfeedLineWidth() + victimLen > cg_gameMessageWidth->value.integer;
    } else {
        wrap = (int32_t)(strlen(victimName) + cl_consoleBufferPos + 3) >= lineLimit;
    }
    if (wrap) {
    // CoD2x: End
        Con_Linefeed(1, duration);
        CL_AddConsoleInfoColor(10, victimColor);
    }

    CL_AddDeathMessageText(victimName, ColorIndex('7'));
    Con_Linefeed(1, duration);
    cl_consoleActiveType = 1;
}

// 00405530    void CL_DeathMessagePrint(char const* attackerName @ eax, float const* attackerColor, char const* victimName, float const* victimColor @ ebx, char const* iconName, float iconWidth, float iconHeight, float const* iconColor, bool horzFlipIcon)
// horzFlipIcon is taken as int: GCC narrows a bool parameter into a local through eax before the registers are read
void CL_DeathMessagePrint_Win32(const float* attackerColor, const char* victimName, const char* iconName, float iconWidth, float iconHeight, const float* iconColor, int32_t horzFlipIcon)
{
    const char* attackerName;
    const float* victimColor;
    ASM( movr, attackerName, "eax" );
    ASM( movr, victimColor, "ebx" );

    CL_DeathMessagePrint(attackerName, attackerColor, victimName, victimColor, iconName, iconWidth, iconHeight, iconColor, (horzFlipIcon & 0xff) != 0);
}

// 004e03f0    void CG_Obituary(struct entityState_s* ent @ eax)
// Hooked in CG_EntityEvent, the entity state is kept for CL_DeathMessagePrint (weapon and extra killfeed icons).
void CG_Obituary(const entityState_t* ent)
{
    // CoD2x: the entity state for CL_DeathMessagePrint
    text_obituaryState = ent;
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x004e03f0, 0, EAX(ent));

    // CoD2x: the entity state for CL_DeathMessagePrint
    text_obituaryState = NULL;
    // CoD2x: End
}

void CG_Obituary_Win32()
{
    const entityState_t* ent;
    ASM( movr, ent, "eax" );

    CG_Obituary(ent);
}



// ======================================================================================================
// Game messages
// ======================================================================================================

/**
 * Draws the game messages (iprintln) and the killfeed above the compass in the bottom left corner, called by
 * CG_Draw2D with cg_drawGameMessages. The messages fade out with the scoreboard.
 */
void CG_DrawGameMessages()
{
    int32_t y = (int32_t)floorf(menu_hud_compass_y - (cg_hudCompassSize->value.decimal - 1.0f) * menu_hud_compass_h + 12.0f + 0.5f);

    float alpha = 1.0f;
    if (cg_showScores) {
        const float* color = CG_FadeColor(cg_scoreFadeTime, 100, 100);
        if (!color)
            return;
        alpha = color[3];
    }

    // CoD2x: position and size by cg_gameMessageOffsetX, cg_gameMessageOffsetY and cg_gameMessageScale. The line height
    // is a whole number (it also picks the font), the font scale corrects the rest.
    /*
    Con_DrawMessageWindow(notify_buffer, 6, y, 2, 12, HORIZONTAL_ALIGN_LEFT, VERTICAL_ALIGN_BOTTOM, alpha, 1.0f);
    */
    float height = 12.0f * cg_gameMessageScale->value.decimal;
    int32_t charHeight = (int32_t)(height + 0.5f);
    if (charHeight < 1)
        charHeight = 1;
    int32_t x = 6 + (int32_t)floorf(cg_gameMessageOffsetX->value.decimal + 0.5f);
    y += (int32_t)floorf(cg_gameMessageOffsetY->value.decimal + 0.5f);

    Con_DrawMessageWindow(notify_buffer, x, y, 2, charHeight, HORIZONTAL_ALIGN_LEFT, VERTICAL_ALIGN_BOTTOM, alpha, height / (float)charHeight);
    // CoD2x: End
}



// ======================================================================================================
// Chat
// ======================================================================================================

// Removes the codes from the player name of a chat line, so the name shows in the team color.
// G_Say builds the line as team color ("^8" / "^9") + cleaned name + "^7: " + message; a CoD2x server
// removes all codes from the name, servers without it leave the double codes ("^^11" -> "^1", "$$11").
// The leading team color and the message are kept.
static void Text_StripChatNameCodes(char* line)
{
    // Older servers: the name is kept as the server sent it, like vanilla
    if (!text_cod2xCodes)
        return;

    char* nameEnd = strstr(line, "^7: ");
    if (!nameEnd)
        return;

    char code;
    char* out = line;
    const char* p = line;
    if (Text_ParseCode(p, &code) == 2 && p[0] == '^') { // team color
        out += 2;
        p += 2;
    }
    while (p < nameEnd) {
        int codeLen = Text_ParseCode(p, &code);
        if (codeLen && p + codeLen <= nameEnd) {
            p += codeLen;
            continue;
        }
        *out++ = *p++;
    }
    memmove(out, nameEnd, strlen(nameEnd) + 1);
}

// Codes a wrapped chat line continues with (vanilla continues only with the last "^N")
struct chatCodes_t {
    char color[2];
    char previousColor[2]; // see textStyle_t
    bool rainbow;
    char font; // style code '+' or '=', 0 = default
    char background[2]; // color code of the background, 0 = none
};

static void ChatCodes_Apply(chatCodes_t* codes, char code)
{
    switch (code)
    {
        case '*': codes->rainbow = true; break;
        case '!': codes->rainbow = false; break;
        case '+':
        case '=': codes->font = code; break;
        case '-': codes->font = 0; break;
        case '_': // the color goes to the background, the text back to the color before it
            codes->background[0] = codes->color[0];
            codes->background[1] = codes->color[1];
            codes->color[0] = codes->previousColor[0];
            codes->color[1] = codes->previousColor[1];
            break;
        case '.': codes->background[0] = 0; break;
        default:
            codes->previousColor[0] = codes->color[0];
            codes->previousColor[1] = codes->color[1];
            Text_ColorCodeToString(code, codes->color);
            if (code == '7') {
                codes->previousColor[0] = '^';
                codes->previousColor[1] = '7';
                codes->rainbow = false;
                codes->font = 0;
                codes->background[0] = 0;
            }
            break;
    }
}

// Writes the codes (at most 10 characters), a color first so it colors the chat background. Returns the length.
static int ChatCodes_Write(const chatCodes_t* codes, char* out)
{
    int len = 0;
    if (codes->background[0]) {
        out[len++] = codes->background[0];
        out[len++] = codes->background[1];
        out[len++] = '#';
        out[len++] = '_';
    }
    if (!codes->background[0] || codes->color[0] != '^' || codes->color[1] != '7') {
        out[len++] = codes->color[0];
        out[len++] = codes->color[1];
    }
    if (codes->rainbow) {
        out[len++] = '$';
        out[len++] = '*';
    }
    if (codes->font) {
        out[len++] = '@';
        out[len++] = codes->font;
    }
    return len;
}

/**
 * Adds a chat line to the chat messages on the HUD, wrapped into lines of 90 characters.
 */
void CG_AddToTeamChat(const char* str)
{
    // CoD2x: player names are colored only on the scoreboard, in the chat the name has the team color
    char line[1024];
    strncpy(line, str, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    Text_StripChatNameCodes(line);
    str = line;
    // CoD2x: End

    int32_t chatHeight = cg_chatHeightDvar->value.integer;
    if (!chatHeight || cg_chatSayFadeTimeDvar->value.integer <= 0) {
        cg_chatRingReadIndex = 0;
        cg_chatRingWriteIndex = 0;
        return;
    }

    int32_t len = 0;
    char* lastSpace = NULL;
    // CoD2x: a wrapped line continues with all codes
    //char lastColor = '7';
    chatCodes_t codes = { { '^', '7' }, { '^', '7' }, false, 0, { 0, 0 } };
    chatCodes_t lastSpaceCodes = codes; // the codes at lastSpace, the text behind it is read again on wrap
    // CoD2x: End

    char* p = cg_chatMessagesBase + CHAT_MESSAGE_SIZE * (cg_chatRingWriteIndex % chatHeight);
    *p = '\0';

    // CoD2x: codes do not count into the line length, so a line full of codes could overflow the message
    char* messageEnd = p + CHAT_MESSAGE_SIZE;
    // CoD2x: End

    while (*str)
    {
        if (len > 89) {
            if (lastSpace) {
                str += lastSpace - p + 1;
                p = lastSpace;
                // CoD2x: the codes behind lastSpace are applied again when the text is read again
                codes = lastSpaceCodes;
                // CoD2x: End
            }
            *p = '\0';
            len = 0;

            cg_chatRingTimestamps[cg_chatRingWriteIndex % chatHeight] = cg.time;
            cg_chatRingWriteIndex++;
            p = cg_chatMessagesBase + CHAT_MESSAGE_SIZE * (cg_chatRingWriteIndex % chatHeight);

            // CoD2x: a wrapped line continues with all codes
            /*
            *p++ = '^';
            *p++ = lastColor;
            */
            messageEnd = p + CHAT_MESSAGE_SIZE;
            p += ChatCodes_Write(&codes, p);
            // CoD2x: End
            lastSpace = NULL;
        }

        // CoD2x: codes do not count into the line length, so a line full of codes could overflow the message
        if (messageEnd - p <= 4) // keep room for the longest code and '\0'
            break;
        // CoD2x: End

        // CoD2x: extended codes
        /*
        if (str[0] == '^' && str[1] && str[1] != '^' && str[1] >= '0' && str[1] <= '9') {
            *p++ = '^';
            *p++ = str[1];
            lastColor = str[1];
            str += 2;
            continue;
        }
        */
        char code;
        int codeLen = Text_ParseCode(str, &code);
        if (codeLen) {
            memcpy(p, str, codeLen);
            p += codeLen;
            str += codeLen;
            ChatCodes_Apply(&codes, code);
            continue;
        }
        // CoD2x: End

        if (*str == ' ') {
            lastSpace = p;
            // CoD2x: the codes at lastSpace
            lastSpaceCodes = codes;
            // CoD2x: End
        }
        *p++ = *str++;
        len++;
    }

    *p = '\0';
    cg_chatRingTimestamps[cg_chatRingWriteIndex % chatHeight] = cg.time;
    cg_chatRingWriteIndex++;
    if (cg_chatRingWriteIndex - cg_chatRingReadIndex > chatHeight)
        cg_chatRingReadIndex = cg_chatRingWriteIndex - chatHeight;
}

// 004d0e10    void CG_AddToTeamChat(char* str @ ecx)
void CG_AddToTeamChat_Win32()
{
    char* str;
    ASM( movr, str, "ecx" );

    CG_AddToTeamChat(str);
}

// 004c7760    void CG_DrawChatMessages()
void CG_DrawChatMessages()
{
    int32_t chatHeight = cg_chatHeightDvar->value.integer;
    if (!chatHeight)
        return;

    int32_t chatX = (int32_t)cg_hudChatPositionDvar->value.vec2[0];
    int32_t chatY = (int32_t)cg_hudChatPositionDvar->value.vec2[1];

    if (cg_chatRingReadIndex == cg_chatRingWriteIndex)
        return;

    if (cg.time - cg_chatRingTimestamps[cg_chatRingReadIndex % chatHeight] > cg_chatSayFadeTimeDvar->value.integer)
        cg_chatRingReadIndex++;

    const float scale = 0.20833333f; // 10 / 48, line height 10
    fontHandle_t* font = UI_GetFontHandle(0, scale); // inlined in vanilla

    for (int32_t i = cg_chatRingWriteIndex - 1; i >= cg_chatRingReadIndex; i--)
    {
        int32_t slot = i % chatHeight;
        float fade = (float)cg_chatSayFadeTimeDvar->value.integer - (float)(cg.time - cg_chatRingTimestamps[slot]);
        float alpha = 1.0f;
        if (fade <= 200.0f) {
            alpha = fade * 0.005f;
            if (alpha <= 0.0f)
                continue;
        }

        const char* text = cg_chatMessagesBase + CHAT_MESSAGE_SIZE * slot;
        float y = (float)(chatY + (i - cg_chatRingWriteIndex) * 10);

        // The background has the color of the code the line starts with
        vec4_t color = { 1.0f, 1.0f, 1.0f, 1.0f };
        // CoD2x: also an extended color code
        /*
        if (text[0] == '^' && text[1] && text[1] != '^' && text[1] >= '0' && text[1] <= '9')
            CL_LookupColor(text[1], color);
        */
        char code;
        if (Text_ParseCode(text, &code) && !Text_IsStyleCode(code))
            Text_GetColor(code, colWhite, color);
        // CoD2x: End
        color[0] *= 0.25f;
        color[1] *= 0.25f;
        color[2] *= 0.25f;
        color[3] = alpha * 0.6f;

        float width = (float)(UI_TextWidth(text, 0, font, scale) + 24);
        UI_DrawHandlePic(0.0f, y, width, 10.0f, HORIZONTAL_ALIGN_LEFT, VERTICAL_ALIGN_TOP, color, cg_chatBackgroundShader);

        vec4_t textColor = { 1.0f, 1.0f, 1.0f, alpha };
        UI_DrawText(text, INT_MAX, font, (float)chatX, y + 9.0f, HORIZONTAL_ALIGN_LEFT, VERTICAL_ALIGN_TOP, scale, textColor, TEXT_STYLE_SHADOWED);
    }
}

// Escapes "&&1" in the arguments of a localized chat message, so a player name like "&&1foo" is not
// read as the placeholder of the name. Returns true if the text changed.
static bool Text_EscapeChatPlaceholder(const char* text, char* out, size_t size)
{
    bool changed = false;
    bool inArgs = !strchr(text, 0x14); // the arguments start after 0x14; plain text -> everything
    size_t len = 0;

    for (size_t i = 0; text[i] && len < size - 1; ++i) {
        if (text[i] == 0x14)
            inArgs = true;

        if (inArgs && text[i] == '&' && text[i + 1] == '&' && text[i + 2] == '1' && (text[i + 3] < '0' || text[i + 3] > '9')) {
            const char* escaped = "^7&^7&^7";
            for (int k = 0; escaped[k] && len < size - 1; ++k)
                out[len++] = escaped[k];
            ++i; // the second '&'
            changed = true;
            continue;
        }

        out[len++] = text[i];
    }
    out[len] = '\0';

    return changed;
}

// 004ab700    char* SEH_LocalizeTextMessage(char* pszInputBuffer, char* pszMessageType, int32_t errType)
// Hooked only in CG_ServerCommand, for the chat commands.
char* SEH_LocalizeTextMessage(const char* pszInputBuffer, const char* pszMessageType, int32_t errType)
{
    // CoD2x: "&&1" in a player name is not the placeholder of the name
    char escaped[1024];
    if (Text_EscapeChatPlaceholder(pszInputBuffer, escaped, sizeof(escaped)))
        pszInputBuffer = escaped;
    // CoD2x: End

    // Call the original function
    char* ret;
    ASM_CALL(RETURN(ret), 0x004ab700, 3, PUSH(pszInputBuffer), PUSH(pszMessageType), PUSH(errType));
    return ret;
}



// ======================================================================================================
// Player names
// ======================================================================================================

// Player names are colored only on the scoreboard, the HUD texts below show them without codes.
// Older servers (text_cod2xCodes is false): the names are left as vanilla shows them.

// 0044a8d0    void I_CleanStr(char* string @ eax)
// Hooked only in CG_DrawCrosshairNames, for the name of the player under the crosshair.
// Vanilla I_CleanStr removes single "^1" codes, but a double "^^11" turns into "^1" and colors the name.
void CG_DrawCrosshairNames_I_CleanStr(char* name)
{
    // CoD2x: all codes are removed, also the double ones and the CoD2x codes
    if (text_cod2xCodes)
        strip_text_codes(name, name, strlen(name) + 1);
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x0044a8d0, 0, EAX(name));
}

void CG_DrawCrosshairNames_I_CleanStr_Win32()
{
    char* name;
    ASM( movr, name, "eax" );
    CG_DrawCrosshairNames_I_CleanStr(name);
}

// "You killed %s" / "You were killed by %s" in the center of the screen, in CG_Obituary:
// va("CGAME_YOUKILLED\x15%s", name) or va("CGAME_YOUKILLED\x15^1&&2^7 %s\x14%s", name, "CGAME_TEAMMATE")
char* CG_Obituary_va(const char* format, const char* name, const char* teammate)
{
    char cleanName[64];
    if (text_cod2xCodes) {
        strip_text_codes(name, cleanName, sizeof(cleanName));
        name = cleanName;
    }
    return va(format, name, teammate);
}

// The killer name for "Fragged by %s" in the killcam menu (ownerdraw CG_KILLER), copied in CG_Obituary
char* CG_Obituary_strncpy(char* dest, const char* name, size_t count)
{
    char cleanName[64];
    if (text_cod2xCodes) {
        strip_text_codes(name, cleanName, sizeof(cleanName));
        name = cleanName;
    }
    return strncpy(dest, name, count); // CG_Obituary terminates it
}

// "Following" + name of the spectated player, in CG_DrawFollow (codes are not counted in the width the name is centered by)
void CG_DrawFollow_UI_DrawText(const char* text, int len, fontHandle_t* font, float x, float y, horizontalAlign_e horzAlign, verticalAlign_e vertAlign, float scale, vec4_t color, textStyle_e style)
{
    char cleanName[64];
    if (text_cod2xCodes) {
        strip_text_codes(text, cleanName, sizeof(cleanName));
        text = cleanName;
    }
    UI_DrawText(text, len, font, x, y, horzAlign, vertAlign, scale, color, style);
}

// 005322c0    void UI_DrawText(char* text, int32_t len, void* font @ esi, float x, float y, int32_t horzAlign @ edi, int32_t vertAlign @ ebx, float scale, float* color, int32_t style)
void CG_DrawFollow_UI_DrawText_Win32(const char* text, int len, float x, float y, float scale, vec4_t color, textStyle_e style)
{
    fontHandle_t* font;     ASM( movr, font, "esi" );
    horizontalAlign_e horzAlign; ASM( movr, horzAlign, "edi" );
    verticalAlign_e vertAlign;   ASM( movr, vertAlign, "ebx" );
    CG_DrawFollow_UI_DrawText(text, len, font, x, y, horzAlign, vertAlign, scale, color, style);
}

// "<old name> renamed to <new name>" game message: va("%s^7 %s %s", oldName, "renamed to", newName)
char* CG_PlayerRenamed_va(const char* format, const char* oldName, const char* renamedTo, const char* newName)
{
    char cleanOldName[64];
    char cleanNewName[64];
    if (text_cod2xCodes) {
        strip_text_codes(oldName, cleanOldName, sizeof(cleanOldName));
        strip_text_codes(newName, cleanNewName, sizeof(cleanNewName));
        oldName = cleanOldName;
        newName = cleanNewName;
    }
    return va(format, oldName, renamedTo, newName);
}



// ======================================================================================================
// Module
// ======================================================================================================

/** Puts our text functions into the renderer function table. The engine fills the table after the
 *  renderer is loaded (game start, vid_restart), so it is checked every frame. */
static void Text_HookRenderer()
{
    if (!gfx_funcs_R_DrawText || gfx_funcs_R_DrawText == R_DrawText)
        return;

    text_gfx.R_TextWidth = gfx_funcs_R_TextWidth;
    text_gfx.R_DrawText = gfx_funcs_R_DrawText;
    text_gfx.R_ConsoleTextWidth = gfx_funcs_R_ConsoleTextWidth;
    text_gfx.R_DrawConsoleText = gfx_funcs_R_DrawConsoleText;
    text_gfx.R_AddCmdDrawTextWithCursor = gfx_funcs_R_AddCmdDrawTextWithCursor;

    gfx_funcs_R_TextWidth = R_TextWidth;
    gfx_funcs_R_DrawText = R_DrawText;
    gfx_funcs_R_ConsoleTextWidth = R_ConsoleTextWidth;
    gfx_funcs_R_DrawConsoleText = R_DrawConsoleText;
    gfx_funcs_R_AddCmdDrawTextWithCursor = R_AddCmdDrawTextWithCursor;
}

/** Called every frame on frame start, and after the client is initialized. */
void text_frame() {
    Text_HookRenderer();

    // In the menus g_cod2x is 0 (reset on disconnect), but no text comes from a server there
    text_cod2xCodes = clientState < CLIENT_STATE_CONNECTING || g_cod2x->value.integer >= TEXT_PROTOCOL;
}

/** Called before the DLL is unloaded (hot reload). */
void text_unload() {
    if (gfx_funcs_R_DrawText != R_DrawText)
        return;

    gfx_funcs_R_TextWidth = text_gfx.R_TextWidth;
    gfx_funcs_R_DrawText = text_gfx.R_DrawText;
    gfx_funcs_R_ConsoleTextWidth = text_gfx.R_ConsoleTextWidth;
    gfx_funcs_R_DrawConsoleText = text_gfx.R_DrawConsoleText;
    gfx_funcs_R_AddCmdDrawTextWithCursor = text_gfx.R_AddCmdDrawTextWithCursor;
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void text_init() {
    // Position and size of the game messages and killfeed, the offsets in 640x480 units
    cg_gameMessageOffsetX = Dvar_RegisterFloat("cg_gameMessageOffsetX", 0.0f, -640.0f, 640.0f, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));
    cg_gameMessageOffsetY = Dvar_RegisterFloat("cg_gameMessageOffsetY", 0.0f, -480.0f, 480.0f, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));
    cg_gameMessageScale = Dvar_RegisterFloat("cg_gameMessageScale", 1.0f, 0.5f, 3.0f, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));
}

/** Called before the entry point is called. Used to patch the memory. */
void text_patch() {
    // Console, game messages and subtitles
    patch_call(0x004020b3, (unsigned int)CL_ConsolePrint_AddLine_Win32);
    patch_call(0x00405007, (unsigned int)CL_ConsolePrint_AddLine_Win32); // CL_ConsolePrint
    patch_call(0x0040503d, (unsigned int)CL_ConsolePrint_AddLine_Win32); // CL_ConsolePrint
    patch_call(0x0040519c, (unsigned int)CL_ConsolePrint_AddLine_Win32); // PbMsgToScreen
    patch_call(0x004051cd, (unsigned int)CL_ConsolePrint_AddLine_Win32); // PbMsgToScreen
    patch_call(0x00405248, (unsigned int)CL_ConsolePrint_AddLine_Win32); // CL_ConsoleFixPosition
    patch_call(0x004bf71b, (unsigned int)CL_ConsolePrint_AddLine_Win32);
    patch_call(0x004bf745, (unsigned int)CL_ConsolePrint_AddLine_Win32);
    patch_call(0x004bf83b, (unsigned int)CL_ConsolePrint_AddLine_Win32);
    patch_call(0x004bf865, (unsigned int)CL_ConsolePrint_AddLine_Win32);

    // Killfeed names
    patch_call(0x004055cb, (unsigned int)CL_AddDeathMessageText_Win32); // CL_DeathMessagePrint
    patch_call(0x004055da, (unsigned int)CL_AddDeathMessageText_Win32); // CL_DeathMessagePrint
    patch_call(0x004056d4, (unsigned int)CL_AddDeathMessageText_Win32); // CL_DeathMessagePrint
    patch_call(0x00405726, (unsigned int)CL_AddDeathMessageText_Win32); // CL_DeathMessagePrint

    // Killfeed icons, game messages position and size
    patch_call(0x004e1919, (unsigned int)CG_Obituary_Win32); // CG_EntityEvent
    patch_call(0x004e0778, (unsigned int)CL_DeathMessagePrint_Win32); // CG_Obituary
    patch_call(0x004cbe01, (unsigned int)CG_DrawGameMessages); // CG_Draw2D

    // Chat
    patch_jump(0x004d0e10, (unsigned int)CG_AddToTeamChat_Win32);
    patch_jump(0x004c7760, (unsigned int)CG_DrawChatMessages);
    patch_call(0x004d1cbf, (unsigned int)SEH_LocalizeTextMessage); // CG_ServerCommand
    patch_call(0x004d1d1c, (unsigned int)SEH_LocalizeTextMessage); // CG_ServerCommand

    // Player names without codes - they are colored only on the scoreboard
    patch_call(0x004c9891, (unsigned int)CG_DrawCrosshairNames_I_CleanStr_Win32); // CG_DrawCrosshairNames
    patch_call(0x004e06d1, (unsigned int)CG_Obituary_va); // CG_Obituary - "You killed" teammate
    patch_call(0x004e070a, (unsigned int)CG_Obituary_va); // CG_Obituary - "You were killed by" teammate
    patch_call(0x004e071e, (unsigned int)CG_Obituary_va); // CG_Obituary - "You killed" / "You were killed by"
    patch_call(0x004e0670, (unsigned int)CG_Obituary_strncpy); // CG_Obituary - killer name for "Fragged by"
    patch_call(0x004cbc23, (unsigned int)CG_DrawFollow_UI_DrawText_Win32); // CG_DrawFollow - name
    patch_call(0x004df827, (unsigned int)CG_PlayerRenamed_va); // "renamed to" message
}
