#include "gsc_match.h"

#include "shared.h"
#include "cod2_common.h"
#include "cod2_script.h"
#include "server.h"
#include "match.h"

int codecallback_test_match_onStartGameType;
int codecallback_test_match_onPlayerConnect;
int codecallback_test_match_onStopGameType;


void gsc_match_playerGetSetData(int action, scr_entref_t ref) {
	int id = ref.entnum;

	if ( id >= MAX_CLIENTS )
	{
		Scr_Error(va("entity %d is not a player", id));
		Scr_AddUndefined();
		return;
	}

	client_t *client = &svs_clients[id];

	// Not connected
	if (client->state < CS_CONNECTED) {
		Scr_AddBool(false);
		return;
	}

	if (!match.activated) {
		Scr_AddBool(false);
		return;
	}

	// Find player by UUID
	const char* player_uuid = Info_ValueForKey(client->userinfo, "match_login");
	MatchPlayer* player = match_find_player_by_uuid(player_uuid);

	// Create player array KEY
	// If UUID is set and is valid, use UUID
	// If UUID is empty or not within team, user player's guid as identification, if guid is empty, use name as fallback
	const char* array_key = va("UUID_%s", player_uuid);
	if (array_key == nullptr || player == nullptr) {
		if (client->guid != 0)
			array_key = va("GUID_%i", client->guid);
		else
			array_key = va("NAME_%s", client->name);
	}

	// Update player data
	match.progressData.playerData[array_key]["key"] = array_key;
	match.progressData.playerData[array_key]["uuid"] = player_uuid ? player_uuid : "";
	// If this is first time we save player data, save also additional data about player
	if (!match.progressData.playerData.contains(array_key)) {
		char buf[32];
		time_to_iso8601(time_utc_ms(), buf, sizeof(buf));
		match.progressData.playerData[array_key]["first_time"] = buf;
	}
	match.progressData.playerData[array_key]["name"] = (player == nullptr) ? client->name : player->name;
	match.progressData.playerData[array_key]["team"] = (player == nullptr) ? "" : va("team%i", player->teamNumber);
	match.progressData.playerData[array_key]["team_name"] = (player == nullptr) ? "" : player->teamName;
	if (player == nullptr) {
		match.progressData.playerData[array_key]["debug"] = (player_uuid && player_uuid[0]) ? "Player's UUID is not part of any team" : "Player did not login with /match login <uuid>";
	} else {
		match.progressData.playerData[array_key].erase("debug");
	}

	if (player != nullptr) {
		for (const auto& key : player->otherData.keys()) {
			match.progressData.playerData[array_key][key] = player->otherData.at(key);
		}
	}



	// Get
	if (action == 0) 
	{
		const char* key = Scr_GetString(0);

		if (!match.progressData.playerData.contains(array_key) || !match.progressData.playerData[array_key].contains(key)) {
			Scr_AddString("");
			return;
		}

		const std::string& value = match.progressData.playerData[array_key][key];
		Scr_AddString(value.c_str());

		//Com_DPrintf("gsc_match_playerGetData(%s) for %d => %s\n", key, id, value.c_str());

	// Set
	} else {

		// Loop through all key-value pairs in parameters
		unsigned int numParams = Scr_GetNumParam();
		for (unsigned int i = 0; i + 1 < numParams; i += 2) {
			const char* key = Scr_GetString(i);
			const char* value = Scr_GetString(i + 1);

			// Save player data
			match.progressData.playerData[array_key][key] = value;

			//Com_DPrintf("gsc_match_playerSetData(%s, %s) for %d\n", key, value, id);
		}

		Scr_AddBool(true);
	}

}



/**
 * Set player data for the match
 * <player> matchSetData(<key>, <value>[, <key>, <value>, ...]);
 */
void gsc_match_playerSetData(scr_entref_t ref) {
	unsigned int numParams = Scr_GetNumParam();

	if (numParams < 2) {
		Scr_Error(va("matchSetData: not enough parameters, expected 2, got %u", numParams));
		Scr_AddUndefined();
		return;
	}
	// Check if the number of parameters is even and at least 2
	if (numParams % 2 != 0) {
		Scr_Error(va("matchSetData: expected even number of parameters, got %u", numParams));
		Scr_AddUndefined();
		return;
	}

	// Set player data
	gsc_match_playerGetSetData(1, ref);
}


void gsc_match_playerGetData(scr_entref_t ref) {
	unsigned int numParams = Scr_GetNumParam();

	if (numParams != 1) {
		Scr_Error(va("matchGetData: expected 1 parameter, got %u", numParams));
		Scr_AddUndefined();
		return;
	}

	// Get player data
	gsc_match_playerGetSetData(0, ref);
}



/**
 * Check if a player is allowed to join the match
 * <player> matchIsAllowed();
 * returns true if the player is allowed, false otherwise
 */
void gsc_match_playerIsAllowed(scr_entref_t ref) {
	int id = ref.entnum;

	//Com_DPrintf("gsc_match_playerIsAllowed() for %d\n", id);

	if ( id >= MAX_CLIENTS )
	{
		Scr_Error(va("entity %d is not a player", id));
		Scr_AddUndefined();
		return;
	}

	client_t *client = &svs_clients[id];

	if (!match.activated) {
		Scr_AddBool(false);
		return;
	}

	const char* login_uuid = Info_ValueForKey(client->userinfo, "match_login");
	if (!login_uuid || login_uuid[0] == '\0') {
		Scr_AddBool(false);
		return;
	}

	// Check if the id matches any of the allowed players
	bool found = false;
	for (int j = 0; j < match.data.team1.num_players; j++) {
		if (strcmp(match.data.team1.players[j].id, login_uuid) == 0) {
			found = true;
			break;
		}
	}
	if (!found) {
		for (int j = 0; j < match.data.team2.num_players; j++) {
			if (strcmp(match.data.team2.players[j].id, login_uuid) == 0) {
				found = true;
				break;
			}
		}
	}
	if (!found) {
		Scr_AddBool(false);
		return;
	}

	Scr_AddBool(true);
}










/**
 * Upload match data to the server
 * level matchUploadData();
 */
void gsc_match_uploadData() {
	//Com_DPrintf("gsc_match_uploadData()\n");

	if (!match.activated) {
		Scr_AddBool(false);
		return;
	}

	unsigned int numParams = Scr_GetNumParam();

	void* callbackDone = (numParams >= 1) ? Scr_GetParamFunction(0) : nullptr;
	void* callbackError = (numParams >= 2) ? Scr_GetParamFunction(1) : nullptr;

	// Upload match data
	match_upload_match_data(
		[callbackDone]() {
			if (callbackDone && Scr_IsSystemActive()) {
				short thread_id = Scr_ExecThread((int)callbackDone, 0);
				Scr_FreeThread(thread_id);
			}
		}, 
		[callbackError](const std::string& error) {
			if (callbackError && Scr_IsSystemActive()) {
				Scr_AddString(error.c_str());
				unsigned short thread_id = Scr_ExecThread((int)callbackError, 1);
				Scr_FreeThread(thread_id);
			}
		}
	);

	Scr_AddBool(true);
}


void gsc_match_getSetData(int action) {
	if (!match.activated) {
		Scr_AddBool(false);
		return;
	}

	// Update predefined data
	match.progressData.globalData["match_id"] = match.data.match_id;
	match.progressData.globalData["team1_id"] = match.data.team1.id;
	match.progressData.globalData["team2_id"] = match.data.team2.id;
	match.progressData.globalData["team1_name"] = match.data.team1.name;
	match.progressData.globalData["team2_name"] = match.data.team2.name;

	// Add other data from match data
	for (const auto& key : match.data.otherData.keys()) {
		match.progressData.globalData[key] = match.data.otherData.at(key);
	}
	// Add team other data
	for (const auto& key : match.data.team1.otherData.keys()) {
		match.progressData.globalData["team1_" + key] = match.data.team1.otherData.at(key);
	}
	for (const auto& key : match.data.team2.otherData.keys()) {
		match.progressData.globalData["team2_" + key] = match.data.team2.otherData.at(key);
	}

	// Get
	if (action == 0) {

		const char* key = Scr_GetString(0);

		if (!match.progressData.globalData.contains(key)) {

			if (strcmp(key, "team1_player_uuids") == 0) {
				Scr_MakeArray();
				for (int i = 0; i < match.data.team1.num_players; i++) {
					Scr_AddString(match.data.team1.players[i].id);
					Scr_AddArray();
				}
			}
			else if (strcmp(key, "team2_player_uuids") == 0) {
				Scr_MakeArray();
				for (int i = 0; i < match.data.team2.num_players; i++) {
					Scr_AddString(match.data.team2.players[i].id);
					Scr_AddArray();
				}
			}
			else if (strcmp(key, "team1_player_names") == 0) {
				Scr_MakeArray();
				for (int i = 0; i < match.data.team1.num_players; i++) {
					Scr_AddString(match.data.team1.players[i].name);
					Scr_AddArray();
				}
			}
			else if (strcmp(key, "team2_player_names") == 0) {
				Scr_MakeArray();
				for (int i = 0; i < match.data.team2.num_players; i++) {
					Scr_AddString(match.data.team2.players[i].name);
					Scr_AddArray();
				}
			}
			else if (strcmp(key, "maps") == 0) {
				Scr_MakeArray();
				for (int i = 0; i < match.data.maps_count; i++) {
					Scr_AddString(match.data.maps[i]);
					Scr_AddArray();
				}
			} else {			
				Scr_AddString("");
			}
			return;
		}

		// Get the value for the key
		auto value = match.progressData.globalData.at(key);
		Scr_AddString(value.c_str());

		//Com_DPrintf("gsc_match_getData(%s) => %s\n", key, value.c_str());

	// Set
	} else {

		// Loop through all key-value pairs in parameters
		unsigned int numParams = Scr_GetNumParam();
		for (unsigned int i = 0; i + 1 < numParams; i += 2) {
			const char* key = Scr_GetString(i);
			const char* value = Scr_GetString(i + 1);

			// Save global data
			match.progressData.globalData[key] = value;

			//Com_DPrintf("gsc_match_setData(%s, %s)\n", key, value);
		}

		Scr_AddBool(true);
	}
}


/**
 * Set match data
 * <player> matchSetData(<key>, <value>[, <key>, <value>, ...]);
 */
void gsc_match_setData() {
	unsigned int numParams = Scr_GetNumParam();

	if (numParams < 2) {
		Scr_Error(va("matchSetData: not enough parameters, expected 2, got %u", numParams));
		return;
	}

	// Check if the number of parameters is even and at least 2
	if (numParams % 2 != 0) {
		Scr_Error(va("matchSetData: expected even number of parameters, got %u", numParams));
		return;
	}

	// Set data
	gsc_match_getSetData(1);
}


/**
 * Get match data for a player
 * "level matchGetData(<key>)"
 */
void gsc_match_getData() {
	unsigned int numParams = Scr_GetNumParam();

	if (numParams < 1) {
		Scr_Error(va("matchGetData: not enough parameters, expected 1, got %u", numParams));
		return;
	}
	
	// Get data
	gsc_match_getSetData(0);
}

/**
 * Update match data by downloading it from the server again
 * level matchRedownloadData();
 * return true if request was successful or false if anything fails
 */
void gsc_match_redownloadData() {
	//Com_DPrintf("gsc_match_redownloadData()\n");

	bool ok = match_redownload();
	Scr_AddBool(ok);
}


/**
 * Clear match progress data
 * level matchClearData();
 */
void gsc_match_clearData() {
	//Com_DPrintf("gsc_match_clearData()\n");

	match.progressData.globalData.clear();
	match.progressData.playerData.clear();
	Scr_AddBool(true);
}


/**
 * Check if the match is activated
 * level matchIsActivated();
 */
void gsc_match_isActivated() {
	//Com_DPrintf("gsc_match_isActivated() => %d\n", match.activated);

	Scr_AddBool(match.activated);
}


/**
 * Cancel the ongoing match, then doing fast_restart.
 * When reason is provided, it will be sent to the match server as an error message.
 * level matchCancel(<reason>);
 */
void gsc_match_cancel() {
	//Com_DPrintf("gsc_match_cancel()\n");

	if (!match.activated) {
		Scr_AddBool(false);
		return;
	}

	unsigned int numParams = Scr_GetNumParam();
	const char* reason = (numParams >= 1) ? Scr_GetString(0) : nullptr;

	match_cancel(reason);

	Scr_AddBool(true);
}


/*
 * Finish the match by kicking all players and cancel the match, then doing fast_restart
 * level matchFinish();
 */
void gsc_match_finish() {
	//Com_DPrintf("gsc_match_finish()\n");

	if (!match.activated) {
		Scr_AddBool(false);
		return;
	}

	match_finish();

	Scr_AddBool(true);
}


/**
 * Called before a map change, restart or shutdown that can be triggered from a script or a command.
 * Returns true to proceed, false to cancel the operation. Return value is ignored when shutdown is true.
 * @param fromScript true if map change was triggered from a script, false if from a command.
 * @param bComplete true if map change or restart is complete, false if it's a round restart so persistent variables are kept.
 * @param shutdown true if the server is shutting down, false otherwise.
 * @param source the source of the map change or restart.
 */
bool gsc_match_beforeMapChangeOrRestart(bool fromScript, bool bComplete, bool shutdown, sv_map_change_source_e source) {
	
	#if DEBUG
		if (codecallback_test_match_onStopGameType && Scr_IsSystemActive())
		{
			const char* sourceStr = sv_map_change_source_to_string(source);
			Scr_AddString(sourceStr);
			Scr_AddBool(shutdown);
			Scr_AddBool(bComplete);
			Scr_AddBool(fromScript);
			unsigned short thread_id = Scr_ExecThread(codecallback_test_match_onStopGameType, 4);
			Scr_FreeThread(thread_id);
		}
	#endif

	return true;
}

void gsc_match_onPlayerConnect(int entnum) {
	#if DEBUG
		if (codecallback_test_match_onPlayerConnect && Scr_IsSystemActive())
		{
			unsigned short thread_id = Scr_ExecEntThreadNum(g_entities[entnum].s.number, 0, codecallback_test_match_onPlayerConnect, 0);
			Scr_FreeThread(thread_id);
		}
	#endif
}

void gsc_match_onStartGameType() {
	#if DEBUG
		if (codecallback_test_match_onStartGameType && Scr_IsSystemActive())
		{
			unsigned short thread_id = Scr_ExecThread(codecallback_test_match_onStartGameType, 0);
			Scr_FreeThread(thread_id);
		}
	#endif
}

