#include "debug_input.h"

/**
 * Debug-only console commands that play the client for automated tests (debug builds, client only).
 * They are normal console commands - type them in the console or send them through the control pipe
 * (debug_pipe.cpp, tools/game_pipe.py). Nothing focuses the window or sends Windows input:
 *
 *   players                          players with id, team, health, distance, visibility and name
 *   bones <id|name>                  which body parts / bones of the player can be aimed at
 *   aim <id|name> [part|bone] [ms|hold]   turns the view to the body part (default head) of the player; with ms or "hold"
 *                                    keeps following it for ms / until "aim off"
 *   look <pitch> <yaw>               sets the view angles (world angles, pitch < 0 = up)
 *   turn <pitch> <yaw>               turns the view by the angles
 *   press <+button> [ms]             holds a button command (+attack, +forward, +gostand, +melee, +reload, +frag, ...)
 *   move <forward|back|left|right|jump|crouch|prone> [ms]   short forms of press
 *   fire [ms]                        press +attack
 *   key <keyname> [ms]               presses a key like a real keyboard / mouse (binds, menus, console, chat): ENTER, ESCAPE,
 *                                    TAB, SPACE, MOUSE1, UPARROW, F1, a ... (the names of bind)
 *   type <text>                      types the text as characters into the focused field (chat, console, menu edit field)
 *   menus [all]                      open menus and their visible items with the cursor position of their center
 *   click <item>|<x> <y> [keyname]   moves the menu cursor to the item (name or text) or to x y (640x480) and clicks
 *   release                          releases every held button / key and stops aiming
 *
 * Buttons and keys are released by debug_input_frame() when their time is over (ms default: press / move 500, fire 50,
 * key 0 = down and up at once). Keys, characters and clicks are queued as system events (Sys_QueEvent) and handled at the
 * start of the next frame like Windows input.
 */

#if DEBUG

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "shared.h"
#include "drawing.h"
#include "window.h"
#include "xray.h"
#include "debug_pipe.h"
#include "../shared/cod2_cmd.h"
#include "../shared/cod2_client.h"
#include "../shared/cod2_math.h"
#include "../shared/cod2_menu.h"

#define DEBUG_INPUT_HOLDS               16
#define DEBUG_INPUT_MAX_TYPE            200     // characters per type command, the system event queue has 256 events

// A held button command ("+attack", released with "-attack") or key (command empty)
struct debugInputHold_t {
    bool active;
    char command[32];
    int key;
    DWORD releaseTime;          // GetTickCount
};
static debugInputHold_t debug_input_holds[DEBUG_INPUT_HOLDS];

// Aim that follows a player each frame
static int debug_input_aimClient = -1;
static char debug_input_aimBone[64];
static float debug_input_aimUp = 0;
static bool debug_input_aimForever = false;
static DWORD debug_input_aimUntil = 0;

struct debugInputPart_t {
    const char* part;
    const char* bone;
    float up;                   // units over the bone: j_head is the joint at the bottom of the head, a shot at it hits the neck
};
// Body parts of the player model - the bone is also accepted directly (aim 3 j_spine4)
static const debugInputPart_t debug_input_parts[] = {
    { "head",       "j_head", 5 },
    { "neck",       "j_neck", 0 },
    { "chest",      "j_spine4", 0 },
    { "stomach",    "j_spine2", 0 },
    { "pelvis",     "pelvis", 0 },
    { "shoulder_l", "j_shoulder_le", 0 },
    { "shoulder_r", "j_shoulder_ri", 0 },
    { "hand_l",     "j_wrist_le", 0 },
    { "hand_r",     "j_wrist_ri", 0 },
    { "knee_l",     "j_knee_le", 0 },
    { "knee_r",     "j_knee_ri", 0 },
    { "foot_l",     "j_ankle_le", 0 },
    { "foot_r",     "j_ankle_ri", 0 },
};

/**
 * Points level_bgs to the cgame animation state while it exists, like CG_DrawActiveFrame does for cgame code. Needed around
 * engine calls that pose player models outside the cgame frame (CG_DObjGetWorldTagPos, traces against players) - they run
 * BG_Player_DoControllers, which reads level_bgs.
 */
struct debugInputCgameBgs_t {
    bgs_t* saved;
    debugInputCgameBgs_t() : saved(level_bgs) { level_bgs = cg_bgs; }
    ~debugInputCgameBgs_t() { level_bgs = saved; }
};


/** Prints the error in red and makes the control pipe reply "error" for the running command. */
static void debug_input_error(const char* format, ...) {
    char msg[512];
    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    Com_Printf("^1%s\n", msg);
    debug_pipe_commandError(msg);
}

static bool debug_input_isNumber(const char* s) {
    if (*s == '-') {
        s++;
    }
    if (!*s) {
        return false;
    }
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) {
            return false;
        }
    }
    return true;
}

/** Text without color and style codes, lowercase if lower. */
static void debug_input_cleanText(const char* in, char* out, size_t size, bool lower) {
    strip_text_codes(in ? in : "", out, size);
    if (lower) {
        I_strlwr(out);
    }
}

/** True if the client is in a game (snapshot and cgame running). */
static bool debug_input_inGame() {
    if (clientState != CLIENT_STATE_ACTIVE || !cg.snap) {
        debug_input_error("not in a game (clientState %s)", get_client_state_name(clientState));
        return false;
    }
    return true;
}


/** Client number of the player by id or name (case-insensitive, codes ignored, exact name first, then a part of it). -1 = error printed. */
static int debug_input_findPlayer(const char* who) {
    if (debug_input_isNumber(who)) {
        int clientNum = atoi(who);
        if (clientNum >= 0 && clientNum < 64 && clientInfo[clientNum].infoValid) {
            return clientNum;
        }
        debug_input_error("no player with id %s", who);
        return -1;
    }

    char wanted[64], name[64];
    debug_input_cleanText(who, wanted, sizeof(wanted), true);

    int found = -1, count = 0;
    for (int i = 0; i < 64; i++) {
        if (!clientInfo[i].infoValid) {
            continue;
        }
        debug_input_cleanText(clientInfo[i].name, name, sizeof(name), true);
        if (strcmp(name, wanted) == 0) {
            return i;
        }
        if (strstr(name, wanted)) {
            found = i;
            count++;
        }
    }
    if (count == 1) {
        return found;
    }
    if (count == 0) {
        debug_input_error("no player name contains '%s' (see players)", who);
    } else {
        debug_input_error("'%s' matches %d players, use the id (see players)", who, count);
    }
    return -1;
}


/** Bone of a body part name and the height over it to aim at, or the name itself (a bone like j_spine4) with 0. */
static const char* debug_input_boneOfPart(const char* part, float* up) {
    for (const debugInputPart_t& p : debug_input_parts) {
        if (Q_stricmp(part, p.part) == 0) {
            *up = p.up;
            return p.bone;
        }
    }
    *up = 0;
    return part;
}


/**
 * World position of the bone of the player as drawn now (follows stance, lean and animations).
 * Returns NULL, or why there is no position (the player is not in the snapshot, dead or has no such bone).
 */
static const char* debug_input_bonePos(int clientNum, const char* bone, vec3_t pos) {
    static char error[128];
    centity_t* cent = &cg_entities[clientNum];
    if (!(cent->currentValid & 1) || cent->currentState.eType != ET_PLAYER) {
        snprintf(error, sizeof(error), "player %d is not in the snapshot (not alive, spectator, or the server does not send him)", clientNum);
        return error;
    }
    if (cent->currentState.eFlags & EF_DEAD) {
        snprintf(error, sizeof(error), "player %d is dead", clientNum);
        return error;
    }
    uint32_t tag = SL_FindString(bone);
    void* dobj = Com_GetClientDObj(cent->nextState.number, centity_dobj(cent));
    debugInputCgameBgs_t bgs;
    if (!tag || !dobj || !CG_DObjGetWorldTagPos(tag, dobj, cent, pos)) {
        snprintf(error, sizeof(error), "player %d has no bone '%s' (see bones %d)", clientNum, bone, clientNum);
        return error;
    }
    return NULL;
}


/**
 * Turns the view to the world angles (pitch, yaw). The player looks at cl_viewangles + delta_angles of the player state
 * (PM_UpdateViewAngles), so the client angles are the wanted angles minus the delta angles.
 */
static void debug_input_setViewAngles(float pitch, float yaw) {
    playerState_t* ps = &cg.predictedPlayerState;
    cl_viewangles[PITCH] = AngleNormalize180(pitch - SHORT2ANGLE(ps->delta_angles[PITCH]));
    cl_viewangles[YAW] = AngleNormalize180(yaw - SHORT2ANGLE(ps->delta_angles[YAW]));
}


/** Turns the view from the eye to the position. Returns the angles. */
static void debug_input_lookAtPos(const vec3_t pos, vec3_t angles) {
    vec3_t dir;
    VectorSubtract(pos, cg.refdef.vieworg, dir);
    VecToAngles(dir, angles);
    angles[PITCH] = AngleNormalize180(angles[PITCH]);
    angles[YAW] = AngleNormalize180(angles[YAW]);
    debug_input_setViewAngles(angles[PITCH], angles[YAW]);
}


/**
 * What is first on the line from the eye to the position of the target player: "clear" (the target), "blocked by player <id> <name>"
 * or "blocked by the world" (the bullet mask: solid, player bodies and what else stops bullets). The client trace uses the player
 * bounding boxes, not the hitboxes, so "clear" is a good sign, not a guarantee.
 */
static void debug_input_lineOfFire(int clientNum, const vec3_t pos, char* out, size_t size) {
    vec3_t zero = { 0, 0, 0 };
    trace_t trace;
    {
        debugInputCgameBgs_t bgs;
        CG_TraceCapsule(&trace, cg.refdef.vieworg, zero, zero, pos, cg.snap->ps.clientNum, 0x2802831);   // mask of Bullet_Fire_Extended (0052778c)
    }
    if (trace.fraction >= 1.0f || trace.entityNum == clientNum) {
        snprintf(out, size, "clear");
    } else if (trace.entityNum < 64) {
        char name[64];
        debug_input_cleanText(clientInfo[trace.entityNum].name, name, sizeof(name), false);
        snprintf(out, size, "blocked by player %d %s", trace.entityNum, name);
    } else {
        snprintf(out, size, "blocked by the world at %.0f%% of the distance", trace.fraction * 100.0f);
    }
}


/** Presses or releases the button command / key of the hold. */
static void debug_input_holdSet(debugInputHold_t* h, bool down) {
    if (h->command[0]) {
        char cmd[34];
        snprintf(cmd, sizeof(cmd), "%c%s", down ? '+' : '-', h->command + 1);
        Cmd_ExecuteString(cmd);
    } else {
        Sys_QueEvent(0, SE_KEY, h->key, down, 0, NULL);
    }
}

/**
 * Holds a button command (+attack, command) or a key (command NULL) for ms, or extends the hold.
 * ms <= 0 = one frame for commands, down and up in the same event loop for keys.
 */
static void debug_input_hold(const char* command, int key, int ms) {
    DWORD release = GetTickCount() + (ms > 0 ? (DWORD)ms : 0);
    debugInputHold_t* slot = NULL;

    for (debugInputHold_t& h : debug_input_holds) {
        if (!h.active) {
            if (!slot) slot = &h;
        } else if (command ? Q_stricmp(h.command, command) == 0 : (!h.command[0] && h.key == key)) {
            h.releaseTime = release;    // already held - extend
            return;
        }
    }
    if (!slot) {
        debug_input_error("too many held buttons and keys, see release");
        return;
    }

    // The command is copied before it runs: it is usually Cmd_Argv(1), and Cmd_ExecuteString tokenizes into the same argv buffer
    snprintf(slot->command, sizeof(slot->command), "%s", command ? command : "");
    slot->key = key;
    slot->releaseTime = release;
    debug_input_holdSet(slot, true);
    if (!command && ms <= 0) {
        debug_input_holdSet(slot, false);
        return;
    }
    slot->active = true;
}


/** Releases every held button and key and stops aiming. Also the command "release". */
static void debug_input_releaseAll() {
    for (debugInputHold_t& h : debug_input_holds) {
        if (h.active) {
            h.active = false;
            debug_input_holdSet(&h, false);
        }
    }
    debug_input_aimClient = -1;
}


/** Milliseconds argument, def if missing. */
static int debug_input_msArg(int index, int def) {
    return Cmd_Argc() > index ? atoi(Cmd_Argv(index)) : def;
}


// players
static void debug_input_cmdPlayers() {
    if (!debug_input_inGame()) {
        return;
    }
    static const char* teams[TEAM_NUM_TEAMS] = { "free", "axis", "allies", "spectator" };
    bool playing = cg.snap->ps.clientNum == cg.clientNum && !(cg.snap->ps.pm_flags & PMF_SPECTATING);

    Com_Printf(" id  team       hp  dist  state    name\n");
    for (int i = 0; i < 64; i++) {
        clientInfo_t* ci = &clientInfo[i];
        if (!ci->infoValid) {
            continue;
        }
        centity_t* cent = &cg_entities[i];
        bool self = (i == cg.clientNum);
        bool inSnapshot = (cent->currentValid & 1) && cent->currentState.eType == ET_PLAYER;

        const char* state = "-";
        char hp[16] = "-", dist[16] = "-";
        playerInfo_t info;
        if (self && playing) {
            state = cg.snap->ps.stats[0] > 0 ? "alive" : "dead";
            snprintf(hp, sizeof(hp), "%d", cg.snap->ps.stats[0]);
        } else if (inSnapshot) {
            state = (cent->currentState.eFlags & EF_DEAD) ? "dead" : "alive";
        }
        if (!self && xray_getPlayerInfo(i, &info)) {
            snprintf(hp, sizeof(hp), "%d", info.health);
        }
        if (!self && inSnapshot) {
            vec3_t d;
            VectorSubtract(cent->lerpOrigin, cg.refdef.vieworg, d);
            snprintf(dist, sizeof(dist), "%d", (int)VectorLength(d));
        }

        char name[64];
        debug_input_cleanText(ci->name, name, sizeof(name), false);
        const char* team = (ci->team >= 0 && ci->team < TEAM_NUM_TEAMS) ? teams[ci->team] : "?";
        Com_Printf("%3d  %-9s %4s %5s  %-7s  %s%s\n", i, team, hp, dist, state, name, self ? "  (you)" : "");
    }
}


// bones <id|name>
static void debug_input_cmdBones() {
    if (Cmd_Argc() < 2) {
        debug_input_error("usage: bones <id|name>");
        return;
    }
    if (!debug_input_inGame()) {
        return;
    }
    int clientNum = debug_input_findPlayer(Cmd_Argv(1));
    if (clientNum < 0) {
        return;
    }
    vec3_t pos;
    const char* error = debug_input_bonePos(clientNum, "j_head", pos);
    if (error) {
        debug_input_error("%s", error);
        return;
    }
    for (const debugInputPart_t& p : debug_input_parts) {
        if (!debug_input_bonePos(clientNum, p.bone, pos)) {
            Com_Printf("%-11s %-14s (%.0f %.0f %.0f)\n", p.part, p.bone, pos[0], pos[1], pos[2]);
        } else {
            Com_Printf("%-11s %-14s not found\n", p.part, p.bone);
        }
    }
}


// aim <id|name> [part|bone] [ms|hold] / aim off
static void debug_input_cmdAim() {
    if (Cmd_Argc() < 2) {
        debug_input_error("usage: aim <id|name> [head|neck|chest|stomach|pelvis|...|bone] [ms|hold], aim off");
        return;
    }
    if (Q_stricmp(Cmd_Argv(1), "off") == 0) {
        debug_input_aimClient = -1;
        Com_Printf("aim off\n");
        return;
    }
    if (!debug_input_inGame()) {
        return;
    }
    int clientNum = debug_input_findPlayer(Cmd_Argv(1));
    if (clientNum < 0) {
        return;
    }
    if (clientNum == cg.clientNum) {
        debug_input_error("can not aim at yourself");
        return;
    }

    // Optional body part and follow time, in any order: aim 3, aim 3 chest, aim 3 hold, aim 3 chest 2000
    const char* part = NULL;
    const char* follow = NULL;
    for (int i = 2; i < Cmd_Argc(); i++) {
        const char* arg = Cmd_Argv(i);
        bool isFollow = debug_input_isNumber(arg) || Q_stricmp(arg, "hold") == 0;
        const char** slot = isFollow ? &follow : &part;
        if (*slot) {
            debug_input_error("usage: aim <id|name> [part|bone] [ms|hold] - '%s' is a second %s", arg, isFollow ? "follow time" : "body part");
            return;
        }
        *slot = arg;
    }
    float up;
    const char* bone = debug_input_boneOfPart(part ? part : "head", &up);
    vec3_t pos, angles;
    const char* error = debug_input_bonePos(clientNum, bone, pos);
    if (error) {
        debug_input_error("%s", error);
        return;
    }
    pos[2] += up;
    debug_input_lookAtPos(pos, angles);

    vec3_t d;
    VectorSubtract(pos, cg.refdef.vieworg, d);
    char name[64], lineOfFire[96];
    debug_input_cleanText(clientInfo[clientNum].name, name, sizeof(name), false);
    debug_input_lineOfFire(clientNum, pos, lineOfFire, sizeof(lineOfFire));
    Com_Printf("aim at %d %s, %s (%.0f %.0f %.0f), distance %.0f, view pitch %.1f yaw %.1f\nline of fire: %s\n",
        clientNum, name, bone, pos[0], pos[1], pos[2], VectorLength(d), angles[PITCH], angles[YAW], lineOfFire);

    // Keep following
    debug_input_aimClient = -1;
    if (follow) {
        snprintf(debug_input_aimBone, sizeof(debug_input_aimBone), "%s", bone);
        debug_input_aimUp = up;
        debug_input_aimClient = clientNum;
        debug_input_aimForever = Q_stricmp(follow, "hold") == 0;
        debug_input_aimUntil = GetTickCount() + (DWORD)atoi(follow);
    }
}


// look <pitch> <yaw>
static void debug_input_cmdLook() {
    if (Cmd_Argc() < 3) {
        debug_input_error("usage: look <pitch> <yaw>   (world angles, pitch -85 up .. 85 down, yaw 0 = +x, 90 = +y)");
        return;
    }
    if (!debug_input_inGame()) {
        return;
    }
    debug_input_aimClient = -1;
    debug_input_setViewAngles((float)atof(Cmd_Argv(1)), (float)atof(Cmd_Argv(2)));
}


// turn <pitch> <yaw>
static void debug_input_cmdTurn() {
    if (Cmd_Argc() < 3) {
        debug_input_error("usage: turn <pitch> <yaw>   (relative angles, yaw > 0 = left)");
        return;
    }
    debug_input_aimClient = -1;
    cl_viewangles[PITCH] = AngleNormalize180(cl_viewangles[PITCH] + (float)atof(Cmd_Argv(1)));
    cl_viewangles[YAW] = AngleNormalize180(cl_viewangles[YAW] + (float)atof(Cmd_Argv(2)));
}


// press <+button> [ms]
static void debug_input_cmdPress() {
    if (Cmd_Argc() < 2 || Cmd_Argv(1)[0] != '+') {
        debug_input_error("usage: press <+button> [ms=500]   (+attack, +forward, +back, +moveleft, +moveright, +gostand, +melee, +reload, +frag, +smoke, +activate, +leanleft, +leanright, +speed, ...)");
        return;
    }
    debug_input_hold(Cmd_Argv(1), 0, debug_input_msArg(2, 500));
}


// move <forward|back|left|right|jump|crouch|prone> [ms]
static void debug_input_cmdMove() {
    static const struct { const char* dir; const char* button; } moves[] = {
        { "forward", "+forward" }, { "back", "+back" }, { "left", "+moveleft" }, { "right", "+moveright" },
        { "jump", "+gostand" }, { "crouch", "+movedown" }, { "prone", "+prone" },
    };
    for (const auto& m : moves) {
        if (Cmd_Argc() >= 2 && Q_stricmp(Cmd_Argv(1), m.dir) == 0) {
            debug_input_hold(m.button, 0, debug_input_msArg(2, 500));
            return;
        }
    }
    debug_input_error("usage: move <forward|back|left|right|jump|crouch|prone> [ms=500]");
}


// fire [ms]
static void debug_input_cmdFire() {
    debug_input_hold("+attack", 0, debug_input_msArg(1, 50));
}


// key <keyname> [ms]
static void debug_input_cmdKey() {
    if (Cmd_Argc() < 2) {
        debug_input_error("usage: key <keyname> [ms=0]   (ENTER, ESCAPE, TAB, SPACE, BACKSPACE, MOUSE1, UPARROW, F1, a, ... - the names of bind)");
        return;
    }
    int key = Key_StringToKeynum(Cmd_Argv(1));
    if (key < 0) {
        debug_input_error("unknown key '%s'", Cmd_Argv(1));
        return;
    }
    debug_input_hold(NULL, key, debug_input_msArg(2, 0));
}


// type <text>
static void debug_input_cmdType() {
    if (Cmd_Argc() < 2) {
        debug_input_error("usage: type <text>   (into the chat field, console or menu edit field that has the input)");
        return;
    }
    if (!(cls_keyCatchers & (KEYCATCH_CONSOLE | KEYCATCH_UI | KEYCATCH_MESSAGE))) {
        debug_input_error("no text field has the input - open the chat (chatmodepublic / chatmodeteam), the console or a menu first");
        return;
    }
    // One event per character - the system event queue holds 256 events (Sys_QueEvent drops the oldest when full)
    int length = Cmd_Argc() - 2;
    for (int i = 1; i < Cmd_Argc(); i++) {
        length += (int)strlen(Cmd_Argv(i));
    }
    if (length > DEBUG_INPUT_MAX_TYPE) {
        debug_input_error("text too long: %d characters, at most %d per type command", length, DEBUG_INPUT_MAX_TYPE);
        return;
    }
    // The arguments joined with spaces
    for (int i = 1; i < Cmd_Argc(); i++) {
        if (i > 1) {
            Sys_QueEvent(0, SE_CHAR, ' ', 0, 0, NULL);
        }
        for (const char* c = Cmd_Argv(i); *c; c++) {
            Sys_QueEvent(0, SE_CHAR, (unsigned char)*c, 0, 0, NULL);
        }
    }
}


/** Cursor position (640x480) of the center of a menu item, like the item hit test sees it. */
static void debug_input_itemCenter(char* item, float* x, float* y) {
    float* r = WINDOWDEF_RECT(item);
    float rx = r[0], ry = r[1], rw = r[2], rh = r[3];
    CalcScreenPlacement(&rx, &ry, &rw, &rh, WINDOWDEF_HORZALIGN(item), WINDOWDEF_VERTALIGN(item));
    *x = (rx + rw / 2) / scrPlace_scaleVirtualToFull[0];
    *y = (ry + rh / 2) / scrPlace_scaleVirtualToFull[1];
}

/** True if the item is drawn: visible and its showDvar / hideDvar condition allows it (like the item hit test 00542330). */
static bool debug_input_itemShown(char* item) {
    if (!item || !(WINDOWDEF_FLAGS(item) & WINDOW_VISIBLE)) {
        return false;
    }
    return !(ITEMDEF_CVARFLAGS(item) & (CVAR_SHOW | CVAR_HIDE)) || Item_EnableShowViaCvar(item, CVAR_SHOW);
}

/** True if the item reacts to the mouse: its enableDvar / disableDvar condition allows it. */
static bool debug_input_itemEnabled(char* item) {
    return !(ITEMDEF_CVARFLAGS(item) & (CVAR_ENABLE | CVAR_DISABLE)) || Item_EnableShowViaCvar(item, CVAR_ENABLE);
}

/** Text of the item without codes and the '@' of a localized string key, lowercase if lower. */
static void debug_input_itemText(char* item, char* out, size_t size, bool lower) {
    const char* text = ITEMDEF_TEXT(item);
    debug_input_cleanText(text && text[0] == '@' ? text + 1 : text, out, size, lower);
}


// menus [all]
static void debug_input_cmdMenus() {
    char* dc = ui_displayContext;   // static in the exe, never NULL
    if (Cmd_Argc() >= 2 && Q_stricmp(Cmd_Argv(1), "all") == 0) {
        for (int i = 0; i < DISPLAYCONTEXT_MENUCOUNT(dc); i++) {
            const char* name = WINDOWDEF_NAME(DISPLAYCONTEXT_MENUS(dc)[i]);
            Com_Printf("%s\n", name ? name : "(no name)");
        }
        return;
    }

    Com_Printf("cursor %d %d, menu input %s\n", DISPLAYCONTEXT_CURSOR_X(dc), DISPLAYCONTEXT_CURSOR_Y(dc), (cls_keyCatchers & KEYCATCH_UI) ? "yes" : "no");

    for (int m = DISPLAYCONTEXT_OPENMENUCOUNT(dc) - 1; m >= 0; m--) {
        char* menu = DISPLAYCONTEXT_MENUSTACK(dc)[m];
        int flags = WINDOWDEF_FLAGS(menu);
        Com_Printf("menu %s%s%s\n", WINDOWDEF_NAME(menu) ? WINDOWDEF_NAME(menu) : "(no name)",
            (flags & WINDOW_VISIBLE) ? "" : " (hidden)", (flags & WINDOW_HASFOCUS) ? " (focus)" : "");
        for (int i = 0; i < MENUDEF_ITEMCOUNT(menu); i++) {
            char* item = MENUDEF_ITEMS(menu)[i];
            if (!debug_input_itemShown(item)) {
                continue;
            }
            char text[128];
            debug_input_itemText(item, text, sizeof(text), false);
            const char* name = WINDOWDEF_NAME(item);
            if (!text[0] && !name) {
                continue; // decoration
            }
            float x, y;
            debug_input_itemCenter(item, &x, &y);
            const char* dvar = ITEMDEF_DVAR(item);
            Com_Printf("  %4.0f %4.0f  %-24s %s%s%s%s\n", x, y, name ? name : "-", text,
                dvar ? "  dvar " : "", dvar ? dvar : "", debug_input_itemEnabled(item) ? "" : "  (disabled)");
        }
    }
}


/** Shown and enabled item of the visible open menus by name or text (exact, case-insensitive), else the first whose text contains it. Top menu first. */
static char* debug_input_findItem(const char* wanted) {
    char* dc = ui_displayContext;
    char lowWanted[128];
    debug_input_cleanText(wanted, lowWanted, sizeof(lowWanted), true);

    char* partial = NULL;
    for (int m = DISPLAYCONTEXT_OPENMENUCOUNT(dc) - 1; m >= 0; m--) {
        char* menu = DISPLAYCONTEXT_MENUSTACK(dc)[m];
        if (!(WINDOWDEF_FLAGS(menu) & WINDOW_VISIBLE)) {
            continue;
        }
        for (int i = 0; i < MENUDEF_ITEMCOUNT(menu); i++) {
            char* item = MENUDEF_ITEMS(menu)[i];
            if (!debug_input_itemShown(item) || !debug_input_itemEnabled(item)) {
                continue;
            }
            char text[128], name[128];
            debug_input_itemText(item, text, sizeof(text), true);
            debug_input_cleanText(WINDOWDEF_NAME(item), name, sizeof(name), true);
            if (strcmp(name, lowWanted) == 0 || strcmp(text, lowWanted) == 0) {
                return item;
            }
            if (!partial && text[0] && strstr(text, lowWanted)) {
                partial = item;
            }
        }
    }
    return partial;
}


// click <item> [keyname] / click <x> <y> [keyname]
static void debug_input_cmdClick() {
    if (Cmd_Argc() < 2) {
        debug_input_error("usage: click <item name or text> [key=MOUSE1], click <x> <y> [key=MOUSE1]   (x y in 640x480, see menus)");
        return;
    }
    if (!(cls_keyCatchers & KEYCATCH_UI)) {
        debug_input_error("no menu has the input");
        return;
    }

    float x, y;
    int keyArg;
    if (Cmd_Argc() >= 3 && debug_input_isNumber(Cmd_Argv(1)) && debug_input_isNumber(Cmd_Argv(2))) {
        x = (float)atoi(Cmd_Argv(1));
        y = (float)atoi(Cmd_Argv(2));
        keyArg = 3;
    } else {
        char* item = debug_input_findItem(Cmd_Argv(1));
        if (!item) {
            debug_input_error("no visible item '%s' in the open menus (see menus)", Cmd_Argv(1));
            return;
        }
        debug_input_itemCenter(item, &x, &y);
        keyArg = 2;
    }

    const char* keyName = Cmd_Argc() > keyArg ? Cmd_Argv(keyArg) : "MOUSE1";
    int key = Key_StringToKeynum(keyName);
    if (key < 0) {
        debug_input_error("unknown key '%s'", keyName);
        return;
    }

    Mouse_SetMenuCursorPos((int)x, (int)y);    // also updates the hover (focus) of the items under the cursor
    Sys_QueEvent(0, SE_KEY, key, 1, 0, NULL);
    Sys_QueEvent(0, SE_KEY, key, 0, 0, NULL);
    Com_Printf("click %.0f %.0f\n", x, y);
}


/** Called every frame on frame start (client only). Releases buttons whose time is over and follows the aimed player. */
void debug_input_frame() {
    DWORD now = GetTickCount();
    for (debugInputHold_t& h : debug_input_holds) {
        if (h.active && (int)(now - h.releaseTime) >= 0) {
            h.active = false;
            debug_input_holdSet(&h, false);
        }
    }

    if (debug_input_aimClient < 0) {
        return;
    }
    if ((!debug_input_aimForever && (int)(now - debug_input_aimUntil) >= 0) || clientState != CLIENT_STATE_ACTIVE || !cg.snap) {
        debug_input_aimClient = -1;
        return;
    }
    // Not visible or dead: keep the last angles and wait
    vec3_t pos, angles;
    if (!debug_input_bonePos(debug_input_aimClient, debug_input_aimBone, pos)) {
        pos[2] += debug_input_aimUp;
        debug_input_lookAtPos(pos, angles);
    }
}


static const struct { const char* name; void (*func)(); } debug_input_commands[] = {
    { "players", debug_input_cmdPlayers },
    { "bones",   debug_input_cmdBones },
    { "aim",     debug_input_cmdAim },
    { "look",    debug_input_cmdLook },
    { "turn",    debug_input_cmdTurn },
    { "press",   debug_input_cmdPress },
    { "move",    debug_input_cmdMove },
    { "fire",    debug_input_cmdFire },
    { "key",     debug_input_cmdKey },
    { "type",    debug_input_cmdType },
    { "menus",   debug_input_cmdMenus },
    { "click",   debug_input_cmdClick },
    { "release", debug_input_releaseAll },
};


/** Called before hot reload. */
void debug_input_unload() {
    debug_input_releaseAll();
    for (const auto& c : debug_input_commands) {
        Cmd_RemoveCommand(c.name);
    }
}


/** Called once on client start (and after hot reload). */
void debug_input_init() {
    for (const auto& c : debug_input_commands) {
        Cmd_AddCommand(c.name, c.func);
    }
}

#else

// Release build: no test input commands
void debug_input_frame() {}
void debug_input_unload() {}
void debug_input_init() {}

#endif
