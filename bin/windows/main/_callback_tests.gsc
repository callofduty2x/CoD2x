callback_test_onStartGameType(p1) {
    assertEx(p1 == "hello from CoD2x", "test_func should receive 'hello from CoD2x', got " + p1);


    v0 = test_returnUndefined();
    assertEx(!isDefined(v0), "test_returnUndefined should return undefined");

    v1 = test_returnBool(true);
    assertEx(v1 == true, "test_returnBool should return true, got " + v1);

    v2 = test_returnInt(42);
    assertEx(v2 == 42, "test_returnInt should return 42, got " + v2);

    v3 = test_returnFloat(3.14);
    assertEx(v3 == 3.14, "test_returnFloat should return 3.14, got " + v3);

    v4 = test_returnString("Hello, World!");
    assertEx(v4 == "Hello, World!", "test_returnString should return 'Hello, World!', got " + v4);

    v5 = test_returnVector((1, 2.2, 333.333));
    assertEx(v5[0] == 1.0 && v5[1] == 2.2 && v5[2] == 333.333, "test_returnVector should return (1.0, 2.2, 333.333), got (" + v5[0] + ", " + v5[1] + ", " + v5[2] + ")");

    array[0] = 10;
    array[1] = 20;
    array[2] = 30;
    arrayReturn = test_returnArray(array);
    assertEx(arrayReturn[0] == 10 && arrayReturn[1] == 20 && arrayReturn[2] == 30, "test_returnArray should return [10, 20, 30], got [" + arrayReturn[0] + ", " + arrayReturn[1] + ", " + arrayReturn[2] + "]");

    varstring = "variable test " + "string";
    test_getAll(true, 1, 2.222, varstring, &"Localized text string", (1, 2, 3), ::print_ok);

    level thread otherTests();
}

callback_test_onStartGameType2(p1) {
    assertEx(p1 == "hello from CoD2x", "test_func should receive 'hello from CoD2x', got " + p1);
    return 1337;
}

callback_test_onStartGameType3() {
    // no return
}

otherTests() {
    //wait 1;


    /****************************************************************************************************************************************************
    * HTTP Fetch
    ****************************************************************************************************************************************************/
    //while(true) {
    //    wait 0.001;
        //http_fetch("http://localhost:8080/api/match/1234", "GET", "", "X-Auth-Token: your-secret-token\r\nHeader2: Value2", 1000, ::onHttpFetchDoneCallback, ::onHttpFetchErrorCallback);
    //}



    /****************************************************************************************************************************************************
    * Websocket
    ****************************************************************************************************************************************************/
    // Websocket test
    /*if (!isDefined(game["ws_connection_id"])) {
        game["ws_connection_id"] = websocket_connect("ws://localhost:8080/ws", "", ::onWebSocketConnect, ::onWebSocketMessage, ::onWebSocketClose, ::onWebSocketError);
    }
    thread websocket_test_thread();*/
}

print_ok() {
    println("=====================");
    println("Script: GSC Test OK");
    println("=====================");

    test_allOk("all ok");
    wait 5;
    test_allOk("all ok");
}


callback_test_onPlayerConnect() {

    println("=====================");
    println("Script: Player connecting: " + self.name);
    println("=====================");

    self thread playerConnect();
}

callback_test_onPlayerConnect2(p1) {
    assertEx(p1 == "hello from CoD2x", "callback_test_onPlayerConnect2 should receive 'hello from CoD2x', got " + p1);
    return 1338;
}

playerConnect() {
    self endon("disconnect");

    self waittill("begin");

    name = self test_playerGetName();
    if (isDefined(name)) {
        assertEx(name == self.name, "test_playerGetName should return the player's name, got " + name);
    } else {
        assertMsg("test_playerGetName should return the player's name, but it returned undefined");
        return;
    }
    println("=====================");
    println("Script: Player name from CoD2x: " + name);
    println("=====================");
    
    /****************************************************************************************************************************************************
    * Player
    ****************************************************************************************************************************************************/
    println("=====================");
    println("Player: ");
    println("  - Name: " + self.name);
    println("  - GUID: " + self getGuid());
    println("  - HWID: " + self getHwid());
    println("  - CDKeyHash: " + self getCDKeyHash());
    println("  - AuthorizationStatus: " + self getAuthorizationStatus());
    println("=====================");
}




/****************************************************************************************************************************************************
* HTTP Fetch
****************************************************************************************************************************************************/
onHttpFetchDoneCallback(status, body, headers) {
    println("=====================");
    println("Script: HTTP Fetch Done Callback");
    println("  Status: " + status);
    println("  Body: " + body);
    println("  Headers:");
    for(i = 0; i < headers.size; i++) {
        println("    " + headers[i]);
    }
    println("=====================");
}
onHttpFetchErrorCallback(error) {
    println("=====================");
    println("Script: HTTP Fetch Error Callback");
    println("  Error: " + error);
    println("=====================");
}





/****************************************************************************************************************************************************
* Websocket
****************************************************************************************************************************************************/
onWebSocketConnect() {
    println("Script: WebSocket #" + game["ws_connection_id"] + " connected");
}
onWebSocketMessage(message) {
    println("Script: WebSocket #" + game["ws_connection_id"] + " message received: " + message);
}
// IS called when connection is closed by remote, by calling websocket_close(), by client if remote is not responding to pings, or on error
onWebSocketClose(isClosedByRemote, isFullyDisconnected) {
    println("Script: WebSocket #" + game["ws_connection_id"] + " closed, isClosedByRemote: " + isClosedByRemote + ", isFullyDisconnected: " + isFullyDisconnected);
    if (isFullyDisconnected)
        game["ws_connection_id"] = -1;
}
onWebSocketError(error) {
    println("Script: WebSocket #" + game["ws_connection_id"] + " error: " + error);
}
websocket_test_thread() {
    i = 0;
    for(;;) {
        wait 0.001;
        if (game["ws_connection_id"] >= 0) {
            websocket_sendText(game["ws_connection_id"], "Hello from CoD2x GSC at " + i);

            if (getCvar("ws") == "close") {
                websocket_close(game["ws_connection_id"]);
                setCvar("ws", "");
            } else if (getCvar("ws") == "send") {
                websocket_sendText(game["ws_connection_id"], "Hello from CoD2x GSC at " + i);
                setCvar("ws", "");
            }
        }
        i++;
    }
}
