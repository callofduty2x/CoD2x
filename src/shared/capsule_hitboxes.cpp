#include "capsule_hitboxes.h"

#include "shared.h"
#include "assembly.h"
#include "capsule_trace.h"
#include "cod2_dvars.h"
#include "cod2_entity.h"
#include "cod2_math.h"
#include "cod2_player.h"
#include "cod2_server.h"
#include "cod2_script.h"
#include "patch.h"
#include <string.h>
#include <stddef.h>
#if COD2X_WIN32
#include "cod2_client.h"
#endif

dvar_t* sv_capsuleHitboxes;
dvar_t* sv_capsuleHitboxScale;
dvar_t* sv_capsuleHitboxesDebug;
dvar_t* sv_debugBotStance;
dvar_t* sv_debugBotPitch;

struct DObjAnimMat
{
	float quat[4];
	float trans[3];
	float transWeight;
};
static_assert(sizeof(DObjAnimMat) == 32);

struct DObjSkeleton
{
	char padding[48];
	DObjAnimMat mat[1];
};

struct XModelHierarchy
{
	unsigned short* names;
};

struct XModelParts
{
	short numBones;
	short numRootBones;
	XModelHierarchy* hierarchy;
};

struct XModel
{
	XModelParts* parts;
	char padding[0x84];
	const char* name;			// offset 0x88
};
static_assert(offsetof(XModel, name) == 0x88);

struct DObj
{
	char padding1[4];
	DObjSkeleton* skel;			// offset 0x04
	char padding2[16];
	unsigned char numModels;	// offset 0x18
	unsigned char numBones;		// offset 0x19
	unsigned char padding3[2];
	XModel* models[8];			// offset 0x1c
};
static_assert(offsetof(DObj, skel) == 0x04);
static_assert(offsetof(DObj, numModels) == 0x18);
static_assert(offsetof(DObj, numBones) == 0x19);
static_assert(offsetof(DObj, models) == 0x1c);

static_assert(sizeof(capsuleTraceResult_t) == 24);
static_assert(offsetof(capsuleTraceResult_t, normal) == 8);
static_assert(offsetof(capsuleTraceResult_t, partName) == 20);
static_assert(offsetof(capsuleTraceResult_t, partGroup) == 22);


static bool capsule_name_equals(const char* a, const char* b)
{
	for (; *a && *b; a++, b++)
	{
		char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
		if (ca != cb)
			return false;
	}
	return *a == *b;
}

// Bones are looked up by name, because the same bone has a different index in each player model.
// A body and a head model can share a bone; the first copy that has been calculated is used.
static void capsule_read_bones(DObj* obj, capsuleBone_t* bones)
{
	memset(bones, 0, sizeof(capsuleBone_t) * CAP_BONE_COUNT);

	if (!obj->skel)
		return;

	int offset = 0;
	for (int m = 0; m < obj->numModels && m < 8; m++)
	{
		XModelParts* parts = obj->models[m]->parts;
		const unsigned short* names = parts->hierarchy->names;

		for (int i = 0; i < parts->numBones && offset + i < obj->numBones; i++)
		{
			const DObjAnimMat* mat = &obj->skel->mat[offset + i];
			if (mat->transWeight == 0.0f)
				continue;

			const char* name = SL_ConvertToString(names[i]);
			if (!name)
				continue;

			for (int b = 0; b < CAP_BONE_COUNT; b++)
			{
				if (!bones[b].valid && capsule_name_equals(name, capsule_bone_names[b]))
				{
					capsule_bone_set(&bones[b], mat->quat, mat->trans, mat->transWeight, names[i]);
					break;
				}
			}
		}
		offset += parts->numBones;
	}
}

// Mode 2 fits the capsules to the models of this player: body, head and headgear
static const capsuleFit_t* capsule_read_fit(DObj* obj, capsuleFit_t* fit)
{
	if (sv_capsuleHitboxes->value.integer != 2)
		return NULL;

	memset(fit, 0, sizeof(*fit));
	for (int m = 0; m < obj->numModels && m < 8; m++)
		capsule_fit_add_model(fit, obj->models[m]->name);
	return fit;
}

// 00488450    void DObjTraceline(struct DObj* obj, float* start, float* end, uint8_t* priorityMap, struct DObjTrace_t* trace)
static void DObjTraceline(DObj* obj, const float* start, const float* end, const unsigned char* priorityMap, capsuleTraceResult_t* trace)
{
	// CoD2x: player hits use capsules, anything that is not a player keeps its stock hitboxes
	if (sv_capsuleHitboxes && sv_capsuleHitboxes->value.integer != 0)
	{
		capsuleBone_t bones[CAP_BONE_COUNT];
		capsule_read_bones(obj, bones);

		capsuleFit_t fit;
		if (capsule_is_player_skeleton(bones) &&
			capsule_trace(bones, start, end, priorityMap, sv_capsuleHitboxScale->value.decimal, trace, capsule_read_fit(obj, &fit)))
		{
			return;
		}
	}
	// CoD2x: End

	// Call the original function
	ASM_CALL(RETURN_VOID, ADDR(0x00488450, 0x080bb27e), 5, PUSH(obj), PUSH(start), PUSH(end), PUSH(priorityMap), PUSH(trace));
}

// The wireframe uses the client's debug lines, so it only exists in the Windows client (listen server)
#if COD2X_WIN32

#define CAPSULE_BUTTON_PRONE 0x100
#define CAPSULE_BUTTON_CROUCH 0x200
#define CAPSULE_PM_NORMAL_LINKED 1

// Inlined in the Windows build, see G_LocationalTrace
#define serverObjMap ((unsigned short*)0x00c28b80)
#define objBuf ((char*)0x00c29c00)

// The thin finger capsules are skipped, they would use up the game's limit of 4096 debug lines
#define CAPSULE_DEBUG_MIN_RADIUS 1.0f

struct capsuleDebugDraw_t
{
	const float* origin;
	vec3_t axis[3];
	const float* color;
};

static void capsule_debug_line(void* context, const float* start, const float* end)
{
	capsuleDebugDraw_t* draw = (capsuleDebugDraw_t*)context;

	vec3_t lineStart, lineEnd;
	for (int i = 0; i < 3; i++)
	{
		lineStart[i] = draw->origin[i] + start[0] * draw->axis[0][i] + start[1] * draw->axis[1][i] + start[2] * draw->axis[2][i];
		lineEnd[i] = draw->origin[i] + end[0] * draw->axis[0][i] + end[1] * draw->axis[1][i] + end[2] * draw->axis[2][i];
	}

	// Without depth test, so the capsules show through the player model.
	// In the server's channel the line stays until the next server frame.
	CL_AddDebugLine(lineStart, lineEnd, draw->color, 0, 0, 1);
}

static void capsule_debug_shape(void* context, const float* a, const float* b, float radiusA, float radiusB, int hit, unsigned short partName)
{
	static const vec4_t colorHead = { 1.0f, 0.15f, 0.15f, 1.0f };
	static const vec4_t colorNeck = { 1.0f, 0.55f, 0.1f, 1.0f };
	static const vec4_t colorChest = { 1.0f, 1.0f, 0.2f, 1.0f };
	static const vec4_t colorBelly = { 0.3f, 1.0f, 0.3f, 1.0f };
	static const vec4_t colorArm = { 0.2f, 1.0f, 1.0f, 1.0f };
	static const vec4_t colorHand = { 0.3f, 0.5f, 1.0f, 1.0f };
	static const vec4_t colorLeg = { 1.0f, 0.3f, 1.0f, 1.0f };
	static const vec4_t colorFoot = { 1.0f, 1.0f, 1.0f, 1.0f };

	if (radiusA < CAPSULE_DEBUG_MIN_RADIUS && radiusB < CAPSULE_DEBUG_MIN_RADIUS)
		return;

	capsuleDebugDraw_t* draw = (capsuleDebugDraw_t*)context;
	switch (hit)
	{
		case 2: draw->color = colorHead; break;
		case 3: draw->color = colorNeck; break;
		case 4: draw->color = colorChest; break;
		case 5: draw->color = colorBelly; break;
		case 10: case 11: draw->color = colorHand; break;
		case 16: case 17: draw->color = colorFoot; break;
		default: draw->color = hit < 10 ? colorArm : colorLeg; break;
	}

	capsule_wireframe(a, b, radiusA, radiusB, capsule_debug_line, draw);
}

// Test helper for comparing the capsules with the model: poses all bots through the input they send.
// 00456010    void SV_ClientThink(struct client_t* cl @ ecx, struct usercmd_s* cmd @ eax), hooked in SV_BotUserMove
static void SV_ClientThink_Win32()
{
	client_t* cl;
	usercmd_t* cmd;
	ASM( movr, cmd, "eax" );
	ASM( movr, cl, "ecx" );

	// CoD2x: stance and view pitch of bots
	if (sv_debugBotStance && sv_debugBotPitch)
	{
		playerState_t* ps = SV_GetPlayerStateByNum(cl - svs_clients);
		int stance = sv_debugBotStance->value.integer;

		cmd->buttons &= ~(CAPSULE_BUTTON_PRONE | CAPSULE_BUTTON_CROUCH);
		if (stance == 1)
			cmd->buttons |= CAPSULE_BUTTON_CROUCH;
		else if (stance == 2)
			cmd->buttons |= CAPSULE_BUTTON_PRONE;

		if (stance != 0)
		{
			// Bots press random buttons, which plays reload and other animations
			cmd->buttons &= CAPSULE_BUTTON_PRONE | CAPSULE_BUTTON_CROUCH;

			// Without a weapon, playeranim.script uses the bomb plant poses
			ps->pm_flags &= ~PMF_DISABLEWEAPON;
			for (int i = 0; cmd->weapon == 0 && i < 8; i++)
				cmd->weapon = ps->weaponslots[i];

			// A player linked to an entity stays crouched when asked to go prone
			if (stance == 2 && ps->pm_type == CAPSULE_PM_NORMAL_LINKED && !(ps->pm_flags & PMF_PRONE))
			{
				ps->pm_flags = (ps->pm_flags & ~PMF_CROUCH) | PMF_PRONE;
				ps->proneDirection = ps->viewangles[1];
				ps->proneDirectionPitch = 0.0f;
				ps->proneTorsoPitch = 0.0f;
			}
		}

		float pitch = sv_debugBotPitch->value.decimal;
		if (pitch != 0.0f)
			cmd->angles[0] = ((int)(pitch * 65536.0f / 360.0f) - ps->delta_angles[0]) & 65535;
	}
	// CoD2x: End

	// Call the original function
	ASM_CALL(RETURN_VOID, 0x00456010, 0, ECX(cl), EAX(cmd));
}

// Called at the end of every server frame
void capsule_hitboxes_frame()
{
	if (!sv_capsuleHitboxesDebug || sv_capsuleHitboxesDebug->value.integer == 0)
		return;

	// 1 = other players only, your own capsules would surround the camera. 2 = all players.
	int hiddenClient = sv_capsuleHitboxesDebug->value.integer == 1 ? cg.predictedPlayerState.clientNum : -1;

	for (int i = 0; i < 64; i++)
	{
		gentity_t* ent = &g_entities[i];
		if (i == hiddenClient || !ent->client || !ent->r.inuse || ent->s.eType != ET_PLAYER || ent->health <= 0)
			continue;

		unsigned short objIndex = serverObjMap[i];
		if (!objIndex)
			continue;

		G_DObjCalcPose(ent);

		capsuleBone_t bones[CAP_BONE_COUNT];
		DObj* obj = (DObj*)(objBuf + 100 * objIndex);
		capsule_read_bones(obj, bones);
		if (!capsule_is_player_skeleton(bones))
			continue;

		capsuleDebugDraw_t draw;
		draw.origin = ent->r.currentOrigin;
		AnglesToAxis(ent->r.currentAngles, draw.axis);
		capsuleFit_t fit;
		capsule_for_each_shape(bones, sv_capsuleHitboxScale->value.decimal, capsule_debug_shape, &draw, capsule_read_fit(obj, &fit));
	}
}

#else

void capsule_hitboxes_frame()
{
}

#endif

void capsule_hitboxes_init()
{
	sv_capsuleHitboxes = Dvar_RegisterInt("sv_capsuleHitboxes", 0, 0, 2, (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
	sv_capsuleHitboxScale = Dvar_RegisterFloat("sv_capsuleHitboxScale", 1.0f, 0.5f, 2.0f, (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
#if COD2X_WIN32
	sv_capsuleHitboxesDebug = Dvar_RegisterInt("sv_capsuleHitboxesDebug", 0, 0, 2, (dvarFlags_e)(DVAR_CHANGEABLE_RESET | DVAR_CHEAT));
	sv_debugBotStance = Dvar_RegisterInt("sv_debugBotStance", 0, 0, 3, (dvarFlags_e)(DVAR_CHANGEABLE_RESET | DVAR_CHEAT));
	sv_debugBotPitch = Dvar_RegisterFloat("sv_debugBotPitch", 0.0f, -85.0f, 85.0f, (dvarFlags_e)(DVAR_CHANGEABLE_RESET | DVAR_CHEAT));
#endif
}

void capsule_hitboxes_patch()
{
	patch_call(ADDR(0x004614ca, 0x0809d1e8), (unsigned int)DObjTraceline); // G_LocationalTrace, the only caller
#if COD2X_WIN32
	patch_call(0x0045c4c2, (unsigned int)SV_ClientThink_Win32); // SV_BotUserMove
#endif
}
