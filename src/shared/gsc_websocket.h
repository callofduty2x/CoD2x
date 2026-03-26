#ifndef GSC_WEBSOCKET_H
#define GSC_WEBSOCKET_H

#include "server.h"

void gsc_websocket_connect();
void gsc_websocket_close();
void gsc_websocket_sendText();
bool gsc_websocket_beforeMapChangeOrRestart(bool fromScript, bool bComplete, bool shutdown, sv_map_change_source_e source);
void gsc_websocket_frame();
void gsc_websocket_init();

#endif