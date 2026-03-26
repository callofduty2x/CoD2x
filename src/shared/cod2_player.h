#ifndef COD2_PLAYER_H
#define COD2_PLAYER_H

#include "cod2_math.h"

#include <stddef.h>

#include "cod2_entity.h"

// Credit: CoD2rev_Server by voron00


#define level_bgs (*((bgs_t**)( ADDR(0x019a1c78, 0x0860b420) )))
#define level_bgs_server (*((bgs_t*)( ADDR(0x017d03d0, 0x0864f9c0) )))
#define globalScriptData (*((animScriptData_t**)( ADDR(0x019a1c7c, 0x0860b424) )))
#define g_xAnimInfo (*((XAnimInfo (*)[1024])( ADDR(0x00f54280, 0x085bc900) )))
#define bg_swingSpeed (*((dvar_t**)( ADDR(0x019439f4, 0x08626ff4) )))


// pm_flags
#define PMF_PRONE 				0x1
#define PMF_CROUCH 				0x2
#define PMF_MANTLE 				0x4
#define PMF_FRAG				0x10
#define PMF_LADDER 				0x20
#define PMF_BACKWARDS_RUN 		0x80
#define PMF_SLIDING 			0x200
#define PMF_MELEE 				0x2000
#define PMF_JUMPING 			0x80000
#define PMF_SPECTATING 			0x1000000
#define PMF_DISABLEWEAPON 		0x4000000

// Player Z offset in 1st person in different stances
#define PLAYER_STAND_HEIGHT 60
#define PLAYER_CROUCH_HEIGHT 40
#define PLAYER_PRONE_HEIGHT 11

// Player transition time in milliseconds between stances
#define PLAYER_PRONE_TIME 400	// prone -> stand, stand -> prone
#define PLAYER_CROUCH_TIME 200  // stand -> crouch, crouch -> stand



struct hudelem_color
{
	unsigned char r;
	unsigned char g;
	unsigned char b;
	unsigned char a;
};

struct hudelem
{
	int type;
	float x;
	float y;
	float z;
	float fontScale;
	int font;
	int alignOrg;
	int alignScreen;
	hudelem_color color;
	hudelem_color fromColor;
	int fadeStartTime;
	int fadeTime;
	int label;
	int width;
	int height;
	int materialIndex;
	int fromWidth;
	int fromHeight;
	int scaleStartTime;
	int scaleTime;
	float fromX;
	float fromY;
	int fromAlignOrg;
	int fromAlignScreen;
	int moveStartTime;
	int moveTime;
	int time;
	int duration;
	float value;
	int text;
	float sort;
	hudelem_color foreground;
};


struct hudElemState
{
	hudelem current[31];
	hudelem archival[31];
};

struct objective_s
{
	int state;
	vec3_t origin;
	int entNum;
	int teamNum;
	int icon;
};

struct MantleState
{
	float yaw;
	int timer;
	int transIndex;
	int flags;
};

typedef struct playerState
{
	int commandTime;
	int pm_type;
	int bobCycle;
	int pm_flags;
	int pm_time;
	vec3_t origin;
	vec3_t velocity;
	vec2_t oldVelocity;
	int weaponTime;
	int weaponDelay;
	int grenadeTimeLeft;
	int weaponRestrictKickTime;
	int foliageSoundTime;
	int gravity;
	float leanf;
	int speed;
	int delta_angles[3];
	int groundEntityNum;
	vec3_t vLadderVec;
	int jumpTime;
	float jumpOriginZ;
	int legsTimer;
	int legsAnim;
	int torsoTimer;
	int torsoAnim;
	int legsAnimDuration;
	int torsoAnimDuration;
	int damageTimer;
	int damageDuration;
	int flinchYaw;
	int movementYaw; // Movement direction angle (based on last origin movement) -> +90 = left, 45=strafe-left, 0=forward, -45=right-strafe, -90 = right
	int eFlags;
	int eventSequence;
	int events[4];
	int eventParms[4];
	int oldEventSequence;
	int clientNum;
	int offHandIndex;
	unsigned int weapon;
	int weaponstate;
	float fWeaponPosFrac;
	int adsDelayTime;
	int viewmodelIndex;
	vec3_t viewangles;
	int viewHeightTarget;
	float viewHeightCurrent;
	int viewHeightLerpTime;
	int viewHeightLerpTarget;
	int viewHeightLerpDown;
	float viewHeightLerpPosAdj;
	vec2_t viewAngleClampBase;
	vec2_t viewAngleClampRange;
	int damageEvent;
	int damageYaw;
	int damagePitch;
	int damageCount;
	int stats[6];
	int ammo[128];
	int ammoclip[128];
	int weapons[2];
	int oldweapons[2];
	char weaponslots[8];
	int weaponrechamber[2];
	int oldweaponrechamber[2];
	vec3_t mins;
	vec3_t maxs;
	float proneDirection;
	float proneDirectionPitch;
	float proneTorsoPitch;
	int viewlocked;
	int viewlocked_entNum;
	int cursorHint;
	int cursorHintString;
	int cursorHintEntIndex;
	int iCompassFriendInfo;
	float fTorsoHeight;
	float fTorsoPitch;
	float fWaistPitch;
	float holdBreathScale;
	int holdBreathTimer;
	MantleState mantleState;
	int entityEventSequence;
	int weapAnim;
	float aimSpreadScale;
	int shellshockIndex;
	int shellshockTime;
	int shellshockDuration;
	objective_s objective[16];
	int deltaTime;
	hudElemState hud;
} playerState_t;
static_assert((sizeof(playerState_t) == 0x26a8));








typedef struct
{
	float fraction;
	vec3_t normal;
	int surfaceFlags;
	int contents;
	void *material;
	unsigned short entityNum;
	unsigned short partName;
	unsigned short partGroup;
	byte allsolid;
	byte startsolid;
} trace_t;

typedef struct usercmd_s
{
	int serverTime;
	int buttons;
	byte weapon;
	byte offHandIndex;
	int angles[3];
	char forwardmove;
	char rightmove;
} usercmd_t;

struct pmove_t
{
	playerState_t *ps;
	usercmd_s cmd;
	usercmd_s oldcmd;
	int tracemask;
	int numtouch;
	int touchents[32];
	vec3_t mins;
	vec3_t maxs;
	float xyspeed;
	int proneChange;
	byte handler;
	byte mantleStarted;
	vec3_t mantleEndPos;
	int mantleDuration;
};

struct pml_t
{
	vec3_t forward;
	vec3_t right;
	vec3_t up;
	float frametime;
	int msec;
	int walking;
	int groundPlane;
	int almostGroundPlane;
	trace_t groundTrace;
	float impactSpeed;
	vec3_t previous_origin;
	vec3_t previous_velocity;
	unsigned int holdrand;
};







struct XAnimState
{
	float time;
	float oldTime;
	int16_t cycleCount;
	int16_t oldCycleCount;
	float goalTime;
	float goalWeight;
	float weight;
	float rate;
};

typedef struct
{
	uint16_t notifyChild;
	int16_t notifyIndex;
	uint16_t notifyName;
	uint16_t notifyType;
	uint16_t prev;
	uint16_t next;
	XAnimState state;
} XAnimInfo;

typedef struct XAnimParts_s
{
	unsigned short numframes;
	byte bLoop;
	byte bDelta;
	float framerate;
	float frequency;
	byte notifyCount;
	char assetType;
	short boneCount;
	uint16_t *names;
	char* simpleQuatBits;
	void *parts;
	void *notify;
	void *deltaPart;
	const char *name;
	bool isDefault;
} XAnimParts;

struct XAnimParent
{
	uint16_t flags;
	uint16_t children;
};

union XAnimEntryUnion
{
	XAnimParts *parts;
	XAnimParent animParent;
};

struct XAnimEntry
{
	uint16_t numAnims;
	uint16_t parent;
	XAnimEntryUnion u;
};

typedef struct XAnim_s
{
	const char *debugName;
	unsigned int size;
	const char **debugAnimNames;
	XAnimEntry entries[1];
} XAnim;

typedef struct XAnimTree_s
{
	XAnim_s *anims;
	uint16_t entnum;
	bool bAbs;
	bool bUseGoalWeight;
	uint16_t infoArray[1];
} XAnimTree;


typedef struct PACKED animation_s
{
	char name[64];
	int initialLerp;
	float moveSpeed;
	int duration;
	int nameHash;
	int flags;
	WL(int _padding1, /**/);
	int64_t movetype;
	int noteType;
	WL(int _padding2, /**/);
} animation_t;
static_assert((sizeof(animation_s) == WL(0x68, 0x60)));
static_assert(offsetof(animation_s, initialLerp) == WL(0x40, 0x40));
static_assert(offsetof(animation_s, flags) == WL(0x50, 0x50));
static_assert(offsetof(animation_s, movetype) == WL(0x58, 0x54));
static_assert(offsetof(animation_s, noteType) == WL(0x60, 0x5c));


struct lerpFrame_t
{
	float yawAngle; // 0 - 360, world angle
	int yawing;
	float pitchAngle; // 0 - 360, world angle
	int pitching;
	int animationNumber;
	animation_s *animation;
	int animationTime;
	vec3_t oldFramePos;
	float animSpeedScale;
	int oldFrameSnapshotTime;
};

struct clientControllers_t
{
	union
	{
		struct
		{
			vec3_t tag_back_low_angles;
			vec3_t tag_back_mid_angles;
			vec3_t tag_back_up_angles;
			vec3_t tag_neck_angles;
			vec3_t tag_head_angles;
			vec3_t tag_pelvis_angles;
			vec3_t tag_origin_angles;
			vec3_t tag_origin_offset;
		};
		vec3_t angles[8];
	};
};

typedef enum {
	TEAM_NONE,
	TEAM_AXIS,
	TEAM_ALLIES,
	TEAM_SPECTATOR,

	TEAM_NUM_TEAMS
} team_t;


typedef struct clientInfo_s
{
	int infoValid;
	int nextValid;
	int clientNum;
	char name[32];
	team_t team;
	team_t oldteam;
	int score;
	int location;
	int health;
	char model[64];
	char attachModelNames[6][64];
	char attachTagNames[6][64];
	lerpFrame_t legs;
	lerpFrame_t torso;
	float movementYaw; // Movement direction angle (based on last origin movement) -> +90 = left, 45=strafe-left, 0=forward, -45=right-strafe, -90 = right
	float lerpLean; // left = -0.5, center = 0, Right = 0.5
	vec3_t playerAngles;
	int leftHandGun;
	int dobjDirty;
	clientControllers_t control;
	int clientConditions[9][2];
	XAnimTree_s *pXAnimTree;
	int iDObjWeapon;
	int stanceTransitionTime;
	int turnAnimEndTime;
	char turnAnimType;
} clientInfo_t;
static_assert((sizeof(clientInfo_t) == 0x4b8));







typedef struct scr_anim_s
{
	union
	{
		struct
		{
			uint16_t index;
			uint16_t tree;
		};
		const char *linkPointer;
	};
} scr_anim_t;
static_assert((sizeof(scr_anim_t) == 0x4));


struct scr_animtree_t
{
	struct XAnim_s *anims;
};

typedef struct snd_alias_list_s
{
	const char *aliasName;
	struct snd_alias_t *head;
	int count;
	snd_alias_list_s *next;
} snd_alias_list_t;

typedef struct
{
	short bodyPart[2];
	short animIndex[2];
	unsigned short animDuration[2];
	snd_alias_list_t *soundAlias;
} animScriptCommand_t;

typedef struct
{
	int index;
	int value[2];
} animScriptCondition_t;

typedef struct
{
	int numConditions;
	animScriptCondition_t conditions[9];
	int numCommands;
	animScriptCommand_t commands[8];
} animScriptItem_t;

typedef struct
{
	int numItems;
	animScriptItem_t *items[128];
} animScript_t;

typedef struct animScriptData_s
{
	animation_s animations[512];
	int numAnimations;
	animScript_t scriptAnims[4][41];
	animScript_t scriptCannedAnims[4][41];
	animScript_t scriptStateChange[4][4];
	animScript_t scriptEvents[19];
	animScriptItem_t scriptItems[2048];
	int numScriptItems;
	scr_animtree_t animTree;
	unsigned short torsoAnim;
	unsigned short legsAnim;
	unsigned short turningAnim;
	unsigned short rootAnim;
	snd_alias_list_t *(*soundAlias)(const char *);
	void (*playSoundAlias)(int, snd_alias_list_t *);
} animScriptData_t;
static_assert((sizeof(animScriptData_t) == WL(0x0b4bc8, 0x0b3bc8)));
static_assert(offsetof(animScriptData_t, numAnimations) == WL(0x00d000, 0x00c000));

struct bgsAnim_t
{
	animScriptData_t animScriptData;
	scr_animtree_t tree;
	scr_anim_t     root;
	scr_anim_t     torso;
	scr_anim_t     legs;
	scr_anim_t     turning;
};
static_assert((sizeof(bgsAnim_t) == WL(0x0b4bdc, 0x0b3bdc)));
static_assert(offsetof(bgsAnim_t, turning) == WL(0x0b4bd8, 0x0b3bd8));

typedef struct bgs_s
{
	bgsAnim_t animData;
	int time;
	int latestSnapshotTime;
	int frametime;
	int anim_user;
	void* GetXModel;
	void* CreateDObj;
	void* SafeDObjFree;
	void* AllocXAnim;
	clientInfo_t clientinfo[64];
} bgs_t;
static_assert((sizeof(bgs_t) == WL(0xc79fc, 0xc69fc)));
static_assert(offsetof(bgs_t, time) == WL(0x0b4bdc, 0x0b3bdc));

typedef enum
{
	ANIM_COND_PLAYERANIMTYPE,
	ANIM_COND_WEAPONCLASS,
	ANIM_COND_MOUNTED,
	ANIM_COND_MOVETYPE,
	ANIM_COND_UNDERHAND,
	ANIM_COND_CROUCHING,
	ANIM_COND_FIRING,
	ANIM_COND_WEAPON_POSITION,
	ANIM_COND_STRAFING,
	NUM_ANIM_CONDITIONS
} scriptAnimConditions_t;

typedef enum
{
	ANIM_MT_UNUSED,
	ANIM_MT_IDLE,
	ANIM_MT_IDLECR,
	ANIM_MT_IDLEPRONE,
	ANIM_MT_WALK,
	ANIM_MT_WALKBK,
	ANIM_MT_WALKCR,
	ANIM_MT_WALKCRBK,
	ANIM_MT_WALKPRONE,
	ANIM_MT_WALKPRONEBK,
	ANIM_MT_RUN,
	ANIM_MT_RUNBK,
	ANIM_MT_RUNCR,
	ANIM_MT_RUNCRBK,
	ANIM_MT_TURNRIGHT,
	ANIM_MT_TURNLEFT,
	ANIM_MT_TURNRIGHTCR,
	ANIM_MT_TURNLEFTCR,
	ANIM_MT_CLIMBUP,
	ANIM_MT_CLIMBDOWN,
	ANIM_MT_MANTLE_ROOT,
	ANIM_MT_MANTLE_UP_57,
	ANIM_MT_MANTLE_UP_51,
	ANIM_MT_MANTLE_UP_45,
	ANIM_MT_MANTLE_UP_39,
	ANIM_MT_MANTLE_UP_33,
	ANIM_MT_MANTLE_UP_27,
	ANIM_MT_MANTLE_UP_21,
	ANIM_MT_MANTLE_OVER_HIGH,
	ANIM_MT_MANTLE_OVER_MID,
	ANIM_MT_MANTLE_OVER_LOW,
	ANIM_MT_FLINCH_FORWARD,
	ANIM_MT_FLINCH_BACKWARD,
	ANIM_MT_FLINCH_LEFT,
	ANIM_MT_FLINCH_RIGHT,
	ANIM_MT_STUMBLE_FORWARD,
	ANIM_MT_STUMBLE_BACKWARD,
	ANIM_MT_STUMBLE_WALK_FORWARD,
	ANIM_MT_STUMBLE_WALK_BACKWARD,
	ANIM_MT_STUMBLE_CROUCH_FORWARD,
	ANIM_MT_STUMBLE_CROUCH_BACKWARD,
	NUM_ANIM_MOVETYPES
} scriptAnimMoveTypes_t;

/**
 * Converts a mask long integer into individual enum items.
 * Returns a static array of strings with the item names (without the ANIM_MT_ prefix).
 */
inline const char **get_scriptAnimMoveTypeStrings(int64_t mask)
{
	static const char *enumNames[] = {
		"UNUSED", "IDLE", "IDLECR", "IDLEPRONE", "WALK", "WALKBK", "WALKCR", "WALKCRBK",
		"WALKPRONE", "WALKPRONEBK", "RUN", "RUNBK", "RUNCR", "RUNCRBK", "TURNRIGHT", "TURNLEFT",
		"TURNRIGHTCR", "TURNLEFTCR", "CLIMBUP", "CLIMBDOWN", "MANTLE_ROOT", "MANTLE_UP_57",
		"MANTLE_UP_51", "MANTLE_UP_45", "MANTLE_UP_39", "MANTLE_UP_33", "MANTLE_UP_27",
		"MANTLE_UP_21", "MANTLE_OVER_HIGH", "MANTLE_OVER_MID", "MANTLE_OVER_LOW",
		"FLINCH_FORWARD", "FLINCH_BACKWARD", "FLINCH_LEFT", "FLINCH_RIGHT", "STUMBLE_FORWARD",
		"STUMBLE_BACKWARD", "STUMBLE_WALK_FORWARD", "STUMBLE_WALK_BACKWARD",
		"STUMBLE_CROUCH_FORWARD", "STUMBLE_CROUCH_BACKWARD"
	};

	static const char *result[NUM_ANIM_MOVETYPES];
	int count = 0;

	for (int i = 0; i < NUM_ANIM_MOVETYPES; ++i)
	{
		if (mask & (1LL << i))
		{
			result[count++] = enumNames[i];
		}
	}

	result[count] = nullptr; // Null-terminate the array
	return result;
}








inline unsigned int BG_GetConditionValue(clientInfo_t *ci, scriptAnimConditions_t condition, int checkConversion)
{
	unsigned int ret;
	ASM_CALL(RETURN(ret), ADDR(0x004f93d0, 0x080d98a4), WL(0, 3), WL(ESI, PUSH)(ci), WL(ECX, PUSH)(condition), WL(EDX, PUSH)(checkConversion));
	return ret;
}

/**
 * Left = -1, center = 0, Right = 1
 */
inline float GetLeanFraction(const float fFrac)
{
	float newFrac = (2.0f - fabs(fFrac)) * fFrac;
	return newFrac;
}

inline void BG_LerpAngles(float *angles_goal, float maxAngleChange, float *angles)
{
	int i;
	float diff;

	for ( i = 0; i < 3; ++i )
	{
		diff = angles_goal[i] - angles[i];

		if ( diff <= maxAngleChange )
		{
			if ( -maxAngleChange <= diff )
				angles[i] = angles_goal[i];
			else
				angles[i] = angles[i] - maxAngleChange;
		}
		else
		{
			angles[i] = angles[i] + maxAngleChange;
		}
	}
}

inline void BG_LerpOverTime(const float *start, const float *goal, float lerpFraction, float *angles)
{
    for (int i = 0; i < 3; ++i)
    {
		angles[i] = start[i] * (1.0f - lerpFraction) + goal[i] * lerpFraction;
    }
}


inline void BG_LerpOffset(float *offset_goal, float maxOffsetChange, float *offset)
{
	float scale;
	float error;
	vec3_t diff;

	VectorSubtract(offset_goal, offset, diff);

	scale = VectorLengthSquared(diff);

	if ( scale != 0.0 )
	{
		error = I_rsqrt(scale) * maxOffsetChange;

		if ( error >= 1.0 )
			VectorCopy(offset_goal, offset);
		else
			VectorMA(offset, error, diff, offset);
	}
}


inline playerState_t* SV_GetPlayerStateByNum(int num) {
	return ((playerState_t* (*)(int))ADDR(0x00456730, 0x08091716))(num);
}

/**
 * Get the player's stance.
 * 1 = stand
 * 2 = crouch
 * 3 = prone
 */
inline int SV_GetPlayerStance(int num) {
	playerState_t *ps = SV_GetPlayerStateByNum(num);

	if (ps->pm_flags & PMF_CROUCH)
		return 2;
	else if (ps->pm_flags & PMF_PRONE)
		return 3;
	else
		return 1;
}


inline void G_GetPlayerViewOrigin(gentity_s* ent, float* origin) {
	WL(
		ASM_CALL(RETURN_VOID, 0x004fdd30, 1, ESI(origin), PUSH(ent)),
		ASM_CALL(RETURN_VOID, 0x080f8916, 2, PUSH(ent), PUSH(origin))
	)
}


#endif