#ifndef COD2_DEFINITIONS_H
#define COD2_DEFINITIONS_H

#define qboolean int
#define qtrue 1
#define qfalse 0

typedef unsigned char byte;
typedef float vec_t;
typedef vec_t vec2_t[2];
typedef vec_t vec3_t[3];
typedef vec_t vec4_t[4];
typedef vec_t vec5_t[5];

#define PITCH               0       // up / down
#define YAW                 1       // left / right
#define ROLL                2       // fall over

#define SHORT2ANGLE(x)      ((x) * (360.0f / 65536))   // 16-bit angle of usercmd / delta_angles to degrees

#endif