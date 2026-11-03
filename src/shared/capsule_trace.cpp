#include "capsule_trace.h"

#include <math.h>
#include <string.h>

// Hit locations, the indexes used by the game's hit location table
enum
{
	HIT_HEAD = 2, HIT_NECK = 3, HIT_TORSO_UPPER = 4, HIT_TORSO_LOWER = 5,
	HIT_R_ARM_UPPER = 6, HIT_L_ARM_UPPER = 7, HIT_R_ARM_LOWER = 8, HIT_L_ARM_LOWER = 9,
	HIT_R_HAND = 10, HIT_L_HAND = 11,
	HIT_R_LEG_UPPER = 12, HIT_L_LEG_UPPER = 13, HIT_R_LEG_LOWER = 14, HIT_L_LEG_LOWER = 15,
	HIT_R_FOOT = 16, HIT_L_FOOT = 17
};

const char* const capsule_bone_names[CAP_BONE_COUNT] =
{
	"J_Spine1", "J_Spine2", "J_Spine4",
	"J_Neck", "J_Head",
	"J_Hip_LE", "J_Hip_RI",
	"J_Clavicle_LE", "J_Clavicle_RI",
	"J_Shoulder_LE", "J_Shoulder_RI",
	"J_Elbow_LE", "J_Elbow_RI",
	"J_Wrist_LE", "J_Wrist_RI",
	"J_Mid_LE_1", "J_Mid_LE_2", "J_Mid_LE_3",
	"J_Mid_RI_1", "J_Mid_RI_2", "J_Mid_RI_3",
	"J_Thumb_LE_1", "J_Thumb_LE_2", "J_Thumb_LE_3",
	"J_Thumb_RI_1", "J_Thumb_RI_2", "J_Thumb_RI_3",
	"J_Index_LE_1", "J_Index_LE_2", "J_Index_LE_3",
	"J_Index_RI_1", "J_Index_RI_2", "J_Index_RI_3",
	"J_Ring_LE_1", "J_Ring_LE_2", "J_Ring_LE_3",
	"J_Ring_RI_1", "J_Ring_RI_2", "J_Ring_RI_3",
	"J_Pinky_LE_1", "J_Pinky_LE_2", "J_Pinky_LE_3",
	"J_Pinky_RI_1", "J_Pinky_RI_2", "J_Pinky_RI_3",
	"J_Knee_LE", "J_Knee_RI",
	"J_Ankle_LE", "J_Ankle_RI",
	"J_Ball_LE", "J_Ball_RI",
};

struct capsuleDef_t
{
	int boneA;
	int boneB;
	float radiusA;
	float radiusB;	// 0 means the same as radiusA
	int hit;
	bool extendEnd;	// Last finger joint: the bone ends before the fingertip does
};

static const capsuleDef_t capsule_defs[] =
{
	{ CAP_J_NECK, CAP_J_HEAD, 3.0f, 0.0f, HIT_NECK, false },
	{ CAP_J_HIP_LE, CAP_J_HIP_RI, 4.8f, 0.0f, HIT_TORSO_LOWER, false },
	{ CAP_J_CLAVICLE_LE, CAP_J_SHOULDER_LE, 2.0f, 0.0f, HIT_TORSO_UPPER, false },
	{ CAP_J_CLAVICLE_RI, CAP_J_SHOULDER_RI, 2.0f, 0.0f, HIT_TORSO_UPPER, false },

	{ CAP_J_SHOULDER_LE, CAP_J_ELBOW_LE, 2.6f, 2.5f, HIT_L_ARM_UPPER, false },
	{ CAP_J_ELBOW_LE, CAP_J_WRIST_LE, 2.3f, 1.9f, HIT_L_ARM_LOWER, false },
	{ CAP_J_WRIST_LE, CAP_J_MID_LE_1, 2.2f, 1.15f, HIT_L_HAND, false },
	{ CAP_J_THUMB_LE_1, CAP_J_THUMB_LE_2, 0.7f, 0.55f, HIT_L_HAND, false },
	{ CAP_J_THUMB_LE_2, CAP_J_THUMB_LE_3, 0.5f, 0.38f, HIT_L_HAND, true },
	{ CAP_J_INDEX_LE_1, CAP_J_INDEX_LE_2, 0.48f, 0.38f, HIT_L_HAND, false },
	{ CAP_J_INDEX_LE_2, CAP_J_INDEX_LE_3, 0.36f, 0.28f, HIT_L_HAND, true },
	{ CAP_J_MID_LE_1, CAP_J_MID_LE_2, 0.48f, 0.38f, HIT_L_HAND, false },
	{ CAP_J_MID_LE_2, CAP_J_MID_LE_3, 0.36f, 0.28f, HIT_L_HAND, true },
	{ CAP_J_RING_LE_1, CAP_J_RING_LE_2, 0.46f, 0.36f, HIT_L_HAND, false },
	{ CAP_J_RING_LE_2, CAP_J_RING_LE_3, 0.34f, 0.26f, HIT_L_HAND, true },
	{ CAP_J_PINKY_LE_1, CAP_J_PINKY_LE_2, 0.4f, 0.32f, HIT_L_HAND, false },
	{ CAP_J_PINKY_LE_2, CAP_J_PINKY_LE_3, 0.3f, 0.22f, HIT_L_HAND, true },

	{ CAP_J_SHOULDER_RI, CAP_J_ELBOW_RI, 2.6f, 2.5f, HIT_R_ARM_UPPER, false },
	{ CAP_J_ELBOW_RI, CAP_J_WRIST_RI, 2.3f, 1.9f, HIT_R_ARM_LOWER, false },
	{ CAP_J_WRIST_RI, CAP_J_MID_RI_1, 2.2f, 1.15f, HIT_R_HAND, false },
	{ CAP_J_THUMB_RI_1, CAP_J_THUMB_RI_2, 0.7f, 0.55f, HIT_R_HAND, false },
	{ CAP_J_THUMB_RI_2, CAP_J_THUMB_RI_3, 0.5f, 0.38f, HIT_R_HAND, true },
	{ CAP_J_INDEX_RI_1, CAP_J_INDEX_RI_2, 0.48f, 0.38f, HIT_R_HAND, false },
	{ CAP_J_INDEX_RI_2, CAP_J_INDEX_RI_3, 0.36f, 0.28f, HIT_R_HAND, true },
	{ CAP_J_MID_RI_1, CAP_J_MID_RI_2, 0.48f, 0.38f, HIT_R_HAND, false },
	{ CAP_J_MID_RI_2, CAP_J_MID_RI_3, 0.36f, 0.28f, HIT_R_HAND, true },
	{ CAP_J_RING_RI_1, CAP_J_RING_RI_2, 0.46f, 0.36f, HIT_R_HAND, false },
	{ CAP_J_RING_RI_2, CAP_J_RING_RI_3, 0.34f, 0.26f, HIT_R_HAND, true },
	{ CAP_J_PINKY_RI_1, CAP_J_PINKY_RI_2, 0.4f, 0.32f, HIT_R_HAND, false },
	{ CAP_J_PINKY_RI_2, CAP_J_PINKY_RI_3, 0.3f, 0.22f, HIT_R_HAND, true },

	{ CAP_J_HIP_LE, CAP_J_KNEE_LE, 5.0f, 4.3f, HIT_L_LEG_UPPER, false },
	{ CAP_J_KNEE_LE, CAP_J_ANKLE_LE, 4.0f, 2.9f, HIT_L_LEG_LOWER, false },
	{ CAP_J_ANKLE_LE, CAP_J_BALL_LE, 2.45f, 2.15f, HIT_L_FOOT, false },
	{ CAP_J_HIP_RI, CAP_J_KNEE_RI, 5.0f, 4.3f, HIT_R_LEG_UPPER, false },
	{ CAP_J_KNEE_RI, CAP_J_ANKLE_RI, 4.0f, 2.9f, HIT_R_LEG_LOWER, false },
	{ CAP_J_ANKLE_RI, CAP_J_BALL_RI, 2.45f, 2.15f, HIT_R_FOOT, false },
};

// The torso is a stack of wide capsules placed across the chest, from the belly up to the shoulders
struct torsoSlice_t
{
	float along;	// 0 at the bottom of the spine, 1 at the top
	float radius;
	float halfWidth;	// Plus the radius, kept narrower than the gap between the chest and the arms
	float forward;		// Towards the belly, negative towards the upper back
	int hit;
};

static const torsoSlice_t torso_slices[] =
{
	{ 0.04f, 6.0f, 2.0f, 0.8f, HIT_TORSO_LOWER },
	{ 0.28f, 6.0f, 1.8f, 0.7f, HIT_TORSO_UPPER },
	{ 0.52f, 6.3f, 2.1f, 0.2f, HIT_TORSO_UPPER },
	{ 0.76f, 6.6f, 2.2f, -1.1f, HIT_TORSO_UPPER },
	{ 0.90f, 5.4f, 3.6f, -1.2f, HIT_TORSO_UPPER },
};

// Between the shoulders and the neck, placed relative to the neck so it follows the head and not the arms
static const float TRAPEZIUS_ALONG = 0.8f;	// 0 at the top of the spine, 1 at the neck
static const float TRAPEZIUS_FORWARD = -1.4f;
static const float TRAPEZIUS_HALF_WIDTH = 3.0f;
static const float TRAPEZIUS_RADIUS = 3.2f;

// The head bone sits at the base of the skull, with its x axis pointing up and its y axis to the face.
// Fitted to the head and helmet models: one capsule for the skull up to the top of the helmet, one for the face.
static const capsuleHeadShape_t head_shapes[] =
{
	{ { 3.5f, 0.6f, 0.0f }, { 5.8f, 0.0f, 0.0f }, 4.3f, 4.3f },
	{ { 1.2f, 3.1f, 0.0f }, { 4.6f, 3.1f, 0.0f }, 3.0f, 2.8f },
};

static const float THIGH_START_OFFSET = 3.0f;
static const float GROIN_LENGTH = 4.0f;
static const float GROIN_RADIUS = 3.8f;
static const float CAP_MIN_LENGTH = 0.4f;
static const float CAP_NO_HIT = 1e30f;


static inline float cap_dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void cap_sub(const float* a, const float* b, float* out) { out[0] = a[0] - b[0]; out[1] = a[1] - b[1]; out[2] = a[2] - b[2]; }
static inline void cap_copy(const float* a, float* out) { out[0] = a[0]; out[1] = a[1]; out[2] = a[2]; }
static inline float cap_clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static inline void cap_add_scaled(float* out, const float* dir, float scale)
{
	out[0] += dir[0] * scale;
	out[1] += dir[1] * scale;
	out[2] += dir[2] * scale;
}

// Point given in the bone's local space to skeleton space
static void cap_bone_point(const capsuleBone_t* bone, const float* local, float* out)
{
	for (int i = 0; i < 3; i++)
		out[i] = local[0] * bone->axis[0][i] + local[1] * bone->axis[1][i] + local[2] * bone->axis[2][i] + bone->origin[i];
}

// Distance to the sphere surface along the ray, or CAP_NO_HIT
static float cap_ray_sphere(const float* origin, const float* dir, const float* center, float radius)
{
	float m[3];
	cap_sub(origin, center, m);
	float b = cap_dot(m, dir);
	float disc = b * b - (cap_dot(m, m) - radius * radius);
	if (disc < 0.0f)
		return CAP_NO_HIT;
	return -b - sqrtf(disc);
}

// Distance to a capsule of constant radius along the ray, negative if there is none in front
static float cap_ray_capsule(const float* origin, const float* dir, const float* a, const float* b, float radius)
{
	float ab[3], m[3];
	cap_sub(b, a, ab);
	cap_sub(origin, a, m);

	float aa = cap_dot(ab, ab);
	if (aa < 1e-8f)
	{
		float t = cap_ray_sphere(origin, dir, a, radius);
		return t == CAP_NO_HIT ? -1.0f : t;
	}

	float nd = cap_dot(ab, dir);
	float mn = cap_dot(ab, m);
	float dm = cap_dot(m, dir);
	float qa = aa - nd * nd;
	float qb = dm * aa - nd * mn;
	float qc = (cap_dot(m, m) * aa - mn * mn) - radius * radius * aa;
	float disc = qb * qb - qa * qc;
	if (disc < 0.0f)
		return -1.0f;

	const float* cap = NULL;
	if (qa > aa * 1e-6f)
	{
		float t = (-qb - sqrtf(disc)) / qa;
		float along = nd * t + mn;
		if (along > 0.0f && along < aa)
			return t;
		cap = along > 0.0f ? b : a;
	}
	else
	{
		cap = mn > 0.0f ? b : a;
	}

	float t = cap_ray_sphere(origin, dir, cap, radius);
	return t == CAP_NO_HIT ? -1.0f : t;
}

// Distance to a capsule whose radius changes from one end to the other, negative if there is none in front
static float cap_ray_tapered(const float* origin, const float* dir, const float* a, const float* b, float radiusA, float radiusB)
{
	float ab[3], m[3];
	cap_sub(b, a, ab);
	cap_sub(origin, a, m);

	float aa = cap_dot(ab, ab);
	if (aa < 1e-8f)
	{
		float t = cap_ray_sphere(origin, dir, a, radiusA);
		return t == CAP_NO_HIT ? -1.0f : t;
	}

	float nd = cap_dot(ab, dir);
	float mn = cap_dot(ab, m);
	float dm = cap_dot(m, dir);
	float mm = cap_dot(m, m);
	float dr = radiusB - radiusA;
	float e = nd * dr;
	float f = mn * dr + radiusA * aa;

	float qa = aa * aa - aa * nd * nd - e * e;
	float qb = 2.0f * aa * aa * dm - 2.0f * aa * mn * nd - 2.0f * f * e;
	float qc = mm * aa * aa - aa * mn * mn - f * f;
	if (qa < 0.0f)
	{
		qa = -qa;
		qb = -qb;
		qc = -qc;
	}

	if (qa > 1e-4f)
	{
		float disc = qb * qb - 4.0f * qa * qc;
		if (disc >= 0.0f)
		{
			float t = (-qb - sqrtf(disc)) / (2.0f * qa);
			float along = nd * t + mn;
			if (along > 0.0f && along < aa)
				return t;
		}
	}

	float tA = cap_ray_sphere(origin, dir, a, radiusA);
	float tB = cap_ray_sphere(origin, dir, b, radiusB);
	if (tA == CAP_NO_HIT) tA = -1.0f;
	if (tB == CAP_NO_HIT) tB = -1.0f;
	if (tA < 0.0f)
		return tB < 0.0f ? -1.0f : tB;
	if (tB < 0.0f)
		return tA;
	return tA < tB ? tA : tB;
}

// Closest point on the segment; returns the fraction along it
static float cap_closest_on_segment(const float* point, const float* a, const float* b, float* out)
{
	float ab[3], ap[3];
	cap_sub(b, a, ab);
	cap_sub(point, a, ap);
	float aa = cap_dot(ab, ab);
	float t = aa > 1e-8f ? cap_clamp01(cap_dot(ap, ab) / aa) : 0.0f;
	cap_copy(a, out);
	cap_add_scaled(out, ab, t);
	return t;
}

static void cap_normal(const float* point, const float* center, float* out)
{
	cap_sub(point, center, out);
	float len = sqrtf(cap_dot(out, out));
	if (len < 0.0001f)
	{
		out[0] = 0.0f;
		out[1] = 0.0f;
		out[2] = 1.0f;
		return;
	}
	out[0] /= len;
	out[1] /= len;
	out[2] /= len;
}


struct capsuleTraceState_t
{
	const float* start;
	float dir[3];
	float length;
	float limit;
	const unsigned char* priorityMap;

	float bestFraction;
	int bestPriority;
	float bestNormal[3];
	unsigned short bestPartName;
	int bestHit;
	bool tested;
};

// A higher priority hit location wins even if it is further along the ray, like in the stock trace
static void cap_test(void* context, const float* a, const float* b, float radiusA, float radiusB, int hit, unsigned short partName)
{
	capsuleTraceState_t* s = (capsuleTraceState_t*)context;
	s->tested = true;

	int priority = s->priorityMap ? s->priorityMap[hit] : 3;
	if (priority <= 1)
		return;

	float t;
	if (fabsf(radiusB - radiusA) < 0.02f)
		t = cap_ray_capsule(s->start, s->dir, a, b, radiusA);
	else
		t = cap_ray_tapered(s->start, s->dir, a, b, radiusA, radiusB);

	float fraction;
	float normal[3];
	float closest[3];
	if (t < 0.0f)
	{
		// No hit in front. The ray can still have started inside of the capsule.
		float along = cap_closest_on_segment(s->start, a, b, closest);
		float offset[3];
		cap_sub(s->start, closest, offset);
		if (sqrtf(cap_dot(offset, offset)) > radiusA + (radiusB - radiusA) * along)
			return;
		cap_normal(s->start, closest, normal);
		fraction = 0.0f;
	}
	else
	{
		fraction = t / s->length;
		if (fraction >= s->limit)
			return;
		float point[3];
		cap_copy(s->start, point);
		cap_add_scaled(point, s->dir, t);
		cap_closest_on_segment(point, a, b, closest);
		cap_normal(point, closest, normal);
	}

	if (priority < s->bestPriority)
		return;
	if (priority == s->bestPriority && fraction >= s->bestFraction)
		return;

	s->bestFraction = fraction;
	s->bestPriority = priority;
	s->bestHit = hit;
	s->bestPartName = partName;
	cap_copy(normal, s->bestNormal);
}

static void cap_torso_shapes(const capsuleBone_t* bones, const capsuleBodyFit_t* body, float scale, capsuleShapeCallback_t callback, void* context)
{
	const capsuleBone_t* lower = &bones[CAP_J_SPINE1];
	const capsuleBone_t* middle = &bones[CAP_J_SPINE2];
	const capsuleBone_t* upper = &bones[CAP_J_SPINE4];
	const capsuleBone_t* shoulderL = &bones[CAP_J_SHOULDER_LE];
	const capsuleBone_t* shoulderR = &bones[CAP_J_SHOULDER_RI];
	if (!lower->valid || !middle->valid || !upper->valid || !shoulderL->valid || !shoulderR->valid)
		return;

	float across[3];
	cap_sub(shoulderR->origin, shoulderL->origin, across);
	float width = sqrtf(cap_dot(across, across));
	if (width < CAP_MIN_LENGTH)
		return;
	across[0] /= width;
	across[1] /= width;
	across[2] /= width;

	// Slices lean towards the middle of the shoulders the closer they are to the top
	float shoulderOffset[3];
	for (int i = 0; i < 3; i++)
		shoulderOffset[i] = (shoulderR->origin[i] + shoulderL->origin[i]) * 0.5f - upper->origin[i];

	float spine[3], forward[3];
	cap_sub(upper->origin, lower->origin, spine);
	forward[0] = spine[1] * across[2] - spine[2] * across[1];
	forward[1] = spine[2] * across[0] - spine[0] * across[2];
	forward[2] = spine[0] * across[1] - spine[1] * across[0];
	float forwardLength = sqrtf(cap_dot(forward, forward));
	if (forwardLength > CAP_MIN_LENGTH)
	{
		forward[0] /= forwardLength;
		forward[1] /= forwardLength;
		forward[2] /= forwardLength;
	}
	else
	{
		forward[0] = forward[1] = forward[2] = 0.0f;
	}

	float toMiddle[3], toUpper[3];
	cap_sub(middle->origin, lower->origin, toMiddle);
	cap_sub(upper->origin, middle->origin, toUpper);
	float lengthLower = sqrtf(cap_dot(toMiddle, toMiddle));
	float lengthUpper = sqrtf(cap_dot(toUpper, toUpper));
	float total = lengthLower + lengthUpper;

	for (unsigned int i = 0; i < sizeof(torso_slices) / sizeof(torso_slices[0]); i++)
	{
		const torsoSlice_t* slice = &torso_slices[i];

		// Position along the curve of the spine
		float center[3];
		float distance = cap_clamp01(slice->along) * total;
		if (total < CAP_MIN_LENGTH)
		{
			cap_copy(lower->origin, center);
		}
		else if (distance < lengthLower || lengthUpper < CAP_MIN_LENGTH)
		{
			cap_copy(lower->origin, center);
			if (lengthLower > CAP_MIN_LENGTH)
				cap_add_scaled(center, toMiddle, cap_clamp01(distance / lengthLower));
		}
		else
		{
			cap_copy(middle->origin, center);
			cap_add_scaled(center, toUpper, (distance - lengthLower) / lengthUpper);
		}
		cap_add_scaled(center, shoulderOffset, slice->along);

		const float sliceForward = body ? body->slices[i].forward : slice->forward;
		const float sliceHalfWidth = body ? body->slices[i].halfWidth : slice->halfWidth;
		const float sliceRadius = body ? body->slices[i].radius : slice->radius;

		cap_add_scaled(center, forward, sliceForward * scale);

		float a[3], b[3];
		cap_copy(center, a);
		cap_copy(center, b);
		cap_add_scaled(a, across, -sliceHalfWidth * scale);
		cap_add_scaled(b, across, sliceHalfWidth * scale);

		float radius = sliceRadius * scale;
		callback(context, a, b, radius, radius, slice->hit, (i == 0 ? lower : upper)->name);
	}

	const capsuleBone_t* neck = &bones[CAP_J_NECK];
	if (neck->valid)
	{
		float center[3];
		cap_copy(upper->origin, center);
		for (int i = 0; i < 3; i++)
			center[i] += (neck->origin[i] - upper->origin[i]) * TRAPEZIUS_ALONG;
		const float trapeziusForward = body ? body->trapezius.forward : TRAPEZIUS_FORWARD;
		const float trapeziusHalfWidth = body ? body->trapezius.halfWidth : TRAPEZIUS_HALF_WIDTH;
		const float trapeziusRadius = body ? body->trapezius.radius : TRAPEZIUS_RADIUS;

		cap_add_scaled(center, forward, trapeziusForward * scale);

		float a[3], b[3];
		cap_copy(center, a);
		cap_copy(center, b);
		cap_add_scaled(a, across, -trapeziusHalfWidth * scale);
		cap_add_scaled(b, across, trapeziusHalfWidth * scale);

		float radius = trapeziusRadius * scale;
		callback(context, a, b, radius, radius, HIT_TORSO_UPPER, upper->name);
	}

	// The groin, it is below the capsule across the hips and between the thighs
	const capsuleBone_t* hipL = &bones[CAP_J_HIP_LE];
	const capsuleBone_t* hipR = &bones[CAP_J_HIP_RI];
	float spineLength = sqrtf(cap_dot(spine, spine));
	if (hipL->valid && hipR->valid && spineLength > CAP_MIN_LENGTH)
	{
		float a[3], b[3];
		for (int i = 0; i < 3; i++)
			a[i] = (hipL->origin[i] + hipR->origin[i]) * 0.5f;
		cap_copy(a, b);
		cap_add_scaled(b, spine, -GROIN_LENGTH / spineLength);

		float radius = GROIN_RADIUS * scale;
		callback(context, a, b, radius, radius, HIT_TORSO_LOWER, lower->name);
	}
}

static void cap_head_shape(const capsuleBone_t* head, const capsuleHeadShape_t* shape, float scale, capsuleShapeCallback_t callback, void* context)
{
	float a[3], b[3];
	cap_bone_point(head, shape->localA, a);
	cap_bone_point(head, shape->localB, b);
	callback(context, a, b, shape->radiusA * scale, shape->radiusB * scale, HIT_HEAD, head->name);
}

static void cap_head_shapes(const capsuleBone_t* bones, const capsuleFit_t* fit, float scale, capsuleShapeCallback_t callback, void* context)
{
	const capsuleBone_t* head = &bones[CAP_J_HEAD];
	if (!head->valid)
		return;

	cap_head_shape(head, fit && fit->head ? &fit->head->skull : &head_shapes[0], scale, callback, context);
	cap_head_shape(head, fit && fit->head ? &fit->head->face : &head_shapes[1], scale, callback, context);
	if (fit && fit->headgear)
		cap_head_shape(head, &fit->headgear->dome, scale, callback, context);
}

// Long coats, they hang from the legs and move with them
static void cap_skirt_shapes(const capsuleBone_t* bones, const capsuleBodyFit_t* body, float scale, capsuleShapeCallback_t callback, void* context)
{
	const capsuleBone_t* hipL = &bones[CAP_J_HIP_LE];
	const capsuleBone_t* hipR = &bones[CAP_J_HIP_RI];
	const capsuleBone_t* kneeL = &bones[CAP_J_KNEE_LE];
	const capsuleBone_t* kneeR = &bones[CAP_J_KNEE_RI];
	const capsuleBone_t* ankleL = &bones[CAP_J_ANKLE_LE];
	const capsuleBone_t* ankleR = &bones[CAP_J_ANKLE_RI];
	const capsuleBone_t* lower = &bones[CAP_J_SPINE1];
	const capsuleBone_t* upper = &bones[CAP_J_SPINE4];
	if (!hipL->valid || !hipR->valid || !kneeL->valid || !kneeR->valid || !ankleL->valid || !ankleR->valid || !lower->valid || !upper->valid)
		return;

	float across[3], spine[3], forward[3];
	cap_sub(hipR->origin, hipL->origin, across);
	cap_sub(upper->origin, lower->origin, spine);
	float acrossLength = sqrtf(cap_dot(across, across));
	if (acrossLength < CAP_MIN_LENGTH)
		return;
	for (int i = 0; i < 3; i++)
		across[i] /= acrossLength;
	forward[0] = spine[1] * across[2] - spine[2] * across[1];
	forward[1] = spine[2] * across[0] - spine[0] * across[2];
	forward[2] = spine[0] * across[1] - spine[1] * across[0];
	float forwardLength = sqrtf(cap_dot(forward, forward));
	if (forwardLength < CAP_MIN_LENGTH)
		return;
	for (int i = 0; i < 3; i++)
		forward[i] /= forwardLength;

	// Upper two levels between the hip and the knee, the last one between the knee and the ankle
	static const float levels[CAPSULE_SKIRT_LEVELS] = { 0.35f, 0.85f, 0.25f };

	for (int side = 0; side < 2; side++)
	{
		const capsuleBone_t* hip = side == 0 ? hipL : hipR;
		const capsuleBone_t* knee = side == 0 ? kneeL : kneeR;
		const capsuleBone_t* ankle = side == 0 ? ankleL : ankleR;

		for (int i = 0; i < CAPSULE_SKIRT_LEVELS; i++)
		{
			const capsuleSkirtFit_t* skirt = &body->skirt[i];
			if (skirt->radius <= 0.0f)
				continue;

			const capsuleBone_t* from = i < 2 ? hip : knee;
			const capsuleBone_t* to = i < 2 ? knee : ankle;
			float axis[3], center[3];
			cap_sub(to->origin, from->origin, axis);
			cap_copy(from->origin, center);
			cap_add_scaled(center, axis, levels[i]);
			cap_add_scaled(center, forward, skirt->forward * scale);

			float a[3], b[3];
			cap_copy(center, a);
			cap_copy(center, b);
			cap_add_scaled(a, forward, -skirt->halfDepth * scale);
			cap_add_scaled(b, forward, skirt->halfDepth * scale);

			float radius = skirt->radius * scale;
			int hit = i < 2 ? (side == 0 ? HIT_L_LEG_UPPER : HIT_R_LEG_UPPER) : (side == 0 ? HIT_L_LEG_LOWER : HIT_R_LEG_LOWER);
			callback(context, a, b, radius, radius, hit, from->name);
		}
	}
}

// The radii of a capsule fitted to the body model, untouched when the model does not change them
static void cap_body_radii(const capsuleBodyFit_t* body, const capsuleDef_t* def, float* radiusA, float* radiusB)
{
	const float* radii = NULL;
	switch (def->hit)
	{
		case HIT_TORSO_LOWER: *radiusA = *radiusB = body->hips; return;
		case HIT_L_ARM_UPPER: case HIT_R_ARM_UPPER: radii = body->upperArm; break;
		case HIT_L_ARM_LOWER: case HIT_R_ARM_LOWER: radii = body->forearm; break;
		case HIT_L_LEG_UPPER: case HIT_R_LEG_UPPER: radii = body->thigh; break;
		case HIT_L_LEG_LOWER: case HIT_R_LEG_LOWER: radii = body->shin; break;
		case HIT_L_FOOT: case HIT_R_FOOT: radii = body->foot; break;
		default: return;
	}
	*radiusA = radii[0];
	*radiusB = radii[1];
}

// Moves the ends of a capsule from the bone origins to where the body part really is
static void cap_shape_capsule(const capsuleDef_t* def, const capsuleBone_t* bones, float* a, float* b)
{
	if (def->hit >= HIT_R_LEG_UPPER && def->hit <= HIT_L_LEG_LOWER)
	{
		// The knee sits in front of the knee bone
		bool left = def->hit == HIT_L_LEG_UPPER || def->hit == HIT_L_LEG_LOWER;
		bool lower = def->hit == HIT_R_LEG_LOWER || def->hit == HIT_L_LEG_LOWER;
		const capsuleBone_t* knee = &bones[left ? CAP_J_KNEE_LE : CAP_J_KNEE_RI];
		const float local[3] = { 0.0f, left ? 1.15f : -1.15f, 0.0f };
		if (knee->valid)
			cap_bone_point(knee, local, lower ? a : b);

		// The wide top of the thigh starts below the hip joint, so shots at the belt count as the torso
		if (!lower)
		{
			float axis[3];
			cap_sub(b, a, axis);
			float length = sqrtf(cap_dot(axis, axis));
			if (length > THIGH_START_OFFSET)
				cap_add_scaled(a, axis, THIGH_START_OFFSET / length);
		}
	}
	else if (def->hit == HIT_R_FOOT || def->hit == HIT_L_FOOT)
	{
		// The foot extends past the bones, both to the heel and to the toes
		bool left = def->hit == HIT_L_FOOT;
		const capsuleBone_t* ankle = &bones[left ? CAP_J_ANKLE_LE : CAP_J_ANKLE_RI];
		const capsuleBone_t* ball = &bones[left ? CAP_J_BALL_LE : CAP_J_BALL_RI];
		const float localHeel[3] = { left ? 2.5f : -2.5f, left ? -0.4f : 0.4f, 0.0f };
		const float localToe[3] = { left ? 2.4f : -2.4f, left ? 0.7f : -0.7f, 0.0f };
		if (ankle->valid)
			cap_bone_point(ankle, localHeel, a);
		if (ball->valid)
			cap_bone_point(ball, localToe, b);
	}

	if (def->extendEnd)
	{
		float axis[3];
		cap_sub(b, a, axis);
		cap_add_scaled(b, axis, 0.65f);
	}
}

void capsule_bone_set(capsuleBone_t* bone, const float* q, const float* origin, float weight, unsigned short name)
{
	float xx = q[0] * q[0] * weight, yy = q[1] * q[1] * weight, zz = q[2] * q[2] * weight;
	float xy = q[0] * q[1] * weight, xz = q[0] * q[2] * weight, yz = q[1] * q[2] * weight;
	float xw = q[0] * q[3] * weight, yw = q[1] * q[3] * weight, zw = q[2] * q[3] * weight;

	bone->valid = true;
	bone->name = name;
	cap_copy(origin, bone->origin);
	bone->axis[0][0] = 1.0f - (yy + zz);
	bone->axis[0][1] = xy + zw;
	bone->axis[0][2] = xz - yw;
	bone->axis[1][0] = xy - zw;
	bone->axis[1][1] = 1.0f - (xx + zz);
	bone->axis[1][2] = yz + xw;
	bone->axis[2][0] = xz + yw;
	bone->axis[2][1] = yz - xw;
	bone->axis[2][2] = 1.0f - (xx + yy);
}

bool capsule_is_player_skeleton(const capsuleBone_t* bones)
{
	return bones[CAP_J_SPINE4].valid && bones[CAP_J_HEAD].valid
		&& bones[CAP_J_KNEE_LE].valid && bones[CAP_J_KNEE_RI].valid
		&& bones[CAP_J_ELBOW_LE].valid && bones[CAP_J_ELBOW_RI].valid;
}

void capsule_for_each_shape(const capsuleBone_t* bones, float scale, capsuleShapeCallback_t callback, void* context, const capsuleFit_t* fit)
{
	if (scale < 0.5f)
		scale = 0.5f;

	cap_torso_shapes(bones, fit ? fit->body : NULL, scale, callback, context);
	cap_head_shapes(bones, fit, scale, callback, context);
	if (fit && fit->body)
		cap_skirt_shapes(bones, fit->body, scale, callback, context);

	for (unsigned int i = 0; i < sizeof(capsule_defs) / sizeof(capsule_defs[0]); i++)
	{
		const capsuleDef_t* def = &capsule_defs[i];
		const capsuleBone_t* boneA = &bones[def->boneA];
		const capsuleBone_t* boneB = &bones[def->boneB];
		if (!boneA->valid || !boneB->valid)
			continue;

		float a[3], b[3];
		cap_copy(boneA->origin, a);
		cap_copy(boneB->origin, b);
		cap_shape_capsule(def, bones, a, b);

		float radiusA = def->radiusA;
		float radiusB = def->radiusB > 0.05f ? def->radiusB : def->radiusA;
		if (fit && fit->body)
			cap_body_radii(fit->body, def, &radiusA, &radiusB);
		callback(context, a, b, radiusA * scale, radiusB * scale, def->hit, boneA->name);
	}
}

int capsule_wireframe(const float* a, const float* b, float radiusA, float radiusB, capsuleLineCallback_t callback, void* context)
{
	const float PI_F = 3.14159265f;
	const int RING_SEGMENTS = 8;
	const int ARC_SEGMENTS = 4;
	int lines = 0;

	// Axis of the capsule and two directions perpendicular to it
	float axis[3];
	cap_sub(b, a, axis);
	float length = sqrtf(cap_dot(axis, axis));
	if (length < 0.0001f)
	{
		axis[0] = 0.0f;
		axis[1] = 0.0f;
		axis[2] = 1.0f;
	}
	else
	{
		axis[0] /= length;
		axis[1] /= length;
		axis[2] /= length;
	}

	const float reference[3] = { fabsf(axis[2]) < 0.9f ? 0.0f : 1.0f, 0.0f, fabsf(axis[2]) < 0.9f ? 1.0f : 0.0f };
	float u[3], v[3];
	u[0] = axis[1] * reference[2] - axis[2] * reference[1];
	u[1] = axis[2] * reference[0] - axis[0] * reference[2];
	u[2] = axis[0] * reference[1] - axis[1] * reference[0];
	float uLength = sqrtf(cap_dot(u, u));
	u[0] /= uLength;
	u[1] /= uLength;
	u[2] /= uLength;
	v[0] = axis[1] * u[2] - axis[2] * u[1];
	v[1] = axis[2] * u[0] - axis[0] * u[2];
	v[2] = axis[0] * u[1] - axis[1] * u[0];

	const float* centers[2] = { a, b };
	const float radii[2] = { radiusA, radiusB };

	// Rings at both ends
	for (int end = 0; end < 2; end++)
	{
		float previous[3];
		for (int i = 0; i <= RING_SEGMENTS; i++)
		{
			float angle = 2.0f * PI_F * i / RING_SEGMENTS;
			float point[3];
			cap_copy(centers[end], point);
			cap_add_scaled(point, u, cosf(angle) * radii[end]);
			cap_add_scaled(point, v, sinf(angle) * radii[end]);
			if (i > 0)
			{
				callback(context, previous, point);
				lines++;
			}
			cap_copy(point, previous);
		}
	}

	// Lines along the sides, and half circles over the rounded ends
	for (int side = 0; side < 8; side++)
	{
		float angle = 0.25f * PI_F * side;
		float direction[3] = { 0.0f, 0.0f, 0.0f };
		cap_add_scaled(direction, u, cosf(angle));
		cap_add_scaled(direction, v, sinf(angle));

		float pointA[3], pointB[3];
		cap_copy(a, pointA);
		cap_add_scaled(pointA, direction, radiusA);
		cap_copy(b, pointB);
		cap_add_scaled(pointB, direction, radiusB);
		callback(context, pointA, pointB);
		lines++;

		// Opposite sides share a half circle, so only two perpendicular directions draw one
		if (side != 0 && side != 2)
			continue;

		for (int end = 0; end < 2; end++)
		{
			float sign = end == 0 ? -1.0f : 1.0f;
			float previous[3];
			for (int i = 0; i <= ARC_SEGMENTS; i++)
			{
				float arc = PI_F * i / ARC_SEGMENTS;
				float point[3];
				cap_copy(centers[end], point);
				cap_add_scaled(point, direction, cosf(arc) * radii[end]);
				cap_add_scaled(point, axis, sign * sinf(arc) * radii[end]);
				if (i > 0)
				{
					callback(context, previous, point);
					lines++;
				}
				cap_copy(point, previous);
			}
		}
	}

	return lines;
}

bool capsule_trace(const capsuleBone_t* bones, const float* start, const float* end, const unsigned char* priorityMap, float scale, capsuleTraceResult_t* result, const capsuleFit_t* fit)
{
	capsuleTraceState_t s;
	memset(&s, 0, sizeof(s));
	s.start = start;
	s.limit = result->fraction;
	s.bestFraction = result->fraction;
	s.priorityMap = priorityMap;

	cap_sub(end, start, s.dir);
	s.length = sqrtf(cap_dot(s.dir, s.dir));
	if (s.length < 0.0001f)
		return false;
	s.dir[0] /= s.length;
	s.dir[1] /= s.length;
	s.dir[2] /= s.length;

	capsule_for_each_shape(bones, scale, cap_test, &s, fit);

	if (!s.tested)
		return false;

	result->surfaceType = 0;
	result->normal[0] = result->normal[1] = result->normal[2] = 0.0f;
	result->partName = 0;
	result->partGroup = 0;

	if (s.bestPriority > 1)
	{
		result->fraction = s.bestFraction;
		cap_copy(s.bestNormal, result->normal);
		result->partName = s.bestPartName;
		result->partGroup = (unsigned short)s.bestHit;
	}
	return true;
}
