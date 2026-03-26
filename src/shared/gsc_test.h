#ifndef GSC_TEST_H
#define GSC_TEST_H

#include "cod2_script.h"

extern int codecallback_test_onStartGameType;
extern int codecallback_test_onStartGameType2;
extern int codecallback_test_onStartGameType3;
extern int codecallback_test_onPlayerConnect;
extern int codecallback_test_onPlayerConnect2;

void gsc_test_playerGetName(scr_entref_t ref);

void gsc_test_returnUndefined();
void gsc_test_returnBool();
void gsc_test_returnInt();
void gsc_test_returnFloat();
void gsc_test_returnString();
void gsc_test_returnVector();
void gsc_test_returnArray();
void gsc_test_getAll();
void gsc_test_allOk();

void gsc_test_onPlayerConnect(int entnum);
void gsc_test_onStartGameType();

#endif