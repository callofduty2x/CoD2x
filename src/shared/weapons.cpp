#include "weapons.h"

#include "shared.h"
#include "cod2_common.h"
#include "cod2_cmd.h"
#include "cod2_math.h"
#include "cod2_entity.h"
#include "cod2_dvars.h"
#include "cod2_server.h"
#include "cod2_client.h"
#include <math.h>
#include <stdio.h>


// Toggle for custom pellet distribution (true) vs original random spread (false)
dvar_t* g_shotgun_spread_fix;

// Bullet penetration through walls
dvar_t* sv_wallbang;
dvar_t* sv_wallbang_depth;
dvar_t* sv_wallbang_damage;
dvar_t* sv_wallbang_maxWalls;
dvar_t* sv_wallbang_debug;
dvar_t* sv_wallbang_surface[SURF_TYPECOUNT];

// Surface type names (as in bulletTrace()["surfacetype"]) and their default max wall thickness multiplier.
// Relative to wood, by the thickness of the material that stops a 7.62mm rifle bullet (US Army field manuals):
// timber ~40 in, packed earth ~30 in, clay ~18 in, dry sand ~15 in, gravel ~12 in, brick ~8 in, concrete ~6 in, steel ~0.5 in.
// 1 unit is about 1 inch, wood 1 = sv_wallbang_depth (24) units.
static const struct { const char* name; float depthScale; } wallbang_surfaces[SURF_TYPECOUNT] = {
    { "none",     10.0f },  // caulk, nodraw and textures without surface type - as if not there (weapon clip and sky are never penetrable)
    { "bark",     1.0f  },  // tree trunk, solid wood
    { "brick",    0.25f },
    { "carpet",   4.0f  },  // soft materials barely slow the bullet down
    { "cloth",    4.0f  },
    { "concrete", 0.15f },
    { "dirt",     0.6f  },
    { "flesh",    1.0f  },
    { "foliage",  4.0f  },
    { "glass",    2.0f  },  // glass contents are passed by the engine itself, this is for solid brushes with glass surface
    { "grass",    0.6f  },  // ground covered by grass, as dirt
    { "gravel",   0.3f  },
    { "ice",      0.5f  },
    { "metal",    0.1f  },  // steel would be ~0.02, only thin sheet metal (doors, fences, car body) is penetrable
    { "mud",      0.45f },  // wet clay
    { "paper",    4.0f  },
    { "plaster",  0.8f  },  // plastered walls of houses, plaster on stone or mud brick
    { "rock",     0.1f  },
    { "sand",     0.4f  },
    { "snow",     1.5f  },  // low density
    { "water",    0.5f  },  // bullet breaks up in about 1 m of water
    { "wood",     1.0f  },
    { "asphalt",  0.15f },  // as concrete
};

// Penetration state of the bullet being fired - reset when Bullet_Fire_Extended is called with recursion 0
static struct {
    int walls;          // number of penetrated walls
    float power;        // remaining penetration power, 1 = full, each wall uses thickness / max thickness
    vec3_t start;       // where the bullet was fired from, for damage range falloff behind a wall
} wallbang_bullet;

#define MAX_BULLET_RECURSIONS   12
#define WALLBANG_MAX_SOLIDS     8   // max touching brushes / overlapping models one wall can be made of
#define WALLBANG_DEBUG_RANGE    256 // max thickness measured for walls that are too thick (debug only)
#define WALLBANG_DEBUG_DURATION 10000 // how long the debug lines are visible, in ms
#define WALLBANG_DEBUG_LINES    16   // max debug prints of one bullet (walls, hit players and the reason it stopped)
#define WALLBANG_DEBUG_LABELS   24   // max damage labels of one bullet (start, entry and exit of each wall, hit players, end)
#define WALLBANG_DEBUG_VISIBLE  128  // max damage labels visible at once (engine limit is 256 debug texts)

struct wallbangLabel_t {
    vec3_t origin;
    const float* color;
    char text[16];
    int endTime;    // svs_time when the label disappears
};

// Debug prints of the bullet being fired - printed together when the bullet is done, numbered when it went through more walls
static struct {
    int clientNum;
    bool wallHit;   // printed only when the bullet hit a wall, not for every hit of a player
    int count;
    char lines[WALLBANG_DEBUG_LINES][160];
    int labelCount;
    wallbangLabel_t labels[WALLBANG_DEBUG_LABELS]; // damage at the points of the bullet path, shown with the debug lines
} wallbang_debug;

// Damage labels being shown - the engine removes debug texts every server frame, they are added again until endTime
static wallbangLabel_t wallbang_labels[WALLBANG_DEBUG_VISIBLE];
static int wallbang_labelsNext; // oldest label, overwritten by the next one

// Debug drawing colors
static const vec4_t wallbang_colPath    = { 1, 1, 1, 1 };       // bullet path before a wall
static const vec4_t wallbang_colEntry   = { 1, 0, 0, 1 };       // entry point
static const vec4_t wallbang_colReach   = { 1, 1, 0, 1 };       // trace (1) - how far the bullet can still go in the wall
static const vec4_t wallbang_colInside  = { 1, 0.5f, 0, 1 };    // path inside the wall, entry -> exit
static const vec4_t wallbang_colExit    = { 0, 1, 0, 1 };       // exit point
static const vec4_t wallbang_colStop    = { 1, 0, 1, 1 };       // where the penetration failed
static const vec4_t wallbang_colBehind  = { 0, 0.6f, 1, 1 };    // bullet path behind a wall



struct WeaponDef
{
    char padding1[0x1dc];
    int shotCount;          // offset 0x1dc
    char padding2[0xc];
    int damage;             // offset 0x1ec
    char padding3[0x12c];
    int rifleBullet;        // offset 0x31c
    int armorPiercing;      // offset 0x320
    char padding4[0x26c];
    int minDamage;          // offset 0x590
    char padding5[0x4];
    float maxDamageRange;   // offset 0x598
    float minDamageRange;   // offset 0x59c
};
static_assert(offsetof(WeaponDef, shotCount) == WL(0x1dc, 0x1dc));
static_assert(offsetof(WeaponDef, damage) == WL(0x1ec, 0x1ec));
static_assert(offsetof(WeaponDef, rifleBullet) == WL(0x31c, 0x31c));
static_assert(offsetof(WeaponDef, armorPiercing) == WL(0x320, 0x320));
static_assert(offsetof(WeaponDef, minDamage) == WL(0x590, 0x590));
static_assert(offsetof(WeaponDef, maxDamageRange) == WL(0x598, 0x598));
static_assert(offsetof(WeaponDef, minDamageRange) == WL(0x59c, 0x59c));

// Which hit part of a player wins when the locational trace hits more parts
#define riflePriorityMap    ((unsigned char*)ADDR(0x005dca38, 0x08187c1f))
#define bulletPriorityMap   ((unsigned char*)ADDR(0x005dca24, 0x08187c0c))

struct weaponParms
{
    float forward[3];
    float right[3];
    float up[3];
    float muzzleTrace[3];
    float gunForward[3];
    WeaponDef* weapDef;
};



// ===========================================================================================
// Multi-Circle Pellet Distribution
// ===========================================================================================

/**
 * Unified hardcoded 8-pellet pattern function with selectable variant and rotation.
 * variant == 0: original pattern (5 outer + 3 inner)
 * variant == 1: alternate pattern (4-cardinal outer + 4-diagonal inner)
 */
void CalculatePelletPosition_8Pellets(int pelletIdx, float rotationAngle, int variant, float* outX, float* outY)
{
    static const float pattern0[8][2] = {
        {  0.000f,  -0.800f },
        {  0.000f,   0.000f },
        {  0.000f,   0.400f },
        {  0.000f,  -0.400f },
        { -0.400f,   0.000f },
        {  0.400f,   0.000f },
        { -0.600f,   0.600f },
        {  0.600f,   0.600f }
    };

    static const float pattern1[8][2] = {
        { -0.100f,   0.000f },
        {  0.100f,   0.000f },
        {  0.000f,   0.500f },
        {  0.000f,  -0.500f },
        { -0.800f,   0.600f },
        {  0.800f,   0.600f },
        { -0.800f,  -0.600f },
        {  0.800f,  -0.600f }
    };

    const float (*pattern)[2] = (variant == 0) ? pattern0 : pattern1;

    float baseX = pattern[pelletIdx][0];
    float baseY = pattern[pelletIdx][1];
    
    // Apply rotation
    float cosTheta = cosf(rotationAngle);
    float sinTheta = sinf(rotationAngle);

    *outX = baseX * cosTheta - baseY * sinTheta;
    *outY = baseX * sinTheta + baseY * cosTheta;
}


// ===========================================================================================
// Original game functions
// ===========================================================================================


float randomf()
{
	return (float)(int)rand() / (float)RAND_MAX;
}
inline void FastSinCos(const float value, float *pSin, float *pCos)
{
	*pSin = sin(value);
	*pCos = cos(value);
}
void gunrandom( float *x, float *y )
{
	float sinT, cosT, r, theta;

	theta = randomf() * 360;
	r = randomf();

	FastSinCos(theta * RADINDEG, &sinT, &cosT);

	*x = r * cosT;
	*y = r * sinT;
}


/*
===============
Bullet_Endpos
===============
*/
void Bullet_Endpos( float spread, vec3_t end, const weaponParms *wp, float maxRange, int shotIndex )
{
	float right;
	float up;

	float aimOffset = tan( spread * RADINDEG ) * maxRange;

	gunrandom(&right, &up);

	right *= aimOffset;
	up    *= aimOffset;

	VectorMA(wp->muzzleTrace, maxRange, wp->forward, end);

	VectorMA(end, right, wp->right, end);
	VectorMA(end, up, wp->up, end);
}


/*
===============
Bullet_GetDamage
===============
*/
// 00527490    int32_t Bullet_GetDamage(struct weaponParms const* wp @ edx, float dist)
int Bullet_GetDamage( const weaponParms *wp, float dist )
{
	float lerpAmount;
	float range;

	if ( wp->weapDef->maxDamageRange > dist )
	{
		return wp->weapDef->damage;
	}

	if ( wp->weapDef->minDamageRange <= dist )
	{
		return wp->weapDef->minDamage;
	}

	range = wp->weapDef->minDamageRange - wp->weapDef->maxDamageRange;

	if ( range == 0 )
	{
		return wp->weapDef->damage;
	}

	lerpAmount = (dist - wp->weapDef->maxDamageRange) / range;

	// lerp() is inlined in the Windows build
	return (int)( (1.0f - lerpAmount) * wp->weapDef->damage + wp->weapDef->minDamage * lerpAmount );
}


void Bullet_Fire_Extended(gentity_t* source, gentity_t* attacker, float* start, const float* end, float spread,
    int recursion, const weaponParms* wp, const gentity_t* weaponEnt, int gameTime);


/**
 * Saves the result of the wall penetration for the shooter (sv_wallbang_debug 1), printed by Wallbang_DebugFlush.
 */
static void Wallbang_DebugPrint(gentity_t* attacker, const char* format, ...)
{
    if (!sv_wallbang_debug->value.boolean || !attacker->client || wallbang_debug.count >= WALLBANG_DEBUG_LINES)
        return;

    va_list args;
    va_start(args, format);
    vsnprintf(wallbang_debug.lines[wallbang_debug.count], sizeof(wallbang_debug.lines[0]), format, args);
    va_end(args);

    wallbang_debug.clientNum = attacker->s.number;
    wallbang_debug.count++;
}

/**
 * Prints the saved results of the bullet to the shooter. Results of a bullet that went through more walls are numbered
 * and the following lines are indented under the first one:
 *   WB: 1. | plaster | 8.3 | ...
 *          2. | wood | 5.2 | ...
 */
static void Wallbang_DebugFlush()
{
    if (!wallbang_debug.wallHit)
        return;

    for (int i = 0; i < wallbang_debug.count; i++)
    {
        const char* text;
        if (wallbang_debug.count == 1)
            text = va("^3WB:^7 %s", wallbang_debug.lines[i]);
        else if (i == 0)
            text = va("^3WB:^7 1. | %s", wallbang_debug.lines[i]);
        else
            text = va("       %i. | %s", i + 1, wallbang_debug.lines[i]);

        SV_GameSendServerCommand(wallbang_debug.clientNum, SV_CMD_CAN_IGNORE, va("%c \"%s\"", 101, text));
    }
    wallbang_debug.count = 0;

    // Show the damage labels of the bullet
    for (int i = 0; i < wallbang_debug.labelCount; i++)
    {
        wallbang_labels[wallbang_labelsNext] = wallbang_debug.labels[i];
        wallbang_labels[wallbang_labelsNext].endTime = svs_time + WALLBANG_DEBUG_DURATION;
        wallbang_labelsNext = (wallbang_labelsNext + 1) % WALLBANG_DEBUG_VISIBLE;
    }
    wallbang_debug.labelCount = 0;
}


/**
 * Saves the damage label of a point of the bullet path (sv_wallbang_debug 1), shown by Wallbang_DebugFlush when the bullet hit a wall.
 * Only on the listen server, the labels are drawn by the local client.
 */
static void Wallbang_DebugLabel(const float* origin, const float* color, int damage)
{
#if COD2X_WIN32
    if (!sv_wallbang_debug->value.boolean || wallbang_debug.labelCount >= WALLBANG_DEBUG_LABELS)
        return;

    wallbangLabel_t* label = &wallbang_debug.labels[wallbang_debug.labelCount++];
    VectorCopy(origin, label->origin);
    label->origin[2] += 4; // above the debug point
    label->color = color;
    snprintf(label->text, sizeof(label->text), "%i", damage);
#endif
}

/**
 * Damage of the bullet at the point of its path, for the debug labels - same as in Bullet_Fire_Extended
 * (range falloff from where the bullet was fired, lowered by the penetration power behind a wall), before the hit location multiplier.
 */
static int Wallbang_DebugDamage(const weaponParms* wp, const float* point, float spread, int walls, float power)
{
    vec3_t dist;
    VectorSubtract(wallbang_bullet.start, point, dist);

    if (walls > 0)
        return Bullet_GetDamage(wp, VectorLength(dist)) * spread * sv_wallbang_damage->value.decimal * power;
    return Bullet_GetDamage(wp, VectorLength(dist)) * spread;
}


/**
 * Debug drawing of the penetration as cg_debugBullets (sv_wallbang_debug 1).
 * Only on the listen server, the lines are drawn by the local client (dedicated server has no renderer, it does nothing).
 * Lines are visible through walls.
 */
static void Wallbang_DebugLine(const float* start, const float* end, const float* color)
{
#if COD2X_WIN32
    if (!sv_wallbang_debug->value.boolean)
        return;

    // Duration is counted in server frames
    CL_AddDebugLine(start, end, color, WALLBANG_DEBUG_DURATION * Dvar_GetInt("sv_fps") / 1000, 0, 1);
#endif
}

static void Wallbang_DebugPoint(const float* point, const float* color)
{
    const float size = 3;
    for (int axis = 0; axis < 3; axis++)
    {
        vec3_t start, end;
        VectorCopy(point, start);
        VectorCopy(point, end);
        start[axis] -= size;
        end[axis] += size;
        Wallbang_DebugLine(start, end, color);
    }
}


/**
 * Real thickness of a solid that could not be penetrated, for the debug print only (sv_wallbang_debug 1).
 * Same traces as Wallbang_Penetrate, but with WALLBANG_DEBUG_RANGE instead of the distance the bullet can still go.
 * Returns -1 if the exit is not found in the range.
 */
static float Wallbang_DebugMeasureThickness(const float* entry, float cosAngle, const float* dir, int passEntityNum, int mask)
{
    if (!sv_wallbang_debug->value.boolean)
        return -1;

    trace_t solidTrace, exitTrace;
    vec3_t from, inside, next, delta;
    float thickness = 0;

    VectorCopy(entry, from);

    for (int solid = 0; solid < WALLBANG_MAX_SOLIDS && thickness < WALLBANG_DEBUG_RANGE; solid++)
    {
        VectorMA(from, 0.25f / fmaxf(cosAngle, 0.125f), dir, inside);
        VectorMA(inside, WALLBANG_DEBUG_RANGE - thickness, dir, next);

        SV_Trace(&solidTrace, inside, vec3_origin, vec3_origin, next, passEntityNum, mask, 0, NULL, 1);
        if (solidTrace.allsolid)
            return -1;
        Vec3Lerp(inside, next, solidTrace.fraction, next);

        SV_Trace(&exitTrace, next, vec3_origin, vec3_origin, from, passEntityNum, mask, 0, NULL, 1);
        if (exitTrace.fraction < 1.0f && !exitTrace.startsolid) {
            vec3_t exitPos;
            Vec3Lerp(next, from, exitTrace.fraction, exitPos);
            VectorSubtract(exitPos, from, delta);
            return thickness + VectorLength(delta);
        }
        if (solidTrace.fraction >= 1.0f)
            return -1;

        // The wall continues by the next surface
        VectorSubtract(next, from, delta);
        thickness += VectorLength(delta);
        VectorCopy(next, from);
        cosAngle = -DotProduct(dir, solidTrace.normal);
    }

    return -1;
}


/**
 * Prints the real thickness of a wall the bullet could not penetrate and the max thickness it could still go through.
 * thickness = walls passed before entry, maxLength = how far the bullet could go from entry.
 */
static void Wallbang_DebugPrintTooThick(gentity_t* attacker, const char* surfName, float thickness, float maxLength,
    const float* entry, float cosAngle, const float* dir, int passEntityNum, int mask)
{
    if (!sv_wallbang_debug->value.boolean)
        return;

    char real[32];
    float measured = Wallbang_DebugMeasureThickness(entry, cosAngle, dir, passEntityNum, mask);
    if (measured < 0)
        snprintf(real, sizeof(real), ">%i or no exit", WALLBANG_DEBUG_RANGE);
    else
        snprintf(real, sizeof(real), "%.1f", thickness + measured);

    Wallbang_DebugPrint(attacker, "%s | %s / %.1f | ^1too thick", surfName, real, thickness + maxLength);
}


/**
 * Bullet penetration through walls (sv_wallbang).
 * Called when the bullet trace stopped on a surface of the world or of an entity that does not take damage.
 * Finds the exit point of the wall, uses the penetration power of the bullet by the wall thickness, spawns the impact
 * effect on the exit side and continues the bullet behind the wall with lower damage.
 *
 *            entry      exit
 *   start ---->|  wall  |<-------- next     (2) reverse trace finds the exit side
 *              |inside----------->|next     (1) trace from inside the wall to the next surface
 *
 * (1) A trace that starts inside a brush does not hit that brush (only startsolid is set), and triangles of static models
 *     and terrain / curved patches are hit only from their front side, so the trace started just behind the entry surface
 *     passes out of the wall and stops on the next surface, limited to the distance the bullet can still go.
 *     If it ends inside the same brush (allsolid), the wall is too thick.
 * (2) The reverse trace from that point back to the entry point hits the exit side of the wall.
 *     If it does not, the wall continues by another touching brush (another material) - repeat from the next surface.
 *     Open surfaces without a back side (terrain hit from above) have no exit, the bullet stops.
 */
static void Wallbang_Penetrate(gentity_t* source, gentity_t* attacker, const float* start, const float* end, const float* hitPos,
    const trace_t* tr, float spread, int recursion, const weaponParms* wp, const gentity_t* weaponEnt, int gameTime)
{
    if (!sv_wallbang->value.boolean || !wp->weapDef->rifleBullet)
        return;

    // Nothing hit, the sky or a dead body
    if (tr->fraction >= 1.0f || (tr->surfaceFlags & SURF_SKY) || g_entities[tr->entityNum].s.eType == ET_PLAYER_CORPSE) {
        // Last point of a bullet behind a wall
        if (wallbang_bullet.walls > 0)
            Wallbang_DebugLabel(hitPos, wallbang_colBehind, Wallbang_DebugDamage(wp, hitPos, spread, wallbang_bullet.walls, wallbang_bullet.power));
        return;
    }

    wallbang_debug.wallHit = true;
    Wallbang_DebugLabel(hitPos, wallbang_colEntry, Wallbang_DebugDamage(wp, hitPos, spread, wallbang_bullet.walls, wallbang_bullet.power));

    if (wallbang_bullet.walls >= sv_wallbang_maxWalls->value.integer) {
        Wallbang_DebugPrint(attacker, "^9max walls (%i) reached", wallbang_bullet.walls);
        Wallbang_DebugPoint(hitPos, wallbang_colStop);
        return;
    }

    vec3_t dir;
    VectorSubtract(end, start, dir);
    Vec3NormalizeTo(dir, dir);

    const int mask = MASK_SHOT & ~CONTENTS_BODY; // world and brush entities only
    const float baseDepth = sv_wallbang_depth->value.decimal;

    trace_t solidTrace;     // (1)
    trace_t exitTrace;      // (2)
    vec3_t entry;           // where the bullet entered the current solid
    vec3_t inside;
    vec3_t next;
    vec3_t exitPos;
    float cosAngle = -DotProduct(dir, tr->normal);
    int surfType = SURF_TYPEINDEX(tr->surfaceFlags);
    float power = wallbang_bullet.power;
    float thickness = 0;
    const trace_t* face = tr; // trace of the surface where the bullet entered the current solid
    char surfName[64]; // materials of the wall for debug, "plaster+brick"

    VectorCopy(hitPos, entry);
    Q_strncpyz(surfName, wallbang_surfaces[surfType].name, sizeof(surfName));

    // Path behind a wall is drawn by Bullet_Fire_Extended
    if (wallbang_bullet.walls == 0)
        Wallbang_DebugLine(start, hitPos, wallbang_colPath);
    Wallbang_DebugPoint(hitPos, wallbang_colEntry);

    // Grazing hit, the bullet would travel too long in the wall (same limit as glass pass-through)
    if (cosAngle < 0.125f) {
        Wallbang_DebugPrint(attacker, "%s | ^1grazing angle", surfName);
        Wallbang_DebugPoint(entry, wallbang_colStop);
        return;
    }

    for (int solid = 0; ; solid++)
    {
        if (solid == WALLBANG_MAX_SOLIDS) {
            Wallbang_DebugPrint(attacker, "%s | %.1f | ^1too many solids", surfName, thickness);
            return;
        }

        float depth = baseDepth * sv_wallbang_surface[surfType]->value.decimal; // max thickness of this material

        // Weapon clip is placed by the mapper to stop bullets
        if (depth <= 0 || (face->contents & CONTENTS_CLIPSHOT)) {
            Wallbang_DebugPrint(attacker, "%s | ^1not penetrable^7 | contents 0x%x flags 0x%x", surfName, face->contents, face->surfaceFlags);
            Wallbang_DebugPoint(entry, wallbang_colStop);
            return;
        }

        // Trace end is 0.125 units in front of the surface, step 0.125 units behind it (as glass pass-through)
        // Faces between touching brushes can be hit at any angle, the step is limited
        VectorMA(entry, 0.25f / fmaxf(cosAngle, 0.125f), dir, inside);

        // How far the bullet can still go in this material, but not past the end of the bullet
        vec3_t toEnd;
        VectorSubtract(end, inside, toEnd);
        float maxLength = fminf(power * depth, DotProduct(toEnd, dir));
        if (maxLength <= 0) {
            Wallbang_DebugPrint(attacker, "%s | ^1bullet range reached", surfName);
            Wallbang_DebugPoint(entry, wallbang_colStop);
            return;
        }
        VectorMA(inside, maxLength, dir, next);

        // (1)
        SV_Trace(&solidTrace, inside, vec3_origin, vec3_origin, next, source->s.number, mask, 0, NULL, 1);

        if (solidTrace.allsolid) {
            Wallbang_DebugPrintTooThick(attacker, surfName, thickness, maxLength, entry, cosAngle, dir, source->s.number, mask);
            Wallbang_DebugLine(inside, next, wallbang_colReach);
            Wallbang_DebugPoint(next, wallbang_colStop);
            return;
        }

        Vec3Lerp(inside, next, solidTrace.fraction, next);
        Wallbang_DebugLine(inside, next, wallbang_colReach);

        // (2)
        SV_Trace(&exitTrace, next, vec3_origin, vec3_origin, entry, source->s.number, mask, 0, NULL, 1);

        bool exitFound = exitTrace.fraction < 1.0f && !exitTrace.startsolid;
        if (exitFound) {
            Vec3Lerp(next, entry, exitTrace.fraction, exitPos);
        } else if (solidTrace.fraction >= 1.0f) {
            // Exit is farther than the bullet can go - static models, terrain and curved patches are only surfaces,
            // a trace inside them is not detected as allsolid
            Wallbang_DebugPrintTooThick(attacker, surfName, thickness, maxLength, entry, cosAngle, dir, source->s.number, mask);
            Wallbang_DebugPoint(next, wallbang_colStop);
            return;
        }
        // else no exit side - the next surface is inside the wall: a touching brush, an overlapping model (sandbags in a pile),
        // the wall continues by it. A gap after a surface without back side is counted as wall too, it is limited by maxLength.

        vec3_t delta;
        VectorSubtract(exitFound ? exitPos : next, entry, delta);
        float length = VectorLength(delta);

        Wallbang_DebugLine(entry, exitFound ? exitPos : next, wallbang_colInside);

        power -= length / depth;
        thickness += length;

        if (power <= 0) {
            Wallbang_DebugPrint(attacker, "%s | %.1f | ^1too thick", surfName, thickness);
            Wallbang_DebugPoint(exitFound ? exitPos : next, wallbang_colStop);
            return;
        }

        if (exitFound)
            break;

        // Face between touching brushes is hidden - mostly caulk without surface type, then the material does not change
        VectorCopy(next, entry);
        face = &solidTrace;
        int nextSurfType = SURF_TYPEINDEX(solidTrace.surfaceFlags);
        if (nextSurfType != SURF_TYPE_NONE && nextSurfType != surfType) {
            surfType = nextSurfType;
            Q_strncpyz(surfName + strlen(surfName), va("+%s", wallbang_surfaces[surfType].name), sizeof(surfName) - strlen(surfName));
        }
        cosAngle = -DotProduct(dir, solidTrace.normal);
    }

    Wallbang_DebugPrint(attacker, "%s | %.1f | power %.2f | damage x%.2f", surfName, thickness, power,
        sv_wallbang_damage->value.decimal * power);
    Wallbang_DebugPoint(exitPos, wallbang_colExit);
    Wallbang_DebugLabel(exitPos, wallbang_colExit, Wallbang_DebugDamage(wp, exitPos, spread, wallbang_bullet.walls + 1, power));

    // Bullet impact on the exit side of the wall (only rifle bullets get here)
    gentity_t* tent = G_TempEntity(exitPos, EV_SHOTGUN_HIT);
    tent->s.eventParm = DirToByte(exitTrace.normal);
    tent->s.eventParm2 = DirToByte(dir);
    tent->s.surfType = SURF_TYPEINDEX(exitTrace.surfaceFlags);
    tent->s.otherEntityNum = weaponEnt->s.number;

    wallbang_bullet.walls++;
    wallbang_bullet.power = power;

    Bullet_Fire_Extended(source, attacker, exitPos, end, spread, recursion + 1, wp, weaponEnt, gameTime);
}


/*
==============
Bullet_Fire_Extended
	A modified Bullet_Fire with more parameters.
	The original Bullet_Fire still passes through here and functions as it always has.

	uses for this include shooting through entities (windows, doors, other players, etc.) and reflecting bullets
==============
*/
// 005276f0    void Bullet_Fire_Extended(struct gentity_s* source, struct gentity_s* attacker, float* start, float const* end, float spread, int32_t recursion, struct weaponParms const* wp, struct gentity_s const* weaponEnt, int32_t gameTime)
void Bullet_Fire_Extended(gentity_t* source, gentity_t* attacker, float* start, const float* end, float spread,
    int recursion, const weaponParms* wp, const gentity_t* weaponEnt, int gameTime)
{
	int mod, event, damage;
	int dflags = 0;
	float dot;
	vec3_t hitPos, reflect, dist;
	gentity_t *traceEnt, *tent;
	trace_t tr;

	if ( recursion > MAX_BULLET_RECURSIONS )
	{
		Com_DPrintf("Bullet_Fire_Extended: Too many resursions, bullet aborted\n");
		return;
	}

	// CoD2x: New bullet, reset wall penetration
	if ( recursion == 0 )
	{
		wallbang_bullet.walls = 0;
		wallbang_bullet.power = 1.0f;
		VectorCopy(start, wallbang_bullet.start);
	}
	// CoD2x: End

	if ( wp->weapDef->rifleBullet )
	{
		mod = MOD_RIFLE_BULLET;
		dflags = DAMAGE_PASSTHRU;
	}
	else
	{
		mod = MOD_PISTOL_BULLET;
	}

	if ( wp->weapDef->armorPiercing )
	{
		dflags |= DAMAGE_NO_ARMOR;
	}

	// CoD2x: Mark damage of bullet that penetrated a wall, so GSC can tell it apart (iDFlags & 64)
	if ( wallbang_bullet.walls > 0 )
	{
		dflags |= DAMAGE_WALLBANG;
	}
	// CoD2x: End

	// G_LocationalTrace() is inlined in the Windows build
	if ( wp->weapDef->rifleBullet )
		SV_Trace(&tr, start, vec3_origin, vec3_origin, end, source->s.number, MASK_SHOT, 1, riflePriorityMap, 1);
	else
		SV_Trace(&tr, start, vec3_origin, vec3_origin, end, source->s.number, MASK_SHOT, 1, bulletPriorityMap, 1);

	Vec3Lerp(start, end, tr.fraction, hitPos);

	// CoD2x: Debug drawing of the bullet path behind a wall (sv_wallbang_debug)
	if ( wallbang_bullet.walls > 0 )
	{
		Wallbang_DebugLine(start, hitPos, wallbang_colBehind);
	}
	// CoD2x: End

	G_CheckHitTriggerDamage(attacker, start, hitPos, wp->weapDef->damage, mod);

	traceEnt = &g_entities[tr.entityNum];

	VectorSubtract(end, start, reflect);
	Vec3NormalizeTo(reflect, reflect);

	VectorMA(reflect, DotProduct(reflect, tr.normal) * -2.0f, tr.normal, reflect);

	// send bullet impact
	if ( !( tr.surfaceFlags & SURF_SKY ) && !traceEnt->client && tr.fraction < 1.0f )
	{
		if ( wp->weapDef->rifleBullet )
			event = EV_SHOTGUN_HIT;
		else
			event = EV_BULLET_HIT_LARGE;

		tent = G_TempEntity(hitPos, event);

		tent->s.eventParm  = DirToByte(tr.normal);
		tent->s.eventParm2 = DirToByte(reflect);

		if ( traceEnt->s.eType == ET_PLAYER_CORPSE )
			tent->s.surfType = SURF_TYPE_FLESH;
		else
			tent->s.surfType = SURF_TYPEINDEX( tr.surfaceFlags );

		tent->s.otherEntityNum = weaponEnt->s.number;
	}

	// glass, bullet continues 0.125 units behind the surface
	if ( tr.contents & CONTENTS_GLASS )
	{
		VectorSubtract(end, start, reflect);
		Vec3NormalizeTo(reflect, reflect);

		dot = DotProduct(tr.normal, reflect);

		if ( -dot >= 0.125f )
			VectorMA(hitPos, 0.25f / -dot, reflect, start);
		else
			VectorMA(hitPos, 0, reflect, start);

		Bullet_Fire_Extended(source, attacker, start, end, spread, recursion + 1, wp, weaponEnt, gameTime);
		return;
	}

	if ( !traceEnt->takedamage )
	{
		// CoD2x: Bullet penetration through walls
		Wallbang_Penetrate(source, attacker, start, end, hitPos, &tr, spread, recursion, wp, weaponEnt, gameTime);
		// CoD2x: End
		return;
	}

	if ( traceEnt == attacker )
	{
		return;
	}

	VectorSubtract( start, hitPos, dist );
	damage = Bullet_GetDamage( wp, VectorLength(dist) ) * spread;

	// CoD2x: Bullet that penetrated a wall - range falloff is measured from where it was fired (not from the wall exit)
	// and the damage falls linearly to zero as the penetration power is used
	if ( wallbang_bullet.walls > 0 )
	{
		VectorSubtract( wallbang_bullet.start, hitPos, dist );
		damage = Bullet_GetDamage( wp, VectorLength(dist) ) * spread * sv_wallbang_damage->value.decimal * wallbang_bullet.power;

		// G_Damage would raise it to 1 for entities
		if ( damage <= 0 )
		{
			return;
		}
	}
	// CoD2x: End

	// CoD2x: Debug print of the hit in the list of walls the bullet went through (sv_wallbang_debug)
	int health = traceEnt->health;
	// CoD2x: End

	G_Damage( traceEnt, attacker, attacker, wp->forward, hitPos, damage, dflags, mod, tr.partGroup, level_time - gameTime );

	// CoD2x: Debug label of the bullet damage at the hit and the print of the health the hit took
	// (after the hit location multiplier and the GSC damage callback)
	Wallbang_DebugLabel(hitPos, wallbang_colPath, damage);
	if ( traceEnt->client )
		Wallbang_DebugPrint(attacker, "^2hit^7 %s | %s | %i damage", svs_clients[traceEnt->s.number].name, g_HitLocNames[tr.partGroup % HITLOC_NUM], health - traceEnt->health);
	// CoD2x: End

	if ( !traceEnt->client )
	{
		return;
	}

	// rifle bullets pass through players
	if ( !( dflags & DAMAGE_PASSTHRU ) )
	{
		return;
	}

	if ( !Dvar_GetInt("scr_friendlyfire") && OnSameTeam(traceEnt, attacker) )
	{
		return;
	}

	// start new bullet at position this hit the player and continue to the end position (ignoring the player in next trace)
	Bullet_Fire_Extended( traceEnt, attacker, hitPos, end, spread * 0.5f, recursion + 1, wp, weaponEnt, gameTime );
}


/**
 * Fires a new bullet (recursion 0) and prints the wall penetration debug of all walls it went through.
 * Same arguments as Bullet_Fire_Extended, replaces its call in Bullet_Fire.
 */
static void Wallbang_BulletFire(gentity_t* source, gentity_t* attacker, float* start, const float* end, float spread,
    int recursion, const weaponParms* wp, const gentity_t* weaponEnt, int gameTime)
{
	wallbang_debug.count = 0;
	wallbang_debug.labelCount = 0;
	wallbang_debug.wallHit = false;

	// Damage at the start, the label is a bit in front of the shooter so it can be read
	vec3_t dir, labelPos;
	VectorSubtract(end, start, dir);
	Vec3NormalizeTo(dir, dir);
	VectorMA(start, 32, dir, labelPos);
	Wallbang_DebugLabel(labelPos, wallbang_colPath, Bullet_GetDamage(wp, 0) * spread);

	Bullet_Fire_Extended(source, attacker, start, end, spread, recursion, wp, weaponEnt, gameTime);

	Wallbang_DebugFlush();
}


#if COD2X_WIN32
/**
 * Adds the damage labels again until they expire - the engine removes debug texts every server frame (sv_wallbang_debug 1).
 */
static void Wallbang_DebugLabelsFrame()
{
	for (int i = 0; i < WALLBANG_DEBUG_VISIBLE; i++)
	{
		wallbangLabel_t* label = &wallbang_labels[i];
		int timeLeft = label->endTime - svs_time;
		if (timeLeft <= 0 || timeLeft > WALLBANG_DEBUG_DURATION) // expired, or svs_time was reset by a new map
			continue;
		CL_AddDebugString(label->origin, label->color, 0.25f, label->text, 1);
	}
}

// 004123f0    void CL_FlushDebugData(int32_t fromServer)
static void CL_FlushDebugData(int fromServer)
{
	// Call the original function
	ASM_CALL(RETURN_VOID, 0x004123f0, 1, PUSH(fromServer));

	// CoD2x: Wallbang damage labels stay visible longer than one server frame
	Wallbang_DebugLabelsFrame();
	// CoD2x: End
}
#endif


/*
===============
G_BulletFireSpread
===============
*/
void G_BulletFireSpread(gentity_t* weaponEnt, gentity_t* attacker, weaponParms* wp, int gameTime, float spread)
{
    vec3_t start;
    int shotCount;

	VectorCopy(wp->muzzleTrace, start);

    shotCount = wp->weapDef->shotCount;
    float minDamageRange = wp->weapDef->minDamageRange;
    
    // CoD2x start
    // Check if custom pellet spread is enabled and weapon is a shotgun (8 pellets)
    if (g_shotgun_spread_fix->value.boolean && shotCount == 8)
    {
        // Compute aim offset radius once (converts angular spread to world distance)
        float aimOffset = tanf(spread * RADINDEG) * minDamageRange;

        // Generate random rotation angle once per shot group (0 to 2π)
        float rotationAngle = (shotCount == 8) ? (randomf() * 2.0f * M_PI) : 0.0f;

        // Randomly choose between 2 patterns
        int patternVariant = (randomf() < 0.5f) ? 0 : 1;

        // Fire each pellet with calculated position from chosen pattern
        for (int i = 0; i < shotCount; i++)
        {
            vec3_t end;
            float pelletX, pelletY;

            CalculatePelletPosition_8Pellets(i, rotationAngle, patternVariant, &pelletX, &pelletY);

            // Transform 2D pattern coords to 3D world space using weapon orientation
            // Formula: end = muzzleTrace + forward*range + right*x*radius + up*y*radius
            VectorMA(wp->muzzleTrace, minDamageRange, wp->forward, end);
            VectorMA(end, pelletX * aimOffset, wp->right, end);
            VectorMA(end, pelletY * aimOffset, wp->up, end);

            // Fire bullet to calculated endpoint
            Wallbang_BulletFire(weaponEnt, attacker, start, end, 1.0f, 0, wp, weaponEnt, gameTime);
        }
    }
    // CoD2x end
    else
    {
        // Random spread using game's default gunrandom() function
        for (int i = 0; i < shotCount; i++)
        {
            vec3_t end;
            
            // Use original bullet spread calculation
            Bullet_Endpos(spread, end, wp, minDamageRange, i);
            
            // Fire bullet
            Wallbang_BulletFire(weaponEnt, attacker, start, end, 1.0f, 0, wp, weaponEnt, gameTime);
        }
    }
}


// 00527af0    weaponDef* G_BulletFireSpread(weaponParms* wp @ eax, gentity_t* weaponEnt, gentity_t* attacker, int gameTime, float spread)
#if COD2X_WIN32
void __cdecl G_BulletFireSpread_Win32(gentity_t* weaponEnt, gentity_t* attacker, int gameTime, float spread)
{
    weaponParms* wp;
    ASM(movr, wp, "eax");
    
    G_BulletFireSpread(weaponEnt, attacker, wp, gameTime, spread);
}
#endif





/** Called once when hot-reloading is activated. */
void weapons_unload()
{

}


/**
 * Initialize weapons module on game start.
 */
void weapons_init()
{
	g_shotgun_spread_fix = Dvar_RegisterBool("g_shotgun_spread_fix", true, (enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));

	// Bullet penetration through walls - only rifle bullets (MOD_RIFLE_BULLET weapons)
	sv_wallbang = Dvar_RegisterBool("sv_wallbang", false, DVAR_CHANGEABLE_RESET);
	sv_wallbang_depth = Dvar_RegisterFloat("sv_wallbang_depth", 24, 0, 256, DVAR_CHANGEABLE_RESET);   // max wall thickness in units for surfaces with multiplier 1
	sv_wallbang_damage = Dvar_RegisterFloat("sv_wallbang_damage", 0.6f, 0, 1, DVAR_CHANGEABLE_RESET); // damage multiplier behind a paper-thin wall, falls linearly to 0 at max thickness
	sv_wallbang_maxWalls = Dvar_RegisterInt("sv_wallbang_maxWalls", 2, 1, 8, DVAR_CHANGEABLE_RESET);   // max walls one bullet can penetrate
	sv_wallbang_debug = Dvar_RegisterBool("sv_wallbang_debug", false, DVAR_CHANGEABLE_RESET);          // print penetration results to the shooter

	// Max wall thickness multiplier for each surface type: sv_wallbang_surface_wood, sv_wallbang_surface_brick, ...
	static char surfaceDvarNames[SURF_TYPECOUNT][40];
	for (int i = 0; i < SURF_TYPECOUNT; i++) {
		snprintf(surfaceDvarNames[i], sizeof(surfaceDvarNames[i]), "sv_wallbang_surface_%s", wallbang_surfaces[i].name);
		sv_wallbang_surface[i] = Dvar_RegisterFloat(surfaceDvarNames[i], wallbang_surfaces[i].depthScale, 0, 10, DVAR_CHANGEABLE_RESET);
	}
}


/**
 * Apply memory patches to hook weapon spread functions.
 */
void weapons_patch()
{
    // Hook G_BulletFireSpread to replace shotgun spread calculation
    patch_call(ADDR(0x00527baf, 0x081204d0), (unsigned int)WL(G_BulletFireSpread_Win32, G_BulletFireSpread));

    // Hook Bullet_Fire_Extended to add bullet penetration through walls (sv_wallbang)
    // Its other callers are the original G_BulletFireSpread (replaced above) and itself (recursion)
    patch_call(ADDR(0x00527be9, 0x0812053d), (unsigned int)Wallbang_BulletFire); // Bullet_Fire

#if COD2X_WIN32
    // Hook CL_FlushDebugData to keep the wallbang damage labels visible (sv_wallbang_debug)
    patch_call(0x0045c87c, (unsigned int)CL_FlushDebugData); // SV_Frame
#endif
}
