#ifndef GSC_HTTP_H
#define GSC_HTTP_H

#include "server.h"

bool gsc_http_beforeMapChangeOrRestart(bool fromScript, bool bComplete, bool shutdown, sv_map_change_source_e source);
void gsc_http_fetch();
void gsc_http_frame();
void gsc_http_init();

#endif