#ifndef COD2_DVARS_H
#define COD2_DVARS_H

#include "cod2_math.h"

#define dedicated (*(dvar_t **)(ADDR(0x00c22f00, 0x084a8780)))
#define sv_maxclients (*(dvar_t **)(ADDR(0x00d52810, 0x0849f74c)))
#define sv_running (*(dvar_t **)(ADDR(0x00c26108, 0x081a218c)))
#define sv_packet_info (*((dvar_s**)( ADDR(0x00d52858, 0x0849f798) )))
#define net_lanauthorize (*((dvar_s**)( ADDR(0x00c8fb14, 0x081fa514) )))
#define showpackets (*((dvar_s**)( ADDR(0x00c84ae8, 0x081fa500) )))
#define dvars_count (*((int*)( ADDR(0x00c5c584, 0x085abe08) )))


enum dvarFlags_e : uint16_t
{
    DVAR_NOFLAG = 0x0,
    DVAR_ARCHIVE = 0x1,     // save to config_mp.cfg
    DVAR_USERINFO = 0x2,    // cvars used in connection string
    DVAR_SERVERINFO = 0x4,  // cvars used to describe the server settings to clients when clients ask
    DVAR_SYSTEMINFO = 0x8,  // cvars that are synchronized to all clients
    DVAR_NOWRITE = 0x10,    // write protected, but can be changed by server
    DVAR_LATCH = 0x20,
    DVAR_ROM = 0x40,
    DVAR_CHEAT = 0x80,
    DVAR_CONFIG = 0x100, // same on client and server side, stored in configstrings
    DVAR_SAVED = 0x200,
    DVAR_SCRIPTINFO = 0x400, // same as DVAR_SERVERINFO, but set from script
    DVAR_CHANGEABLE_RESET = 0x1000, // allow changing min and max values when cvar is re-registered
    DVAR_RENDERER = 0x2000,
    DVAR_EXTERNAL = 0x4000,
    DVAR_AUTOEXEC = 0x8000
};

enum dvarType_e : uint8_t
{
    DVAR_TYPE_BOOL = 0x0,
    DVAR_TYPE_FLOAT = 0x1,
    DVAR_TYPE_VEC2 = 0x2,
    DVAR_TYPE_VEC3 = 0x3,
    DVAR_TYPE_VEC4 = 0x4,
    DVAR_TYPE_INT = 0x5,
    DVAR_TYPE_ENUM = 0x6,
    DVAR_TYPE_STRING = 0x7,
    DVAR_TYPE_COLOR = 0x8
};

union DvarLimits
{
	struct
	{
		int stringCount;
		const char **strings;
	} enumeration;
	struct
	{
		int min;
		int max;
	} integer;
	struct
	{
		float min;
		float max;
	} decimal;
};

typedef struct
{
	union
	{
		bool boolean;
		int integer;
		float decimal;
		float* vec2;
		float* vec3;
		float* vec4;
		const char *string;
		unsigned char color[4];
	};
} dvarValue_t;
static_assert(sizeof(dvarValue_t) == 0x4, "sizeof(dvarValue_t)");

typedef struct dvar_s
{
    char const* name;
    enum dvarFlags_e flags;
    enum dvarType_e type;
    bool modified;
    dvarValue_t value;
    dvarValue_t latchedValue;
    dvarValue_t defaultValue;
    union DvarLimits limits;
    struct dvar_s* next;
    struct dvar_s* hashNext;
} dvar_t;







inline dvar_t* Dvar_RegisterBool(const char* name, bool value, enum dvarFlags_e flags) {
    return ((dvar_t* (__cdecl*)(const char*, int, unsigned short))ADDR(0x00438040, 0x080b3fd2))(name, value, flags);
}
inline dvar_t* Dvar_RegisterInt(const char* name, int value, int min, int max, enum dvarFlags_e flags) {
    return ((dvar_t* (__cdecl*)(const char*, int, int, int, unsigned short))ADDR(0x004380a0, 0x080b403a))(name, value, min, max, flags);
}
inline dvar_t* Dvar_RegisterFloat(const char* name, float value, float min, float max, enum dvarFlags_e flags) {
    return ((dvar_t* (__cdecl*)(const char*, float, float, float, unsigned short))ADDR(0x00438100, 0x080b408c))(name, value, min, max, flags);
}
inline dvar_t* Dvar_RegisterString(const char* name, const char* value, enum dvarFlags_e flags) {
    return ((dvar_t* (__cdecl*)(const char*, const char*, unsigned short))ADDR(0x00438340, 0x080b4232))(name, value, flags);
}
inline dvar_t* Dvar_RegisterEnum(const char* name, const char** value, int defaultIndex, enum dvarFlags_e flags) {
    return ((dvar_t* (__cdecl*)(const char*, const char**, int, unsigned short))ADDR(0x004383a0, 0x080b4292))(name, value, defaultIndex, flags);
}
inline dvar_t* Dvar_RegisterVec3(const char *name, float x, float y, float z, float min, float max, unsigned short flags) {
    return ((dvar_t* (__cdecl*)(const char*, float, float, float, float, float, unsigned short))ADDR(0x00438210, 0x080b4148))
        (name, x, y, z, min, max, flags);
}

inline void Dvar_SetBool(struct dvar_s* dvar, int value) {
    ((void (__cdecl*)(struct dvar_s*, int))ADDR(0x00438b90, 0x080b4980))(dvar, value);
}
inline void Dvar_SetInt(struct dvar_s* dvar, int value) {
    ((void (__cdecl*)(struct dvar_s*, int))ADDR(0x00438bf0, 0x080b49aa))(dvar, value);
}
inline void Dvar_SetFloat(struct dvar_s* dvar, float value) {
    ((void (__cdecl*)(struct dvar_s*, float))ADDR(0x00438c10, 0x080b49cc))(dvar, value);
}
inline void Dvar_SetString(struct dvar_s* dvar, const char* value) {
    ((void (__cdecl*)(struct dvar_s*, const char*))ADDR(0x00438ca0, 0x080b4a80))(dvar, value);
}
inline void Dvar_SetVec3(struct dvar_s *dvar, float x, float y, float z) {
    ((void (__cdecl*)(struct dvar_s*, float, float, float))ADDR(0x00438c50, 0x080b4a18))(dvar, x, y, z);
}

inline bool Dvar_GetBool(const char* name) {
    return ((bool (__cdecl*)(const char*))ADDR(0x00437480, 0x080b2faa))(name);
}
inline int Dvar_GetInt(const char* name) {
    return ((int (__cdecl*)(const char*))ADDR(0x004374c0, 0x080b2ffc))(name);
}
inline float Dvar_GetFloat(const char* name) {
    return ((float (__cdecl*)(const char*))ADDR(0x004374f0, 0x080b3054))(name);
}
inline const char* Dvar_GetString(const char* name) {
    return ((const char* (__cdecl*)(const char*))ADDR(0x004375b0, 0x080b318a))(name);
}

inline dvar_t* Dvar_GetDvarByName(const char* name) {
    return ((dvar_t* (__cdecl*)(const char*))ADDR(0x004373a0, 0x080b2e54))(name);
}

inline void Dvar_StringToColor(struct dvar_s* dvar, float (*vec4)[4]) {
    ASM_CALL(RETURN_VOID, 0x00437640, 0, EAX(dvar), EDI(vec4));
}


#endif