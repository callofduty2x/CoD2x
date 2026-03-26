#ifndef GSC_MATCH_H
#define GSC_MATCH_H

#include "cod2_script.h"
#include "server.h"

extern int codecallback_test_match_onStartGameType;
extern int codecallback_test_match_onPlayerConnect;
extern int codecallback_test_match_onStopGameType;

void gsc_match_playerSetData(scr_entref_t ref);
void gsc_match_playerGetData(scr_entref_t ref);
void gsc_match_playerIsAllowed(scr_entref_t ref);
void gsc_match_uploadData();
void gsc_match_setData();
void gsc_match_getData();
void gsc_match_redownloadData();
void gsc_match_clearData();
void gsc_match_isActivated();
void gsc_match_cancel();
void gsc_match_finish();

bool gsc_match_beforeMapChangeOrRestart(bool fromScript, bool bComplete, bool shutdown, sv_map_change_source_e source);
void gsc_match_onPlayerConnect(int entnum);
void gsc_match_onStartGameType();

#endif