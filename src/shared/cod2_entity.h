#ifndef COD2_ENTITY_H
#define COD2_ENTITY_H

#include "cod2_math.h"

#define GENTITYNUM_BITS     10
#define MAX_GENTITIES       ( 1 << GENTITYNUM_BITS ) // 1024

#define g_entities 			(*((gentity_t (*)[MAX_GENTITIES])(ADDR(0x01744380, 0x08716400))))



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


enum entityType_e
{
	ET_GENERAL = 0x0,
	ET_PLAYER = 0x1,
	ET_PLAYER_CORPSE = 0x2,
	ET_ITEM = 0x3,
	ET_MISSILE = 0x4,
	ET_INVISIBLE = 0x5,
	ET_SCRIPTMOVER = 0x6,
	ET_FX = 0x7,
	ET_LOOP_FX = 0x8,
	ET_TURRET = 0x9,
	ET_EVENTS = 0xA,
};


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
	int scale;
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


#endif