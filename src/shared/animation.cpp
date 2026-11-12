#include "animation.h"

#include "shared.h"
#include <cfloat>

#include "cod2_client.h"
#include "cod2_dvars.h"
#include "cod2_common.h"

extern dvar_t* g_cod2x;

dvar_t* player_offsetEnable;
dvar_t* player_offsetNeutral;
dvar_t* player_offsetAngleBackLow;
dvar_t* player_offsetAngleBackMid;
dvar_t* player_offsetAngleBackUp;
dvar_t* player_offsetAngleNeck;
dvar_t* player_offsetAngleHead;
dvar_t* player_offsetAnglePelvis;
dvar_t* player_offsetPositionOrigin;
dvar_t* player_offsetAngleOrigin;
dvar_t* player_debug;
dvar_t* player_debugEyePosition;

/*dvar_t* f1;
dvar_t* f2;
dvar_t* f3;

dvar_t* i1;
dvar_t* i2;
dvar_t* i3;

dvar_t* x;
dvar_t* y;
dvar_t* z;*/


enum controllerMovementType_e {
	CONTROLLER_DEFAULT                = 1u << 0, 
	CONTROLLER_STRAIGHT_STANCE        = 1u << 1, 
	CONTROLLER_TURNING_STANCE         = 1u << 2, 
	CONTROLLER_MOVING_LEFT            = 1u << 3, 
	CONTROLLER_MOVING_RIGHT           = 1u << 4, 
	CONTROLLER_MOVING                 = 1u << 5, 
	CONTROLLER_MOVING_BACKWARDS       = 1u << 6, 
	CONTROLLER_STAND_LEAN_LEFT        = 1u << 7, 
	CONTROLLER_CROUCH_LEAN_LEFT       = 1u << 8,
	CONTROLLER_PRONE_FORWARD_RELOAD   = 1u << 9,
	CONTROLLER_PRONE_BACKWARDS_FIRE   = 1u << 10,
	CONTROLLER_PRONE_BACKWARDS_RELOAD = 1u << 11,
	CONTROLLER_CLIMBUP                = 1u << 12,
	CONTROLLER_CLIMBDOWN              = 1u << 13,
	CONTROLLER_MOVING_LEFT_STRAFE     = 1u << 14,
	CONTROLLER_MOVING_RIGHT_STRAFE    = 1u << 15,
	CONTROLLER_STAND    			  = 1u << 16,
	CONTROLLER_CROUCH    			  = 1u << 17,
	CONTROLLER_PRONE    			  = 1u << 18
};

struct animationPlayerData_s {
	int stanceTransitionType;
	int stanceTransitionTime;
	int stanceTransitionTimeEnd;

	int controllerMovementType;
	int controllerMovementTypeLast;
	int controllerMovementEndTime;
	int controllerMovementTime;
	int controllerMovementIsRunning;
	clientControllers_t controllerMovementStart;

	float lerpLean;
};

animationPlayerData_s animationPlayerData[64 * 2]; // 64 players * 2 (server + client side)




/**
 * Since the animations runs both on server and client side, this function is used to determine if we are running on server or client side (listen server runs both client and server).
 */
bool animation_isServer() {
	return (&(*level_bgs) == &level_bgs_server);
}

/**
 * This function get the index for the player data storage, what holds our data for the player.
 * Since the animations runs both on server and client side, we need to store the data for both to be able to diagnose it when running listen server (server+client)
 */
int animation_getIndexForPlayerData(int clientNum) {
	int index = clientNum + (animation_isServer() ? 0 : 64);
	return index;
}


/**
 * Function that recomputes PITCH and ROLL based on YAW. Used to fix diagonal bug
 */
void animation_adjustRotation(const vec3_t parent_angles, const vec3_t child_angles, vec3_t out) {
    vec3_t newAngles;
	
    float yawDiff = child_angles[YAW] - parent_angles[YAW];
    float rad = DEG2RAD(yawDiff);
    float cp = cosf(rad);
    float sp = sinf(rad);
    
    // Rotate the child joint’s pitch and roll angles by the compensation angle.
    newAngles[PITCH] = child_angles[PITCH] * cp - child_angles[ROLL] * sp;
    newAngles[ROLL]  = child_angles[PITCH] * sp + child_angles[ROLL] * cp; 
    // Leave yaw unaltered (the game applies the child yaw rotation as given)
    newAngles[YAW] = child_angles[YAW];

	VectorCopy(newAngles, out);
}



/**
 * Functions responsible for calculating the player tag angles like head, torso, pelvis, etc.
 */
void BG_Player_DoControllersInternal(entityState_s *es, clientInfo_t *ci, clientControllers_t *control)
{
	float ang;
	float ang2;
	float vTorsoScale;
	float fCos;
	float fSin;
	vec3_t vHeadAngles;
	vec3_t vTorsoAngles;
	float fLeanFrac;

	// CoD2x: add possibility to reset player animations
	if (player_offsetNeutral->value.boolean && sv_running->value.boolean) {
		playerState_t* ps = SV_GetPlayerStateByNum(es->clientNum);

		ps->torsoAnim = 0 | ((ps->torsoAnim & ANIM_TOGGLEBIT) ^ ANIM_TOGGLEBIT);
		ps->legsAnim = 0 | ((ps->legsAnim & ANIM_TOGGLEBIT) ^ ANIM_TOGGLEBIT);

		es->torsoAnim = 0 | ((es->torsoAnim & ANIM_TOGGLEBIT) ^ ANIM_TOGGLEBIT);
		es->legsAnim = 0 | ((es->legsAnim & ANIM_TOGGLEBIT) ^ ANIM_TOGGLEBIT);

		return;
	}
	// CoD2x: end


	if ( (es->eFlags & EF_TURRET_ACTIVE) != 0 )
		return;

	float movementYawAngle = ci->movementYaw;

	VectorClear(vTorsoAngles);
	VectorCopy(ci->playerAngles, vHeadAngles);

	control->tag_origin_angles[1] = ci->legs.yawAngle;
	vTorsoAngles[1] = ci->torso.yawAngle;


	// Not on a ladder
	if ( (BG_GetConditionValue(ci, ANIM_COND_MOVETYPE, 0) & (1<<ANIM_MT_CLIMBUP | 1<<ANIM_MT_CLIMBDOWN)) == 0 )
	{
		vTorsoAngles[0] = ci->torso.pitchAngle;

		if ( (es->eFlags & EF_PRONE) != 0 )
		{
			vTorsoAngles[0] = AngleNormalize180(vTorsoAngles[0]);

			if ( vTorsoAngles[0] <= 0.0 )
				vTorsoAngles[0] = vTorsoAngles[0] * 0.25;
			else
				vTorsoAngles[0] = vTorsoAngles[0] * 0.5;
		}
	}

	AnglesSubtract(vHeadAngles, vTorsoAngles, vHeadAngles);
	AnglesSubtract(vTorsoAngles, control->tag_origin_angles, vTorsoAngles);
	VectorSet(control->tag_origin_offset, 0.0, 0.0, es->fTorsoHeight);

	fLeanFrac = GetLeanFraction(ci->lerpLean);
	vTorsoAngles[2] = fLeanFrac * 50.0 * 0.925; // angle left to right when leaning
	vHeadAngles[2] = vTorsoAngles[2];

	if ( fLeanFrac != 0.0 )
	{
		if ( (es->eFlags & EF_CROUCHING) != 0 )
		{
			if ( fLeanFrac <= 0.0 )
				control->tag_origin_offset[1] = -fLeanFrac * 12.5 + control->tag_origin_offset[1];
			else
				control->tag_origin_offset[1] = -fLeanFrac * 2.5 + control->tag_origin_offset[1];
		}
		else if ( fLeanFrac <= 0.0 )
		{
			control->tag_origin_offset[1] = -fLeanFrac * 5.0 + control->tag_origin_offset[1];
		}
		else
		{
			control->tag_origin_offset[1] = -fLeanFrac * 2.5 + control->tag_origin_offset[1];
		}
	}

	if ( (es->eFlags & EF_DEAD) == 0 )
		control->tag_origin_angles[1] = AngleSubtract(control->tag_origin_angles[1], ci->playerAngles[1]);

	// Prone
	if ( (es->eFlags & EF_PRONE) != 0 )
	{
		if ( fLeanFrac != 0.0 )
			vHeadAngles[2] = vHeadAngles[2] * 0.5;

		vTorsoScale = vTorsoAngles[1] * 0.0174532925199433;
		I_sinCos(vTorsoScale, &fSin, &fCos);

		control->tag_origin_angles[0] += es->fTorsoPitch;
		control->tag_origin_offset[0] += (1.0 - fCos) * -24.0;
		control->tag_origin_offset[1] += fSin * -12.0;

		if ( fLeanFrac * fSin > 0.0 )
			control->tag_origin_offset[1] += -fLeanFrac * (1.0 - fCos) * 16.0;

		control->tag_back_low_angles[0] = 0.0;
		control->tag_back_low_angles[1] = vTorsoAngles[2] * -1.2;
		control->tag_back_low_angles[2] = vTorsoAngles[2] * 0.3;

		if ( es->fTorsoPitch != 0.0 || es->fWaistPitch != 0.0 )
		{
			ang = AngleSubtract(es->fTorsoPitch, es->fWaistPitch);
			control->tag_back_low_angles[0] += ang;
		}

		control->tag_back_mid_angles[0] = 0.0;
		control->tag_back_mid_angles[1] = vTorsoAngles[1] * 0.1 - vTorsoAngles[2] * 0.2;
		control->tag_back_mid_angles[2] = vTorsoAngles[2] * 0.2;
		control->tag_back_up_angles[0] = vTorsoAngles[0];
		control->tag_back_up_angles[1] = vTorsoAngles[1] * 0.8 + vTorsoAngles[2];
		control->tag_back_up_angles[2] = vTorsoAngles[2] * -0.2;
	}
	else
	{
		if ( fLeanFrac != 0.0 )
		{
			// Crouch
			if ( (es->eFlags & EF_CROUCHING) != 0 )
			{
				if ( fLeanFrac <= 0.0 )
				{
					vTorsoAngles[2] = vTorsoAngles[2] * 1.25;
					vHeadAngles[2] = vHeadAngles[2] * 1.25;
				}
			}
			else // Stand
			{
				vTorsoAngles[2] = vTorsoAngles[2] * 1.25;
				vHeadAngles[2] = vHeadAngles[2] * 1.25;
			}
		}

		control->tag_origin_angles[2] += fLeanFrac * 3.75;

		control->tag_back_low_angles[0] = vTorsoAngles[0] * 0.2;
		control->tag_back_low_angles[1] = vTorsoAngles[1] * 0.4;
		control->tag_back_low_angles[2] = vTorsoAngles[2] * 0.5;

		if ( es->fTorsoPitch != 0.0 || es->fWaistPitch != 0.0 )
		{
			ang2 = AngleSubtract(es->fTorsoPitch, es->fWaistPitch);
			control->tag_back_low_angles[0] += ang2;
		}

		control->tag_back_mid_angles[0] = vTorsoAngles[0] * 0.3;
		control->tag_back_mid_angles[1] = vTorsoAngles[1] * 0.4;
		control->tag_back_mid_angles[2] = vTorsoAngles[2] * 0.5;
		control->tag_back_up_angles[0] = vTorsoAngles[0] * 0.5;
		control->tag_back_up_angles[1] = vTorsoAngles[1] * 0.2;
		control->tag_back_up_angles[2] = vTorsoAngles[2] * -0.6;
	}

	control->tag_neck_angles[0] = vHeadAngles[0] * 0.3;
	control->tag_neck_angles[1] = vHeadAngles[1] * 0.3;
	control->tag_neck_angles[2] = 0.0;
	control->tag_head_angles[0] = vHeadAngles[0] * 0.7;
	control->tag_head_angles[1] = vHeadAngles[1] * 0.7;
	control->tag_head_angles[2] = vHeadAngles[2] * -0.3;


	VectorClear(control->tag_pelvis_angles);
	if ( es->fWaistPitch != 0.0 || es->fTorsoPitch != 0.0 )
		control->tag_pelvis_angles[0] = AngleSubtract(es->fWaistPitch, es->fTorsoPitch);



	// CoD2x
	if (g_cod2x->value.integer >= 3)
	{
		// This function is called twice for listen server, so we need to save data for both server and client
		int dataIndex = animation_getIndexForPlayerData(ci->clientNum);

		// Determine current moving states
		bool isMovingFW = BG_GetConditionValue(ci, ANIM_COND_MOVETYPE, 0) & (1<<ANIM_MT_RUN | 1<<ANIM_MT_WALK | 1<<ANIM_MT_RUNCR | 1<<ANIM_MT_WALKCR | 1<<ANIM_MT_WALKPRONE);
		bool isMovingBW = BG_GetConditionValue(ci, ANIM_COND_MOVETYPE, 0) & (1<<ANIM_MT_RUNBK | 1<<ANIM_MT_WALKBK | 1<<ANIM_MT_RUNCRBK | 1<<ANIM_MT_WALKCRBK | 1<<ANIM_MT_WALKPRONEBK);	
		bool isMovingLeft = level_bgs->animData.animScriptData.animations[es->legsAnim & ~ANIM_TOGGLEBIT].flags & 0x10;
		bool isMovingRight = level_bgs->animData.animScriptData.animations[es->legsAnim & ~ANIM_TOGGLEBIT].flags & 0x20;

		// Set current controller movement type
		// This is used to determine if moving type changed so individual controller offsets could be time-lerped
		animationPlayerData[dataIndex].controllerMovementType = CONTROLLER_DEFAULT;
		if (isMovingFW) {
			animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_MOVING;
		} else if (isMovingBW) {
			animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_MOVING_BACKWARDS;
		}
		if (isMovingLeft) {
			animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_MOVING_LEFT;
		} else if (isMovingRight) {
			animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_MOVING_RIGHT;
		}
		if ((isMovingFW && movementYawAngle > 20.0f) || (isMovingBW && movementYawAngle < -20.0f)) {
			animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_MOVING_LEFT_STRAFE;
		} else if ((isMovingFW && movementYawAngle < -20.0f) || (isMovingBW && movementYawAngle > 20.0f)) {
			animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_MOVING_RIGHT_STRAFE;
		}
	

		// Stand or crouch
		if ((es->eFlags & EF_PRONE) == 0){

			animationPlayerData[dataIndex].controllerMovementType |= (es->eFlags & EF_CROUCHING) ? CONTROLLER_CROUCH : CONTROLLER_STAND;
			animationPlayerData[dataIndex].controllerMovementTime = 250;
	
			// Moving to left side while leaning left - move the player a bit to the left 
			if (fLeanFrac < 0.0 && (isMovingFW || isMovingBW) && movementYawAngle > 0.0f && movementYawAngle < 90.0f) {
				float offset = -fLeanFrac;

				// Crouch
				if (es->eFlags & EF_CROUCHING) {
					control->tag_origin_angles[0] += offset * 3.8f; // forward
					control->tag_origin_angles[2] -= offset * 3.8f; // left

					animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_CROUCH_LEAN_LEFT;

				// Stand
				} else {
					control->tag_origin_angles[0] += offset * 7.2f; // forward
					control->tag_origin_angles[2] -= offset * 7.2f; // left

					animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_STAND_LEAN_LEFT;
				}
			}

			// Leaning left in crouch, rotate torso so the head is more down then before
			if ((es->eFlags & EF_CROUCHING) && fLeanFrac < 0.0 && (isMovingFW || isMovingBW)) {
				control->tag_back_low_angles[0] += 40 * -fLeanFrac;
				control->tag_back_low_angles[1] += 30 * -fLeanFrac;
				control->tag_back_mid_angles[0] += -20 * -fLeanFrac;
				control->tag_back_up_angles[0]  += -20 * -fLeanFrac;
			}

			// Fix diagonal rotation misalignment (weapon is rotated down and right when moving forward and left)
			{
				vec3_t comp_back_low;
				animation_adjustRotation(control->tag_origin_angles, control->tag_back_low_angles, comp_back_low);	
				VectorCopy(comp_back_low, control->tag_back_low_angles);
			}
		}

		// Prone
		else {

			animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_PRONE;
			animationPlayerData[dataIndex].controllerMovementTime = 400;

			// If lean is 1 or -1, make it 0, and if its 0, make it 1, lineaaryrly
			float scale = 1.0f;
			if (ci->lerpLean < 0.0f) {
				scale = (0.250f + ci->lerpLean) / 0.250f;
			} else if (ci->lerpLean > 0.0f) {
				scale = (0.250f - ci->lerpLean) / 0.250f;
			}

			// Make the butt more "flat", hiding the side items a bit down
			// This is applied directly to the controller because the lerp transition should have normal speed
			control->tag_back_low_angles[0] += -30 * scale;
			control->tag_back_mid_angles[0] += 30 * scale;
			if ((isMovingFW || isMovingBW) && !(isMovingLeft || isMovingRight)) {
				control->tag_back_low_angles[0] += -10 * scale;
				control->tag_back_mid_angles[0] += 10 * scale;
			}
			control->tag_pelvis_angles[0] += 3;  // rotate pelvis so head is more down, legs more up

			animation_t* torsoAnim = ci->torso.animation;

			if (isMovingBW && scale != 0.0f) {
					
				control->tag_origin_offset[1] += -8; // move whole body right a bit
				control->tag_origin_offset[2] += -3; // move whole body down a bit
				control->tag_back_low_angles[0] += -20;
				control->tag_pelvis_angles[0] += 7;

				// Moving backward - default
				if (!torsoAnim) {
					control->tag_origin_offset[2] += -3; // move whole body down a bit

				// Reloading while moving backwards		
				} else if (torsoAnim && strstr(torsoAnim->name, "reload")) {
					control->tag_pelvis_angles[0] += 14; // rotate pelvis so head is more down, legs more up
					control->tag_origin_offset[2] += -3; // move while body down a bit

					animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_PRONE_BACKWARDS_RELOAD;

				// Firing while moving backwards
				} else {
					control->tag_pelvis_angles[0] += 13;  // rotate pelvis so head is more down, legs more up
					control->tag_origin_offset[2] += -3; // move while body down a bit

					animationPlayerData[dataIndex].controllerMovementTime = 100;
					animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_PRONE_BACKWARDS_FIRE;
				}

			// Left or right
			} else if ((isMovingLeft || isMovingRight) && scale != 0.0f) {
				control->tag_origin_offset[2] += -1; // move whole body down a bit

				animationPlayerData[dataIndex].controllerMovementTime = 150;

			// Forward
			} else if (isMovingFW) 
			{
				// Reloading while moving backwards		
				if (torsoAnim && strstr(torsoAnim->name, "reload")) {
					control->tag_pelvis_angles[0] += 3; // rotate pelvis so head is more down, legs more up

					animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_PRONE_FORWARD_RELOAD;
				}
			}
		}


		// Ladder - climp up - improve head rotation
		if ((BG_GetConditionValue(ci, ANIM_COND_MOVETYPE, 0) & (1<<ANIM_MT_CLIMBUP)) != 0) 
		{
			// Compensate the animation head rotation to neutral
			control->tag_neck_angles[0] = 30;
			control->tag_neck_angles[1] = -12;
			control->tag_neck_angles[2] = -9;

			control->tag_neck_angles[0] += fclamp(ci->playerAngles[0], -80, 70);
			control->tag_neck_angles[1] += AngleNormalize180(ci->playerAngles[1] - ci->torso.yawAngle);
			control->tag_neck_angles[2] += 0;

			vec3_t zeroVec = {0, 0, 0};
			animation_adjustRotation(zeroVec, control->tag_neck_angles, control->tag_neck_angles);

			control->tag_neck_angles[0] += sinf(DEG2RAD(fclamp(ci->playerAngles[0], 0, 90))) * sinf(DEG2RAD(fclamp(movementYawAngle, -90, 0))) * 45;
			control->tag_neck_angles[1] += sinf(DEG2RAD(fclamp(ci->playerAngles[0], 0, 90))) * sinf(DEG2RAD(fclamp(movementYawAngle, 0, 90))) * 30;

			animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_CLIMBUP;
		}

		// Ladder - climp down - improve head rotation
		else if ((BG_GetConditionValue(ci, ANIM_COND_MOVETYPE, 0) & (1<<ANIM_MT_CLIMBDOWN)) != 0) 
		{
			// Compensate the animation head rotation to neutral
			control->tag_neck_angles[0] = -30;
			control->tag_neck_angles[1] = -12;
			control->tag_neck_angles[2] = -9;

			control->tag_neck_angles[0] += fclamp(ci->playerAngles[0], -80, 70);
			control->tag_neck_angles[1] += AngleNormalize180(ci->playerAngles[1] - ci->torso.yawAngle);
			control->tag_neck_angles[2] += 0;

			vec3_t zeroVec = {0, 0, 0};
			animation_adjustRotation(zeroVec, control->tag_neck_angles, control->tag_neck_angles);

			animationPlayerData[dataIndex].controllerMovementType |= CONTROLLER_CLIMBDOWN;
		}


		// Fix grenade throw animation
		if (animation_isServer() && ci->legs.animation != nullptr) {

			// Playing stand grenade throw anim but player is in crouch or prone
			if (strcmp(ci->legs.animation->name, "pb_stand_grenade_throw") == 0 && ((es->eFlags & EF_PRONE) || (es->eFlags & EF_CROUCHING)))
			{
				playerState_t* ps = SV_GetPlayerStateByNum(es->clientNum);
				ps->legsTimer = 0;
			}
			if (strcmp(ci->legs.animation->name, "pb_crouch_grenade_throw") == 0 && (es->eFlags & EF_CROUCHING) == 0)
			{
				playerState_t* ps = SV_GetPlayerStateByNum(es->clientNum);
				ps->legsTimer = 0;
			}
		}


		// Fix transition between stances (stand, crouch, prone)
		{
			int stanceTransitionType = animationPlayerData[dataIndex].stanceTransitionType;
			int stanceTransitionTime = animationPlayerData[dataIndex].stanceTransitionTime;
			int stanceTransitionTimeEnd = animationPlayerData[dataIndex].stanceTransitionTimeEnd;
			
			// 0 = starting, 1 = done
			float stanceTransitionFraction = 0.0;
			if (stanceTransitionType > 0) 
				stanceTransitionFraction = 1 - ((float)(stanceTransitionTimeEnd - level_bgs->time) / (float)stanceTransitionTime);
	
			// Transition is in progress
			if (stanceTransitionFraction > 0.0 && stanceTransitionFraction < 1.0) {
				/**
				 * 0 - No transition
				 * 1 - Stand to Crouch
				 * 2 - Crouch to Prone
				 * 3 - Prone to Crouch
				 * 4 - Crouch to Stand
				 */
				switch (stanceTransitionType) {		
					// Crouch to Prone
					case 2: {
						float fraction1  = fclamp(((stanceTransitionFraction) / (1 - 0.4))  , 0, 1); // from start to the moment of touching the ground
						float fraction2  = fclamp(((stanceTransitionFraction - .4) / (1 - .4)), 0, 1); // from moment of touching the ground until the end of the crouch
				
						float curve1 = 0.5 * (1 - cos(2 * M_PI * fraction1));
						float curve2 = 0.5 * (1 - cos(2 * M_PI * fraction2));
						
						control->tag_origin_offset[0] += curve1 * -5; // move character backward a bit to center the head to 1st view
						control->tag_origin_offset[2] += curve2 * 12; // height bounce after touching the ground
						
						// Lift the head+body a bit to simulate the bounce off the ground (same as in 1st view)
						control->tag_origin_angles[0] += curve2 * -10; // rotation of whole character, pivot point is the origin
						control->tag_back_low_angles[0] += curve2 * 10; // rotation of BACK_LOW
						break;
					}
					// Prone to Crouch
					case 3: {
						float curve1 = 0.5 * (1 - cos(2 * M_PI * stanceTransitionFraction));			
						control->tag_origin_offset[0] += curve1 * -5; // move character backward a bit to center the head to 1st view
						break;
					}
				}
			}
		}
	}


	////////////////
	//	DEBUGGING
	////////////////

	int isServer = animation_isServer() ? 1 : 0;

	// Get player origin
	vec3_t viewOrigin;
	VectorCopy(es->pos.trBase, viewOrigin);
	viewOrigin[2] += 60;
	vec4_t crossPointColor = { 1, .5, .5, 1 };
	// Its client running the server, we can get exact position of the player
	if (COD2X_WIN32 && (player_debug->value.integer > 0 || player_debugEyePosition->value.boolean)) {
		
		if (sv_running->value.boolean) {
			gentity_t* ent = &g_entities[ci->clientNum];
			if (ent->client) {
				G_GetPlayerViewOrigin(ent, viewOrigin);
				Vector4Set(crossPointColor, 1, 1, 1, 1);
			}
		}

		/*Dvar_SetFloat(x, viewOrigin[0]);
		Dvar_SetFloat(y, viewOrigin[1]);
		Dvar_SetFloat(z, viewOrigin[2]);*/

		CL_AddDebugCrossPoint(viewOrigin, 3, crossPointColor, 1, 0, 0);
	}
	

	// Debugging for listen server
	if (COD2X_WIN32 && player_debug->value.integer > 0 && (isServer + 1) == player_debug->value.integer)
	{
		if (1) {
			vec4_t color = { 1, 1, 1, 1 };

			const char** strings = get_scriptAnimMoveTypeStrings(BG_GetConditionValue(ci, ANIM_COND_MOVETYPE, 0));
			// Join strings with new line
			char bufferMoveType[1024];
			bufferMoveType[0] = '\0';
			sprintf(bufferMoveType, "MOVETYPE 0x%x: ", BG_GetConditionValue(ci, ANIM_COND_MOVETYPE, 0));
			for (int i = 0; strings[i] != nullptr; ++i) {
				strcat(bufferMoveType, strings[i]);
				strcat(bufferMoveType, ".");
			}

			const char** strings2 = get_entityFlagStrings(es->eFlags);
			char bufferEFlags[1024];
			// Join strings with new line
			bufferEFlags[0] = '\0';
			strcat(bufferEFlags, "eFlags:");
			for (int i = 0; strings2[i] != nullptr; ++i) {
				strcat(bufferEFlags, strings2[i]);
				strcat(bufferEFlags, ".");
			}

			vec3_t forward;
			vec3_t right;
			AngleVectors(ci->playerAngles, forward, right, nullptr);
			
			vec3_t stringOrigin;

			VectorMA(viewOrigin, 25, right, stringOrigin);
			VectorMA(stringOrigin, 50, forward, stringOrigin);

			stringOrigin[2] -= 5;
			const char* strServerClient = (isServer ? "Server" : "Client");
			CL_AddDebugString(stringOrigin, color, .25, strServerClient, isServer);
			stringOrigin[2] -= 5;
			CL_AddDebugString(stringOrigin, color, .25, bufferEFlags, isServer);
			stringOrigin[2] -= 5;
			CL_AddDebugString(stringOrigin, color, .25, bufferMoveType, isServer);		
			stringOrigin[2] -= 10;
			CL_AddDebugString(stringOrigin, color, .25, ci->torso.animation ? va("%i %s (0x%x)", ci->torso.animationNumber, ci->torso.animation->name, level_bgs->animData.animScriptData.animations[es->torsoAnim & ~ANIM_TOGGLEBIT].flags) : "-no torso anim-", isServer);
			stringOrigin[2] -= 5;
			CL_AddDebugString(stringOrigin, color, .25, ci->legs.animation ? va("%i %s (0x%x)", ci->legs.animationNumber, ci->legs.animation->name, level_bgs->animData.animScriptData.animations[es->legsAnim & ~ANIM_TOGGLEBIT].flags) : "-no torso anim-", isServer);

			if (sv_running->value.boolean) {
				playerState_t* ps = SV_GetPlayerStateByNum(es->clientNum);

				stringOrigin[2] -= 10;
				CL_AddDebugString(stringOrigin, color, .25, va("torsoAnim:%i, torsoAnimDuration:%i, torsoTimer:%i", ps->torsoAnim, ps->torsoAnimDuration, ps->torsoTimer), isServer);
				
				stringOrigin[2] -= 5;
				CL_AddDebugString(stringOrigin, color, .25, va("legsAnim:%i, legsAnimDuration:%i, legsTimer:%i", ps->legsAnim, ps->legsAnimDuration, ps->legsTimer), isServer);	
			}

			VectorMA(viewOrigin, -50, right, stringOrigin);
			VectorMA(stringOrigin, 50, forward, stringOrigin);

			stringOrigin[2] -= 5;
			CL_AddDebugString(stringOrigin, color, .25, va("ci->movementYaw: %.2f", ci->movementYaw), isServer);
			stringOrigin[2] -= 5;
			CL_AddDebugString(stringOrigin, color, .25, va("torso yaw: %.2f", ci->torso.yawAngle), isServer);
			stringOrigin[2] -= 5;
			CL_AddDebugString(stringOrigin, color, .25, va("legs yaw:  %.2f", ci->legs.yawAngle), isServer);
			stringOrigin[2] -= 5;
			CL_AddDebugString(stringOrigin, color, .25, va("torso-legs dif: %.2f", AngleSubtract(ci->legs.yawAngle, ci->torso.yawAngle)), isServer);
			stringOrigin[2] -= 5;
			CL_AddDebugString(stringOrigin, color, .25, va("movementYawAngle: %.2f", movementYawAngle), isServer);
			stringOrigin[2] -= 5;
			CL_AddDebugString(stringOrigin, color, .25, va("frameTime: %i", level_bgs->frametime), isServer);
			stringOrigin[2] -= 5;
			CL_AddDebugString(stringOrigin, color, .25, va("leanf: %.3f %.3f", es->leanf, fLeanFrac), isServer);
			stringOrigin[2] -= 5;
			CL_AddDebugString(stringOrigin, color, .25, va("fTorsoHeight: %.3f", es->fTorsoHeight), isServer);


			VectorMA(viewOrigin, -80, right, stringOrigin);

			stringOrigin[2] += 50;
	
			for (int i = 0; i < 8; ++i) {
				stringOrigin[2] -= 4;
				const char* angleNames[] = {
					"back_low ",
					"back_mid ",
					"back_up   ",
					"neck       ",
					"head       ",
					"pelvis     ",
					"origin     ",
					"offset     "
				};
				// Calculate distance between control->angles[i] and ci->control->angles[i]
				float angleDifference[3];
				AnglesSubtract(control->angles[i], ci->control.angles[i], angleDifference);
				float distance = VectorLength(angleDifference);
	
				vec4_t color;
				if (distance < 0.01) {
					Vector4Set(color, 0, 1, 0, 1);
				} else {
					Vector4Set(color, 1, 0, 0, 1);
				}
	
				CL_AddDebugString(stringOrigin, color, 0.3, va("%s: %03.3f %03.3f %03.3f -> %03.3f %03.3f %03.3f (%01.2f)", 
					angleNames[i], control->angles[i][0], control->angles[i][1], control->angles[i][2], ci->control.angles[i][0], ci->control.angles[i][1], ci->control.angles[i][2], distance), isServer);
			}
	
			stringOrigin[2] -= 6;

			vec4_t color2 = { 1, 1, 1, 1 };
			CL_AddDebugString(stringOrigin, color2, 0.3, va("player angles: %.3f %.3f %.3f", ci->playerAngles[0], ci->playerAngles[1], ci->playerAngles[2]), isServer);
		}
	}
}




#define controller_names (((uint16_t**)ADDR(0x005d8e18, 0x08184ca8)))
#define scr_const_tag_origin (*((uint16_t*)ADDR(0x019439c2, 0x088532c2)))

void DObjSetControlTagAngles(void *obj, int *partBits, unsigned int boneIndex, float *angles)
{
	ASM_CALL(RETURN_VOID, ADDR(0x00488f80, 0x080bbea4), WL(2, 4), WL(EBX, PUSH)(obj), PUSH(partBits), WL(EAX, PUSH)(boneIndex), PUSH(angles));
}

void DObjSetLocalTag(void *obj, int *partBits, unsigned int boneIndex, const float *trans, const float *angles)
{
	ASM_CALL(RETURN_VOID, ADDR(0x00488ff0, 0x080bbf22), WL(3, 5), WL(EBX, PUSH)(obj), PUSH(partBits), WL(EAX, PUSH)(boneIndex), PUSH(trans), PUSH(angles));
}

/**
 * Is called for both server and client
 * For server it might be called multiple times for the same player in one frame
 */
void BG_Player_DoControllers(void *obj, entityState_s *es, int* partBits, clientInfo_t *ci, int frametime)
{
	float maxOffsetChange;
	clientControllers_t controllers;
	float maxAngleChange;
	int i;

	memset(&controllers, 0, sizeof(clientControllers_t));


	BG_Player_DoControllersInternal(es, ci, &controllers);
	maxAngleChange = (float)frametime * 0.36;
	maxOffsetChange = (float)frametime * 0.1;


	// CoD2x: Slow down the lerp because the rotation is sum of body_low, body_mid and body_up
	// This was causing the weapon moving side to side when player movement changes
	// It is because of the legs turn angle - the tag_origin was lerping too slow compared to sum of the body angles
	if (g_cod2x->value.integer >= 3) {
		
		/*const char* t = animation_isServer() ? "Server" : "Client";
		int tt = animation_isServer() ? 2 : 5;
		Com_Printf("^%i %i: obj:%x es:%x ci:%x  %s, frametime:%i\n", tt, level_bgs->time, obj, es, ci, t, frametime);*/

		// New controller angles changeable via dvars
		if (player_offsetEnable->value.boolean) {
			VectorAddTo(controllers.tag_back_low_angles, 	player_offsetAngleBackLow->value.vec3);
			VectorAddTo(controllers.tag_back_mid_angles, 	player_offsetAngleBackMid->value.vec3);
			VectorAddTo(controllers.tag_back_up_angles, 	player_offsetAngleBackUp->value.vec3);
			VectorAddTo(controllers.tag_neck_angles, 		player_offsetAngleNeck->value.vec3);
			VectorAddTo(controllers.tag_head_angles, 		player_offsetAngleHead->value.vec3);
			VectorAddTo(controllers.tag_pelvis_angles, 		player_offsetAnglePelvis->value.vec3);
			VectorAddTo(controllers.tag_origin_offset, 		player_offsetPositionOrigin->value.vec3);
			VectorAddTo(controllers.tag_origin_angles, 		player_offsetAngleOrigin->value.vec3);
		}

		int dataIndex = animation_getIndexForPlayerData(ci->clientNum);
		animationPlayerData_s* data = &animationPlayerData[dataIndex];

		// Movement type changed, save current angles and do transition
		if (data->controllerMovementType != data->controllerMovementTypeLast) {
			data->controllerMovementTypeLast = data->controllerMovementType;
			if (data->controllerMovementTime <= 0)
				data->controllerMovementTime = 300;
			data->controllerMovementEndTime = level_bgs->time + data->controllerMovementTime;
			data->controllerMovementIsRunning = true;

			memcpy(&data->controllerMovementStart, ci->control.angles, sizeof(data->controllerMovementStart));

			if (player_debug->value.integer > 0 && !animation_isServer()) {
				// Print to console
				Com_Printf("%i  Controller movement type: %i   %i\n", level_bgs->time, data->controllerMovementTypeLast, data->controllerMovementTime);
			}
		}

		float fractionRaw = 1 - (float)(data->controllerMovementEndTime - level_bgs->time) / (float)data->controllerMovementTime;
		float fraction = fclamp(fractionRaw, 0, 1);

		for (i = 0; i <= 7; ++i) {
			if (fractionRaw >= 0 && fractionRaw <= 1) {
				BG_LerpOverTime(data->controllerMovementStart.angles[i], controllers.angles[i], fraction, ci->control.angles[i]);

				if (player_debug->value.integer > 0 && i == 0 && !animation_isServer()) {
					//Com_Printf("^2.", fraction);
				}

			} else if (i == 7) { // tag_origin_offset
				BG_LerpOffset(controllers.tag_origin_offset, maxOffsetChange, ci->control.tag_origin_offset);
			} else {
				BG_LerpAngles(controllers.angles[i], maxAngleChange, ci->control.angles[i]);
			}
			if (i != 6 && i != 7) { // skip tag_origin_angles and tag_origin_offset
				DObjSetControlTagAngles(obj, partBits, *controller_names[i], ci->control.angles[i]);
			}
		}

		DObjSetLocalTag(obj, partBits, scr_const_tag_origin, ci->control.tag_origin_offset, ci->control.tag_origin_angles);

		if (data->controllerMovementIsRunning && fractionRaw > 1) {
			data->controllerMovementIsRunning = false;

			if (player_debug->value.integer > 0 && !animation_isServer())
				Com_Printf("%i  Controller movement finished\n", level_bgs->time);
		}
	}
	// CoD2x: end

	else {
		for ( i = 0; i < 6; ++i )
		{
			BG_LerpAngles(controllers.angles[i], maxAngleChange, ci->control.angles[i]);
			DObjSetControlTagAngles(obj, partBits, *controller_names[i], ci->control.angles[i]);
		}
		BG_LerpAngles(controllers.tag_origin_angles, maxAngleChange, ci->control.tag_origin_angles);	
		BG_LerpOffset(controllers.tag_origin_offset, maxOffsetChange, ci->control.tag_origin_offset);
	
		DObjSetLocalTag(obj, partBits, scr_const_tag_origin, ci->control.tag_origin_offset, ci->control.tag_origin_angles);
	}
}


// Is called for both client and server
void BG_Player_DoControllers_Win32(void* obj, int* partBits, clientInfo_t* ci, int32_t frametime) {
    // Get num in eax
    entityState_s* es;
    ASM( movr, es, "eax" );
    // Call our function
    BG_Player_DoControllers(obj, es, partBits, ci, frametime);
}

void BG_Player_DoControllers_Linux(void* obj, entityState_s* es, int* partBits, clientInfo_t* ci, int32_t frametime) {
    // Call our function
    BG_Player_DoControllers(obj, es, partBits, ci, frametime);
}




/** Function responsible for swinging into desired angle smoothly */
void BG_SwingAngles(float destination, float swingTolerance, float clampTolerance, float speed, float *angle, int *swinging) {
	ASM_CALL(RETURN_VOID, ADDR(0x004fa000, 0x080da7dc), WL(4, 6), 
		PUSH(destination), 
		PUSH(swingTolerance), 
		PUSH(clampTolerance), 
		PUSH(speed), 
		WL(ESI, PUSH)(angle), 
		WL(ECX, PUSH)(swinging));
}


/**
 * Function responsible for calculating the player angles of the torso and legs.
 */
void BG_PlayerAngles(const entityState_s *es, clientInfo_t *ci)
{
	vec3_t playerAngles;
	vec3_t torsoAngles;
	vec3_t legsAngles;
	float legsYawDirection; // +90 = left, 45=strafe-left, 0=forward, -45=right-strafe, -90 = right
	float swingTolerance;
	float clampTolerance;
	float destination;
	float swingSpeed;

	swingSpeed = bg_swingSpeed->value.decimal;

	// CoD2x: Set speed to 1 to perfectly match player angles with the character
	// It will remove incorrect (slowed) character movements
	if (g_cod2x->value.integer >= 3) {
		swingSpeed = 1.0;
	}
	// CoD2x: end


	legsYawDirection = ci->movementYaw; 

	
	VectorCopy(ci->playerAngles, playerAngles);
	playerAngles[1] = AngleNormalize360(playerAngles[1]);

	VectorClear(legsAngles);
	VectorClear(torsoAngles);

	if ( (es->eFlags & EF_TURRET_ACTIVE) != 0 )
	{
		ci->torso.yawing = 1;
		ci->torso.pitching = 1;
		ci->legs.yawing = 1;
	}
	else if ( (BG_GetConditionValue(ci, ANIM_COND_MOVETYPE, 0) & (1<<ANIM_MT_CLIMBUP | 1<<ANIM_MT_CLIMBDOWN)) != 0 )
	{
		ci->torso.yawing = 1;
		ci->torso.pitching = 1;
		ci->legs.yawing = 1;
	}
	else if ( (es->eFlags & EF_MANTLE) != 0 )
	{
		ci->torso.yawing = 1;
		ci->torso.pitching = 1;
		ci->legs.yawing = 1;
	}
	else if ( (BG_GetConditionValue(ci, ANIM_COND_MOVETYPE, 0) & (1<<ANIM_MT_IDLE | 1<<ANIM_MT_IDLECR)) != 0 )
	{
		if ( BG_GetConditionValue(ci, ANIM_COND_FIRING, 1) )
		{
			ci->torso.yawing = 1;
			ci->torso.pitching = 1;
		}
	}
	else
	{
		ci->torso.yawing = 1;
		ci->torso.pitching = 1;
		ci->legs.yawing = 1;
	}



	// Legs angle point to direction of player movement
	legsAngles[1] = playerAngles[1] + legsYawDirection;

	/*
	 Torso swing YAW (left-right)
	*/
	if ( (es->eFlags & EF_DEAD) != 0 )
	{
		legsAngles[1] = playerAngles[1];
		torsoAngles[1] = playerAngles[1];
		clampTolerance = 90.0;
	}
	else if ( (BG_GetConditionValue(ci, ANIM_COND_MOVETYPE, 0) & (1<<ANIM_MT_CLIMBUP | 1<<ANIM_MT_CLIMBDOWN)) != 0 )
	{
		torsoAngles[1] = legsAngles[1];
		clampTolerance = 0.0;
	}
	else if ( (es->eFlags & EF_MANTLE) != 0 )
	{
		torsoAngles[1] = playerAngles[1];
		legsAngles[1] = playerAngles[1];
		torsoAngles[0] = 90.0;
		clampTolerance = 90.0;
	}
	else if ( (es->eFlags & EF_PRONE) != 0 )
	{
		torsoAngles[1] = playerAngles[1];
		clampTolerance = 90.0;
	}
	else if ( (es->eFlags & EF_FIRING) != 0 )
	{
		torsoAngles[1] = playerAngles[1];
		clampTolerance = 45.0;
	}
	else if ( (es->eFlags & EF_AIMDOWNSIGHT) != 0 ) 
	{
		torsoAngles[1] = playerAngles[1];
		clampTolerance = 90.0;
	}
	// CoD2x: Remove the (legsYawDirection * 0.3) offset in the else statement below to make weapon pointing straight when moving side to side
	else if (g_cod2x->value.integer >= 3)
	{
		torsoAngles[1] = playerAngles[1];
		clampTolerance = 90.0;
	}
	// CoD2x: end
	else 
	{
		torsoAngles[1] = legsYawDirection * 0.3 + playerAngles[1];
		clampTolerance = 90.0;
	}

	BG_SwingAngles(torsoAngles[1], 0.0, clampTolerance, swingSpeed, &ci->torso.yawAngle, &ci->torso.yawing);


	/*
	 Legs swing YAW (left-right)
	*/
	clampTolerance = 150.0;
	if ( (es->eFlags & EF_DEAD) != 0 )
	{
		swingTolerance = 0.0;
	}
	else if ( (es->eFlags & EF_PRONE) != 0 )
	{
		ci->legs.yawing = 0;
		ci->legs.yawAngle = playerAngles[1] + legsYawDirection; // set to disable swing
		
		// Disabled since 1.4.4.1
		// CoD2x: Ignore legs yaw direction to make legs aligned exacly according to player angles.
		// Previously, the legs were "sticked" to the ground to max 90 degrees
		if (g_cod2x->value.integer >= 3 && g_cod2x->value.integer < 4) {
			ci->legs.yawAngle = playerAngles[1];
		}
		// CoD2x: end
		
		legsAngles[1] = ci->legs.yawAngle; // set angles the same so the swing is not applied
		swingTolerance = 1.0;
	}
	// Going left or right directly
	else if ( (level_bgs->animData.animScriptData.animations[es->legsAnim & ~ANIM_TOGGLEBIT].flags & 0x30) != 0 )
	{
		ci->legs.yawing = 0;
		legsAngles[1] = playerAngles[1];
		swingTolerance = 0.0;
	}
	else if ( ci->legs.yawing )
	{
		swingTolerance = 0.0;
	} 
	else 
	{
		swingTolerance = 40.0;

		// CoD2x: Disable the swing tolerance to rotate the player character imidietly when standing
		if (g_cod2x->value.integer >= 3) {
			swingTolerance = 0.0;
		}
		// CoD2x: end
	}

	BG_SwingAngles( legsAngles[1], swingTolerance, clampTolerance, swingSpeed, &ci->legs.yawAngle, &ci->legs.yawing);



	/*
	  Override yaw angles for MG and climbing
	 */
	if ( (es->eFlags & EF_TURRET_ACTIVE) != 0 )
	{
		ci->torso.yawAngle = playerAngles[1];
		ci->legs.yawAngle = playerAngles[1];
	}
	else if ( (BG_GetConditionValue(ci, ANIM_COND_MOVETYPE, 0) & (1<<ANIM_MT_CLIMBUP | 1<<ANIM_MT_CLIMBDOWN)) != 0 )
	{
		ci->torso.yawAngle = playerAngles[1] + legsYawDirection;
		ci->legs.yawAngle = playerAngles[1] + legsYawDirection;
	}


	/*
	 Torso swing PITCH (up-down)
	*/
	if ( (es->eFlags & EF_DEAD) != 0 )
	{
		destination = 0.0;
	}
	else if ( (es->eFlags & EF_TURRET_ACTIVE) != 0 )
	{
		destination = 0.0;
	}
	else if ( (BG_GetConditionValue(ci, ANIM_COND_MOVETYPE, 0) & (1<<ANIM_MT_CLIMBUP | 1<<ANIM_MT_CLIMBDOWN)) != 0 )
	{
		destination = 0.0;
	}
	else if ( es->eFlags == EF_MANTLE )
	{
		destination = 0.0;
	}
	// CoD2x: Remove the * 0.6 to make the player up-down look match the 1st view (disabled)
	/*else if (g_cod2x->value.integer >= 3) 
	{
		destination = AngleNormalize180(playerAngles[0]);
	}*/
	// CoD2x: end
	else
	{
		destination = AngleNormalize180(playerAngles[0]) * 0.6;
	}

	swingSpeed = 0.15;

	// CoD2x: Set speed to 1 to perfectly match player angles with the character
	// It will remove incorrect (slowed) character movements
	if (g_cod2x->value.integer >= 3) {
		swingSpeed = 1.0;
	}
	// CoD2x: end

	BG_SwingAngles(destination, 0.0, 45.0, swingSpeed, &ci->torso.pitchAngle, &ci->torso.pitching);
}


void BG_PlayerAngles_Win32(const entityState_s *es) {
    // Get num in eax
    clientInfo_t* ci;
    ASM( movr, ci, "edi" );

    // Call our function
    BG_PlayerAngles(es, ci);
}

void BG_PlayerAngles_Linux(const entityState_s *es, clientInfo_t *ci) {
    // Call our function
    BG_PlayerAngles(es, ci);
}






animation_s *BG_GetAnimationForIndex(int client, unsigned int index)
{
	if ( index >= (unsigned int)(globalScriptData->numAnimations) )
		Com_Error(ERR_DROP, "BG_GetAnimationForIndex: index out of bounds");

	return &globalScriptData->animations[index];
}

/** Checks if animation movetype is CROUCH. CoD2x added additional crouch movetypes */
int BG_IsCrouchingAnim(const clientInfo_t *ci, int animNum) // 0x004f9750, 0x080d9c02
{
	animation_s *index = BG_GetAnimationForIndex(ci->clientNum, animNum & ~ANIM_TOGGLEBIT);
	int64_t animTypes = 1<<ANIM_MT_IDLECR | 1<<ANIM_MT_WALKCR | 1<<ANIM_MT_WALKCRBK; // original
	// CoD2x: added additional crouch movetypes
	animTypes |= 1LL<<ANIM_MT_RUNCR | 1LL<<ANIM_MT_RUNCRBK | 1LL<<ANIM_MT_TURNRIGHTCR | 1LL<<ANIM_MT_TURNLEFTCR | 1LL<<ANIM_MT_STUMBLE_CROUCH_FORWARD | 1LL<<ANIM_MT_STUMBLE_CROUCH_BACKWARD; // CoD2x
	return ((int64_t)index->movetype & animTypes) != 0;
}

/** Checks if animation movetype is PRONE. Its the same as original function */
int BG_IsProneAnim(const clientInfo_t *ci, int animNum) // 0x004f97b0, 0x080d9c58
{
	animation_s *index = BG_GetAnimationForIndex(ci->clientNum, animNum & ~ANIM_TOGGLEBIT);
	return ((int64_t)index->movetype & (1<<ANIM_MT_IDLEPRONE | 1<<ANIM_MT_WALKPRONE | 1<<ANIM_MT_WALKPRONEBK)) != 0;
}


/**
 * Get the stance transition type based on the current and new animation numbers.
 * 0 - No transition
 * 1 - Stand to Crouch
 * 2 - Crouch to Prone
 * 3 - Prone to Crouch
 * 4 - Crouch to Stand
 */
int animation_getStanceTransitionType(const clientInfo_t *ci, int animNumOld, int animNumNew) {

	int crouchAnimCurrent = BG_IsCrouchingAnim(ci, animNumOld);
	int crouchAnimNew = BG_IsCrouchingAnim(ci, animNumNew);

	int proneAnimCurrent = BG_IsProneAnim(ci, animNumOld);
	int proneAnimNew = BG_IsProneAnim(ci, animNumNew);

	int standAnimCurrent = !crouchAnimCurrent && !proneAnimCurrent;
	int standAnimNew = !crouchAnimNew && !proneAnimNew;

	//Com_Printf("crouch: %i -> %i, prone: %i -> %i\n", 
	//	crouchAnimCurrent, crouchAnimNew, proneAnimCurrent, proneAnimNew);

	if (standAnimCurrent && crouchAnimNew) {
		return 1; // Stand to Crouch
	} else if (crouchAnimCurrent && proneAnimNew) {
		return 2; // Crouch to Prone
	} else if (proneAnimCurrent && crouchAnimNew) {
		return 3; // Prone to Crouch
	} else if (crouchAnimCurrent && standAnimNew) {
		return 4; // Crouch to Stand
	} else {
		return 0; // No transition
	}
}



/** 
 * Is called when the torso or leg animation number changed (via some event). 
 * CoD2x only modify the lf->animationTime to fix the transition time between stand/crouch/prone and then call the original function
 */
void BG_SetNewAnimation(lerpFrame_t* lf, clientInfo_t* ci, int iAnimNumNew, int32_t isComplete) {
	int transitionMin;  
	animation_s *pAnimCurrent;
	animation_s *pAnimNew;	
	int iAnimIndexNew;	
    int iAnimNumCurrent;	
	
	pAnimCurrent = lf->animation;
	iAnimNumCurrent = lf->animationNumber;
	iAnimIndexNew = iAnimNumNew & ~ANIM_TOGGLEBIT;
	
    //Com_Printf("BG_SetNewAnimation: %i -> %i, time: %i\n", iAnimNumCurrent, iAnimNumNew, level_bgs->time);
    
	if (iAnimIndexNew >= level_bgs->animData.animScriptData.numAnimations)
	{
		return;
	}

	// Set the new animation
	if (iAnimIndexNew > 0)
	{
		pAnimNew = &level_bgs->animData.animScriptData.animations[iAnimIndexNew];
		lf->animationTime = pAnimNew->initialLerp;
	}
	else
	{
		pAnimNew = NULL;
		lf->animationTime = 200;
	}

	// First leg animation
	if ( pAnimCurrent == NULL && lf == &ci->legs )
	{
		lf->animationTime = 0;
	}
	else
	{
        transitionMin = -1;

		if ( !pAnimNew || lf->animationTime <= 0 )
		{
			if ( !pAnimNew || pAnimNew->moveSpeed == 0.0 )
			{
				if ( !pAnimCurrent || pAnimCurrent->moveSpeed == 0.0 )
					transitionMin = 170;
				else
					transitionMin = 250;
			}
			else
			{
				transitionMin = 120;
			}
		}

		// CoD2x: Save the transition type and the time needed for that
		int transitionType = 0;
		if (lf == &ci->legs && iAnimIndexNew > 0) {
			transitionType = animation_getStanceTransitionType(ci, iAnimNumCurrent, iAnimIndexNew);
		}
		if (transitionType > 0) {

			// This function is called twice for listen server, so we need to save data for both client and server
			int dataIndex = animation_getIndexForPlayerData(ci->clientNum);

			switch (transitionType) {
				case 1: // "Stand to Crouch";
					lf->animationTime = PLAYER_CROUCH_TIME;
					animationPlayerData[dataIndex].stanceTransitionTime = PLAYER_CROUCH_TIME;
					animationPlayerData[dataIndex].stanceTransitionTimeEnd = level_bgs->time + PLAYER_CROUCH_TIME;
					break;
				case 2: // "Crouch to Prone";
					lf->animationTime = (PLAYER_PRONE_TIME * 0.58); // in 1st view the drop is faster, so the transition time must be faster to match the animation
					animationPlayerData[dataIndex].stanceTransitionTime = PLAYER_PRONE_TIME;
					animationPlayerData[dataIndex].stanceTransitionTimeEnd = level_bgs->time + PLAYER_PRONE_TIME;
					break;
				case 3: // "Prone to Crouch";
					lf->animationTime = PLAYER_PRONE_TIME;
					animationPlayerData[dataIndex].stanceTransitionTime = PLAYER_PRONE_TIME;
					animationPlayerData[dataIndex].stanceTransitionTimeEnd = level_bgs->time + PLAYER_PRONE_TIME;
					break;
				case 4: // "Crouch to Stand";
					lf->animationTime = PLAYER_CROUCH_TIME;
					animationPlayerData[dataIndex].stanceTransitionTime = PLAYER_CROUCH_TIME;
					animationPlayerData[dataIndex].stanceTransitionTimeEnd = level_bgs->time + PLAYER_CROUCH_TIME;
					break;
			}
			animationPlayerData[dataIndex].stanceTransitionType = transitionType;

			/*const char* transitionMessages[] = {
				"-",
				"Stand to Crouch",
				"Crouch to Prone",
				"Prone to Crouch",
				"Crouch to Stand"
			};
			const char* transitionMessage = transitionMessages[transitionType];*/

			//Com_Printf("%i  i: %i, duration: %i, moveSpeed: %f, lerp: %i, name: %s, movetype: %llx, flags: %i, %s\n", 
			//   level_bgs->time, ci->clientNum, pAnimNew->duration, pAnimNew->moveSpeed, pAnimNew->initialLerp, pAnimNew->name, (long long)pAnimNew->movetype, pAnimNew->flags, transitionMessage);

			//Com_Printf("%i  server: %i, client:%i, animNum: %i -> %i, movetype: %llx, flags: %i, ^3%s^7  name: %s\n", 
			//   level_bgs->time, &(*level_bgs) == &level_bgs_server, ci->clientNum, lf->animationNumber, iAnimNumNew, pAnimNew->movetype, pAnimNew->flags, transitionMessage, pAnimNew->name);
		}
		// Always set min transition time
		else if ( lf->animationTime < transitionMin )
			lf->animationTime = transitionMin;
	}
}

void BG_SetNewAnimation_Win32(clientInfo_t* ci, int iAnimNumNew, int32_t isComplete) {
    lerpFrame_t* lf;
    ASM( movr, lf, "ebx" );

    // Call our function
    BG_SetNewAnimation(lf, ci, iAnimNumNew, isComplete);

    // Call the original function
	ASM_CALL(RETURN_VOID, 0x004f9850, 3, EBX(lf), PUSH(ci), PUSH(iAnimNumNew), PUSH(isComplete));
}

void BG_SetNewAnimation_Linux(clientInfo_t* ci, lerpFrame_t* lf, int iAnimNumNew, int32_t isComplete) {
    // Call our function
    BG_SetNewAnimation(lf, ci, iAnimNumNew, isComplete);

    // Call the original function
	ASM_CALL(RETURN_VOID, 0x080d9d00, 4, PUSH(ci), PUSH(lf), PUSH(iAnimNumNew), PUSH(isComplete));
}




/** 
 * Function responsible for setting player movement.
 * The variable ps->movementYaw is then transfered thru es->angles[2] to ci->movementYaw
 */
void PM_SetMovementDir(pmove_t *pm, pml_t *pml)
{
	float dir[3];
	float moveyaw = 0;
	float moved[3];
	float speed = 0;
	playerState_t *ps;

	ps = pm->ps;

	if ( (ps->pm_flags & PMF_PRONE) == 0 || (ps->eFlags & EF_TURRET_ACTIVE) != 0 )
	{
		// This part of original logic is never called because there is another function for ladder
		/*if ( (ps->pm_flags & PMF_LADDER) != 0 )
		{
			float ladderYaw = VecToYaw(ps->vLadderVec) + 180.0;
			moveyaw = AngleDelta(ladderYaw, ps->viewangles[1]);
			moveyaw = fclamp(moveyaw, -90, 90);
		}
		else
		{*/
			moved[0] = ps->origin[0] - pml->previous_origin[0];
			moved[1] = ps->origin[1] - pml->previous_origin[1];
			moved[2] = ps->origin[2] - pml->previous_origin[2];

			speed = VectorLength(moved);
			speed = speed / (float)pml->frametime; // Normalize the speed according to frametime

			if ((pm->cmd.forwardmove | pm->cmd.rightmove) && ps->groundEntityNum != 1023 && speed > 5.0)
			{
				Vec3NormalizeTo(moved, dir);
				VecToAngles(dir, dir);
				moveyaw = AngleDelta(dir[1], ps->viewangles[1]);

				bool isMovingBackwards = pm->cmd.forwardmove < 0;

				// CoD2x: Add angle check to avoid situation when user is pressing BACK arrow but the character is actually not moving backward
				if (g_cod2x->value.integer >= 3) 
					isMovingBackwards = isMovingBackwards && (moveyaw > 90.0f || moveyaw < -90.0f);
				// CoD2x: end

				if (isMovingBackwards)
					moveyaw = AngleNormalize180((float)moveyaw + 180.0);
				
				moveyaw = fclamp(moveyaw, -90, 90);
			}
		//}
	}
	else
	{
		moveyaw = AngleDelta(ps->proneDirection, ps->viewangles[1]);
		moveyaw = fclamp(moveyaw, -90, 90);
	}

	ps->movementYaw = (int)moveyaw;
}

void PM_SetMovementDir_Win32() {
	// Get num in eax
	pmove_t *pm;
	pml_t *pml;
	ASM( movr, pm, "eax" );
	ASM( movr, pml, "ecx" );
	// Call our function
	PM_SetMovementDir(pm, pml);
}






bool animation_fixApplied = 0;
void* animation_originalPointer;
byte* animation_originalData[8][7];

/**
 * Called when g_cod2x dvar changes. We need to be able to restore original functionalities when we connect original 1.3 server
 */
void animation_changeFix(int enable) {
	if (enable) {

		if (animation_fixApplied)
			return;

		animation_originalPointer = patch_call(ADDR(0x004f9dd1, 0x080da4cd), (unsigned int)ADDR(BG_SetNewAnimation_Win32, BG_SetNewAnimation_Linux));

		// Remove write to lf->animationTime, as it is handled in our function BG_SetNewAnimation
		patch_nop(ADDR(0x004f9991, 0x080d9e9c), 7, animation_originalData[0]);   
		patch_nop(ADDR(0x004f98ff, 0x080d9df4), 3, animation_originalData[1]);   
		patch_nop(ADDR(0x004f9a07, 0x080d9f56), 3, animation_originalData[2]);
		patch_nop(ADDR(0x004f997e, 0x080d9eb2), 7, animation_originalData[3]);

		// Disable playing stance transition animations defined in playeranim.script in IWD files
		patch_nop(ADDR(0x00517e60, 0x080e4cab), 5, animation_originalData[4]); // ANIM_ET_PRONE_TO_STAND
		patch_nop(ADDR(0x00517f89, 0x080e4e4f), 5, animation_originalData[5]); // ANIM_ET_CROUCH_TO_STAND
		patch_nop(ADDR(0x00517db1, 0x080e4bde), 5, animation_originalData[6]); // ANIM_ET_STAND_TO_CROUCH
		patch_nop(ADDR(0x00517d16, 0x080e4b65), 5, animation_originalData[7]); // ANIM_ET_PRONE_TO_CROUCH

		animation_fixApplied = true;
	} 
	else 
	{
		if (!animation_fixApplied)
			return;
		
		// Restore the original code
		patch_call(ADDR(0x004f9dd1, 0x080da4cd), (unsigned int)animation_originalPointer);

		// Restore the original code
		patch_copy(ADDR(0x004f9991, 0x080d9e9c), animation_originalData[0], 7);   
		patch_copy(ADDR(0x004f98ff, 0x080d9df4), animation_originalData[1], 3);   
		patch_copy(ADDR(0x004f9a07, 0x080d9f56), animation_originalData[2], 3);
		patch_copy(ADDR(0x004f997e, 0x080d9eb2), animation_originalData[3], 7);

		// Restore the original code
		patch_copy(ADDR(0x00517e60, 0x080e4cab), animation_originalData[4], 5); // ANIM_ET_PRONE_TO_STAND
		patch_copy(ADDR(0x00517f89, 0x080e4e4f), animation_originalData[5], 5); // ANIM_ET_CROUCH_TO_STAND
		patch_copy(ADDR(0x00517db1, 0x080e4bde), animation_originalData[6], 5); // ANIM_ET_STAND_TO_CROUCH
		patch_copy(ADDR(0x00517d16, 0x080e4b65), animation_originalData[7], 5); // ANIM_ET_PRONE_TO_CROUCH

		animation_fixApplied = false;
	}
}



/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void animation_init() {
	
	player_debug = 					Dvar_RegisterInt("player_debug",					0, 0, 2, 					(enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));
	player_debugEyePosition = 		Dvar_RegisterBool("player_debugEyePosition",		false, 						(enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));
	player_offsetEnable = 			Dvar_RegisterBool("player_offsetEnable", 			false, 						(enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));
	player_offsetNeutral = 			Dvar_RegisterBool("player_offsetNeutral", 			false, 						(enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));
	player_offsetAngleBackLow = 	Dvar_RegisterVec3("player_offsetAngleBackLow", 		0, 0, 0, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));
	player_offsetAngleBackMid = 	Dvar_RegisterVec3("player_offsetAngleBackMid", 		0, 0, 0, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));
	player_offsetAngleBackUp = 		Dvar_RegisterVec3("player_offsetAngleBackUp", 		0, 0, 0, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));
	player_offsetAngleNeck = 		Dvar_RegisterVec3("player_offsetAngleNeck", 		0, 0, 0, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));
	player_offsetAngleHead = 		Dvar_RegisterVec3("player_offsetAngleHead", 		0, 0, 0, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));
	player_offsetAnglePelvis = 		Dvar_RegisterVec3("player_offsetAnglePelvis", 		0, 0, 0, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));
	player_offsetPositionOrigin = 	Dvar_RegisterVec3("player_offsetPositionOrigin", 	0, 0, 0, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));
	player_offsetAngleOrigin = 		Dvar_RegisterVec3("player_offsetAngleOrigin", 		0, 0, 0, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));
	
	//f1 = Dvar_RegisterFloat("f1", 1, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
	//f2 = Dvar_RegisterFloat("f2", 1, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
	//f3 = Dvar_RegisterFloat("f3", 1, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));

	//i1 = Dvar_RegisterInt("i1", 0, -INT32_MIN, INT32_MAX, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
	//i2 = Dvar_RegisterInt("i2", 0, -INT32_MIN, INT32_MAX, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
	//i3 = Dvar_RegisterInt("i3", 0, -INT32_MIN, INT32_MAX, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));

	//x = Dvar_RegisterFloat("x", 1, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
	//y = Dvar_RegisterFloat("y", 1, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
	//z = Dvar_RegisterFloat("z", 1, -FLT_MAX, FLT_MAX, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
}



/** Called before the entry point is called. Used to patch the memory. */
void animation_patch() {

	// Hook PM_SetMovementDir
	patch_call(ADDR(0x005160c7, 0x080e259e), (unsigned int)ADDR(PM_SetMovementDir_Win32, PM_SetMovementDir)); // PM_AirMove
	patch_call(ADDR(0x00516440, 0x080e2a13), (unsigned int)ADDR(PM_SetMovementDir_Win32, PM_SetMovementDir)); // PM_WalkMove

	// Hook BG_PlayerAngles
    patch_call(ADDR(0x004fb159, 0x080dbdcf), (unsigned int)ADDR(BG_PlayerAngles_Win32, BG_PlayerAngles_Linux));

	// Hook BG_Player_DoControllers
    patch_call(ADDR(0x00500d2e, 0x080f72f1), (unsigned int)ADDR(BG_Player_DoControllers_Win32, BG_Player_DoControllers_Linux)); // server side in G_PlayerController
    #if COD2X_WIN32
		patch_call(0x004cdcc7, (unsigned int)BG_Player_DoControllers_Win32); // client side in CG_PLayer_DoControllers
	#endif
}