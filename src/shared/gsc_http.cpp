#include "gsc_http.h"

#include "shared.h"
#include "cod2_common.h"
#include "cod2_script.h"
#include "http_client.h"
#include "server.h"


HttpClient* gsc_http_client = nullptr;
int gsc_http_pending_requests = 0;


/**
 * Fetch a URL with the specified method, data and headers.
 * The connection is closed after the response is received.
 * The request is asynchronous, the response is handled in the onDoneCallback or onErrorCallback
 *   onDoneCallback is called with (status, body, headers[])
 *   onErrorCallback is called with (error)
 * Headers needs to be separated by \r\n, e.g. "Content-Type: application/json\r\nAccept: application/json"
 * Example:
 * http_fetch("https://url.com/post", "POST", "{data: true}", "Header:Value\r\nHeader2:Value2", 5000, ::onDoneCallback, ::onErrorCallback)
 */
void gsc_http_fetch() {

    if (Scr_GetNumParam() != 7) {
		Scr_Error(va("http_fetch: invalid number of parameters, expected 7, got %d\n", Scr_GetNumParam()));
        Scr_AddUndefined();
        return;
    }

    const char* url = Scr_GetString(0);
    const char* method = Scr_GetString(1);
    const char* data = Scr_GetString(2);
    const char* headers = Scr_GetString(3);
	int timeout = Scr_GetInt(4);
	void* onDoneCallback = Scr_GetParamFunction(5);
	void* onErrorCallback = Scr_GetParamFunction(6);

	if (!gsc_http_client) {
		gsc_http_client = new HttpClient();
	}

    // Increase pending requests count
    gsc_http_pending_requests++;

	gsc_http_client->request(method, url, data, strlen(data), headers,
		[onDoneCallback](const HttpClient::Response& res) {
            gsc_http_pending_requests--;

			// Handle successful response
			if (onDoneCallback /*&& Scr_IsSystemActive()*/)
			{

				// Add headers to the script engine
				Scr_MakeArray();
				for (const auto& header : res.headers) {
					Scr_AddString(header.first.c_str());
					Scr_AddArray();
					Scr_AddString(header.second.c_str());
					Scr_AddArray();
				}
				Scr_AddString(res.body.c_str());
				Scr_AddInt(res.status);

				// Run function that print test was OK
				short thread_id = Scr_ExecThread((int)onDoneCallback, 3);
				Scr_FreeThread(thread_id);
			}

		}, [onErrorCallback, url](const std::string& error) {
            gsc_http_pending_requests--;

			if (onErrorCallback && Scr_IsSystemActive())
			{
				Scr_AddString(error.c_str());

				// Run function that print test was OK
				short thread_id = Scr_ExecThread((int)onErrorCallback, 1);
				Scr_FreeThread(thread_id);
			} else {
				Com_Printf("HTTP error while fetching %s: %s\n", url, error.c_str());
			}
		},
		timeout
	);


}


/**
 * Called before a map change, restart or shutdown that can be triggered from a script or a command.
 * Returns true to proceed, false to cancel the operation. Return value is ignored when shutdown is true.
 * @param fromScript true if map change was triggered from a script, false if from a command.
 * @param bComplete true if map change or restart is complete, false if it's a round restart so persistent variables are kept.
 * @param shutdown true if the server is shutting down, false otherwise.
 * @param source the source of the map change or restart.
 */
bool gsc_http_beforeMapChangeOrRestart(bool fromScript, bool bComplete, bool shutdown, sv_map_change_source_e source) {

    if (shutdown && gsc_http_client) {

        if (gsc_http_pending_requests > 0) {
            // Since server is shutting down, Com_Frame is not called, so we need to poll here to process the pending requests (max 1 sec)
            for(int i = 0; i < 10 && gsc_http_pending_requests > 0; i++) {
                gsc_http_client->poll(100);
            }
        }

        delete gsc_http_client;
        gsc_http_client = nullptr;
    }


	return true;
}

/** Called every frame on frame start. */
void gsc_http_frame() {
    if (gsc_http_client) {
        gsc_http_client->poll();
    }
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void gsc_http_init() {
}
