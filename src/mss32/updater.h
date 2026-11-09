#ifndef UPDATER_H
#define UPDATER_H

#include "shared.h"

bool updater_resolveServerAddress();
void updater_checkForUpdate();
void updater_updatePacketResponse(struct netaddr_s addr);
void updater_renderer();
void updater_frame();
void updater_unload();
void updater_init();
void updater_patch();

#endif // UPDATER_H