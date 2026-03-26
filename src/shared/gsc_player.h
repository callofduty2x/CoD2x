#ifndef GSC_PLAYER_H
#define GSC_PLAYER_H

#include "cod2_script.h"

void gsc_player_getip(scr_entref_t id);
void gsc_player_playerGetHWID(scr_entref_t ref);
void gsc_player_playerGetCDKeyHash(scr_entref_t ref);
void gsc_player_playerGetAuthorizationStatus(scr_entref_t ref);
void gsc_player_getViewOrigin(scr_entref_t ref);
void gsc_player_getStance(scr_entref_t ref);
void gsc_player_isUsingTurret(scr_entref_t ref);

void gsc_player_frame();
void gsc_player_init();

#endif