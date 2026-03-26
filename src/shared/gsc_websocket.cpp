#include "gsc_websocket.h"

#include <vector>

#include "shared.h"
#include "cod2_common.h"
#include "cod2_cmd.h"
#include "cod2_script.h"
#include "server.h"
#include "websocket.h"

WebSocketClient* gsc_websocket_test = nullptr;
WebSocketClient* gsc_websocket_client = nullptr;

// Maximum number of simultaneous websocket clients
constexpr int MAX_WEBSOCKET_CLIENTS = 16;

// Array of pointers to WebSocketClient, nullptr means slot is free
WebSocketClient* gsc_websocket_clients[MAX_WEBSOCKET_CLIENTS] = {nullptr};




/**
 * Connects to a ws:// or wss:// URL with optional headers and callbacks.
 * Returns connection index or -1 on error.
 * USAGE: websocket_connect(url, headers, onConnectCallback, onMessageCallback, onCloseCallback, onErrorCallback, reconnectDelayMs=2000, pingIntervalMs=15000)
 * - url: WebSocket URL to connect to (ws:// or wss://)
 * - headers: Optional additional HTTP headers to include in the handshake, separated by \r\n
 * - onConnectCallback: Function to call when connection is established. No parameters.
 * - onMessageCallback: Function to call when a TEXT message is received. One string parameter: the message.
 * - onCloseCallback: Function to call when connection is closed. One boolean parameter: true if closed by remote, false if closed by client.
 * - onErrorCallback: Function to call when an error occurs. One string parameter: the error message.
 * - reconnectDelayMs: Optional delay in milliseconds before attempting to reconnect after a disconnect. Default is 2000 ms.
 * - pingIntervalMs: Optional interval in milliseconds between ping messages to maintain the connection. Default is 15000 ms. Set to 0 to disable pings.
 */
void gsc_websocket_connect() {
	if (Scr_GetNumParam() < 6) {
		Scr_Error(va("websocket_connect: not enough parameters, expected 6, got %u", Scr_GetNumParam()));
		Scr_AddInt(-1);
		return;
	}

	// Find free slot
	int idx = -1;
	for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; ++i) {
		if (gsc_websocket_clients[i] == nullptr) {
			idx = i;
			break;
		}
	}
	if (idx == -1) {
		Scr_Error("No free websocket client slots available.");
		Scr_AddInt(-1);
		return;
	}

	const char* url = Scr_GetString(0);
	const char* headers = Scr_GetString(1);
	void* onConnectCallback = Scr_GetParamFunction(2);
	void* onMessageCallback = Scr_GetParamFunction(3);
	void* onCloseCallback = Scr_GetParamFunction(4);
	void* onErrorCallback = Scr_GetParamFunction(5);

	WebSocketClient* client;

	if (Scr_GetNumParam() >= 8) {
		int reconnectDelayMs = Scr_GetInt(6);
		int pingIntervalMs = Scr_GetInt(7);

		client = new WebSocketClient(headers, reconnectDelayMs, pingIntervalMs);
	}else {
		client = new WebSocketClient(headers);
	}

	client->onOpen([onConnectCallback, idx]() {
		Com_DPrintf("WebSocket client #%d connected.\n", idx);
		if (onConnectCallback && Scr_IsSystemActive()) {
			short thread_id = Scr_ExecThread((int)onConnectCallback, 0);
			Scr_FreeThread(thread_id);
		}
	});
	client->onMessage([onMessageCallback](const std::string& message) {
		if (onMessageCallback && Scr_IsSystemActive()) {
			Scr_AddString(message.c_str());
			short thread_id = Scr_ExecThread((int)onMessageCallback, 1);
			Scr_FreeThread(thread_id);
		}
	});
	client->onClose([onCloseCallback, idx](bool isClosedByRemote, bool isFullyDisconnected) {
		Com_DPrintf("WebSocket client #%d disconnected, isClosedByRemote: %d, isFullyDisconnected: %d\n", idx, isClosedByRemote ? 1 : 0, isFullyDisconnected ? 1 : 0);
		if (onCloseCallback && Scr_IsSystemActive()) {
			Scr_AddBool(isFullyDisconnected);
			Scr_AddBool(isClosedByRemote);
			short thread_id = Scr_ExecThread((int)onCloseCallback, 2);
			Scr_FreeThread(thread_id);
		}
	});
	client->onError([onErrorCallback](const std::string& error) {
		if (onErrorCallback && Scr_IsSystemActive()) {
			Scr_AddString(error.c_str());
			short thread_id = Scr_ExecThread((int)onErrorCallback, 1);
			Scr_FreeThread(thread_id);
		}
	});

	gsc_websocket_clients[idx] = client;
	client->connect(url);

	Scr_AddInt(idx); // Return index to script
}

/**
 * Closes the connection at given index.
 * Returns true if close was requested, false on error.
 * USAGE: websocket_close(connectionId)
 */
void gsc_websocket_close() {
	if (Scr_GetNumParam() < 1) {
		Scr_AddBool(false);
		return;
	}
	int idx = Scr_GetInt(0);
	if (idx < 0 || idx >= MAX_WEBSOCKET_CLIENTS || gsc_websocket_clients[idx] == nullptr) {
		Scr_AddBool(false);
		return;
	}

	// Request close
	gsc_websocket_clients[idx]->close();

	Scr_AddBool(true);
}

/**
 * Sends a TEXT message to the connection at given index.
 * Returns true if message was sent, false if not connected or on error.
 * USAGE: websocket_sendText(connectionId, message)
 */
void gsc_websocket_sendText() {
	if (Scr_GetNumParam() < 2) {
		Scr_Error(va("websocket_sendText: not enough parameters, expected 2, got %u", Scr_GetNumParam()));
		Scr_AddBool(false);
		return;
	}
	int idx = Scr_GetInt(0);
	const char* message = Scr_GetString(1);
	if (idx < 0 || idx >= MAX_WEBSOCKET_CLIENTS || gsc_websocket_clients[idx] == nullptr) {
		//Scr_Error("Invalid connectionId");
		Scr_AddBool(false);
		return;
	}

	bool result = gsc_websocket_clients[idx]->sendText(message);
	Scr_AddBool(result);
}


/**
 * Called before a map change, restart or shutdown that can be triggered from a script or a command.
 * Returns true to proceed, false to cancel the operation. Return value is ignored when shutdown is true.
 * @param fromScript true if map change was triggered from a script, false if from a command.
 * @param bComplete true if map change or restart is complete, false if it's a round restart so persistent variables are kept.
 * @param shutdown true if the server is shutting down, false otherwise.
 * @param source the source of the map change or restart.
 */
bool gsc_websocket_beforeMapChangeOrRestart(bool fromScript, bool bComplete, bool shutdown, sv_map_change_source_e source) {
	
	if (bComplete || shutdown) {
		// On complete map change or shutdown, request close of all websocket connections
		for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; ++i) {
			if (gsc_websocket_clients[i]) {
				gsc_websocket_clients[i]->close();
				gsc_websocket_clients[i]->poll(10); // try to process the close request immediately before map change
			}
		}
	}

	// If server is shutting down, Com_Frame is not called, so we need to poll here to process the pending requests (max 1 sec)
	if (shutdown) {
		for(int i = 0; i < 10; i++) {
			bool anyActive = false;
			for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; ++i) {
				if (gsc_websocket_clients[i] && !gsc_websocket_clients[i]->isDisconnected()) {
					gsc_websocket_clients[i]->poll(100);
					anyActive = true;
				}
			}
			if (!anyActive)
				break;
		}
	}
	
	if (bComplete || shutdown) {
		// On complete map change or shutdown, close all websocket connections
		for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; ++i) {
			if (gsc_websocket_clients[i]) {
				delete gsc_websocket_clients[i];
				gsc_websocket_clients[i] = nullptr;
			}
		}
	}

	return true;
}

/** Called every frame on frame start. */
void gsc_websocket_frame() {
    for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; ++i) {
        if (gsc_websocket_clients[i]) {
            gsc_websocket_clients[i]->poll();
            if (gsc_websocket_clients[i]->isDisconnected()) {
                delete gsc_websocket_clients[i];
                gsc_websocket_clients[i] = nullptr;
            }
        }
    }

	#if DEBUG
		if (gsc_websocket_test == nullptr) {
			gsc_websocket_test = new WebSocketClient();

			gsc_websocket_test->onOpen([] { 
				Com_Printf("ws> onOpen: connection established.\n");
			});
			gsc_websocket_test->onMessage([](const std::string& msg) { 
				Com_Printf("ws> onMessage: %s\n", msg.c_str()); 
			});
			gsc_websocket_test->onClose([](bool isClosedByRemote, bool isFullyDisconnected) { 
				Com_Printf("ws> onClose: client disconnected, isClosedByRemote: %d, isFullyDisconnected: %d\n", isClosedByRemote ? 1 : 0, isFullyDisconnected ? 1 : 0);
			});
			gsc_websocket_test->onError([](const std::string& error) {
				Com_Printf("ws> onError: %s\n", error.c_str());
			});

			//Cbuf_AddText("ws reconnect\n");
		}
		//gsc_websocket_test->poll();
	#endif
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void gsc_websocket_init() {

	#if DEBUG
		Cmd_AddCommand("ws", []() { 
			int count = Cmd_Argc();
			if (count < 2) return;
			
			const char* command = Cmd_Argv(1);
			if (Q_stricmp(command, "send") == 0) {

				const char* message = Cmd_Argv(2);
				if (message[0] == '\0') {
					Com_Printf("Please provide a message to send.\n");
					return;
				}
				if (gsc_websocket_test->sendText(message)) {
					Com_Printf("Sent: %s\n", message);
				} else {
					Com_Printf("Failed to send message, websocket not connected.\n");
				}
				return;

			} else if (Q_stricmp(command, "close") == 0) {
				gsc_websocket_test->close();
				Com_Printf("WebSocket connection closed.\n");
				return;

			} else if (Q_stricmp(command, "reconnect") == 0) {
				Com_Printf("WebSocket connection reconnecting...\n");
				gsc_websocket_test->connect("wss://localhost:8080/ws");
				return;

			} else {
				Com_Printf("Unknown match command: %s\n", command);
				return;
			}
		});
	#endif
}
