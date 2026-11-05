#ifndef COD2_ENTITY_H
#define COD2_ENTITY_H

#include "shared.h"
#include "cod2_math.h"

#define GENTITYNUM_BITS     10
#define MAX_GENTITIES       ( 1 << GENTITYNUM_BITS ) // 1024

#define ENTITYNUM_NONE		( MAX_GENTITIES - 1 )
#define ENTITYNUM_WORLD		( MAX_GENTITIES - 2 )

#define ET_PLAYER			1
#define ET_PLAYER_CORPSE	2

#define g_entities 			(*((gentity_t (*)[MAX_GENTITIES])(ADDR(0x01744380, 0x08716400))))
#define level_time 			(*((int*)(ADDR(0x0193a96c, 0x0864c56c))))

// Entity events (es->eType = ET_EVENTS + event)
#define EV_BULLET_HIT_LARGE		0xb6	// bullet impact of pistol bullet
#define EV_SHOTGUN_HIT			0xb7	// bullet impact of rifle bullet

// Means of death
#define MOD_PISTOL_BULLET		1
#define MOD_RIFLE_BULLET		2

// G_Damage dflags, passed to GSC as iDFlags
#define DAMAGE_RADIUS				0x1		// damage was indirect
#define DAMAGE_NO_ARMOR				0x2		// armor does not protect from this damage
#define DAMAGE_NO_KNOCKBACK			0x4		// do not affect velocity, just view angles
#define DAMAGE_NO_TEAM_PROTECTION	0x8		// GSC only
#define DAMAGE_NO_PROTECTION		0x10	// GSC only
#define DAMAGE_PASSTHRU				0x20	// rifle bullet, can pass through players
#define DAMAGE_WALLBANG				0x40	// CoD2x: bullet penetrated a wall before the hit (sv_wallbang)

// Hit locations (trace partGroup, G_Damage hitLoc), names as passed to GSC as sHitLoc: "none", "helmet", "head", ..., "gun"
#define HITLOC_NUM			19
#define g_HitLocNames		(*((const char* (*)[HITLOC_NUM])(ADDR(0x005dc9d8, 0x08187bc0))))



// es->eFlags,  ps->eFlags
#define ENTITY_FLAG_UNK2 				0x2
#define ENTITY_FLAG_CROUCH 				0x4
#define ENTITY_FLAG_PRONE 				0x8
#define ENTITY_FLAG_FIRING 				0x40
#define ENTITY_FLAG_NO_COMMUNICATION 	0x80
#define ENTITY_FLAG_MG 					0x300
#define ENTITY_FLAG_MANTLE 				0x4000
#define ENTITY_FLAG_DEAD 				0x20000
#define ENTITY_FLAG_ADS 				0x40000 // weapon zoomed in
#define ENTITY_FLAG_ANIM_COMPLETED 		0x80000 // also writed as ((unsigned int)es->eFlags >> 19) & 1
#define ENTITY_FLAG_MENU 				0x200000 // menu or console opened
#define ENTITY_FLAG_PING 				0x400000 // compass player ping
#define ENTITY_FLAG_0x800000 			0x800000 // something with compass, friendly compass player

inline const char **get_entityFlagStrings(int mask)
{
    static const char *result[33];
    int count = 0;
    if (mask & 0x1)                      		result[count++] = "0x00000001";    		// bit 0 (0x1)
    if (mask & ENTITY_FLAG_UNK2)           		result[count++] = "UNK2";        		// bit 1 (0x2)
    if (mask & ENTITY_FLAG_CROUCH)         		result[count++] = "CROUCH";      		// bit 2 (0x4)
    if (mask & ENTITY_FLAG_PRONE)          		result[count++] = "PRONE";       		// bit 3 (0x8)
    if (mask & 0x10)                     		result[count++] = "0x00000010";    		// bit 4 (0x10)
    if (mask & 0x20)                     		result[count++] = "0x00000020";    		// bit 5 (0x20)
    if (mask & ENTITY_FLAG_FIRING)         		result[count++] = "FIRING";      		// bit 6 (0x40)
    if (mask & ENTITY_FLAG_NO_COMMUNICATION) 	result[count++] = "NO_COMMUNICATION"; 	// bit 7 (0x80)
    if (mask & ENTITY_FLAG_MG)             		result[count++] = "MG";          		// bits 8-9 (0x300)
    if (mask & 0x400)                    		result[count++] = "0x00000400";    		// bit 10 (0x400)
    if (mask & 0x800)                    		result[count++] = "0x00000800";    		// bit 11 (0x800)
    if (mask & 0x1000)                   		result[count++] = "0x00001000";    		// bit 12 (0x1000)
    if (mask & 0x2000)                   		result[count++] = "0x00002000";    		// bit 13 (0x2000)
    if (mask & ENTITY_FLAG_MANTLE)         		result[count++] = "MANTLE";      		// bit 14 (0x4000)
    if (mask & 0x8000)                   		result[count++] = "0x00008000";    		// bit 15 (0x8000)
    if (mask & 0x10000)                  		result[count++] = "0x00010000";    		// bit 16 (0x10000)
    if (mask & ENTITY_FLAG_DEAD)           		result[count++] = "DEAD";        		// bit 17 (0x20000)
    if (mask & ENTITY_FLAG_ADS)            		result[count++] = "ADS";         		// bit 18 (0x40000)
    if (mask & ENTITY_FLAG_ANIM_COMPLETED) 		result[count++] = "ANIM_COMPLETED"; 	// bit 19 (0x80000)
    if (mask & 0x100000)                 		result[count++] = "0x100000";    		// bit 20 (0x100000)
    if (mask & ENTITY_FLAG_MENU)           		result[count++] = "MENU";        		// bit 21 (0x200000)
    if (mask & ENTITY_FLAG_PING)           		result[count++] = "PING";        		// bit 22 (0x400000)
    if (mask & ENTITY_FLAG_0x800000)       		result[count++] = "0x0800000";    		// bit 23 (0x800000)
    if (mask & 0x1000000)                		result[count++] = "0x1000000";   		// bit 24 (0x1000000)
    if (mask & 0x2000000)                		result[count++] = "0x2000000";   		// bit 25 (0x2000000)
    if (mask & 0x4000000)                		result[count++] = "0x4000000";   		// bit 26 (0x4000000)
    if (mask & 0x8000000)                		result[count++] = "0x8000000";   		// bit 27 (0x8000000)
    if (mask & 0x10000000)               		result[count++] = "0x10000000";  		// bit 28 (0x10000000)
    if (mask & 0x20000000)               		result[count++] = "0x20000000";  		// bit 29 (0x20000000)
    if (mask & 0x40000000)               		result[count++] = "0x40000000";  		// bit 30 (0x40000000)
    if (mask & 0x80000000)               		result[count++] = "0x80000000";  		// bit 31 (0x80000000)
    result[count] = nullptr; // Null-terminate the array
    return result;
}


typedef enum
{
	TR_STATIONARY,
	TR_INTERPOLATE,
	TR_LINEAR,
	TR_LINEAR_STOP,
	TR_SINE,
	TR_GRAVITY,
	TR_GRAVITY_PAUSED,
	TR_ACCELERATE,
	TR_DECCELERATE
} trType_t;

typedef struct
{
	trType_t trType;
	int trTime;
	int trDuration;
	vec3_t trBase;
	vec3_t trDelta;
} trajectory_t;

typedef struct entityState_s
{
	int number;
	int eType;
	int eFlags;
	trajectory_t pos;  // for calculating position
	trajectory_t apos; // for calculating angles
	int time;
	int time2;
	vec3_t origin2;
	vec3_t angles2; // contans additional data, like movement direction
	int otherEntityNum;
	int attackerEntityNum;
	int groundEntityNum;
	int constantLight;
	int loopSound;
	int surfType;
	int index;
	int clientNum;
	int iHeadIcon;
	int iHeadIconTeam;
	int solid;
	int eventParm;
	int eventSequence;
	int events[4];
	int eventParms[4];
	int weapon;
	int legsAnim;
	int torsoAnim;
	float leanf;
	union
	{
		int scale;
		int eventParm2; // bullet impact: reflected direction (DirToByte)
	};
	int dmgFlags;
	int animMovetype;
	float fTorsoHeight;
	float fTorsoPitch;
	float fWaistPitch;
} entityState_t;

typedef struct
{
	byte linked;
	byte bmodel;
	byte svFlags;
	int clientMask[2];
	byte inuse;
	int broadcastTime;
	vec3_t mins;
	vec3_t maxs;
	int contents;
	vec3_t absmin;
	vec3_t absmax;
	vec3_t currentOrigin;
	vec3_t currentAngles;
	int ownerNum;
	int eventTime;
} entityShared_t;

typedef struct gentity_s
{
	entityState_t s;
	entityShared_t r;
	void *client;
	void *pTurretInfo;
	byte physicsObject;
	byte takedamage;
	byte active;
	byte nopickup;
	byte model;
	byte attachIgnoreCollision;
	byte handler;
	byte team;
	uint16_t classname;
	uint16_t target;
	uint16_t targetname;
	int spawnflags;
	int flags;
	int eventTime;
	int freeAfterEvent;
	int unlinkAfterEvent;
	int clipmask;
	int framenum;
	gentity_s *parent;
	int nextthink;
	int health;
	int maxHealth;
	int dmg;
	int count;
	gentity_s *chain;
	int _padding[24];
	void *tagInfo;
	gentity_s *tagChildren;
	byte attachModelNames[8];
	uint16_t attachTagNames[8];
	int useCount;
	gentity_s *nextFree;
} gentity_t;
static_assert((sizeof(gentity_t) == 560));



// Spawns a temporary entity at origin (snapped to integers) that only carries the event to clients and is freed after it.
// Returns the entity so the caller can fill the event parameters (eventParm, surfType, ...).
// 0050ff70    struct gentity_s* G_TempEntity(float const* origin, int32_t event)
inline gentity_t* G_TempEntity(const float* origin, int event) {
    gentity_t* ret;
    ASM_CALL(RETURN(ret), ADDR(0x0050ff70, 0x0811efc4), 2, PUSH(origin), PUSH(event));
    return ret;
}

// Damages the entity targ. Players go through G_DamageClient - the hit location multiplier of the weapon is applied
// and the GSC callback CodeCallback_PlayerDamage is called with dflags as iDFlags.
// Other entities with takedamage lose health directly and get the "damage" / "death" notify.
// dir is the direction of the attack (bullets pass weaponParms, its first member is forward), hitLoc is the trace partGroup.
// 00522ca0    void G_Damage(struct gentity_s* targ @ ebx, struct gentity_s* inflictor, struct gentity_s* attacker, float const* dir @ eax, float const* point, int32_t damage, int32_t dflags @ edx, int32_t mod, int32_t hitLoc, int32_t timeOffset)
inline void G_Damage(gentity_t* targ, gentity_t* inflictor, gentity_t* attacker, const float* dir, const float* point, int damage, int dflags, int mod, int hitLoc, int timeOffset) {
    WL(
        ASM_CALL(RETURN_VOID, 0x00522ca0, 7, EBX(targ), PUSH(inflictor), PUSH(attacker), EAX(dir), PUSH(point), PUSH(damage), EDX(dflags), PUSH(mod), PUSH(hitLoc), PUSH(timeOffset)),
        ASM_CALL(RETURN_VOID, 0x08101c58, 10, PUSH(targ), PUSH(inflictor), PUSH(attacker), PUSH(dir), PUSH(point), PUSH(damage), PUSH(dflags), PUSH(mod), PUSH(hitLoc), PUSH(timeOffset))
    )
}

// Damages trigger_damage entities crossed by the line from start to end (bullets and melee)
// 00523ed0    void G_CheckHitTriggerDamage(struct gentity_s* pActivator, float const* start @ ecx, float const* end, int32_t damage, int32_t mod)
inline void G_CheckHitTriggerDamage(gentity_t* pActivator, const float* start, const float* end, int damage, int mod) {
    WL(
        ASM_CALL(RETURN_VOID, 0x00523ed0, 4, PUSH(pActivator), ECX(start), PUSH(end), PUSH(damage), PUSH(mod)),
        ASM_CALL(RETURN_VOID, 0x0811cea8, 5, PUSH(pActivator), PUSH(start), PUSH(end), PUSH(damage), PUSH(mod))
    )
}

// Returns 1 if both entities are players in the same team (team none is never the same team)
// 00520b10    int32_t OnSameTeam(struct gentity_s* ent1 @ eax, struct gentity_s* ent2 @ ecx)
inline int OnSameTeam(gentity_t* ent1, gentity_t* ent2) {
    int ret;
    WL(
        ASM_CALL(RETURN(ret), 0x00520b10, 0, EAX(ent1), ECX(ent2)),
        ASM_CALL(RETURN(ret), 0x0811c0d0, 2, PUSH(ent1), PUSH(ent2))
    )
    return ret;
}


#endif