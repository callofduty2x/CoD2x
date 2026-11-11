#include "drawing.h"

#include <math.h>

#include "../shared/cod2_client.h"
#include "../shared/cod2_dvars.h"
#include "demo.h"
#include "radar.h"
#include "text.h"
#include "xray.h"
#include "../shared/cod2_entity.h"
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


// ======================================================================================================
// Head info - nation icon, name and weapon over the players for spectators (and dead players over their teammates), like in CS:GO,
// and the size of the game icons over the head
// ======================================================================================================

#define bg_weaponDefs           (*((void* (*)[0x80])0x01728200))    // BG_GetWeaponDef is inlined in the Windows build
#define bg_numWeapons           (*((int*)0x019a1c74))

// WeaponDef fields (see the weapon field table at 0x005db8a4)
#define WeaponDef_displayName(def)  (*(const char**)((char*)(def) + 0x004))   // localization reference
#define WeaponDef_hudIcon(def)      (*(const char**)((char*)(def) + 0x1b8))   // material of the HUD
#define WeaponDef_killIcon(def)     (*(const char**)((char*)(def) + 0x34c))   // material of the killfeed
#define WeaponDef_wideKillIcon(def) (*(int*)((char*)(def) + 0x350))           // the kill icon is 2:1

#define cg_drawCrosshairNames   (*((dvar_t**)0x0166bad8))   // bool, name of the teammate under the crosshair (CG_DrawCrosshairNames)

#define centity_dobj(cent)     (*(int*)((char*)(cent) + 0x220))    // handle for Com_GetClientDObj
#define centity_voiceIconTime(cent) (*(int*)((char*)(cent) + 0x218))  // the talking icon (material at + 0x214) is shown until this time (CG_PlayerSprites)

// Entity events with the offhand weapon in eventParm (CG_EntityEvent plays their sound only for the followed player, but they come for all players)
#define EV_PREP_OFFHAND         0xa5    // grenade pulled
#define EV_USE_OFFHAND          0xa6    // grenade thrown
#define HEAD_OFFHAND_TIME   5000    // the pulled grenade is forgotten after this time (ms), if the throw event is lost

// Elements of the head info, from the bottom up, and the states of the viewer. Value of the element cvars:
// 0 = off, 1 = only when visible (hidden behind the world per pixel, like the 3D nation icon of the game), 2 = also through walls.
// Spectators (and demo playback) see everybody, dead and playing players only their teammates, so it tells nothing about the enemies.
enum { HEAD_NATION_ICON, HEAD_NAME, HEAD_WEAPON_ICON, HEAD_WEAPON_NAME, HEAD_ELEMENTS };
enum { HEAD_SPECTATOR, HEAD_DEAD, HEAD_ALIVE, HEAD_STATES };

dvar_t* cg_headNationIcon[HEAD_STATES];     // cg_headNationIconSpectator / Dead / Alive - the nation icon of the game (GSC self.headicon)
dvar_t* cg_headName[HEAD_STATES];           // cg_headNameSpectator / Dead / Alive - player name
dvar_t* cg_headWeaponIcon[HEAD_STATES];     // cg_headWeaponIconSpectator / Dead / Alive - icon of the weapon in the hands
dvar_t* cg_headWeaponName[HEAD_STATES];     // cg_headWeaponNameSpectator / Dead / Alive - name of the weapon in the hands
dvar_t* cg_headNameScale;                   // size of the player name (and the weapon name)
dvar_t* cg_headWeaponIconScale;             // size of the weapon icon
dvar_t* cg_headIconScale;                   // size of all the game icons over the head (nation / script icon, killcam, lag, voice / chat), on top of their own size cvars, 0 = hidden
dvar_t* cg_headOffset;                      // extra height over the head of the game icons and the head info (world units)
dvar_t* cg_headMaxDistance;                 // not drawn for players further away (world units), 0 = always

// What is drawn over one player
struct headInfo_t {
    bool element[HEAD_ELEMENTS];    // drawn by the head info - it always has its place, so the elements above it never move
    bool depthTest[HEAD_ELEMENTS];  // only when visible: hidden behind the world per pixel, like the 3D nation icon of the game (not faded)
    bool gameNationIcon;            // the nation icon is drawn by the game (only when visible, nothing else is drawn), not by the head info
    bool needVisibility;            // the head visibility is needed for the center fade or the fallback of depthTest (see Head_Draw)
    bool hidden;                    // the head is not visible: the elements drawn through walls fade near the screen center (cg_xrayCenterFadeRadius)
};

// Icon sizes of the game, for the place of the head info above them (see CG_PlayerSprites). Registered by the cgame (CG_Init_Cgame_Cvars) after our
// init and registered again on every map load. Before the registration they can exist as string dvars (from the config).
static dvar_t* cg_scriptIconSize;
static dvar_t* cg_connectionIconSize;
static dvar_t* cg_voiceIconSize;
static dvar_t* cg_headIconMinScreenRadius;
static dvar_t* cg_constantSizeHeadIcons;

// Grenade (offhand weapon) pulled by the player
static struct { int weapon; int time; } head_offhand[64];



/** Position of the world point on the screen, relative to the screen center in the 640x480 virtual space. False if behind the view. */
static bool Head_WorldToScreen(const vec3_t point, float* x, float* y) {
    refdef_t* rd = &cg.refdef;
    vec3_t delta;

    VectorSubtract(point, rd->vieworg, delta);
    float forward = DotProduct(delta, rd->viewaxis[0]);
    if (forward < 1.0f)
        return false;

    // viewaxis[1] points left, viewaxis[2] up
    float px = DotProduct(delta, rd->viewaxis[1]) / forward / tanf(rd->fov_x * (float)M_PI / 360.0f);
    float py = DotProduct(delta, rd->viewaxis[2]) / forward / tanf(rd->fov_y * (float)M_PI / 360.0f);

    // The virtual space is 480 units high, the width depends on the aspect ratio
    *x = -px * (rd->width / 2.0f) * 480.0f / rd->height;
    *y = -py * 240.0f;
    return true;
}

/** Weapon definition of the weapon index, NULL if invalid. */
static void* Head_WeaponDef(int weapon) {
    if (weapon <= 0 || weapon > bg_numWeapons)
        return NULL;
    return bg_weaponDefs[weapon];
}

/** Weapon the player has in the hands: the pulled grenade, nothing on a mounted MG, on a ladder or while mantling, otherwise the weapon. */
static int Head_HeldWeapon(centity_t* cent, clientInfo_t* ci) {
    int clientNum = cent->currentState.clientNum;
    if (head_offhand[clientNum].weapon && cg.time >= head_offhand[clientNum].time && cg.time - head_offhand[clientNum].time < HEAD_OFFHAND_TIME)
        return head_offhand[clientNum].weapon;

    if (cent->currentState.eFlags & (ENTITY_FLAG_MG | ENTITY_FLAG_MANTLE))
        return 0;
    if (BG_GetConditionValue(ci, ANIM_COND_MOVETYPE, 0) & (1 << ANIM_MT_CLIMBUP | 1 << ANIM_MT_CLIMBDOWN))
        return 0;
    return cent->currentState.weapon;
}

/** Position of the head of the player (the head bone, follows the stance, lean and animations), or 61 units over the origin. */
static void Head_GetHeadPos(centity_t* cent, vec3_t pos) {
    void* dobj = Com_GetClientDObj(cent->nextState.number, centity_dobj(cent));
    if (!dobj || !CG_DObjGetWorldTagPos(scr_const_j_head, dobj, cent, pos)) {
        VectorCopy(cent->lerpOrigin, pos);
        pos[2] += 61;
    }
}

/** True if the head of the player is visible (nothing solid between the camera and the head). */
static bool Head_IsVisible(centity_t* cent) {
    vec3_t head;
    vec3_t zero = { 0, 0, 0 };
    trace_t trace;
    Head_GetHeadPos(cent, head);
    head[2] += 4;
    CG_TraceCapsule(&trace, cg.refdef.vieworg, zero, zero, head, cg.snap->ps.clientNum, 0x811);
    return trace.fraction >= 0.99f;
}

/**
 * What is drawn over the player for the current viewer. Returns false if the head info does not care about the player (not a shown player,
 * like enemies of a dead or playing player), then the game draws its icons as usual.
 */
static bool Head_GetInfo(centity_t* cent, headInfo_t* info) {
    memset(info, 0, sizeof(*info));

    int state;
    if (xray_viewer == XRAY_VIEWER_SPECTATOR)   state = HEAD_SPECTATOR;
    else if (xray_viewer == XRAY_VIEWER_DEAD)   state = HEAD_DEAD;
    else if (xray_viewer == XRAY_VIEWER_PLAYER) state = HEAD_ALIVE;
    else return false;

    clientInfo_t* ci = &clientInfo[cent->currentState.clientNum];
    if (cent->currentState.eType != ET_PLAYER || !(cent->currentValid & 1) || (cent->currentState.eFlags & ENTITY_FLAG_DEAD))
        return false;
    if (ci->infoValid != qtrue || ci->team == TEAM_SPECTATOR)
        return false;
    // Not the player we are (or we follow)
    if (cent->currentState.clientNum == cg.snap->ps.clientNum || cent->currentState.clientNum == cg.clientNum)
        return false;
    // Dead and playing players: only teammates (free-for-all players have none)
    int localTeam = clientInfo[cg.clientNum].team;
    if (state != HEAD_SPECTATOR && (localTeam == TEAM_NONE || ci->team != localTeam))
        return false;

    dvar_t** cvars[HEAD_ELEMENTS] = { cg_headNationIcon, cg_headName, cg_headWeaponIcon, cg_headWeaponName };
    int value[HEAD_ELEMENTS];
    bool anyVisibleOnly = false;
    bool anyThroughWalls = false;
    for (int e = 0; e < HEAD_ELEMENTS; e++) {
        value[e] = cvars[e][state]->value.integer;
        anyVisibleOnly |= value[e] == 1;
        anyThroughWalls |= value[e] == 2;
        // Every element that is on has its place, visible or not, so the stack does not change its height
        info->element[e] = value[e] != 0;
        // Only when visible: hidden behind the world per pixel by the renderer (see xray_headDepthTest)
        info->depthTest[e] = value[e] == 1;
    }
    // Visibility of the head is needed for the center fade of the elements drawn through walls, and for the elements drawn only when visible
    // if the renderer can not hide them per pixel (then they are drawn whole only when the head is visible, in their place).
    // The trace is done only by Head_Draw, CG_PlayerSprites does not need it.
    info->needVisibility = (anyThroughWalls && xray_centerFadeEnabled()) || (anyVisibleOnly && !xray_headDepthAvailable());

    // Nation icon only when visible and nothing else is on: the game draws it as usual (its 3D icon is hidden behind walls exactly).
    // With the name or the weapon it's drawn by the head info, in the right order. By the cvars only, so it does not move when the player hides.
    if (value[HEAD_NATION_ICON] == 1 && !value[HEAD_NAME] && !value[HEAD_WEAPON_ICON] && !value[HEAD_WEAPON_NAME]) {
        info->element[HEAD_NATION_ICON] = false;
        info->gameNationIcon = true;
    }

    // Too far: the head info draws nothing (the nation icon of the game stays as it is)
    if (cg_headMaxDistance->value.decimal > 0) {
        vec3_t delta;
        VectorSubtract(cent->lerpOrigin, cg.refdef.vieworg, delta);
        if (DotProduct(delta, delta) > cg_headMaxDistance->value.decimal * cg_headMaxDistance->value.decimal) {
            memset(info->element, 0, sizeof(info->element));
            memset(info->depthTest, 0, sizeof(info->depthTest));
            info->needVisibility = false;
        }
    }
    return true;
}

/** Value of the float dvar of the game, the default value if it does not exist yet or is not a float dvar yet. */
static float Head_GameDvarFloat(dvar_t** dvar, const char* name, float defaultValue) {
    if (!*dvar)
        *dvar = Dvar_GetDvarByName(name);
    if (!*dvar || (*dvar)->type != DVAR_TYPE_FLOAT)
        return defaultValue;
    return (*dvar)->value.decimal;
}

/** Size cvar of a game icon (cg_scriptIconSize, cg_voiceIconSize, ...) scaled by cg_headIconScale. The engine draws the icon with the radius size + 10 world units. */
static float Head_ScaledIconSize(float size) {
    return (size + 10.0f) * cg_headIconScale->value.decimal - 10.0f;
}

/** Team of the followed player, as CG_PlayerSprites reads it (from the next snapshot). */
static int Head_ViewTeam() {
    snapshot_t* snap = cg.nextSnap ? cg.nextSnap : cg.snap;
    return clientInfo[snap->ps.clientNum].team;
}

/** Material name of the nation icon of the player (GSC self.headicon) if it's shown to us, the same rule as CG_PlayerSprites, NULL if none. */
static const char* Head_NationIconName(centity_t* cent) {
    int viewTeam = Head_ViewTeam();
    int headIconTeam = cent->nextState.iHeadIconTeam;
    if (!cent->nextState.iHeadIcon || (headIconTeam && viewTeam != TEAM_SPECTATOR && viewTeam != headIconTeam))
        return NULL;
    const char* name = CL_GetConfigString(cent->nextState.iHeadIcon + 0x1e);
    return name[0] ? name : NULL;
}

/**
 * Top of the other icons the game draws over the head of a player with the head info (world units over the head bone), 0 if there is none.
 * The nation icon is drawn by the head info, the game draws one of the lag / chat icons (CG_PlayerSprites). An icon is centered 21 units over the head
 * and has the radius size + 10 world units, but at least cg_headIconMinScreenRadius of the screen height (CG_PlayerFloatSprite, CL_DrawSprite).
 */
static float Head_GameIconsTop(centity_t* cent, const vec3_t head) {
    int viewTeam = Head_ViewTeam();
    int team = clientInfo[cent->nextState.clientNum].team;
    int eFlags = cent->nextState.eFlags;
    float size = -1;
    if (eFlags & ENTITY_FLAG_NO_COMMUNICATION)     // connection problem icon (CG_PlayerSprites 004dbdcc: test al, al / jns)
        size = Head_GameDvarFloat(&cg_connectionIconSize, "cg_connectionIconSize", 0);
    else if ((team == viewTeam || viewTeam == TEAM_SPECTATOR) && (centity_voiceIconTime(cent) > cg.time || (eFlags & ENTITY_FLAG_MENU)))
        size = Head_GameDvarFloat(&cg_voiceIconSize, "cg_voiceIconSize", 0);
    float scale = cg_headIconScale->value.decimal;
    if (size < 0 || scale <= 0)
        return 0;

    // Radius of the icon in world units, made bigger by the engine for far players (both scaled by cg_headIconScale, see CG_PlayerFloatSprite)
    vec3_t delta;
    VectorSubtract(head, cg.refdef.vieworg, delta);
    float minRadius = scale * Head_GameDvarFloat(&cg_headIconMinScreenRadius, "cg_headIconMinScreenRadius", 0.02f) * sqrtf(DotProduct(delta, delta)) * tanf(cg.refdef.fov_y * (float)M_PI / 360.0f) * 2;
    float radius = (size + 10) * scale > minRadius ? (size + 10) * scale : minRadius;

    return 21 + radius;
}

/**
 * Radius of the game icon (size cvar like cg_scriptIconSize) on the screen in the 640x480 virtual space, drawn at the world point, the same as the
 * game draws it (CG_PlayerFloatSprite, RB_TessEntity): the radius size + 10 world units scaled by cg_headIconScale, but at least
 * cg_headIconMinScreenRadius (also scaled) of the screen height. With cg_constantSizeHeadIcons it's (size + 10) * 0.0043 of the screen height.
 * So the size depends only on the size cvar, cg_headIconScale and the distance. 0 if hidden or behind the view.
 */
static float Head_IconScreenRadius(float size, const vec3_t point) {
    float scale = cg_headIconScale->value.decimal;
    if (scale <= 0)
        return 0;
    float radius = (size + 10.0f) * scale;

    if (!cg_constantSizeHeadIcons)
        cg_constantSizeHeadIcons = Dvar_GetDvarByName("cg_constantSizeHeadIcons");
    if (cg_constantSizeHeadIcons && cg_constantSizeHeadIcons->type == DVAR_TYPE_BOOL && cg_constantSizeHeadIcons->value.boolean)
        return radius * 0.0043f * 480.0f;

    // Part of the screen height: the radius projected at the depth of the point
    vec3_t delta;
    VectorSubtract(point, cg.refdef.vieworg, delta);
    float depth = DotProduct(delta, cg.refdef.viewaxis[0]);
    if (depth < 1.0f)
        return 0;
    float part = radius / (2 * depth * tanf(cg.refdef.fov_y * (float)M_PI / 360.0f));
    float minPart = scale * Head_GameDvarFloat(&cg_headIconMinScreenRadius, "cg_headIconMinScreenRadius", 0.02f);
    return (part > minPart ? part : minPart) * 480.0f;
}

/** Point over the player's head where the head info starts (its bottom), above the icons of the game. */
static void Head_GetPoint(centity_t* cent, vec3_t point) {
    Head_GetHeadPos(cent, point);

    // The offset moves the game icons too (CG_PlayerFloatSprite)
    point[2] += cg_headOffset->value.decimal;
    float icons = Head_GameIconsTop(cent, point);
    point[2] += icons > 0 ? icons + 2 : 10;    // just above the game icons, or just over the head
}

/**
 * Alpha of the element of the head info with the box (relative to the screen center in the 640x480 virtual space): drawn through walls for a hidden
 * player it fades near the screen center (by the closest point of the box), so it does not cover an enemy we aim at. A visible player is not faded,
 * an element drawn only when visible neither (its hidden pixels are not drawn).
 */
static float Head_Fade(const headInfo_t* info, int element, float x0, float y0, float x1, float y1) {
    return info->hidden && !info->depthTest[element] ? xray_centerFadeAlpha(x0, y0, x1, y1) : 1.0f;
}

/**
 * Start of drawing of the element, its place is kept whatever this returns. An element drawn only when visible is hidden behind the world per pixel
 * at the depth of the head info point (the stencil test of xray_headDepthTest), marked = the point is already tested for this player.
 * If the renderer can not do it, the element is drawn whole only when the head is visible. False if it's not drawn now.
 */
static bool Head_BeginElement(const headInfo_t* info, int element, const vec3_t point, bool* marked) {
    if (!info->depthTest[element])
        return true;
    if (!xray_headDepthAvailable())
        return !info->hidden;
    return xray_headDepthTest(point, marked);
}

/** End of drawing of the element started by Head_BeginElement (when it returned true). */
static void Head_EndElement(const headInfo_t* info, int element) {
    if (info->depthTest[element] && xray_headDepthAvailable())
        xray_headDepthTestEnd();
}

/** Head info: nation icon, name, weapon icon and weapon name over the players, like in CS:GO. Called in CG_Draw2D, under the HUD. */
static void Head_Draw() {
    if (xray_viewer == XRAY_VIEWER_NONE)
        return;

    // Text scale 0.2 and the weapon icon 2.4 lines high at scale 1
    float scale = 0.2f * cg_headNameScale->value.decimal;
    float lineHeight = (float)UI_TextHeight(fontNormal, scale) * 0.75f;  // the font line is taller than the letters
    float iconUnit = (float)UI_TextHeight(fontNormal, 0.2f) * 0.75f * 2.4f;
    float iconHeight = iconUnit * cg_headWeaponIconScale->value.decimal;
    float nationSize = Head_GameDvarFloat(&cg_scriptIconSize, "cg_scriptIconSize", 0);
    const float alpha = 0.85f;  // of the icons and the weapon name
    bool depthTested = false;   // a point was marked for the depth test, the mark is cleared at the end

    for (int i = 0; i < 64; i++) {
        centity_t* cent = &cg_entities[i];
        headInfo_t info;
        if (!Head_GetInfo(cent, &info))
            continue;
        if (!info.element[HEAD_NATION_ICON] && !info.element[HEAD_NAME] && !info.element[HEAD_WEAPON_ICON] && !info.element[HEAD_WEAPON_NAME])
            continue;
        clientInfo_t* ci = &clientInfo[cent->currentState.clientNum];
        info.hidden = info.needVisibility && !Head_IsVisible(cent);

        vec3_t point;
        Head_GetPoint(cent, point);

        float x, y;
        if (!Head_WorldToScreen(point, &x, &y))
            continue;

        void* weaponDef = info.element[HEAD_WEAPON_ICON] || info.element[HEAD_WEAPON_NAME] ? Head_WeaponDef(Head_HeldWeapon(cent, ci)) : NULL;

        // Elements set to only when visible are hidden behind the world per pixel at the depth of the point, see Head_BeginElement
        bool marked = false;

        // Nation icon of the game (GSC self.headicon), then the game does not draw it (CG_PlayerSprites).
        // The same size as the icon drawn by the game, whatever else is drawn.
        const char* nationIcon = info.element[HEAD_NATION_ICON] ? Head_NationIconName(cent) : NULL;
        float nationHeight = nationIcon ? Head_IconScreenRadius(nationSize, point) * 2 : 0;
        if (nationIcon && nationHeight > 0) {
            if (Head_BeginElement(&info, HEAD_NATION_ICON, point, &marked)) {
                materialHandle_t* icon = CG_RegisterMaterialNoMip(nationIcon, MATERIAL_TYPE_DEFAULT);
                vec4_t color = { 1.0f, 1.0f, 1.0f, alpha * Head_Fade(&info, HEAD_NATION_ICON, x - nationHeight / 2, y - nationHeight, x + nationHeight / 2, y) };
                CG_DrawRotatedPic(x - nationHeight / 2, y - nationHeight, nationHeight, nationHeight, HORIZONTAL_ALIGN_CENTER, VERTICAL_ALIGN_CENTER, 0, color, icon);
                Head_EndElement(&info, HEAD_NATION_ICON);
            }
            y -= nationHeight + 2;
        }

        // Player name in the team color, without its color codes (player names are colored only on the scoreboard)
        if (info.element[HEAD_NAME]) {
            if (Head_BeginElement(&info, HEAD_NAME, point, &marked)) {
                char name[sizeof(ci->name)];
                strip_text_codes(ci->name, name, sizeof(name));
                vec4_t teamColor;
                xray_getTeamColor(ci->team, teamColor);
                float width = (float)UI_TextWidth(name, INT_MAX, fontNormal, scale);
                teamColor[3] *= Head_Fade(&info, HEAD_NAME, x - width / 2, y - lineHeight, x + width / 2, y);
                UI_DrawText(name, INT_MAX, fontNormal, x - width / 2, y, HORIZONTAL_ALIGN_CENTER, VERTICAL_ALIGN_CENTER, scale, teamColor, TEXT_STYLE_SHADOWED);
                Head_EndElement(&info, HEAD_NAME);
            }
            y -= lineHeight + 1;
        }

        // The killfeed icon (side view), or the HUD icon if the weapon has none (some grenades)
        if (weaponDef && info.element[HEAD_WEAPON_ICON] && iconHeight > 0) {
            const char* iconName = WeaponDef_killIcon(weaponDef);
            bool wide = WeaponDef_wideKillIcon(weaponDef);
            if (!iconName[0]) {
                iconName = WeaponDef_hudIcon(weaponDef);
                wide = false;
            }
            if (iconName[0]) {
                if (Head_BeginElement(&info, HEAD_WEAPON_ICON, point, &marked)) {
                    materialHandle_t* icon = CG_RegisterMaterialNoMip(iconName, MATERIAL_TYPE_DEFAULT);
                    float iconWidth = wide ? iconHeight * 2 : iconHeight;
                    vec4_t color = { 1.0f, 1.0f, 1.0f, alpha * Head_Fade(&info, HEAD_WEAPON_ICON, x - iconWidth / 2, y - iconHeight, x + iconWidth / 2, y) };
                    CG_DrawRotatedPic(x - iconWidth / 2, y - iconHeight, iconWidth, iconHeight, HORIZONTAL_ALIGN_CENTER, VERTICAL_ALIGN_CENTER, 0, color, icon);
                    Head_EndElement(&info, HEAD_WEAPON_ICON);
                }
                y -= iconHeight + 1;
            }
        }

        // Weapon name
        if (weaponDef && info.element[HEAD_WEAPON_NAME] && WeaponDef_displayName(weaponDef)[0]) {
            if (Head_BeginElement(&info, HEAD_WEAPON_NAME, point, &marked)) {
                char name[64];
                strip_text_codes(SEH_LocalizeTextMessage(WeaponDef_displayName(weaponDef), "weapon name", 0), name, sizeof(name));
                float nameWidth = (float)UI_TextWidth(name, INT_MAX, fontNormal, scale * 0.8f);
                vec4_t color = { 1.0f, 1.0f, 1.0f, alpha * Head_Fade(&info, HEAD_WEAPON_NAME, x - nameWidth / 2, y - lineHeight * 0.8f, x + nameWidth / 2, y) };
                UI_DrawText(name, INT_MAX, fontNormal, x - nameWidth / 2, y, HORIZONTAL_ALIGN_CENTER, VERTICAL_ALIGN_CENTER, scale * 0.8f, color, TEXT_STYLE_SHADOWED);
                Head_EndElement(&info, HEAD_WEAPON_NAME);
            }
        }

        depthTested |= marked;
    }

    // The stencil mark of the depth test is cleared, after all the head info is drawn
    if (depthTested)
        xray_headDepthClear();
}

/**
 * True if the name under the crosshair of the game (CG_DrawCrosshairNames) is not drawn, because the head info draws the name of that player.
 * Runs the crosshair scan of the game (CG_ScanForCrosshairEntity) like CG_DrawCrosshairNames does, so the target is the current one.
 * Nothing is scanned when the game would not scan either (crosshair names off, third person) or the head name is off for the viewer.
 */
static bool Head_HidesCrosshairName() {
    int state;
    if (xray_viewer == XRAY_VIEWER_SPECTATOR)   state = HEAD_SPECTATOR;
    else if (xray_viewer == XRAY_VIEWER_DEAD)   state = HEAD_DEAD;
    else if (xray_viewer == XRAY_VIEWER_PLAYER) state = HEAD_ALIVE;
    else return false;
    if (!cg_headName[state]->value.integer)
        return false;
    if (!cg_drawCrosshairNames->value.boolean || cg.in_thirdperson)
        return false;

    CG_ScanForCrosshairEntity();

    int clientNum = cg.crosshairClientNum;
    if (clientNum < 0 || clientNum >= 64)
        return false;

    // The same rules as Head_Draw: the name is drawn if the player is shown and the name is on (not too far)
    headInfo_t info;
    return Head_GetInfo(&cg_entities[clientNum], &info) && info.element[HEAD_NAME];
}



// 004dbcc0    void CG_PlayerSprites(struct centity_t* cent @ eax)
static void CG_PlayerSprites(centity_t* cent)
{
    // CoD2x: Size of the nation icon scaled by cg_headIconScale (the engine also places the next icon by it), set only for this call -
    // the dvar is registered again on map load and must keep its own value
    if (!cg_scriptIconSize)
        cg_scriptIconSize = Dvar_GetDvarByName("cg_scriptIconSize");
    float savedSize = 0;
    bool setSize = cg_scriptIconSize && cg_scriptIconSize->type == DVAR_TYPE_FLOAT;
    if (setSize) {
        savedSize = cg_scriptIconSize->value.decimal;
        cg_scriptIconSize->value.decimal = Head_ScaledIconSize(savedSize);
    }
    // CoD2x: End

    // CoD2x: For players of the head info the game draws the nation icon only if it's set to be drawn only when visible and nothing else is on,
    // otherwise it's hidden or drawn by the head info (in the right order with the name)
    headInfo_t info;
    int savedHeadIcon = cent->nextState.iHeadIcon;
    bool hideHeadIcon = Head_GetInfo(cent, &info) && !info.gameNationIcon;
    if (hideHeadIcon)
        cent->nextState.iHeadIcon = 0;
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x004dbcc0, 0, EAX(cent));

    // CoD2x: Back to the head icon of the player
    if (hideHeadIcon)
        cent->nextState.iHeadIcon = savedHeadIcon;
    // CoD2x: End

    // CoD2x: Back to the value of the dvar
    if (setSize)
        cg_scriptIconSize->value.decimal = savedSize;
    // CoD2x: End
}

static void CG_PlayerSprites_Win32() {
    centity_t* cent;
    ASM( movr, cent, "eax" );

    CG_PlayerSprites(cent);
}


// 004dbba0    void CG_PlayerFloatSprite(struct centity_t* cent @ esi, void* material, float size, int32_t height, uint8_t constantSize)
static void CG_PlayerFloatSprite(centity_t* cent, void* material, float size, int height, int constantSize, bool nationIcon)
{
    // CoD2x: All icons over the head are scaled by cg_headIconScale (0 = hidden) and moved by the head info offset. The minimum screen size
    // (cg_headIconMinScreenRadius, keeps far icons readable) is scaled too, otherwise a smaller icon would stay the same at a distance.
    // The size of the nation icon is already scaled (cg_scriptIconSize in CG_PlayerSprites).
    float scale = cg_headIconScale->value.decimal;
    if (scale <= 0)
        return;
    if (!nationIcon)
        size = Head_ScaledIconSize(size);
    height += (int)cg_headOffset->value.decimal;

    Head_GameDvarFloat(&cg_headIconMinScreenRadius, "cg_headIconMinScreenRadius", 0);
    float savedMinRadius = 0;
    bool setMinRadius = cg_headIconMinScreenRadius && cg_headIconMinScreenRadius->type == DVAR_TYPE_FLOAT;
    if (setMinRadius) {
        savedMinRadius = cg_headIconMinScreenRadius->value.decimal;
        cg_headIconMinScreenRadius->value.decimal *= scale;
    }
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x004dbba0, 4, ESI(cent), PUSH(material), PUSH(size), PUSH(height), PUSH(constantSize));

    // CoD2x: Back to the value of the dvar
    if (setMinRadius)
        cg_headIconMinScreenRadius->value.decimal = savedMinRadius;
    // CoD2x: End
}

static void CG_PlayerFloatSprite_Win32(void* material, float size, int height, int constantSize) {
    centity_t* cent;
    ASM( movr, cent, "esi" );

    CG_PlayerFloatSprite(cent, material, size, height, constantSize, false);
}

// The call of the nation icon (script head icon)
static void CG_PlayerFloatSprite_NationIcon_Win32(void* material, float size, int height, int constantSize) {
    centity_t* cent;
    ASM( movr, cent, "esi" );

    CG_PlayerFloatSprite(cent, material, size, height, constantSize, true);
}


// 004e0980    void CG_EntityEvent(int32_t event @ eax, struct centity_t* cent @ ecx)
static void CG_EntityEvent(int event, centity_t* cent)
{
    // CoD2x: Remember the grenade pulled by a player, until it's thrown
    int number = cent->nextState.number;
    if (number >= 0 && number < 64) {
        if (event == EV_PREP_OFFHAND) {
            head_offhand[number].weapon = cent->nextState.eventParm;
            head_offhand[number].time = cg.time;
        } else if (event == EV_USE_OFFHAND) {
            head_offhand[number].weapon = 0;
        }
    }
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x004e0980, 0, EAX(event), ECX(cent));
}

static void CG_EntityEvent_Win32() {
    int event;
    centity_t* cent;
    ASM( movr, event, "eax" );
    ASM( movr, cent, "ecx" );

    CG_EntityEvent(event, cent);
}



/** Registers the cvars of the head info. */
static void Head_RegisterDvars() {
    enum dvarFlags_e flags = (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET);  // not saved to the config
    static const char* states[HEAD_STATES] = { "Spectator", "Dead", "Alive" };
    char name[64];

    // Elements per state: 0 = off, 1 = only when the player is visible, 2 = also through walls
    static const int nationIcon[HEAD_STATES] = { 0, 1, 1 };     // the game draws it for everybody by default
    static const int playerName[HEAD_STATES] = { 2, 2, 0 };
    static const int weaponIcon[HEAD_STATES] = { 2, 2, 0 };
    static const int weaponName[HEAD_STATES] = { 0, 0, 0 };
    for (int i = 0; i < HEAD_STATES; i++) {
        snprintf(name, sizeof(name), "cg_headNationIcon%s", states[i]);
        cg_headNationIcon[i] = Dvar_RegisterInt(name, nationIcon[i], 0, 2, flags);
        snprintf(name, sizeof(name), "cg_headName%s", states[i]);
        cg_headName[i] = Dvar_RegisterInt(name, playerName[i], 0, 2, flags);
        snprintf(name, sizeof(name), "cg_headWeaponIcon%s", states[i]);
        cg_headWeaponIcon[i] = Dvar_RegisterInt(name, weaponIcon[i], 0, 2, flags);
        snprintf(name, sizeof(name), "cg_headWeaponName%s", states[i]);
        cg_headWeaponName[i] = Dvar_RegisterInt(name, weaponName[i], 0, 2, flags);
    }

    cg_headNameScale =          Dvar_RegisterFloat("cg_headNameScale", 1.0f, 0.3f, 3.0f, flags);
    cg_headWeaponIconScale =    Dvar_RegisterFloat("cg_headWeaponIconScale", 1.0f, 0.0f, 3.0f, flags);
    cg_headIconScale =          Dvar_RegisterFloat("cg_headIconScale", 1.0f, 0.0f, 10.0f, flags);
    cg_headOffset =             Dvar_RegisterFloat("cg_headOffset", 0.0f, -64.0f, 128.0f, flags);
    cg_headMaxDistance =        Dvar_RegisterFloat("cg_headMaxDistance", 0.0f, 0.0f, 10000.0f, flags);
}

/** Patches of the head info. */
static void Head_Patch() {
    patch_call(0x004cbc7b, (unsigned int)CG_PlayerSprites_Win32); // 004cbc7b  call CG_PlayerSprites   (in CG_DrawPlayerSprites)

    // CG_PlayerFloatSprite calls in CG_PlayerSprites
    patch_call(0x004dbd69, (unsigned int)CG_PlayerFloatSprite_NationIcon_Win32);    // 004dbd69  e832feffff  call CG_PlayerFloatSprite   (script head icon)
    patch_call(0x004dbdb7, (unsigned int)CG_PlayerFloatSprite_Win32);               // 004dbdb7  e8e4fdffff  call CG_PlayerFloatSprite   (you in killcam)
    patch_call(0x004dbded, (unsigned int)CG_PlayerFloatSprite_Win32);               // 004dbded  e8aefdffff  call CG_PlayerFloatSprite   (connection problem)
    patch_call(0x004dbe4f, (unsigned int)CG_PlayerFloatSprite_Win32);               // 004dbe4f  e84cfdffff  call CG_PlayerFloatSprite   (talking / chat)
    patch_call(0x004e0393, (unsigned int)CG_EntityEvent_Win32); // 004e0393  e8e8050000  call CG_EntityEvent   (predicted player events)
    patch_call(0x004e1ac6, (unsigned int)CG_EntityEvent_Win32); // 004e1ac6  e8b5eeffff  call CG_EntityEvent   (event entities)
    patch_call(0x004e1b4f, (unsigned int)CG_EntityEvent_Win32); // 004e1b4f  e82ceeffff  call CG_EntityEvent   (entity events)
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


// 004c97c0    void CG_DrawCrosshairNames()
void CG_DrawCrosshairNames() {
    // CoD2x: Name and weapon over the players for spectators (and dead players over their teammates)
    Head_Draw();
    // CoD2x: End

    // CoD2x: The name under the crosshair is not drawn when the head info draws the name of that player (the crosshair scan is already done)
    if (Head_HidesCrosshairName())
        return;
    // CoD2x: End

    // Call the original function
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

    Head_RegisterDvars();
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

    Head_Patch();
}
