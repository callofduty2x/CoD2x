#include "weapons.h"

#include "shared.h"
#include "cod2_common.h"
#include "cod2_cmd.h"
#include "cod2_math.h"
#include "cod2_entity.h"
#include "cod2_dvars.h"
#include <math.h>


// Toggle for custom pellet distribution (true) vs original random spread (false)
dvar_t* g_shotgun_spread_fix;



struct WeaponDef
{
    char padding1[0x1dc];
    int shotCount;          // offset 0x1dc
    char padding2[0x3bc];
    float minDamageRange;   // offset 0x59c
};
static_assert(offsetof(WeaponDef, shotCount) == WL(0x1dc, 0x1dc));
static_assert(offsetof(WeaponDef, minDamageRange) == WL(0x59c, 0x59c));

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
        {0.0f, -0.9f},     
        {-0.826f, -0.321f},
        {-0.514f, 0.735f}, 
        {0.514f, 0.735f},  
        {0.826f, -0.321f}, 
        {0.0f, -0.275f},   
        {-0.275f, 0.165f}, 
        {0.275f, 0.165f}   
    };

    static const float pattern1[8][2] = {
        { 0.260f,  0.342f },
        {-0.300f,  0.342f },
        {-0.020f, -0.460f },
        {-0.020f, -0.020f },
        {-0.712f, -0.712f },
        { 0.674f,  0.674f },
        { 0.664f, -0.712f },
        {-0.712f,  0.664f } 
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


/**
 * Wrapper for game's Bullet_Fire_Extended function.
 */
void Bullet_Fire_Extended(gentity_t* source, gentity_t* attacker, vec3_t start, vec3_t end, float spread, 
    int recursion, weaponParms* wp, gentity_t* weaponEnt, int gameTime)
{
    ASM_CALL(RETURN_VOID, ADDR(0x005276f0, 0x0811fe90), 9,
        PUSH(source),
        PUSH(attacker),
        PUSH(start),
        PUSH(end),
        PUSH(spread),
        PUSH(recursion),
        PUSH(wp),
        PUSH(weaponEnt),
        PUSH(gameTime)
    );
}


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
            Bullet_Fire_Extended(weaponEnt, attacker, start, end, 1.0f, 0, wp, weaponEnt, gameTime);
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
            Bullet_Fire_Extended(weaponEnt, attacker, start, end, 1.0f, 0, wp, weaponEnt, gameTime);
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
}


/**
 * Apply memory patches to hook weapon spread functions.
 */
void weapons_patch()
{
    // Hook G_BulletFireSpread to replace shotgun spread calculation
    patch_call(ADDR(0x00527baf, 0x081204d0), (unsigned int)WL(G_BulletFireSpread_Win32, G_BulletFireSpread));
}
