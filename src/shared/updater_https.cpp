// http_client.h pulls in mongoose -> winsock2.h, which must be included before windows.h.
#include "http_client.h"
#include "json.h"

#include "updater_https.h"

#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#include "shared.h"        // WL
#include "version.h"       // APP_VERSION
#include "logger.h"        // logger_add (thread-safe)


// Endpoint that receives the JSON update-check request and returns a JSON response.
#define UPDATE_HTTPS_URL "https://master.cod2x.me/api/updater/check"


// Only allow one in-flight HTTPS check at a time (update checks can be triggered repeatedly).
static std::atomic<bool> updater_https_inProgress(false);

// Result of the last finished check, handed over to the main thread by updater_https_pollResult().
static std::mutex updater_https_resultMutex;
static bool updater_https_hasResult = false;
static UpdaterHttpsResult updater_https_result;


// Parse the JSON response body into 'out'. Returns false if the required field is missing.
static bool updater_https_parseResponse(const char* body, UpdaterHttpsResult* out) {
    if (!body || !out)
        return false;

    // Required field; the server sends it as a string "0" / "1" / "2" (a number is accepted too)
    char buf[1024];
    if (!json_get_str(body, "$.update.available", buf, sizeof(buf)))
        return false;
    if (strcmp(buf, "0") == 0)      out->available = 0;
    else if (strcmp(buf, "1") == 0) out->available = 1;
    else if (strcmp(buf, "2") == 0) out->available = 2;
    else return false;

    if (json_get_str(body, "$.update.url", buf, sizeof(buf)))
        out->url = buf;
    if (json_get_str(body, "$.update.version", buf, sizeof(buf)))
        out->version = buf;

    // An update without a download URL can't be applied
    if (out->available != 0 && out->url.empty())
        return false;

    return true;
}

static void updater_https_setResult(const UpdaterHttpsResult& result) {
    std::lock_guard<std::mutex> lock(updater_https_resultMutex);
    updater_https_result = result;
    updater_https_hasResult = true;
}

// Appends ,"name":"value" (escaped)
static void updater_https_jsonAddString(std::string& json, const char* name, const std::string& value) {
    json += ",\"";
    json += name;
    json += "\":\"" + json_escape_string(value) + "\"";
}


bool updater_https_checkForUpdate(const UpdaterHttpsRequest& request) {
    // Skip if a previous HTTPS check is still running.
    bool expected = false;
    if (!updater_https_inProgress.compare_exchange_strong(expected, true)) {
        return false;
    }

    // Build the request body. game/version/os decide the update, the rest is only logged.
    std::string json;
    json += "{\"game\":\"CoD2x MP\"";
    updater_https_jsonAddString(json, "version", APP_VERSION);
    updater_https_jsonAddString(json, "os", WL("win-x86", "linux-i386"));
    updater_https_jsonAddString(json, "type", request.type);
    if (!request.cdkeyHash.empty())  updater_https_jsonAddString(json, "cdkeyHash", request.cdkeyHash);
    if (!request.regid.empty())      updater_https_jsonAddString(json, "regid", request.regid);
    if (request.hasHwid)             json += ",\"hwid\":" + std::to_string(request.hwid);
    if (!request.hwid2.empty())      updater_https_jsonAddString(json, "hwid2", request.hwid2);
    if (!request.hwid2Old.empty())   updater_https_jsonAddString(json, "hwid2Old", request.hwid2Old);
    if (!request.hwid2Diff.empty())  updater_https_jsonAddString(json, "hwid2Diff", request.hwid2Diff);
    if (request.hasHwid2DiffCnt)     json += ",\"hwid2DiffCnt\":" + std::to_string(request.hwid2DiffCnt);
    if (!request.systemInfo.empty()) updater_https_jsonAddString(json, "systemInfo", request.systemInfo);
    json += "}";

    // Run the whole round-trip on a background thread so the caller is never blocked.
    std::thread([json]() {

        UpdaterHttpsResult result;
        result.error = "No response";

        HttpClient client;
        client.postJson(
            UPDATE_HTTPS_URL,
            json.c_str(),
            [&result](const HttpClient::Response& res) {
                if (res.status < 200 || res.status >= 300) {
                    result.error = "HTTP " + std::to_string(res.status);
                    logger_add("[UPDATER-HTTPS] Request rejected by endpoint (HTTP %d): %s", res.status, res.body.c_str());
                    return;
                }

                UpdaterHttpsResult parsed;
                if (!updater_https_parseResponse(res.body.c_str(), &parsed)) {
                    result.error = "Invalid response";
                    logger_add("[UPDATER-HTTPS] Failed to parse response: %s", res.body.c_str());
                    return;
                }

                parsed.ok = true;
                result = parsed;
                logger_add("[UPDATER-HTTPS] available=%d version='%s' url='%s'",
                    parsed.available, parsed.version.c_str(), parsed.url.c_str());
            },
            [&result](const std::string& err) {
                result.error = err;
                logger_add("[UPDATER-HTTPS] Request failed: %s", err.c_str());
            },
            5000);

        // Drive the client until the request completes (connection closed) or times out.
        client.poll_max(7000);

        updater_https_setResult(result);
        updater_https_inProgress = false;
    }).detach();

    return true;
}


bool updater_https_pollResult(UpdaterHttpsResult* out) {
    std::lock_guard<std::mutex> lock(updater_https_resultMutex);
    if (!updater_https_hasResult)
        return false;
    *out = updater_https_result;
    updater_https_hasResult = false;
    return true;
}
