#ifndef COD2_MATH_H
#define COD2_MATH_H

#include "math.h"

#include "cod2_definitions.h"

// Credit to CoD2rev_Server by voron00

#define DotProduct(a,b)         ((a)[0]*(b)[0]+(a)[1]*(b)[1]+(a)[2]*(b)[2])
#define Dot2Product(a,b)        ((a)[0]*(b)[0]+(a)[1]*(b)[1])
#define VectorSubtract(a,b,c)   ((c)[0]=(a)[0]-(b)[0],(c)[1]=(a)[1]-(b)[1],(c)[2]=(a)[2]-(b)[2])
#define Vector2Subtract(a,b,c)  ((c)[0]=(a)[0]-(b)[0],(c)[1]=(a)[1]-(b)[1])
#define VectorAdd(a,b,c)        ((c)[0]=(a)[0]+(b)[0],(c)[1]=(a)[1]+(b)[1],(c)[2]=(a)[2]+(b)[2])
#define VectorAddTo(a, b)		((a)[0]+=(b)[0],(a)[1]+=(b)[1],(a)[2]+=(b)[2])
#define Vector2Add(a,b,c)       ((c)[0]=(a)[0]+(b)[0],(c)[1]=(a)[1]+(b)[1])
#define VectorCopy(a,b)         ((b)[0]=(a)[0],(b)[1]=(a)[1],(b)[2]=(a)[2])
#define Vector2Copy(a,b)        ((b)[0]=(a)[0],(b)[1]=(a)[1])
#define Vec2Multiply(v)         ((v)[0]*(v)[0]+(v)[1]*(v)[1])
#define VectorMA2(v, s, b, o)   ((o)[0]=(v)[0]+(b)[0]*(s),(o)[1]=(v)[1]+(b)[1]*(s))

#define	VectorScale(v, s, o)    ((o)[0]=(v)[0]*(s),(o)[1]=(v)[1]*(s),(o)[2]=(v)[2]*(s))
#define	Vec2Scale(v, s, o)      ((o)[0]=(v)[0]*(s),(o)[1]=(v)[1]*(s))
#define VectorMA(v, s, b, o)    ((o)[0]=(v)[0]+(b)[0]*(s),(o)[1]=(v)[1]+(b)[1]*(s),(o)[2]=(v)[2]+(b)[2]*(s)) // Multiply & Add -> o[] = v[] + b[] * s;
#define CrossProduct(a,b,c)     ((c)[0]=(a)[1]*(b)[2]-(a)[2]*(b)[1],(c)[1]=(a)[2]*(b)[0]-(a)[0]*(b)[2],(c)[2]=(a)[0]*(b)[1]-(a)[1]*(b)[0])

#define DotProduct4( x,y )        ( ( x )[0] * ( y )[0] + ( x )[1] * ( y )[1] + ( x )[2] * ( y )[2] + ( x )[3] * ( y )[3] )
#define VectorSubtract4( a,b,c )  ( ( c )[0] = ( a )[0] - ( b )[0],( c )[1] = ( a )[1] - ( b )[1],( c )[2] = ( a )[2] - ( b )[2],( c )[3] = ( a )[3] - ( b )[3] )
#define VectorAdd4( a,b,c )       ( ( c )[0] = ( a )[0] + ( b )[0],( c )[1] = ( a )[1] + ( b )[1],( c )[2] = ( a )[2] + ( b )[2],( c )[3] = ( a )[3] + ( b )[3] )
#define VectorCopy4( a,b )        ( ( b )[0] = ( a )[0],( b )[1] = ( a )[1],( b )[2] = ( a )[2],( b )[3] = ( a )[3] )
#define VectorScale4( v, s, o )   ( ( o )[0] = ( v )[0] * ( s ),( o )[1] = ( v )[1] * ( s ),( o )[2] = ( v )[2] * ( s ),( o )[3] = ( v )[3] * ( s ) )
#define VectorMA4( v, s, b, o )   ( ( o )[0] = ( v )[0] + ( b )[0] * ( s ),( o )[1] = ( v )[1] + ( b )[1] * ( s ),( o )[2] = ( v )[2] + ( b )[2] * ( s ),( o )[3] = ( v )[3] + ( b )[3] * ( s ) )

#define VectorClear(a)		      ((a)[0]=(a)[1]=(a)[2]=0)
#define Vector2Clear(a)		      ((a)[0]=(a)[1]=0)
#define VectorNegate( a,b )       ( ( b )[0] = -( a )[0],( b )[1] = -( a )[1],( b )[2] = -( a )[2] )
#define VectorSet(v, x, y, z)	  ((v)[0]=(x), (v)[1]=(y), (v)[2]=(z))
#define Vector4Set(v, x, y, z, w) ((v)[0]=(x), (v)[1]=(y), (v)[2]=(z), (v)[3]=(w))
#define Vector4Copy( a,b )        ( ( b )[0] = ( a )[0],( b )[1] = ( a )[1],( b )[2] = ( a )[2],( b )[3] = ( a )[3] )

#define SnapVector( v ) {v[0] = (int)v[0]; v[1] = (int)v[1]; v[2] = (int)v[2];}

#define DEG2RAD(x) ((x) * (M_PI / 180.0f))
#define RAD2DEG(x) ((x) * (180.0f / M_PI))

#define DEGINRAD  57.29577951308232 // degrees in one radian
#define RADINDEG  0.0174532925199433 // radian in one degree

#ifndef M_PI
#define M_PI        3.14159265358979323846f // matches value in gcc v2 math.h
#endif


inline float fclamp(float val, float min, float max)
{
	return fmax(min, fmin(val, max));
}

inline void I_sinCos(float value, float *pSin, float *pCos)
{
	*pSin = sin(value);
	*pCos = cos(value);
}

/**
 * Computes an approximation of the reciprocal square root of a given number -> 1/sqrt(number).
 */
inline float I_rsqrt(const float number)
{
	long i;
	float x2, y;
	const float threehalfs = 1.5F;

	x2 = number * 0.5F;
	y = number;
	i = *(long*)&y;
	i = 0x5f3759df - (i >> 1);
	y = *(float*)&i;
	y = y * (threehalfs - (x2 * y * y)); 

	return y;
}




/**
 * This function takes an input angle (in degrees) and ensures that the
 * resulting angle is within the range of 0 (inclusive) to 360 (exclusive).
 * It uses bitwise operations to efficiently wrap the angle within this range.
 */
inline float AngleNormalize360( float angle )
{
	return ( 360.0 / 65536 ) * ( (int)( angle * ( 65536 / 360.0 ) ) & 65535 );
}

/**
 * Normalizes an angle to the range [-180, 180] degrees.
 */
inline float AngleNormalize180( float angle )
{
	angle = AngleNormalize360( angle );

	if ( angle > 180.0 )
	{
		angle -= 360.0;
	}

	return angle;
}

/**
 * Subtracts two angles and normalizes the result to the range [-180, 180] degrees.
 */
inline float AngleSubtract( float a1, float a2 )
{
	float a = a1 - a2;
	a = AngleNormalize180(a);
	return a;
}

/**
 * Adds two angles and normalizes the result to the range [-180, 180] degrees.
 */
inline float AngleAdd( float a1, float a2 )
{
	float a = a1 + a2;
	a = AngleNormalize180(a);
	return a;
}

inline void AnglesSubtract(const vec3_t v1, const vec3_t v2, vec3_t v3)
{
	v3[0] = AngleNormalize180(v1[0] - v2[0]);
	v3[1] = AngleNormalize180(v1[1] - v2[1]);
	v3[2] = AngleNormalize180(v1[2] - v2[2]);
}

inline float AngleDelta( float angle1, float angle2 )
{
	return AngleNormalize180( angle1 - angle2 );
}

inline void AngleVectors( const vec3_t angles, vec3_t forward, vec3_t right, vec3_t up )
{
	float angle;
	static float sr, sp, sy, cr, cp, cy;
	// static to help MS compiler fp bugs

	angle = angles[YAW] * ( M_PI * 2 / 360 );
	sy = sin( angle );
	cy = cos( angle );

	angle = angles[PITCH] * ( M_PI * 2 / 360 );
	sp = sin( angle );
	cp = cos( angle );

	angle = angles[ROLL] * ( M_PI * 2 / 360 );
	sr = sin( angle );
	cr = cos( angle );

	if ( forward )
	{
		forward[0] = cp * cy;
		forward[1] = cp * sy;
		forward[2] = -sp;
	}

	if ( right )
	{
		right[0] = ( -1 * sr * sp * cy + - 1 * cr * -sy );
		right[1] = ( -1 * sr * sp * sy + - 1 * cr * cy );
		right[2] = -1 * sr * cp;
	}

	if ( up )
	{
		up[0] = ( cr * sp * cy + - sr * -sy );
		up[1] = ( cr * sp * sy + - sr * cy );
		up[2] = cr * cp;
	}
}

inline void AnglesToAxis( const vec3_t angles, vec3_t axis[3] )
{
	vec3_t right;
	vec3_t vec3_origin = {0, 0, 0};

	AngleVectors( angles, axis[0], right, axis[2] );
	VectorSubtract(vec3_origin, right, axis[1] );
}






inline float VectorLength( const vec3_t v )
{
	return sqrt( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
}

inline vec_t VectorLengthSquared( const vec3_t v )
{
	return ( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
}

inline float Vec3ToYaw( const vec3_t vec )
{
	float yaw;

	if ( vec[YAW] == 0 && vec[PITCH] == 0 )
	{
		return 0.0;
	}

	yaw = ( atan2( vec[YAW], vec[PITCH] ) * 180 / M_PI );

	if(yaw >= 0.0)
	{
		return yaw;
	}

	return yaw + 360;
}

inline vec_t Vec3NormalizeTo( const vec3_t v, vec3_t out )
{
	float length = VectorLength(v);

	if ( length == 0.0 )
	{
		VectorClear(out);
	}
	else
	{
		VectorScale(v, 1.0 / length, out);
	}

	return length;
}

inline void MatrixMultiply( const float in1[3][3], const float in2[3][3], float out[3][3] )
{
	out[0][0] = in1[0][0] * in2[0][0] + in1[0][1] * in2[1][0] + in1[0][2] * in2[2][0];
	out[0][1] = in1[0][0] * in2[0][1] + in1[0][1] * in2[1][1] + in1[0][2] * in2[2][1];
	out[0][2] = in1[0][0] * in2[0][2] + in1[0][1] * in2[1][2] + in1[0][2] * in2[2][2];
	out[1][0] = in1[1][0] * in2[0][0] + in1[1][1] * in2[1][0] + in1[1][2] * in2[2][0];
	out[1][1] = in1[1][0] * in2[0][1] + in1[1][1] * in2[1][1] + in1[1][2] * in2[2][1];
	out[1][2] = in1[1][0] * in2[0][2] + in1[1][1] * in2[1][2] + in1[1][2] * in2[2][2];
	out[2][0] = in1[2][0] * in2[0][0] + in1[2][1] * in2[1][0] + in1[2][2] * in2[2][0];
	out[2][1] = in1[2][0] * in2[0][1] + in1[2][1] * in2[1][1] + in1[2][2] * in2[2][1];
	out[2][2] = in1[2][0] * in2[0][2] + in1[2][1] * in2[1][2] + in1[2][2] * in2[2][2];
}

inline void VecToAngles( const vec3_t value1, vec3_t angles )
{
	float forward;
	float yaw, pitch;

	if ( value1[1] == 0 && value1[0] == 0 )
	{
		yaw = 0;
		if ( value1[2] > 0 )
		{
			pitch = 270;
		}
		else
		{
			pitch = 90;
		}
	}
	else
	{
		yaw = ( atan2( value1[1], value1[0] ) * 180 / M_PI );
		if(yaw < 0.0)
		{
			yaw += 360.0;
		}
		forward = sqrt( value1[0] * value1[0] + value1[1] * value1[1] );
		pitch = ( atan2( value1[2], forward ) * -180 / M_PI );
		if(pitch < 0.0)
		{
			pitch += 360.0;
		}

	}

	angles[PITCH] = pitch;
	angles[YAW] = yaw;
	angles[ROLL] = 0;
}

inline float VecToYaw( const vec3_t vec )
{
	float yaw;

	if ( vec[YAW] == 0 && vec[PITCH] == 0 )
	{
		return 0.0;
	}

	yaw = ( atan2( vec[YAW], vec[PITCH] ) * 180 / M_PI );

	if(yaw >= 0.0)
	{
		return yaw;
	}

	return yaw + 360;
}

inline vec_t VecToSignedPitch(const vec3_t vec)
{
	float t;

	if ( 0.0 != vec[1] || 0.0 != vec[0] )
	{
		t = atan2(vec[2], sqrt(vec[1] * vec[1] + vec[0] * vec[0]));
		return t * -180.0 / M_PI;
	}

	if ( -vec[2] < 0.0 )
	{
		return -90.0;
	}

	return 90.0;
}

inline void AxisToAngles( vec3_t axis[3], vec3_t angles )
{
	float a;
	vec3_t right;
	float temp;
	float pitch;
	float fCos;
	float fSin;

	// first get the pitch and yaw from the forward vector
	VecToAngles( axis[0], angles );

	// now get the roll from the right vector
	VectorCopy( axis[1], right );

	// get the angle difference between the tmpAxis[2] and axis[2] after they have been reverse-rotated
	a = (-angles[YAW] * 0.017453292);
	fCos = cos(a);
	fSin = sin(a);

	temp = fCos * right[0] - fSin * right[1];
	right[1] = fSin * right[0] + fCos * right[1];

	a = -angles[0] * 0.017453292;
	fCos = cos(a);
	fSin = sin(a);

	right[0] = (fSin * right[2]) + (fCos * temp);
	right[2] = (fCos * right[2]) - (fSin * temp);

	// now find the angles, the PITCH is effectively our ROLL
	pitch = VecToSignedPitch(right);

	if ( right[1] >= 0.0 )
	{
		angles[ROLL] = -pitch;
		return;
	}

	if ( pitch >= 0.0 )
	{
		angles[ROLL] = pitch - 180.0;
		return;
	}

	angles[ROLL] = pitch + 180.0;
}

#endif