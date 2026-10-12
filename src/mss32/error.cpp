// http_client.h pulls in mongoose -> winsock2.h, which must be included before windows.h.
#include "../shared/http_client.h"
#include "../shared/json.h"

#include "error.h"

#include <windows.h>
#include <string>
#include <vector>
#include <cstring>

#include "shared.h"
#include "system.h"
#include "console.h"
#include "../shared/cod2_dvars.h"
#include "../shared/logger.h"

// Telemetry endpoint that receives error reports as JSON.
#define ERROR_TELEMETRY_ENDPOINT_URL "https://master.cod2x.me/api/telemetry/cod2x-errors"
//#define ERROR_TELEMETRY_ENDPOINT_URL "http://localhost:8080/api/telemetry/cod2x-errors"

// Set ERROR_TEST to 1 to simulate an error after ERROR_TEST_DELAY_MS to verify telemetry sending.
// When enabled, the DEBUG early-return in error_sendErrorData is bypassed so the report is sent.
#define ERROR_TEST 0
#define ERROR_TEST_DELAY_MS 5000

// Max time the error path waits for the report to be sent before the error message box is shown.
// The message box is often closed quickly and the process exits, which would abort the request.
#define ERROR_SEND_WAIT_MS 3000

extern char hwid_regid[33];
extern dvar_t* cl_hwid2;


// Append a "fieldName":[ "line", ... ] array, splitting text on newlines (empty lines skipped).
static void error_jsonAppendLinesArray(std::string& out, const char* fieldName, const char* text) {
    out += "\"";
    out += fieldName ? fieldName : "";
    out += "\":[";

    const char* p = text ? text : "";
    bool first = true;
    while (*p) {
        const char* lineStart = p;
        while (*p && *p != '\n' && *p != '\r') ++p;
        size_t lineLen = (size_t)(p - lineStart);
        if (lineLen > 0) {
            if (!first) out += ",";
            first = false;
            out += "\"";
            out += json_escape_string(std::string(lineStart, lineLen));
            out += "\"";
        }
        while (*p == '\n' || *p == '\r') ++p;
    }
    out += "]";
}

// Background thread: owns the JSON payload and POSTs it to the telemetry endpoint,
// polling its own HttpClient until the request completes or times out.
static DWORD WINAPI error_sendThread(LPVOID param) {
    std::string* json = (std::string*)param;

    HttpClient client;
    client.postJson(
        ERROR_TELEMETRY_ENDPOINT_URL,
        json->c_str(),
        [](const HttpClient::Response& res) {
            if (res.status >= 200 && res.status < 300)
                logger_add("[TELEMETRY] Error report sent successfully (HTTP %d)", res.status);
            else
                logger_add("[TELEMETRY] Error report rejected by endpoint (HTTP %d)", res.status);
        },
        [](const std::string& err) {
            logger_add("[TELEMETRY] Failed to send error report: %s", err.c_str());
        },
        5000);

    // Drive the client until the request finishes (connection closed) or the timeout elapses.
    client.poll_max(7000);

    delete json;
    return 0;
}

// Build a JSON error report and send it to the telemetry backend on a background thread.
// Waits at most ERROR_SEND_WAIT_MS for the request to finish, so a slow network never blocks the error path for long.
void error_sendErrorData(const char* message) {

    #if DEBUG && !ERROR_TEST
        return;
    #endif

    // Gather recent logger entries and the console mirror at the time of the error.
    char loggerBuf[8192] = {0};
    logger_get_recent(loggerBuf, sizeof(loggerBuf));

    std::vector<char> logsBuf(CONSOLE_MIRROR_CAPACITY + 1, 0);
    console_getLogs(logsBuf.data(), logsBuf.size());

    const char* installId = hwid_regid[0] ? hwid_regid : "";
    const char* hwid2 = (cl_hwid2 && cl_hwid2->value.string) ? cl_hwid2->value.string : "";

    // Build the JSON payload (mirrors the crash telemetry structure in exception.cpp).
    std::string* json = new std::string();
    json->reserve(4096 + strlen(loggerBuf) + strlen(logsBuf.data()));

    *json += "{";
    *json += "\"version\":\"" + json_escape_string(APP_VERSION) + "\",";
    *json += "\"type\":\"win-client\",";
    *json += "\"installId\":\"" + json_escape_string(installId) + "\",";
    *json += "\"hwid2\":\"" + json_escape_string(hwid2) + "\",";
    *json += "\"errorInfo\":{\"message\":\"" + json_escape_string(message ? message : "") + "\"},";

    *json += "\"systemInfo\":{";
    *json += "\"winBuild\":\"" + json_escape_string(SYS_WIN_BUILD) + "\",";
    *json += "\"winEdition\":\"" + json_escape_string(SYS_WIN_EDITION) + "\",";
    *json += "\"winDisplayVersion\":\"" + json_escape_string(SYS_WIN_DISPLAY_VERSION) + "\",";
    *json += "\"language\":\"" + json_escape_string(SYS_LANGUAGE_CODE) + "\",";
    *json += "\"virtualization\":\"" + json_escape_string(SYS_VIRTUALIZATION) + "\",";
    *json += "\"compatibilityFlags\":\"" + json_escape_string(SYS_COMPATIBILITY_FLAGS) + "\",";
    *json += "\"versionInfo\":\"" + json_escape_string(SYS_VERSION_INFO) + "\",";
    *json += "\"isElevated\":" + std::string(SYS_IS_ELEVATED ? "true" : "false");
    *json += "},";

    error_jsonAppendLinesArray(*json, "loggerLines", loggerBuf);
    *json += ",";
    error_jsonAppendLinesArray(*json, "logLines", logsBuf.data());
    *json += "}";

    // Send on a background thread (it owns the payload) and give it a moment to finish.
    // The caller shows a message box right after and the process usually exits once it's closed,
    // so without waiting the report would often be lost. If the wait times out, the thread continues.
    HANDLE hThread = CreateThread(NULL, 0, error_sendThread, json, 0, NULL);
    if (hThread) {
        WaitForSingleObject(hThread, ERROR_SEND_WAIT_MS);
        CloseHandle(hThread);
    } else
        delete json;
}

/** Called when a fatal error occurs. */
int Sys_Error_MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {

    error_sendErrorData(lpText);

    return MessageBoxA(hWnd, lpText, lpCaption, uType);
}

/** Called when a DirectX fatal error occurs. */
int Sys_DirectXFatalError_MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {

    error_sendErrorData(lpText);

    return MessageBoxA(hWnd, lpText, lpCaption, uType);
}




#if ERROR_TEST
static DWORD WINAPI error_testThread(LPVOID) {
    Sleep(ERROR_TEST_DELAY_MS);

    // Simulate an error to verify the telemetry sending path.
    error_sendErrorData("Simulated test error for telemetry verification");

    return 0;
}
#endif

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void error_init() {
#if ERROR_TEST
    // Spawn a thread that triggers a simulated error after ERROR_TEST_DELAY_MS ms.
    HANDLE hThread = CreateThread(NULL, 0, error_testThread, NULL, 0, NULL);
    if (hThread)
        CloseHandle(hThread);
#endif
}

/** Called before the entry point is called. Used to patch the memory. */
void error_patch() {

    // Hook call to MessageBoxA in Sys_Error that is called when fatal game error occurs
    patch_nop(0x004657f9, 6); // first replace 6th byte, becuase next function writes only to 5 bytes
    patch_call(0x004657f9, (unsigned int)Sys_Error_MessageBoxA);

    // Hook call to MessageBoxA in Sys_DirectXFatalError that is called when DirectX error occurs
    patch_call(0x00465590, (unsigned int)Sys_DirectXFatalError_MessageBoxA);
}
