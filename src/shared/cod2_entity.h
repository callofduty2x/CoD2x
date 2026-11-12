#ifndef COD2_ENTITY_H
#define COD2_ENTITY_H

#include "shared.h"
#include "cod2_math.h"

#include <stddef.h>

#define GENTITYNUM_BITS     10
#define MAX_GENTITIES       ( 1 << GENTITYNUM_BITS ) // 1024

#define ENTITYNUM_NONE		( MAX_GENTITIES - 1 )
#define ENTITYNUM_WORLD		( MAX_GENTITIES - 2 )

// Entity types (es->eType). 0..6 named by Svcmd_EntityList, 7..9 by CG_ProcessEntity (CG_SoundBlend, loop fx, turret; names from CoD4).
// Event-only (temp) entities have eType = ET_EVENTS + event (G_TempEntity), eType is sent with 8 bits.
typedef enum
{
	ET_GENERAL = 0,			// model (index = xmodel) or brush model (solid = SOLID_BMODEL, index = brush model), triggers, exploded missiles
	ET_PLAYER = 1,			// client 0..63, built from the playerState (BG_PlayerStateToEntityState)
	ET_PLAYER_CORPSE = 2,	// body of a dead player, entities 64..71 (G_SpawnPlayerClone)
	ET_ITEM = 3,			// weapon / ammo on the ground, index = item
	ET_MISSILE = 4,			// grenade, rocket (fire_grenade, fire_rocket)
	ET_INVISIBLE = 5,		// not drawn by the client: spectating / intermission clients (SpectatorClientEndFrame, IntermissionClientEndFrame), Win 0x4ffcd0
	ET_SCRIPTMOVER = 6,		// script_model, script_brushmodel, script_origin (InitScriptMover)
	ET_SOUND_BLEND = 7,		// two sound aliases blended by leanf (CG_SoundBlend; Mac CG_ProcessEntity case 7, CoD4 also 7; CoD2rev's ET_FX is wrong)
	ET_LOOP_FX = 8,			// playLoopedFx (Scr_PlayLoopedFX), CoD4 name; CoD2 has no one-shot fx entity (playFx is the event EV_PLAY_FX)
	ET_TURRET = 9,			// misc_turret / mg42 (G_SpawnTurret), CoD4: ET_MG42
	ET_EVENTS = 10			// ET_EVENTS + event: temp entity only carrying the event
} entityType_t;

// Entity events (events[], or eType = ET_EVENTS + event). Only the ones CoD2x uses or that were verified by their senders are listed
// (see docs/network-protocol.md for the known numbers). Names from CoD4 / Q3 where the meaning matches.
#define EV_PREP_OFFHAND			0xa5	// grenade pulled, eventParm = offhand weapon (CG_EntityEvent plays its sound only for the followed player)
#define EV_USE_OFFHAND			0xa6	// grenade thrown, eventParm = offhand weapon
#define EV_BULLET_HIT_LARGE		0xb6	// bullet impact of pistol bullet; also sent by G_RunMissile as the missile water splash
										// (surfType 20 water, eventParm2 = DirToByte(reflect), otherEntityNum = missile)
#define EV_SHOTGUN_HIT			0xb7	// bullet impact of rifle bullet
#define EV_OBITUARY				0xc6	// GScr_Obituary: otherEntityNum = victim, attackerEntityNum = attacker, eventParm = weapon index, only for
										// MOD 7, 8, 9, 11, 12 (melee, head shot, crush, falling, suicide) MOD | 0x80. CG_Obituary (Win 0x4e03f0)
										// decodes (signed char) eventParm >= 0 as weapon index, else & 0x7f as means of death

#define MAX_EVENTS				4		// es->events[] / eventParms[], indexed by eventSequence & (MAX_EVENTS - 1)

// es->solid: bounding box of a non-brush entity packed by SV_LinkEntity (CONTENTS_SOLID | CONTENTS_BODY entities, else 0)
//   bits  0- 7  maxs[0] (x / y radius), 1..255
//   bits  8-15  1 - mins[2], 1..255
//   bits 16-23  maxs[2] + 32, 1..255
#define SOLID_BMODEL			0xffffff	// brush model, es->index is the brush model number

// es->legsAnim, es->torsoAnim (10 bits): animation index, the toggle bit flips when the same animation is started again
#define ANIM_TOGGLEBIT			0x200

// es->iHeadIconTeam: 0 = shown to everybody, otherwise only to the team (team_t: 1 = axis, 2 = allies, 3 = spectator) (ClientScr_SetHeadIconTeam)
// es->iHeadIcon: 0 = none, else configstring CS_HEAD_ICONS + iHeadIcon - 1 (precacheHeadIcon)

// es->dmgFlags of usable entities: cursor hint (GScr_SetCursorHint, trigger_use_shared, read on the server by Player_UpdateCursorHints)
#define HINT_INHERIT			-1		// trigger_use: the hint of the targeted entity
#define HINT_NONE				1
#define HINT_ACTIVATE			2
#define HINT_HEALTH				3
#define HINT_FRIENDLY			4		// higher values: weapon hint, 4 + weapon index (turrets, items)

#define g_entities 			(*((gentity_t (*)[MAX_GENTITIES])(ADDR(0x01744380, 0x08716400))))
#define level_time 			(*((int*)(ADDR(0x0193a96c, 0x0864c56c))))

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



// es->eFlags, ps->eFlags (24 bits on the network). Names from Q3 / CoD4 where the bit has the same meaning, the others are marked.
#define EF_NONSOLID_BMODEL		0x1			// trigger brush models (SP_trigger_*), script notsolid() on brush models (cleared by solid()), CoD4 name
#define EF_TELEPORT_BIT			0x2			// toggled on teleport / spawn / setOrigin / new corpse: no lerp from the old position
#define EF_CROUCHING			0x4			// PM_CheckDuck
#define EF_PRONE				0x8			// PM_CheckDuck
//								0x10		// unknown, no writer found
#define EF_NODRAW				0x20		// not drawn (CG_General, CG_Item, CG_Missile, CG_ScriptMover skip it). Rewritten every frame for every
											// non-client entity by G_RunEntity / G_RunFrameForEntity (Win 0x4fd090) from ent->flags & FL_NODRAW (0x800:
											// script hide() / show(), script_origin; G_ExplodeMissile sets FL_NODRAW)
#define EF_FIRING				0x40		// attack button with ammo (Pmove), turret firing (turret_think_client)
#define EF_CONNECTION			0x80		// no command from the client for 1 s, connection problem icon (ClientEndFrame, CG_PlayerSprites), Q3 name
#define EF_TURRET_ACTIVE_PRONE	0x100		// using a turret, prone stance turret (turret_use), CoD4 name
#define EF_TURRET_ACTIVE_DUCK	0x200		// using a turret, crouch stance turret, CoD4 name
#define EF_TURRET_ACTIVE		0x300		// using a turret (both bits = standing turret)
#define EF_FIXED_LIGHTING_ORIGIN 0x400		// CoD2x name (CoD2rev: EF_PROJECTILE), written only by fire_rocket (Win 0x522370). Read by CG_General and
											// CG_ScriptMover (helper Win 0x4cbf20): the first lerp origin is latched as a fixed lighting origin and the
											// refEntity gets renderFx 0x80 (lighting origin). Matters once the exploded rocket becomes ET_GENERAL;
											// CG_Missile does not read it
#define EF_AIMASSIST			0x800		// brush model entity with aim assist (GScr_EnableAimAssist / GScr_DisableAimAssist), CoD2rev name
#define EF_MANTLE				0x4000		// mantling (Mantle_CheckLedge / Mantle_Move)
#define EF_EXPLODED				0x10000		// CoD2x name (CoD2rev: EF_UNKNOWN). Exploded missile kept alive for its delayed effect (G_ExplodeMissile,
											// Win 0x521825; time .. time2 = the effect). G_RunEntity frees it when eFlags == 0x10000 exactly and
											// level.time > time2 (inference: probably never matches, EF_NODRAW is set too). CG_EntityEvent 191 plays the
											// explosion sound only if this is not set or cg.time - time < 50
#define EF_DEAD					0x20000		// dead player / corpse
#define EF_AIMDOWNSIGHT			0x40000		// weapon zoomed in (ADS), CoD4 name
#define EF_BODY_START			0x80000		// new corpse: set with EF_DEAD by PlayerCmd_ClonePlayer, cleared by BodyEnd. The client passes it to
											// BG_SetNewAnimation as "new", so the death animation starts without blending. CoD2rev name
#define EF_VOTED				0x100000	// player voted (Cmd_Vote_f, Cmd_CallVote_f), Q3 name
#define EF_TALK					0x200000	// menu, console or chat open (talk button in Pmove), shows the talk icon, Q3 name
#define EF_PING					0x400000	// compass ping (PlayerCmd_pingPlayer, cleared in ClientEndFrame), CoD2x name (CoD2rev: EF_TAUNT)
#define EF_FRIENDLY_PING		0x800000	// ps only: the out-of-PVS teammate on the compass is pinged (ClientEndFrame), CoD2x name (CoD2rev: EF_PING)
#define EF_BOUNCE				0x1000000	// grenade bounces (fire_grenade), read only on the server (G_RunMissile, G_BounceMissile, G_MissileImpact),
											// above the 24 sent bits, never sent. CoD2rev name

inline const char **get_entityFlagStrings(int mask)
{
    static const char *result[33];
    int count = 0;
    if (mask & EF_NONSOLID_BMODEL)         		result[count++] = "0x00000001";    		// bit 0 (0x1)
    if (mask & EF_TELEPORT_BIT)           		result[count++] = "UNK2";        		// bit 1 (0x2)
    if (mask & EF_CROUCHING)         			result[count++] = "CROUCH";      		// bit 2 (0x4)
    if (mask & EF_PRONE)          				result[count++] = "PRONE";       		// bit 3 (0x8)
    if (mask & 0x10)                     		result[count++] = "0x00000010";    		// bit 4 (0x10)
    if (mask & EF_NODRAW)                     	result[count++] = "0x00000020";    		// bit 5 (0x20)
    if (mask & EF_FIRING)         				result[count++] = "FIRING";      		// bit 6 (0x40)
    if (mask & EF_CONNECTION) 					result[count++] = "NO_COMMUNICATION"; 	// bit 7 (0x80)
    if (mask & EF_TURRET_ACTIVE)             	result[count++] = "MG";          		// bits 8-9 (0x300)
    if (mask & EF_FIXED_LIGHTING_ORIGIN)		result[count++] = "0x00000400";    		// bit 10 (0x400)
    if (mask & EF_AIMASSIST)             		result[count++] = "AIMASSIST";    		// bit 11 (0x800)
    if (mask & 0x1000)                   		result[count++] = "0x00001000";    		// bit 12 (0x1000)
    if (mask & 0x2000)                   		result[count++] = "0x00002000";    		// bit 13 (0x2000)
    if (mask & EF_MANTLE)         				result[count++] = "MANTLE";      		// bit 14 (0x4000)
    if (mask & 0x8000)                   		result[count++] = "0x00008000";    		// bit 15 (0x8000)
    if (mask & EF_EXPLODED)              		result[count++] = "0x00010000";    		// bit 16 (0x10000)
    if (mask & EF_DEAD)           				result[count++] = "DEAD";        		// bit 17 (0x20000)
    if (mask & EF_AIMDOWNSIGHT)            		result[count++] = "ADS";         		// bit 18 (0x40000)
    if (mask & EF_BODY_START) 					result[count++] = "BODY_START"; 		// bit 19 (0x80000)
    if (mask & EF_VOTED)                 		result[count++] = "0x100000";    		// bit 20 (0x100000)
    if (mask & EF_TALK)           				result[count++] = "MENU";        		// bit 21 (0x200000)
    if (mask & EF_PING)           				result[count++] = "PING";        		// bit 22 (0x400000)
    if (mask & EF_FRIENDLY_PING)       			result[count++] = "0x0800000";    		// bit 23 (0x800000)
    if (mask & EF_BOUNCE)                		result[count++] = "0x1000000";   		// bit 24 (0x1000000)
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


// trajectory_t::trType (8 bits), verified by the switch in BG_EvaluateTrajectory
typedef enum
{
	TR_STATIONARY,			// trBase
	TR_INTERPOLATE,			// trBase, interpolated between snapshots (players)
	TR_LINEAR,				// trBase + trDelta * t (rockets, grenade spin in apos)
	TR_LINEAR_STOP,			// TR_LINEAR until trTime + trDuration
	TR_SINE,				// trBase + trDelta * sin(t / trDuration * 2pi)
	TR_GRAVITY,				// TR_LINEAR with gravity 800 (grenades)
	TR_GRAVITY_PAUSED,		// trBase
	TR_ACCELERATE,			// accelerates over trDuration in the trDelta direction (script movers)
	TR_DECCELERATE			// deccelerates over trDuration
} trType_t;

typedef struct
{
	trType_t trType;		// 8 bits
	int trTime;				// 32 bits
	int trDuration;			// 32 bits
	vec3_t trBase;			// floats, apos.trBase as angles (16 bits)
	vec3_t trDelta;			// floats
} trajectory_t;

// Network state of an entity, delta-coded with entityStateFields (Win 0x0059bda0, Linux 0x081417a0, 59 fields). The bits are the netfield
// bits (0 = float, -100 = angle). Fields have a different meaning per eType, the aliases name it. See docs/network-protocol.md section 4.5.
typedef struct entityState_s
{
	int number;				// 0x00  entity number (10 bits, written before the fields)
	int eType;				// 0x04  8 bits, entityType_t, ET_EVENTS + event
	int eFlags;				// 0x08  24 bits, EF_*
	trajectory_t pos;		// 0x0c  position
	trajectory_t apos;		// 0x30  angles
	int time;				// 0x54  32 bits, ET_MISSILE: level.time + 50 at launch (fire_rocket, fire_grenade); exploded missile: effect start;
							//       EV_EARTHQUAKE: duration (ms)
	int time2;				// 0x58  32 bits, exploded missile: effect end time
	vec3_t origin2;			// 0x5c  floats, ET_LOOP_FX: forward direction
	vec3_t angles2;			// 0x68  floats, ET_PLAYER / ET_PLAYER_CORPSE: [1] movement direction; ET_TURRET: gun pitch, yaw, barrel roll;
							//       ET_LOOP_FX: [0] cull distance, [1] repeat period (ms); EV_EARTHQUAKE: [0] scale, [1] radius
	int otherEntityNum;		// 0x74  10 bits, ET_PLAYER: turret in use; ET_TURRET: player using it; bullet impact events: shooter; EV_OBITUARY: victim
	int attackerEntityNum;	// 0x78  10 bits, EV_OBITUARY: attacker (ENTITYNUM_WORLD if none)
	int groundEntityNum;	// 0x7c  10 bits, entity standing on, ENTITYNUM_NONE in the air
	int constantLight;		// 0x80  32 bits, dynamic light: r | g << 8 | b << 16 | (intensity / 4) << 24 (script movers "light" / "color", CG_EntityEffects)
	int loopSound;			// 0x84  8 bits, looping sound alias, configstring CS_SOUND_ALIASES + n (playLoopSound, turret fire), 0 = none
	union {
		int surfType;		// 0x88  8 bits, SURF_TYPE_* of an impact (bullet hit, grenade, explosion events / exploded missile).
							//       In archivedEntityFields too, like dmgFlags (killcam keeps the value)
		// CoD2x: for ET_PLAYER the server reuses this field for the player's health (see playerinfo.h)
		int playerHealth;	//       ET_PLAYER: health 0..255, sent as 8 bits
		// CoD2x: End
	};
	union {
		int index;			// 0x8c  10 bits, model index
		int brushmodel;		//       solid == SOLID_BMODEL: brush model number (inline model)
		int xmodel;			//       ET_GENERAL, ET_SCRIPTMOVER, ET_TURRET: configstring CS_MODELS + n (G_DObjUpdate)
		int item;			//       ET_ITEM: item index (bg_itemlist)
	};
	int clientNum;			// 0x90  8 bits, ET_PLAYER / ET_PLAYER_CORPSE: client number
	int iHeadIcon;			// 0x94  4 bits, ET_PLAYER: head icon, CS_HEAD_ICONS + n - 1, 0 = none
	int iHeadIconTeam;		// 0x98  2 bits, ET_PLAYER: team that sees the head icon, 0 = everybody
	int solid;				// 0x9c  24 bits, packed bounding box or SOLID_BMODEL (SV_LinkEntity)
	int eventParm;			// 0xa0  8 bits, parameter of a temp entity event (DirToByte normal of impacts, fx id, EV_OBITUARY weapon / MOD | 0x80)
							//       ET_LOOP_FX: up direction (DirToByte)
	int eventSequence;		// 0xa4  8 bits, number of events sent, events[eventSequence & 3] is the newest
	int events[4];			// 0xa8  8 bits each, entity events of a non-temp entity (G_AddEvent)
	int eventParms[4];		// 0xb8  8 bits each, their parameters; ET_SOUND_BLEND: [0] and [1] the two sound aliases (CS_SOUND_ALIASES + n)
	int weapon;				// 0xc8  7 bits, weapon index: ET_PLAYER held weapon, ET_MISSILE, ET_TURRET; EV_OBITUARY (CoD2x)
	int legsAnim;			// 0xcc  10 bits, ET_PLAYER / ET_PLAYER_CORPSE: animation | ANIM_TOGGLEBIT
	int torsoAnim;			// 0xd0  10 bits, the same
	float leanf;			// 0xd4  float, ET_PLAYER / ET_PLAYER_CORPSE: lean fraction; ET_SOUND_BLEND: blend fraction of the two aliases
	union
	{
		int scale;			// 0xd8  8 bits
		int hintString;		//       usable entities: hint string, configstring CS_HINTSTRINGS + n, 255 = none (GScr_SetHintString)
		int fxId;			//       ET_LOOP_FX: effect, configstring CS_EFFECT_NAMES + n (loadFx id)
		int eventParm2;		//       bullet impact events: reflected direction (DirToByte), CoD4 name
	};
	union {
		int dmgFlags;		// 0xdc  32 bits, original netfield name; in 1.3 used only as cursorHint, never as damage flags
		int cursorHint;		//       usable entities: HINT_* (GScr_SetCursorHint, trigger_use_shared), read only by the server
		// CoD2x: for ET_PLAYER the server reuses this field for weapons and grenades (see playerinfo.h)
		int playerInfo;		//       ET_PLAYER: packed weapons and grenades, 0xC1 in the top byte
		int obituaryIcons;	//       EV_OBITUARY: shader indexes (CS_SHADERS + n) of up to 4 extra killfeed icons, 8 bits each (gsc.cpp)
		// CoD2x: End
	};
	int animMovetype;		// 0xe0  4 bits, ET_PLAYER: animation move type
	float fTorsoHeight;		// 0xe4  float, ET_PLAYER: prone torso height
	float fTorsoPitch;		// 0xe8  float, ET_PLAYER: prone torso pitch
	float fWaistPitch;		// 0xec  float, ET_PLAYER: prone waist pitch
} entityState_t;
static_assert(sizeof(entityState_t) == 0xf0, "entityState_t");
static_assert(offsetof(entityState_t, pos) == 0x0c && offsetof(entityState_t, apos) == 0x30, "entityState_t::pos");
static_assert(offsetof(entityState_t, time) == 0x54 && offsetof(entityState_t, otherEntityNum) == 0x74, "entityState_t::time");
static_assert(offsetof(entityState_t, playerHealth) == 0x88 && offsetof(entityState_t, index) == 0x8c, "entityState_t::playerHealth");
static_assert(offsetof(entityState_t, solid) == 0x9c && offsetof(entityState_t, events) == 0xa8, "entityState_t::solid");
static_assert(offsetof(entityState_t, weapon) == 0xc8 && offsetof(entityState_t, leanf) == 0xd4, "entityState_t::weapon");
static_assert(offsetof(entityState_t, scale) == 0xd8 && offsetof(entityState_t, playerInfo) == 0xdc, "entityState_t::playerInfo");
static_assert(offsetof(entityState_t, fWaistPitch) == 0xec, "entityState_t::fWaistPitch");

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

// Returns the index of the weapon (1 ..), 0 if there is no weapon of that name. While the level is
// initializing the weapon is loaded and registered if it is not yet.
// 005282b0    int32_t G_GetWeaponIndexForName(char const* name @ eax)
inline int G_GetWeaponIndexForName(const char* name) {
    int ret;
    ASM_CALL(RETURN(ret), ADDR(0x005282b0, 0x08120dd0), WL(0, 1), WL(EAX, PUSH)(name));
    return ret;
}

// Returns the index of the shader (material) in the configstrings CS_SHADERS (1 .. 127), the name is lowercased.
// While the level is initializing a new shader is added (precacheShader), later a shader that is not there
// is a script error "shader ... not precached" and 0 is returned.
// 0050e2a0    int32_t G_ShaderIndex(char const* name @ eax)
inline int G_ShaderIndex(const char* name) {
    int ret;
    ASM_CALL(RETURN(ret), ADDR(0x0050e2a0, 0x0811d44a), WL(0, 1), WL(EAX, PUSH)(name));
    return ret;
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