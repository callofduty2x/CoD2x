#include "match.h"

#include <string>

#include "shared.h"
#include "cod2_dvars.h"
#include "cod2_cmd.h"
#include "cod2_common.h"
#include "cod2_script.h"
#include "http_client.h"
#include "cod2_server.h"
#include "server.h"
#include "json.h"

dvar_t *match_login; // Cvar to store match login hash
Match match;
extern bool gsc_allowOneTimeLevelChange;


// TODO secure vypsani uuid, aby neslo zneuzit
std::string match_create_json_data()
{
    std::string json;
    json += "{\n";
    json += "  \"type\": \"data\",\n";

    char buf[32];
    time_to_iso8601(match.start_time, buf, sizeof(buf));
    json += "  \"start_time\": \"" + std::string(buf) + "\",\n";

    // Print globalData as individual JSON items
    for (const auto& key : match.progressData.globalData.keys()) {
        json += "  \"" + json_escape_string(key) + "\": \"" + json_escape_string(match.progressData.globalData.at(key)) + "\",\n";
    }

    // Print player data as an array
    json += "  \"players\": [\n";
    bool firstPlayer = true;
    for (const auto& key : match.progressData.playerData.keys()) {
        if (!firstPlayer) json += ",\n";
        firstPlayer = false;
        json += "    {\n";
        bool firstField = true;
        for (const auto& player_key : match.progressData.playerData.at(key).keys()) {
            if (!firstField) json += ",\n";
            firstField = false;
            json += "      \"" + json_escape_string(player_key) + "\": \"" + json_escape_string(match.progressData.playerData.at(key).at(player_key)) + "\"";
        }
        json += "\n    }";
    }
    json += "\n  ]\n";
    json += "}\n";

    return json;
}



bool match_upload_match_data(std::function<void()> onDone, std::function<void(const std::string&)> onError) {
    if (!match.activated) {
        Com_Printf("Match is not activated, cannot upload data.\n");
        return false;
    }

    if (match.uploading) {
        Com_Printf("Match upload already in progress.\n");
        return false;
    }

    // Create JSON data
    std::string json_data = match_create_json_data();
    if (json_data.empty()) {
        Com_Printf("Failed to create JSON data for match upload.\n");
        return false;
    }

    match.uploading = true;

    match.httpClient->postJson(match.url, json_data.c_str(),
        [onError, onDone](const HttpClient::Response& res) {
            match.uploading = false;
            if (res.status != 200 && res.status != 201) {
                Com_Printf("Match uploading error, invalid status: %d\n%s\n", res.status, res.body.c_str());
                Com_Printf("Uploaded JSON data:\n%s\n", match_create_json_data().c_str());
                if (onError) onError("Invalid status: " + std::to_string(res.status));
                return;
            }
            //Com_Printf("Match upload succeeded: %s\n", res.body.c_str());

            if (onDone) onDone();
        },
        [onError](const std::string& error) {
            match.uploading = false;
            Com_Printf("Match uploading error: %s\n", error.c_str());
            if (onError) onError(error);
        }
    );

    match.httpClient->poll();

    return true;
}




bool match_upload_error(const char* error, const char* errorMessage) {
    if (!match.activated && !match.downloading && !match.loading) {
        Com_Printf("Match is not activated, cannot upload data.\n");
        return false;
    }

    if (match.uploadingError) {
        return false;
    }

    // Create JSON data
    std::string json;
    json += "{\n";
    json += "  \"type\": \"error\",\n";
    json += "  \"error\": \"" + json_escape_string(error) + "\",\n";
    json += "  \"errorMessage\": \"" + json_escape_string(errorMessage) + "\"\n";
    json += "}\n";

    match.uploadingError = true;

    // Send POST request to URL
    match.httpClient->postJson(match.url, json.c_str(),
        [](const HttpClient::Response& res) {
            match.uploadingError = false;
            if (res.status != 200 && res.status != 201) {
                Com_Printf("Match error uploading failed, invalid status: %d\n%s\n", res.status, res.body.c_str());
                return;
            }
            //Com_Printf("Match error uploading succeeded: %s\n", res.body.c_str());
        },
        [](const std::string& error) {
            match.uploadingError = false;
            Com_Printf("Match error uploading failed: %s\n", error.c_str());
        }
    );

    match.httpClient->poll();

    return true;
}




MatchPlayer* match_find_player_by_uuid(const char* uuid)
{
    if (!uuid || uuid[0] == '\0') return nullptr;

    for (int j = 0; j < match.data.team1.num_players; j++) {
        if (strcmp(match.data.team1.players[j].id, uuid) == 0) {
            return &match.data.team1.players[j];
        }
    }
    for (int j = 0; j < match.data.team2.num_players; j++) {
        if (strcmp(match.data.team2.players[j].id, uuid) == 0) {
            return &match.data.team2.players[j];
        }
    }
    return nullptr;
}



// Parses match data from a JSON string and fills the match.data struct.
// Returns true on success, false on failure.
bool match_parse_json_match_data(const char* json_str, MatchData* match_data) {
    if (!json_str || json_str[0] == '\0') return false;

    match_data->json = json_str;

    // matchId
    if (!json_get_str(json_str, "$.matchId", match_data->match_id, MAX_ID_LENGTH) || match_data->match_id[0] == '\0') {
        match_data->error = "Missing or invalid 'matchId'";
        return false;
    }

    // maps
    int map_count = 0;
    if (!json_iter_array(json_str, "$.maps", [&](int idx, const char* val, int len) {
        if (len <= 0) {
            match_data->error = "Map name is empty on index " + std::to_string(idx);
            return false;
        }
        
        if (map_count >= MAX_MAPS) {
            match_data->error = "Too many maps (max " STRINGIFY(MAX_MAPS) ")";
            return false;
        }
        
        const char* inner = val;
        int innerLen = len;
        if (innerLen >= 2 && inner[0] == '"' && inner[innerLen - 1] == '"') {
            inner += 1;
            innerLen -= 2;
        }
        if (innerLen <= 0 || innerLen >= MAX_MAP_NAME_LENGTH) {
            match_data->error = "Invalid map name length on index " + std::to_string(idx);
            return false;
        }
        memcpy(match_data->maps[map_count], inner, static_cast<size_t>(innerLen));
        match_data->maps[map_count][innerLen] = '\0';
        map_count++;

        const char* mapName = match_data->maps[map_count - 1];
        if (!SV_MapExists(mapName)) {
            match_data->error = "Map '" + std::string(mapName) + "' does not exist on the server";
            return false;
        }

        return true;
    })) {
        return false;
    }
    match_data->maps_count = map_count;


    // Helper for teams
    auto fill_team = [&](int teamNumber, MatchTeam* team) -> bool {
        char path[64];
        // id
        snprintf(path, sizeof(path), "$.team%i.id", teamNumber);
        if (!json_get_str(json_str, path, team->id, MAX_ID_LENGTH) || team->id[0] == '\0') {
            match_data->error = "Invalid team id";
            return false;
        }

        // name
        snprintf(path, sizeof(path), "$.team%i.name", teamNumber);
        if (!json_get_str(json_str, path, team->name, MAX_NAME_LENGTH) || team->name[0] == '\0') {
            match_data->error = "Invalid team name";
            return false;
        }

        // players
        snprintf(path, sizeof(path), "$.team%i.players", teamNumber);
        team->num_players = 0;
        if (!json_iter_array(json_str, path, [&](int idx, const char* val, int len) {

            if (team->num_players >= MAX_TEAM_PLAYERS) {
                match_data->error = "Too many players in team " + std::to_string(teamNumber) + " (max " STRINGIFY(MAX_TEAM_PLAYERS) ")";
                return false;
            }

            std::string json_str_holder(val, len);
            const char* json_str = json_str_holder.c_str();

            // Fill team number for player for easy access
            team->players[team->num_players].teamNumber = teamNumber;

            // Fill team name for player for easy access
            strncpy(team->players[team->num_players].teamName, team->name, MAX_NAME_LENGTH);
            team->players[team->num_players].teamName[MAX_NAME_LENGTH - 1] = '\0';

            // player id
            if (!json_get_str(json_str, "$.uuid", team->players[team->num_players].id, MAX_ID_LENGTH)) {
                match_data->error = "Invalid player id in team " + std::to_string(teamNumber) + " on index " + std::to_string(idx);
                return false;
            }
            // player name
            if (!json_get_str(json_str, "$.name", team->players[team->num_players].name, MAX_NAME_LENGTH)) {
                match_data->error = "Invalid player name in team " + std::to_string(teamNumber) + " on index " + std::to_string(idx);
                return false;
            }

            // Save any other simple values
            json_iter_object(json_str, "$", [team](const std::string& key, const std::string& value) {
                if (key == "uuid" || key == "name") {
                    return true; // Continue iteration
                }
                team->players[team->num_players].otherData[key] = value;
                return true; // Continue iteration
            });

            team->num_players++;

            return true;
        })) {
            return false;
        }
        if (team->num_players <= 0) {
            match_data->error = "Team " + std::to_string(teamNumber) + " has no players";
            return false;
        }

        // Save any other simple values
        char teamPath[32];
        snprintf(teamPath, sizeof(teamPath), "$.team%i", teamNumber);
        json_iter_object(json_str, teamPath, [team](const std::string& key, const std::string& value) {         
            // Ignore keys we processed already
            if (key == "id" || key == "name" || key == "players") {
                return true; // Continue iteration
            }
            team->otherData[key] = value;
            return true; // Continue iteration
        });

        return true;
    };

    if (!fill_team(1, &match_data->team1) || !fill_team(2, &match_data->team2)) {
        return false;
    }


    // Save any other simple values
    json_iter_object(json_str, "$", [match_data](const std::string& key, const std::string& value) {
        // Ignore keys we processed already
        if (key == "matchId" || key == "maps" || key == "team1" || key == "team2") {
            return true; // Continue iteration
        }
        match_data->otherData[key] = value;
        return true; // Continue iteration
    });


    return true;
}


/**
 * Update match data by downloading it from the server again.
 * This is useful when host-players are added to match while the match is already in progress.
 */
bool match_redownload() {

    // Theres nothing to update
    if (!match.activated) {
        return false;
    }

    match.httpClient->get(
        match.url,
        [](const HttpClient::Response& res) {

            if (res.status != 200 && res.status != 201) {
                Com_Printf("Match redownloading error, invalid status of downloading data: %d\n%s\n", res.status, res.body.c_str());
                return;
            }
            //Com_Printf("GET succeeded: %s\n", res.body.c_str());

            MatchData matchData = MatchData{};

            bool status = match_parse_json_match_data(res.body.c_str(), &matchData);
            if (!status) {
                Com_Printf("Match redownloading error, failed to parse match data:\n%s\n%s\n", res.body.c_str(), matchData.error.c_str());
                match_upload_error("Failed to parse match data", matchData.error.c_str());
                return;
            }

            // Compare matchData with match.data
            // Validate if match id and maps are the same
            if (strcmp(match.data.match_id, matchData.match_id) != 0) {
                Com_Printf("Match redownloading error, match id does not match: %s != %s\n", match.data.match_id, matchData.match_id);
                match_upload_error("Match redownloading error", "Match ID does not match the original one");
                return;
            }
            if (match.data.maps_count != matchData.maps_count) {
                Com_Printf("Match redownloading error, number of maps does not match: %d != %d\n", match.data.maps_count, matchData.maps_count);
                match_upload_error("Match redownloading error", "Number of maps does not match the original one");
                return;
            }
            for (int i = 0; i < match.data.maps_count; i++) {
                if (strcmp(match.data.maps[i], matchData.maps[i]) != 0) {
                    Com_Printf("Match redownloading error, map %d does not match: %s != %s\n", i, match.data.maps[i], matchData.maps[i]);
                    match_upload_error("Match redownloading error", "Map rotation does not match the original one");
                    return;
                }
            }

            // Update match data with new data
            match.data = matchData;

        },
        [](const std::string& error) {
            Com_Printf("Match redownloading error while downloading data: %s\n", error.c_str());
        }
    );

    return true;
}



void match_cancel(const char* reason) {
    if (match.activated) {
        match.canceling = true;
        if (reason != nullptr && reason[0] != '\0') {
            snprintf(match.cancelReason, sizeof(match.cancelReason), "Match was canceled. Reason: %s", reason);
            match.cancelReason[sizeof(match.cancelReason) - 1] = '\0';
        } else {
            match.cancelReason[0] = '\0';
        }
        gsc_allowOneTimeLevelChange = true; // Allow map change for fast_restart even if level change is disabled via GSC script
        Com_Printf("Match is being canceled...\n");
        if (match.cancelReason[0] != '\0') {
            Com_Printf("%s\n", match.cancelReason);
        }
        Cbuf_AddText("fast_restart\n");
    } else {
        Com_Printf("No match is currently active.\n");
    }

    // Clean up match state
    match.downloading = false;
    match.loading = false;
}



void match_finish() {

    if (match.activated && !match.canceling) 
    {
        // Kick all players, match is finished
        for (int i = 0; i < sv_maxclients->value.integer; i++) {
            client_t* client = &svs_clients[i];
            
            if (client && client->state) {
                SV_DropClient(client, "\n^2Match has finished^7");
            }
        }

        Com_Printf("======================================================\n");
        Com_Printf("Match finished successfully.\n");
        Com_Printf("- URL: %s\n", match.url);
        Com_Printf("- Match ID: %s\n", match.data.match_id);
        Com_Printf("- Teams: %s (%s) vs %s (%s)\n", match.data.team1.name, match.data.team1.id, match.data.team2.name, match.data.team2.id);
        Com_Printf("======================================================\n");

        // Cancel request
        match_cancel(nullptr);
    }
}




void match_cmd_usage() {
    Com_Printf("USAGE: match <command> <options>\n");
    Com_Printf("Client:\n");
    Com_Printf("  match login <hash>\n");
    Com_Printf("    <hash> - The hash to login with to the match server; will be removed after disconnect\n");
    Com_Printf("Server:\n");
    Com_Printf("  match create <endpoint>\n");
    Com_Printf("    <endpoint> - URL for getting match data\n");
    Com_Printf("    Example: match create http://web.com/matches/MATCHID1\n");
    Com_Printf("  match status\n");
    Com_Printf("    Displays the current match status\n");
    Com_Printf("  match upload\n");
    Com_Printf("    Forces upload of match data\n");
    Com_Printf("  match cancel\n");
    Com_Printf("    Cancels the current match\n");

}

void match_cmd() {

    int count = Cmd_Argc();
    if (count < 2) {
        match_cmd_usage();
        return;
    }

    const char* command = Cmd_Argv(1);

    if (Q_stricmp(command, "login") == 0) {

        if (dedicated->value.integer > 0) {
            Com_Printf("Match login command is only available for clients.\n");
            return;
        }

        const char* hash = Cmd_Argv(2);

        if (hash[0] == '\0') {
            Com_Printf("Please provide a hash to login.\n");
            return;
        }

        Dvar_SetString(match_login, hash);

        return;

    } else if (Q_stricmp(command, "create") == 0) {

        // match create "http://localhost:8080/api/match/1234"

        // There is a problem when URL is passed without quotes - // is then handled as separator for some reason 

        const char* endpoint = Cmd_Argv(2);
        if (endpoint[0] == '\0') {
            Com_Printf("Please provide an endpoint URL.\n");
            return;
        }

        if (match.downloading) {
            Com_Printf("Match is already downloading, please wait or cancel it first.\n");
            return;
        }

        if (match.activated || match.loading) {
            Com_Printf("Match is already in progress, cancel it first.\n");
            return;
        }

        // Clean up previous httpClient if it exists
        if (match.httpClient != nullptr) {
            delete match.httpClient;
            match.httpClient = nullptr;
        }

        match.data = MatchData{};
        
        // Safely copy URL with bounds checking
        size_t endpoint_len = strlen(endpoint);
        if (endpoint_len >= sizeof(match.url)) {
            Com_Printf("Match endpoint URL too long (max %zu characters)\n", sizeof(match.url) - 1);
            return;
        }
        strncpy(match.url, endpoint, sizeof(match.url) - 1);
        match.url[sizeof(match.url) - 1] = '\0';
        
        match.downloading = true;
        match.loading = false;
        match.activated = false;
        match.uploading = false;
        match.uploadingError = false;
        match.canceling = false;
        match.cancelReason[0] = '\0';
        match.httpClient = new HttpClient();
        match.start_time = time_utc_ms();
        match.start_tick = ticks_ms();
        match.progressData.globalData.clear();
        match.progressData.playerData.clear();

        // Parse headers if exists and add them to httpClient
        const char *headers = Cmd_Argv(3);
        if (headers[0] != '\0') {
            std::string headersStr(headers);
            size_t pos = 0;
            while (true) {
                size_t next = headersStr.find('|', pos);
                std::string header = headersStr.substr(pos, next - pos);
                if (!header.empty())
                    match.httpClient->headers.push_back(header);
                if (next == std::string::npos)
                    break;
                pos = next + 1;
            }
        }

        const char* url = match.url;
        Com_Printf("==============================================\n");
        Com_Printf("Downloading match data from %s...\n", url);
        Com_Printf("==============================================\n");

        match.httpClient->get(
            url,
            [url](const HttpClient::Response& res) {

                // Match downloading was canceled in the meantime
                if (match.downloading == false || match.canceling == true)
                    return;

                if (res.status != 200 && res.status != 201) {
                    Com_Printf("Match creating error, invalid status of downloading data: %d\n%s\n", res.status, res.body.c_str());
                    match.downloading = false;
                    return;
                }
                //Com_Printf("GET succeeded: %s\n", res.body.c_str());

                match.data = MatchData{};
                bool status = match_parse_json_match_data(res.body.c_str(), &match.data);
                if (!status) {
                    Com_Printf("Match creating error, failed to parse match data:\n%s\n%s\n", res.body.c_str(), match.data.error.c_str());
                    match_upload_error("Failed to parse match data", match.data.error.c_str());
                    match.downloading = false;
                    return;
                }

                Com_Printf("==============================================\n");
                Com_Printf("Match downloading from %s completed!\n", url);
                Com_Printf("Loading first map...\n");
                Com_Printf("==============================================\n");

                match.downloading = false;
                match.loading = true;
                
                if (match.data.maps_count > 0) {
                    Cbuf_AddText(va("map %s\n", match.data.maps[0]));
                } else {
                    Cbuf_AddText("map_restart\n");
                }

            },
            [url](const std::string& error) {
                match.downloading = false;
                Com_Printf("==============================================\n");
                Com_Printf("Match creating error while downloading data: %s\n", error.c_str());
                Com_Printf("  - error: %s\n", error.c_str());
                Com_Printf("  - URL: %s\n", url);
                Com_Printf("==============================================\n");
            }
        );

        return;

    } else if (Q_stricmp(command, "status") == 0) {

        if (match.activated) {
            auto progressData = match_create_json_data();

            Com_Printf("Match is currently active.\n");
            Com_Printf("URL: %s\n", match.url);
            Com_Printf("Match data:\n%s\n", match.data.json.c_str());
            Com_Printf("Progress data:\n%s\n", progressData.c_str());
        } else {
            if (match.downloading) {
                Com_Printf("Match is currently downloading...\n");
            } else if (match.loading) {
                Com_Printf("Match is currently loading...\n");
            } else {
                Com_Printf("No match is currently active.\n");
            }
        }

    } else if (Q_stricmp(command, "redownload") == 0) {

        match_redownload();


    } else if (Q_stricmp(command, "upload") == 0) {

        match_upload_match_data();


    } else if (Q_stricmp(command, "cancel") == 0) {

        match_cancel("console command");

        return;

    } else {
        Com_Printf("Unknown match command: %s\n", command);
        match_cmd_usage();
        return;
    }

}


/** Called when the server is started via /map /devmap /map_restart /map_rotate /fast_restart or GSC map_restart(true/false) */
void match_onStartGameType() {
    //Com_Printf("############### Match onStartGameType called\n");
    if (match.loading) {
        match.loading = false;
        match.activated = true;
        
        Com_Printf("======================================================\n");
        Com_Printf("Match started successfully.\n");
        Com_Printf("- URL: %s\n", match.url);
        Com_Printf("- Match ID: %s\n", match.data.match_id);
        Com_Printf("- Teams: %s (%s) vs %s (%s)\n", match.data.team1.name, match.data.team1.id, match.data.team2.name, match.data.team2.id);
        Com_Printf("- Maps: ");
        for (int i = 0; i < match.data.maps_count; ++i) {
            if (i > 0) Com_Printf(", ");
            Com_Printf("%s", match.data.maps[i]);
        }
        Com_Printf("\n");
        Com_Printf("======================================================\n");
    }
}

/**
 * Called before a map change, restart or shutdown that can be triggered from a script or a command.
 * Returns true to proceed, false to cancel the operation. Return value is ignored when shutdown is true.
 * @param fromScript true if map change was triggered from a script, false if from a command.
 * @param bComplete true if map change or restart is complete, false if it's a round restart so persistent variables are kept.
 * @param shutdown true if the server is shutting down, false otherwise.
 * @param source the source of the map change or restart.
 * - is called after OnStopGameType callback, so the mod had time to complete the score (for example when players disconnect before full map end)
 */
bool match_beforeMapChangeOrRestart(bool fromScript, bool bComplete, bool shutdown, sv_map_change_source_e source) {

    // Upload error about server error
    if (shutdown && match.activated && com_errorEntered && com_last_error && com_last_error[0] != '\0') {
        // If there was a fatal error, we cannot upload the match data, just cancel
        match_upload_error("Server shutdown error", com_last_error);
    }

    // Canceling because of finished match, /match cancel or server shutdown
    // GSC script had time to complete the score and upload the final results, so we just cancel
    if (match.canceling || shutdown) {

        if (match.canceling && match.cancelReason[0] != '\0') {
            match_upload_error("Match canceled", match.cancelReason);
        }

        if (shutdown && (match.uploading || match.uploadingError)) {
            // Since server is shutting down, Com_Frame is not called, so we need to poll here to process the pending match data upload
            for(int i = 0; i < 10 && (match.uploading || match.uploadingError) && match.httpClient; i++) {
                match.httpClient->poll(100);
            }
        }
        
        if (match.canceling)
            Com_Printf("Match ended.\n");

        match.canceling = false;
        match.cancelReason[0] = '\0';
        match.uploading = false;
        match.uploadingError = false;
        match.activated = false;
        match.loading = false;
        match.downloading = false;
        match.progressData.globalData.clear();
        match.progressData.playerData.clear();
        
    }

    return true;
}



/** Called every frame on frame start. */
void match_frame() {

    // Run event loop until no connections left
    if (match.httpClient)
         match.httpClient->poll();

    // Check if the match has timed out
    if (ticks_ms() > (match.start_tick + 5000) && (match.loading || match.downloading) && !match.activated) {
        match.loading = false;
        match.downloading = false;
        Com_Printf("Match %s timed out\n", match.loading ? "loading" : "downloading");
    }
}



/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void match_init_client() {
    // Cvar provided to servers
    match_login = Dvar_RegisterString("match_login", "", (dvarFlags_e)(DVAR_USERINFO | DVAR_NOWRITE));
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void match_init() {
    Cmd_AddCommand("match", match_cmd); 

    #if DEBUG
    if (dedicated->value.integer > 0) {
        for (int i = 0; i < 20; i++) {
            Com_Printf("                                        \n");
        }

        //Cbuf_AddText("match create \"http://localhost:8080/api/match/1234\" \"X-Auth-Token: your-secret-token|Header2: Value2\"\n");
        //Cbuf_AddText("match create \"https://fpschallenge.eu/api/v2/cod2/match/200415\"\n");

        // match create "http://localhost:8080/api/match/1234" "X-Auth-Token: your-secret-token|Header2: Value2"
        // match create "https://fpschallenge.eu/api/v2/cod2/match/200415"
    }
    #endif
}

/** Called before the entry point is called. Used to patch the memory. */
void match_patch() {

}