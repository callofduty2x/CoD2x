// Standalone test of the capsule geometry, no game needed.
// Build with any C++ compiler: g++ -I../../src/shared capsule_trace_test.cpp ../../src/shared/capsule_trace.cpp -o capsule_trace_test
//
// Builds a standing skeleton facing +x (left is +y, up is +z) and fires rays at it from the front.

#include "capsule_trace.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static capsuleBone_t skeleton[CAP_BONE_COUNT];

static void set_bone(int index, float x, float y, float z)
{
	// Identity rotation: a weight of 2 is what the game stores for a unit quaternion
	const float quat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	const float origin[3] = { x, y, z };
	capsule_bone_set(&skeleton[index], quat, origin, 2.0f, (unsigned short)(index + 100));
}

static void build_skeleton()
{
	memset(skeleton, 0, sizeof(skeleton));
	set_bone(CAP_J_SPINE1, 0, 0, 38);
	set_bone(CAP_J_SPINE2, 0, 0, 43);
	set_bone(CAP_J_SPINE4, 0, 0, 52);
	set_bone(CAP_J_NECK, 0, 0, 55);
	set_bone(CAP_J_HEAD, 0, 0, 58);
	// The real head bone: x axis up, y axis to the face (+x here)
	const float headAxis[3][3] = { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } };
	memcpy(skeleton[CAP_J_HEAD].axis, headAxis, sizeof(headAxis));
	set_bone(CAP_J_HIP_LE, 0, 4, 36);
	set_bone(CAP_J_HIP_RI, 0, -4, 36);
	set_bone(CAP_J_CLAVICLE_LE, 0, 2, 55);
	set_bone(CAP_J_CLAVICLE_RI, 0, -2, 55);
	set_bone(CAP_J_SHOULDER_LE, 0, 8, 54);
	set_bone(CAP_J_SHOULDER_RI, 0, -8, 54);
	set_bone(CAP_J_ELBOW_LE, 0, 16, 44);
	set_bone(CAP_J_ELBOW_RI, 0, -16, 44);
	set_bone(CAP_J_WRIST_LE, 0, 22, 34);
	set_bone(CAP_J_WRIST_RI, 0, -22, 34);
	set_bone(CAP_J_MID_LE_1, 0, 24, 30);
	set_bone(CAP_J_MID_RI_1, 0, -24, 30);
	set_bone(CAP_J_KNEE_LE, 0, 7, 20);
	set_bone(CAP_J_KNEE_RI, 0, -7, 20);
	set_bone(CAP_J_ANKLE_LE, 0, 7, 5);
	set_bone(CAP_J_ANKLE_RI, 0, -7, 5);
	set_bone(CAP_J_BALL_LE, 3, 7, 2);
	set_bone(CAP_J_BALL_RI, 3, -7, 2);
}

// Fires along -x from x=100, returns the hit location or 0
static int shoot(float y, float z, capsuleTraceResult_t* out, const unsigned char* priorityMap = NULL, float scale = 1.0f)
{
	const float start[3] = { 100, y, z };
	const float end[3] = { -100, y, z };
	memset(out, 0, sizeof(*out));
	out->fraction = 1.0f;
	if (!capsule_trace(skeleton, start, end, priorityMap, scale, out))
		return -1;
	return out->partGroup;
}

static void expect_hit(const char* what, float y, float z, int hit)
{
	capsuleTraceResult_t r;
	int got = shoot(y, z, &r);
	bool ok = got == hit;
	if (ok && hit != 0)
	{
		ok = r.fraction > 0.0f && r.fraction < 1.0f && fabsf(sqrtf(r.normal[0] * r.normal[0] + r.normal[1] * r.normal[1] + r.normal[2] * r.normal[2]) - 1.0f) < 0.001f && r.partName != 0;
	}
	printf("%s %-28s y=%6.1f z=%6.1f -> location %d (expected %d), fraction %.3f\n", ok ? "ok  " : "FAIL", what, y, z, got, hit, r.fraction);
	if (!ok)
		failures++;
}

static void check(const char* what, bool ok)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

int main()
{
	build_skeleton();
	check("is a player skeleton", capsule_is_player_skeleton(skeleton));

	expect_hit("head", 0, 65, 2);
	expect_hit("top of the head", 0, 67.5f, 2);
	expect_hit("above the head", 0, 69, 0);
	expect_hit("chest", 0, 48, 4);
	expect_hit("belly", 0, 39, 5);
	expect_hit("hips, off to the side", 7, 36, 5);
	expect_hit("left upper arm", 12, 49, 7);
	expect_hit("right upper arm", -12, 49, 6);
	expect_hit("left forearm", 19, 39, 9);
	expect_hit("right forearm", -19, 39, 8);
	expect_hit("left hand", 23, 32, 11);
	expect_hit("right hand", -23, 32, 10);
	expect_hit("groin", 0, 31, 5);
	expect_hit("left thigh", 7, 26, 13);
	expect_hit("right thigh", -7, 26, 12);
	expect_hit("left shin", 7, 12, 15);
	expect_hit("right shin", -7, 12, 14);
	expect_hit("left foot", 7, 2, 17);
	expect_hit("right foot", -7, 2, 16);
	expect_hit("miss to the left", 40, 40, 0);
	expect_hit("gap between the legs", 0, 12, 0);
	expect_hit("gap between the chest and the arm", 10.5f, 42, 0);

	capsuleTraceResult_t r;

	// The neck is mostly behind the head and chest, it is hit first only with a higher priority
	{
		unsigned char priority[19];
		memset(priority, 2, sizeof(priority));
		priority[3] = 3;
		shoot(0, 57, &r, priority);
		check("neck", r.partGroup == 3);
	}

	// From the side, through the upper back
	{
		const float start[3] = { -5, 100, 50 };
		const float end[3] = { -5, -100, 50 };
		capsuleTraceResult_t back;
		memset(&back, 0, sizeof(back));
		back.fraction = 1.0f;
		capsule_trace(skeleton, start, end, NULL, 1.0f, &back);
		check("upper back is hit from the side", back.partGroup == 4);
	}

	// Priority: the arm is in front of the chest, but the chest wins when it has the higher priority
	{
		build_skeleton();
		set_bone(CAP_J_ELBOW_LE, 40, 0, 48);
		unsigned char priority[19];
		memset(priority, 2, sizeof(priority));
		priority[4] = 3;
		shoot(0, 48, &r, priority);
		check("higher priority wins over a closer hit", r.partGroup == 4);
		memset(priority, 2, sizeof(priority));
		shoot(0, 48, &r, priority);
		check("closest wins with equal priority", r.partGroup == 7);
		memset(priority, 1, sizeof(priority));
		shoot(0, 48, &r, priority);
		check("priority 1 and below can not be hit", r.partGroup == 0 && r.fraction == 1.0f);
		build_skeleton();
	}

	// A wall in front of the player
	{
		const float start[3] = { 100, 0, 48 };
		const float end[3] = { -100, 0, 48 };
		memset(&r, 0, sizeof(r));
		r.fraction = 0.1f;
		capsule_trace(skeleton, start, end, NULL, 1.0f, &r);
		check("nothing past the given fraction", r.partGroup == 0 && r.fraction == 0.1f);
	}

	// Starting inside of the body
	{
		const float start[3] = { 0, 0, 48 };
		const float end[3] = { 100, 0, 48 };
		memset(&r, 0, sizeof(r));
		r.fraction = 1.0f;
		capsule_trace(skeleton, start, end, NULL, 1.0f, &r);
		check("started inside hits at fraction 0", r.partGroup == 4 && r.fraction == 0.0f);
	}

	// Shooting away from the body must not hit it
	{
		const float start[3] = { 50, 0, 48 };
		const float end[3] = { 150, 0, 48 };
		memset(&r, 0, sizeof(r));
		r.fraction = 1.0f;
		capsule_trace(skeleton, start, end, NULL, 1.0f, &r);
		check("shooting away from the body misses", r.partGroup == 0);
	}

	// The scale cvar
	{
		shoot(0, 70, &r, NULL, 1.0f);
		bool notHitAtNormalSize = r.partGroup == 0;
		shoot(0, 70, &r, NULL, 2.0f);
		check("a bigger scale reaches further", notHitAtNormalSize && r.partGroup == 2);
	}

	// Missing bones skip only the affected capsules, no bones at all falls back to the stock boxes
	{
		skeleton[CAP_J_BALL_LE].valid = false;
		check("still a player without a foot bone", capsule_is_player_skeleton(skeleton));
		shoot(0, 48, &r);
		check("chest is hit without a foot bone", r.partGroup == 4);

		memset(skeleton, 0, sizeof(skeleton));
		check("no bones is not a player", !capsule_is_player_skeleton(skeleton));
		check("no bones reports nothing tested", shoot(0, 48, &r) == -1);
		build_skeleton();
	}

	// Bone rotation is applied to the local offsets: turn the knee 90 degrees around z
	{
		const float s = sqrtf(0.5f);
		const float quat[4] = { 0.0f, 0.0f, s, s };
		const float origin[3] = { 0, 0, 0 };
		capsuleBone_t bone;
		capsule_bone_set(&bone, quat, origin, 2.0f, 1);
		check("quaternion gives a rotation matrix",
			fabsf(bone.axis[0][1] - 1.0f) < 0.001f && fabsf(bone.axis[1][0] + 1.0f) < 0.001f && fabsf(bone.axis[2][2] - 1.0f) < 0.001f);
	}

	// Every wireframe point lies on the surface of the capsule
	{
		struct wireCheck_t
		{
			const float* a;
			const float* b;
			float radius;
			float worst;
			static void line(void* context, const float* start, const float* end)
			{
				wireCheck_t* c = (wireCheck_t*)context;
				const float* points[2] = { start, end };
				for (int p = 0; p < 2; p++)
				{
					float ab[3] = { c->b[0] - c->a[0], c->b[1] - c->a[1], c->b[2] - c->a[2] };
					float ap[3] = { points[p][0] - c->a[0], points[p][1] - c->a[1], points[p][2] - c->a[2] };
					float t = (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / (ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]);
					t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
					float d[3] = { ap[0] - ab[0] * t, ap[1] - ab[1] * t, ap[2] - ab[2] * t };
					float error = fabsf(sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - c->radius);
					if (error > c->worst)
						c->worst = error;
				}
			}
		};

		const float a[3] = { 1, 2, 3 };
		const float b[3] = { 4, 9, 3 };
		wireCheck_t c = { a, b, 2.5f, 0.0f };
		int lines = capsule_wireframe(a, b, 2.5f, 2.5f, wireCheck_t::line, &c);
		check("wireframe has the expected number of lines", lines == 40);
		check("wireframe points are on the capsule surface", c.worst < 0.001f);

		const float upA[3] = { 0, 0, 0 };
		const float upB[3] = { 0, 0, 10 };
		wireCheck_t up = { upA, upB, 1.0f, 0.0f };
		capsule_wireframe(upA, upB, 1.0f, 1.0f, wireCheck_t::line, &up);
		check("wireframe works for a vertical capsule", up.worst < 0.001f);

		int shapes = 0;
		build_skeleton();
		capsule_for_each_shape(skeleton, 1.0f, [](void* context, const float*, const float*, float, float, int, unsigned short) { (*(int*)context)++; }, &shapes);
		check("shapes are reported for the skeleton", shapes > 5);
	}

	// Capsules fitted to a player model
	{
		build_skeleton();
		capsuleFit_t fit;
		memset(&fit, 0, sizeof(fit));

		capsule_fit_add_model(&fit, "not_a_player_model");
		check("an unknown model changes nothing", !fit.body && !fit.head && !fit.headgear);

		capsule_fit_add_model(&fit, "playerbody_russian_coat01");
		capsule_fit_add_model(&fit, "HEAD_US_RANGER_DAVID");
		capsule_fit_add_model(&fit, "helmet_us_ranger_generic");
		check("models are found by name, in any case", fit.body && fit.head && fit.headgear);

		int shared = 0, fitted = 0;
		capsuleShapeCallback_t count = [](void* context, const float*, const float*, float, float, int, unsigned short) { (*(int*)context)++; };
		capsule_for_each_shape(skeleton, 1.0f, count, &shared);
		capsule_for_each_shape(skeleton, 1.0f, count, &fitted, &fit);
		check("a long coat and a helmet add capsules", fitted > shared);

		// From the side, in front of the legs: only a long coat hangs there
		const float start[3] = { 9, 100, 22 };
		const float end[3] = { 9, -100, 22 };
		memset(&r, 0, sizeof(r));
		r.fraction = 1.0f;
		capsule_trace(skeleton, start, end, NULL, 1.0f, &r);
		check("no hit in front of the legs without a coat", r.partGroup == 0);

		memset(&r, 0, sizeof(r));
		r.fraction = 1.0f;
		capsule_trace(skeleton, start, end, NULL, 1.0f, &r, &fit);
		check("a long coat counts as the leg", r.partGroup == 13);
	}

	printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
	return failures ? 1 : 0;
}
