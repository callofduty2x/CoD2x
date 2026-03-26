#ifndef SERVER_H
#define SERVER_H

#include <cstdint>

typedef enum {
	SV_MAP_CHANGE_SOURCE_MAP,
	SV_MAP_CHANGE_SOURCE_FAST_RESTART,
	SV_MAP_CHANGE_SOURCE_MAP_RESTART,
	SV_MAP_CHANGE_SOURCE_MAP_ROTATE,
	SV_MAP_CHANGE_SOURCE_MAP_SHUTDOWN,
} sv_map_change_source_e;

inline const char* sv_map_change_source_to_string(sv_map_change_source_e source) {
    switch (source) {
        case SV_MAP_CHANGE_SOURCE_MAP: 			return "map";
        case SV_MAP_CHANGE_SOURCE_FAST_RESTART: return "fast_restart";
        case SV_MAP_CHANGE_SOURCE_MAP_RESTART: 	return "map_restart";
        case SV_MAP_CHANGE_SOURCE_MAP_ROTATE: 	return "map_rotate";
        case SV_MAP_CHANGE_SOURCE_MAP_SHUTDOWN: return "shutdown";
        default: 								return "unknown";
    }
}

typedef struct leakyBucket_s leakyBucket_t;
struct leakyBucket_s
{
	int type;
	unsigned char adr[4];
	uint64_t lastTime;
	signed char	burst;
	long hash;
	leakyBucket_t *prev, *next;
};

void server_fix_clip_bug(bool enable);
void server_init();
void server_patch();

#endif