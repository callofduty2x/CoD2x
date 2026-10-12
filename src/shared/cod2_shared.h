#ifndef COD2_SHARED_H
#define COD2_SHARED_H

#include "shared.h"
#include <stdio.h>
#if COD2X_LINUX
#include <strings.h>
#endif
#include <cctype>


#define va ((char* (*)(const char *, ...))ADDR(0x0044a990, 0x080b7fa6))

#define Q_COLOR_ESCAPE	'^'
#define Q_IsColorString(p)	( p && *(p) == Q_COLOR_ESCAPE && *((p)+1) && *((p)+1) != Q_COLOR_ESCAPE )


// Compare two strings ignoring case
inline int Q_stricmp(const char *s1, const char *s2) {
    #if COD2X_WIN32
        return stricmp(s1, s2);
    #endif
    #if COD2X_LINUX
        return strcasecmp(s1, s2);
    #endif
}

// Compare two strings of certain length, ignoring case
inline int I_strnicmp(const char *s0, const char *s1, int n) {
    #if COD2X_WIN32
        return strnicmp(s0, s1, n);
    #endif
    #if COD2X_LINUX
        return strncasecmp(s0, s1, n);
    #endif
}

inline void Q_strncpyz( char *dest, const char *src, int destsize )
{
	strncpy( dest, src, destsize-1 );
	dest[destsize-1] = 0;
}

// Convert string to lower case
inline char *I_strlwr(char *s)
{
	char* iter;

	for (iter = s; *iter; ++iter)
	{
		if (isupper(*iter))
		{
			*iter += 32;
		}
	}
	return s;
}

inline void Info_SetValueForKey(const char* buffer, const char* keyName, const char* value) {  
    #if COD2X_WIN32
        const void* original_func = (void*)0x0044ae10; // (char* buffer, char* keyName @ eax, int32_t value)
        ASM( push,     value          ); // 3th argument
        ASM( push,     buffer         ); // 2rd argument
        ASM( mov,      "eax", keyName ); // 1nd argument
        ASM( call,     original_func  );
        ASM( add_esp,  8              ); // Clean up the stack (2 arguments × 4 bytes = 8)
    #endif
    #if COD2X_LINUX
        ((char* (*)(const char* buffer, const char* keyName, const char* value))0x080b85ce)(buffer, keyName, value);
    #endif
}


inline char* Info_ValueForKey(const char* buffer, const char* keyName) {   
    #if COD2X_WIN32
        const void* original_func = (void*)(0x0044aa90); // char* Info_ValueForKey(char* buffer @ ecx, char* key)
        char* result;
        ASM( push,     keyName          ); // 2nd argument                    
        ASM( mov,      "ecx", buffer    ); // 1st argument
        ASM( call,     original_func    ); 
        ASM( add_esp,  4                ); // Clean up the stack (1 argument × 4 bytes = 4)     
        ASM( movr,     result, "eax"    ); // Store the return value in the 'result' variable
        return result;
    #endif
    #if COD2X_LINUX
        return ((char* (*)(const char* buffer, const char* keyName))0x080b8108)(buffer, keyName);
    #endif
}


enum cs_index_t
{
    CS_SERVERINFO = 0,       // an info string with all the serverinfo cvars
    CS_SYSTEMINFO = 1,       // an info string for server system to client system configuration (timescale, etc)
	CS_GAME_VERSION = 2,     // cod
	CS_AMBIENT = 3,
	CS_MESSAGE = 4,
	CS_SCORES1 = 5,
	CS_SCORES2 = 6,
	CS_WEAPONS = 7,
	CS_ITEMS = 8,
	CS_NORTHYAW = 11,
	CS_FOGVARS = 12,
	CS_LEVEL_START_TIME = 13,
	CS_MOTD = 14,
	CS_VOTE_TIME = 15,
	CS_VOTE_STRING = 16,
	CS_VOTE_YES = 17,
	CS_VOTE_NO = 18,
	CS_VOTE_MAPNAME = 19,
	CS_VOTE_GAMETYPE = 20,
	CS_MULTI_MAPWINNER = 22,
	CS_STATUS_ICONS = 23,
	CS_HEAD_ICONS = 31,
	CS_TAGS = 110,
	CS_MODELS = 334,
	CS_SOUND_ALIASES = 590,
	CS_EFFECT_NAMES = 846,
	CS_EFFECT_TAGS = 910,
	CS_SHELLSHOCKS = 1166,
	CS_SCRIPT_MENUS = 1246,
	CS_HINTSTRINGS = 1278,
	CS_LOCALIZED_STRINGS = 1310,
	CS_SHADERS = 1566
};

#endif