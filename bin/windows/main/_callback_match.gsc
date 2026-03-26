callback_match_onStartGameType() {

    if (matchIsActivated()) {

        if (!isDefined(game["match_initialized"])) {
            matchClearData();
            game["match_initialized"] = true;
        }

        matchSetData(
            "team1_score", "0",
            "team2_score", "0",
            "map", getCvar("mapname")
        );

        wait 1;

        println("=====================");
        println("callback_match_onStartGameType()");
        println("Script: Match ID: " + matchGetData("match_id"));
        println("Script: Team 1 ID: " + matchGetData("team1_id"));
        println("Script: Team 2 ID: " + matchGetData("team2_id"));
        println("Script: Team 1 Name: " + matchGetData("team1_name"));
        println("Script: Team 2 Name: " + matchGetData("team2_name"));
        println("Script: Team 1 Tag: " + matchGetData("team1_tag"));
        println("Script: Team 2 Tag: " + matchGetData("team2_tag"));
        println("Script: Team 1 Score: " + matchGetData("team1_score"));
        println("Script: Team 2 Score: " + matchGetData("team2_score"));
        println("Script: Map: " + matchGetData("map"));
        println("Script: Maps");
        for(i = 0; i < matchGetData("maps").size; i++) {
            println("   " + matchGetData("maps")[i]);
        }
        println("Script: Team 1 Players:");
        team1_player_uuids = matchGetData("team1_player_uuids");
        team1_player_names = matchGetData("team1_player_names");
        for(i = 0; i < team1_player_names.size; i++) {
            println("   " + team1_player_names[i] + "    (" + team1_player_uuids[i] + ")");
        }
        println("Script: Team 2 Players:");
        team2_player_uuids = matchGetData("team2_player_uuids");
        team2_player_names = matchGetData("team2_player_names");
        for(i = 0; i < team2_player_names.size; i++) {
            println("   " + team2_player_names[i] + "    (" + team2_player_uuids[i] + ")");
        }

        // Read other custom data
        println("Script: Other Data:");
        playersCount = matchGetData("playersCount");
        println("   playersCount: " + playersCount);
        forceNickNames = matchGetData("forceNickNames");
        println("   forceNickNames: " + forceNickNames);
        team1_specialTeamKey = matchGetData("team1_specialTeamKey");
        println("   team1_specialTeamKey: " + team1_specialTeamKey);
        team2_specialTeamKey = matchGetData("team2_specialTeamKey");
        println("   team2_specialTeamKey: " + team2_specialTeamKey);
        demoUploadURL = matchGetData("demoUploadURL");
        println("   demoUploadURL: " + demoUploadURL);

        //matchCancel("Some error occurred");

        matchUploadData(::onUploadDone, ::onUploadError);

        for(;;) {
            wait 2;

            //matchUploadData(::onUploadDone, ::onUploadError);
        }
        
        println("=====================");

    } else {
        println("=====================");
        println("callback_match_onStartGameType()");
        println("  Match is not activated.");
        println("=====================");
    }
}


callback_match_onPlayerConnect() {
    self endon("disconnect");

    matchRedownloadData();

    println("=====================");
    println("matchRedownloadData()");
    println("=====================");

    self waittill("begin");

    if (matchIsActivated()) {

        println("=====================");
        println("Script: Player isAllowed: " + self matchPlayerIsAllowed());
        println("Script: Player UUID: " + self matchPlayerGetData("uuid"));
        println("Script: Player name: " + self matchPlayerGetData("name"));
        println("Script: Player team: " + self matchPlayerGetData("team")); // team1 / team2 / ""
        println("Script: Player team name: " + self matchPlayerGetData("team_name"));
        println("=====================");

        wait 1;

        // Set data every player will contain
        // Adding first key-value pair will add that player on match progress data
        self matchPlayerSetData(
            "score", "0",
            "kills", "0",
            "deaths", "0",
            "assists", "0",
            "plants", "0",
            "defuses", "0"
        );
        
        wait 1;

        //matchUploadData(::onUploadDone, ::onUploadError);

        wait 1;

        //matchUploadData();

        // Restore previous value if player reconnects
        i = 0;
        i2 = self matchPlayerGetData("myValue");
        if (i2 != "") {
            self matchPlayerSetData("myValue", i2);
            println("Restored myValue: " + i2);
            i = int(i2);
        }


        
        for (;;) {
            wait 2;
            self matchPlayerSetData("myValue", i);
            i2 = self matchPlayerGetData("myValue");
            assertEx(i == int(i2), "matchGetData should return the value set by matchSetData, got " + i2 + " expected " + i);

            //println("Script: myValue set to " + i);
            i++;

            // Easy way how to to test map end
            if (self.name == "endmap") {
                exitLevel(false); // map rotate
                return;
            }
        }

    } else {
        println("=====================");
        println("callback_match_onStartGameType()");
        println("  Match is not activated.");
        println("=====================");
    }
}


onUploadDone() {

    println("=====================");
    println("Script: onUploadDone()");
    println("    Match data uploaded successfully.");
    println("=====================");

}

onUploadError(error) {

    println("=====================");
    println("Script: onUploadError()");
    println("    Match data upload failed.");
    println("    Error: " + error);
    println("=====================");

}


/*================
Called by code before map change, map restart, or server shutdown.
  fromScript: true if map change was triggered from a script, false if from a command.
  bComplete: true if map change or restart is complete, false if it's a round restart so persistent variables are kept.
  shutdown: true if the server is shutting down, false otherwise.
  source: "map", "fast_restart", "map_restart", "map_rotate", "shutdown"
================*/
callback_match_onStopGameType(fromScript, bComplete, shutdown, source) {
    println("=====================");
    println("callback_match_onStopGameType()");
    println("  fromScript: " + fromScript);
    println("  bComplete: " + bComplete);
    println("  shutdown: " + shutdown);
    println("  source: " + source);
    println("  matchIsActivated(): " + matchIsActivated());
    println("=====================");

    // Finish player score to valid values
    if (bComplete && matchIsActivated()) {
        matchSetData("team1_score", 13);
        matchSetData("team2_score", 9);
        
        matchUploadData();
    }

    
}