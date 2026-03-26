#ifndef GSC_H
#define GSC_H

#include "server.h"

bool gsc_beforeMapChangeOrRestart(bool fromScript, bool bComplete, bool shutdown, sv_map_change_source_e source);
void gsc_frame();
void gsc_init();
void gsc_patch();

#endif