#include "gsc.h"

#include <stdarg.h> // va_list, va_start, va_end

#include "shared.h"
#include "gsc_test.h"
#include "gsc_match.h"
#include "gsc_http.h"
#include "gsc_websocket.h"
#include "gsc_player.h"
#include "cod2_common.h"
#include "cod2_script.h"
#include "cod2_math.h"
#include "cod2_server.h"
#include "server.h"
#include "match.h"
#include "http_client.h"



int codecallback_OnStopGameType = 0;
bool gsc_allowOneTimeLevelChange = false;


// Array of custom methods for entities
scr_method_t scriptMethods[] =
{
	#if DEBUG
	{"test_playerGetName", gsc_test_playerGetName, 0},
	#endif

	{"getIp", gsc_player_getip, 0},
	{"getHWID", gsc_player_playerGetHWID, 0},
	{"getCDKeyHash", gsc_player_playerGetCDKeyHash, 0},
	{"getAuthorizationStatus", gsc_player_playerGetAuthorizationStatus, 0},

	{"getViewOrigin", gsc_player_getViewOrigin, 0},
	{"getStance", gsc_player_getStance, 0},
	{"isUsingTurret", gsc_player_isUsingTurret, 0},

	{"matchPlayerGetData", gsc_match_playerGetData, 0},
	{"matchPlayerSetData", gsc_match_playerSetData, 0},
	{"matchPlayerIsAllowed", gsc_match_playerIsAllowed, 0},

	{NULL, NULL, 0} // Terminator
};

// Array of custom script functions
scr_function_t scriptFunctions[] = {

	#if DEBUG
	{"test_returnUndefined", gsc_test_returnUndefined, 0},
	{"test_returnBool", gsc_test_returnBool, 0},
	{"test_returnInt", gsc_test_returnInt, 0},
	{"test_returnFloat", gsc_test_returnFloat, 0},
	{"test_returnString", gsc_test_returnString, 0},
	{"test_returnVector", gsc_test_returnVector, 0},
	{"test_returnArray", gsc_test_returnArray, 0},
	{"test_getAll", gsc_test_getAll, 0},
	{"test_allOk", gsc_test_allOk, 0},
	#endif

	{"http_fetch", gsc_http_fetch, 0},

	{"websocket_connect", gsc_websocket_connect, 0},
	{"websocket_sendText", gsc_websocket_sendText, 0},
	{"websocket_close", gsc_websocket_close, 0},

	{"matchUploadData", gsc_match_uploadData, 0},
	{"matchSetData", gsc_match_setData, 0},
	{"matchGetData", gsc_match_getData, 0},
	{"matchRedownloadData", gsc_match_redownloadData, 0},
	{"matchClearData", gsc_match_clearData, 0},
	{"matchIsActivated", gsc_match_isActivated, 0},
	{"matchCancel", gsc_match_cancel, 0},
	{"matchFinish", gsc_match_finish, 0},

	{NULL, NULL, 0}
};

// Array of custom callbacks
callback_t callbacks[] =
{
	#if DEBUG
	{ &codecallback_test_onStartGameType, 			"_callback_tests", "callback_test_onStartGameType", true},
	{ &codecallback_test_onStartGameType2, 			"_callback_tests", "callback_test_onStartGameType2", true},
	{ &codecallback_test_onStartGameType3, 			"_callback_tests", "callback_test_onStartGameType3", true},
	{ &codecallback_test_onPlayerConnect, 			"_callback_tests", "callback_test_onPlayerConnect", true},
	{ &codecallback_test_onPlayerConnect2, 			"_callback_tests", "callback_test_onPlayerConnect2", true},

	{ &codecallback_test_match_onStartGameType, 	"_callback_match", "callback_match_onStartGameType", true},
	{ &codecallback_test_match_onPlayerConnect, 	"_callback_match", "callback_match_onPlayerConnect", true},
	{ &codecallback_test_match_onStopGameType, 		"_callback_match", "callback_match_onStopGameType", true},
	#endif

	// Callback to do any action before current level is exited after map change / restart / kill
	{ &codecallback_OnStopGameType, 	"maps/mp/gametypes/_callbacksetup", "CodeCallback_StopGameType", false},
};

// This function is called when scripts are being compiled and function names are being resolved.
xfunction_t Scr_GetCustomFunction(const char **fname, int *fdev)
{
	// Try to find original function
	xfunction_t m = Scr_GetFunction(fname, fdev);
	if ( m )
		return m;

	// Try to find new custom function
	for ( int i = 0; scriptFunctions[i].name; i++ )
	{
		if ( strcasecmp(*fname, scriptFunctions[i].name) )
			continue;

		scr_function_t func = scriptFunctions[i];
		*fname = func.name;
		*fdev = func.developer;
		return func.call;
	}

	return NULL;
}

// This function is called when scripts are being compiled and method names are being resolved.
xmethod_t Scr_GetCustomMethod(const char **fname, qboolean *fdev)
{
	// Try to find original method
	xmethod_t m = Scr_GetMethod(fname, fdev);
	if ( m )
		return m;

	// Try to find new custom method
	for ( int i = 0; scriptMethods[i].name; i++ )
	{
		if ( strcasecmp(*fname, scriptMethods[i].name) )
			continue;

		scr_method_t func = scriptMethods[i];

		*fname = func.name;
		*fdev = func.developer;

		return func.call;
	}

	return NULL;
}

// Called when CodeCallback_PlayerConnect is called
void gsc_onPlayerConnect(int entnum) {
	gsc_test_onPlayerConnect(entnum);
	gsc_match_onPlayerConnect(entnum);
}
short CodeCallback_PlayerConnect_Win32(int entnum, int classnum, int paramcount) {
	int handle; ASM( movr, handle, "eax" );
	gsc_onPlayerConnect(entnum);
	short ret;
	ASM_CALL(RETURN(ret), 0x00482190, 3, EAX(handle), PUSH(entnum), PUSH(classnum), PUSH(paramcount));
	return ret;
}
void CodeCallback_PlayerConnect_Linux(gentity_t *ent) {
	gsc_onPlayerConnect(ent->s.number);
	ASM_CALL(RETURN_VOID, 0x08118350, 1, PUSH(ent));
}


// Called when CodeCallback_StartGameType is called
void gsc_onStartGameType() {
	gsc_test_onStartGameType();
	match_onStartGameType();
	gsc_match_onStartGameType();
}
short CodeCallback_StartGameType_Win32(int paramcount) {
	int handle; ASM( movr, handle, "eax" );
	gsc_onStartGameType();
	short ret;
	ASM_CALL(RETURN(ret), 0x00482080, 1, EAX(handle), PUSH(paramcount));
	return ret;
}
void CodeCallback_StartGameType_Linux() {
	gsc_onStartGameType();
	ASM_CALL(RETURN_VOID, 0x08118322);
}


// Loading callback handles from scripts
void GScr_LoadGameTypeScript() {
	// Load original callbacks
	ASM_CALL(RETURN_VOID, ADDR(0x00503f90, 0x08110286), 0);

	// Load new custom callbacks
	for (size_t i = 0; i < sizeof(callbacks)/sizeof(callbacks[0]); i++)
	{
		callback_t *cb = &callbacks[i];
		*cb->variable = Scr_GetFunctionHandle(cb->scriptName, cb->functionName, cb->isNeeded);
	}
}

// Called on map restart / fast restart (bComplete is 1), and for gsc map restart with persist variables (bComplete is 0)
void Scr_ShutdownSystem(uint8_t sys, int bComplete) {
	Com_Printf("Shutting down script system (complete: %d)\n", bComplete);

	WL(
		ASM_CALL(RETURN_VOID, 0x00482870, 1, PUSH(bComplete)),
		ASM_CALL(RETURN_VOID, 0x08084522, 2, PUSH(sys), PUSH(bComplete))
	)
}
void Scr_ShutdownSystem_Win32(int bComplete) {
	Scr_ShutdownSystem(0, bComplete);
}


/**
 * Called before a map change, restart or shutdown that can be triggered from a script or a command.
 * Returns true to proceed, false to cancel the operation. Return value is ignored when shutdown is true.
 * @param fromScript true if map change was triggered from a script, false if from a command.
 * @param bComplete true if map change or restart is complete, false if it's a round restart so persistent variables are kept.
 * @param shutdown true if the server is shutting down, false otherwise.
 * @param source the source of the map change or restart.
 */
bool gsc_beforeMapChangeOrRestart(bool fromScript, bool bComplete, bool shutdown, sv_map_change_source_e source) {
	
	bool allowMapChange = true;

	// Call the OnStopGameType callback if it exists
	if (codecallback_OnStopGameType && Scr_IsSystemActive())
	{
		// Convert source to string
		const char* sourceStr = sv_map_change_source_to_string(source);

		Com_DPrintf("------- StopGameType(fromScript: %d, bComplete: %d, shutdown: %d, source: %s) -------\n", fromScript, bComplete, shutdown, sourceStr);
		Scr_AddString(sourceStr);
		Scr_AddBool(shutdown);
		Scr_AddBool(bComplete);
		Scr_AddBool(fromScript);
		Scr_AddExecThread(codecallback_OnStopGameType, 4);
		
		const char* typeName = Scr_GetTypeName(0);
		if (typeName != NULL && strcmp(typeName, "int") == 0) { // function might return undefined
			int returnValue = Scr_GetInt(0);		
			if (returnValue == 0) {
				allowMapChange = false;
			}
		}

		Scr_ClearOutParams();

		Com_DPrintf("------- StopGameType returned allowMapChange: %d -------\n", allowMapChange);
	}

	if (!allowMapChange && shutdown == false && gsc_allowOneTimeLevelChange == false) {
		Com_Printf("Map change is disabled (prevented by script)\n");
		return false;
	}

	gsc_allowOneTimeLevelChange = false;

	return true;
}

/** Called every frame on frame start. */
void gsc_frame() {
	gsc_http_frame();
	gsc_websocket_frame();
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void gsc_init() {
	gsc_http_init();
	gsc_websocket_init();
}

/** Called before the entry point is called. Used to patch the memory. */
void gsc_patch()
{
    patch_call(ADDR(0x46E7BF, 0x08070BE7), (int)Scr_GetCustomFunction);
    patch_call(ADDR(0x46EA03, 0x08070E0B), (int)Scr_GetCustomMethod);
	patch_call(ADDR(0x5043fa, 0x0811048c), (int)GScr_LoadGameTypeScript);
	patch_call(ADDR(0x004fc874, 0x08109572), (int)WL(Scr_ShutdownSystem_Win32, Scr_ShutdownSystem));

	WL(
		patch_call(0x004fc79a, (unsigned int)CodeCallback_StartGameType_Win32),
		patch_call(0x08109499, (unsigned int)CodeCallback_StartGameType_Linux);
	);
	WL(
		patch_call(0x004fe43a, (unsigned int)CodeCallback_PlayerConnect_Win32),
		patch_call(0x080f9091, (unsigned int)CodeCallback_PlayerConnect_Linux);
	);
}