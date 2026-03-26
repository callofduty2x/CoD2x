#include "gsc_test.h"

#include <stdarg.h> // va_list, va_start, va_end

#include "shared.h"
#include "cod2_common.h"
#include "cod2_script.h"
#include "cod2_math.h"
#include "cod2_server.h"


int codecallback_test_onStartGameType = 0;
int codecallback_test_onStartGameType2 = 0;
int codecallback_test_onStartGameType3 = 0;
int codecallback_test_onPlayerConnect = 0;
int codecallback_test_onPlayerConnect2 = 0;

#if DEBUG

void gsc_test_playerGetName(scr_entref_t ref) {
	int id = ref.entnum;

	if ( id >= MAX_CLIENTS )
	{
		Scr_Error(va("entity %d is not a player", id));
		Scr_AddUndefined();
		return;
	}

	client_t *client = &svs_clients[id];
	Scr_AddString(client->name);
}








void gsc_test_returnUndefined() {
	Scr_AddUndefined();
}
void gsc_test_returnBool() {
	Scr_AddBool(Scr_GetInt(0));
}
void gsc_test_returnInt() {
	Scr_AddInt(Scr_GetInt(0));
}
void gsc_test_returnFloat() {
	float f = Scr_GetFloat(0);
	Scr_AddFloat(f);
}
void gsc_test_returnString() {
	const char *str = Scr_GetString(0);
	Scr_AddString(str);
}
void gsc_test_returnVector() {
	vec3_t vec;
	Scr_GetVector(0, vec);
	Scr_AddVector(vec);
}
void gsc_test_returnArray() {
	Scr_MakeArray();
	Scr_AddInt(10);
	Scr_AddArray();
	Scr_AddInt(20);
	Scr_AddArray();
	Scr_AddInt(30);
	Scr_AddArray();
}
void gsc_test_allOk() {
	const char *str = Scr_GetString(0);
	if (strcmp(str, "all ok") != 0) Com_Error(ERR_DROP, "gsc_test_allOk: expected 'all ok', got '%s'", str);

	Com_Printf("============================\n");
	Com_Printf("CoD2x: GSC test OK\n");
	Com_Printf("============================\n");
}

void gsc_test_getAll() {
	unsigned int numParams = Scr_GetNumParam();

	if (numParams < 7) {
		Com_Error(ERR_DROP, "gsc_test_getAll: not enough parameters, expected 7, got %u", numParams);
	}

	int boolValue = Scr_GetInt(0);
	int intValue = Scr_GetInt(1);
	float floatValue = Scr_GetFloat(2);
	const char* stringValue = Scr_GetString(3);
	const char* localizedStringValue = Scr_GetLocalizedString(4);
	vec3_t vectorValue;
	Scr_GetVector(5, vectorValue);
	void* paramFunction = Scr_GetParamFunction(6);

	if (!boolValue) Com_Error(ERR_DROP, "gsc_test_getAll: boolValue is false");
	if (intValue != 1) Com_Error(ERR_DROP, "gsc_test_getAll: intValue is not 1");
	if (floatValue != 2.222f) Com_Error(ERR_DROP, "gsc_test_getAll: floatValue is not 2.222");
	if (strcmp(stringValue, "variable test string") != 0) Com_Error(ERR_DROP, "gsc_test_getAll: stringValue is not 'variable test string'");
	if (strcmp(localizedStringValue, "Localized text string") != 0) Com_Error(ERR_DROP, "gsc_test_getAll: localizedStringValue is not 'Localized text string'");
	if (vectorValue[0] != 1.0 || vectorValue[1] != 2.0 || vectorValue[2] != 3.0) 
		Com_Error(ERR_DROP, "gsc_test_getAll: vectorValue is not (1.0, 2.0, 3.0)");
	if (paramFunction == NULL) Com_Error(ERR_DROP, "gsc_test_getAll: paramFunction is NULL");

	// Run function that print test was OK
	short thread_id = Scr_ExecThread((int)paramFunction, 0);
	Scr_FreeThread(thread_id);
}
#endif // DEBUG


void gsc_test_onPlayerConnect(int entnum) {
	#if DEBUG
		if (codecallback_test_onPlayerConnect && Scr_IsSystemActive())
		{
			unsigned short thread_id = Scr_ExecEntThreadNum(g_entities[entnum].s.number, 0, codecallback_test_onPlayerConnect, 0);
			Scr_FreeThread(thread_id);
		}
		if (codecallback_test_onPlayerConnect2 && Scr_IsSystemActive())
		{
			Scr_AddString("hello from CoD2x");
			Scr_AddExecEntThreadNum(g_entities[entnum].s.number, 0, codecallback_test_onPlayerConnect2, 1);

			const char* typeName = Scr_GetTypeName(0);
			if (typeName == NULL || strcmp(typeName, "int") != 0) {
				Com_Error(ERR_DROP, "gsc_test_onPlayerConnect: expected type of return value to be 'int', got '%s'", typeName ? typeName : "NULL");
			}
			int i = Scr_GetInt(0);
			if (i != 1338) {
				Com_Error(ERR_DROP, "gsc_test_onPlayerConnect: expected return value to be 1338, got %d", i);
			}
		}
	#endif
}

void gsc_test_onStartGameType() {
	#if DEBUG
		if (codecallback_test_onStartGameType && Scr_IsSystemActive())
		{
			Scr_AddString("hello from CoD2x");
			unsigned short thread_id = Scr_ExecThread(codecallback_test_onStartGameType, 1);
			Scr_FreeThread(thread_id);
		}
		if (codecallback_test_onStartGameType2 && Scr_IsSystemActive())
		{
			Scr_AddString("hello from CoD2x");
			
			Scr_AddExecThread(codecallback_test_onStartGameType2, 1);

			const char* typeName = Scr_GetTypeName(0);
			if (typeName == NULL || strcmp(typeName, "int") != 0) {
				Com_Error(ERR_DROP, "gsc_test_onStartGameType: expected type of return value to be 'int', got '%s'", typeName ? typeName : "NULL");
			}
			int i = Scr_GetInt(0);
			if (i != 1337) {
				Com_Error(ERR_DROP, "gsc_test_onStartGameType: expected return value to be 1337, got %d", i);
			}
		}
		if (codecallback_test_onStartGameType3 && Scr_IsSystemActive())
		{
			Scr_AddExecThread(codecallback_test_onStartGameType3, 0);

			const char* typeName = Scr_GetTypeName(0);
			if (typeName == NULL || strcmp(typeName, "undefined") != 0) {
				Com_Error(ERR_DROP, "gsc_test_onStartGameType: expected type of return value to be 'undefined', got '%s'", typeName ? typeName : "NULL");
			}
		}
	#endif
}

