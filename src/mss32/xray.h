#ifndef XRAY_H
#define XRAY_H

#include "shared.h"
#include "../shared/cod2_definitions.h"

// Who is watching the game
enum xrayViewer_e {
    XRAY_VIEWER_NONE,           // not in game
    XRAY_VIEWER_PLAYER,         // playing player
    XRAY_VIEWER_DEAD,           // dead player in the spectator session, following teammates
    XRAY_VIEWER_SPECTATOR,      // real spectator (team spectator) or demo playback
};

extern xrayViewer_e xray_viewer;

void xray_getTeamColor(int team, vec4_t color);
bool xray_centerFadeEnabled();
float xray_centerFadeAlpha(float x0, float y0, float x1, float y1);

bool xray_headDepthAvailable();
bool xray_headDepthTest(const vec3_t point, bool* marked);
void xray_headDepthTestEnd();
void xray_headDepthClear();

void xray_frame();
void xray_rendered();
void xray_unload();
void xray_init();
void xray_patch();

#endif
