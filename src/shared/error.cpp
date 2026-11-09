// http_client.h pulls in mongoose -> winsock2.h, which must be included before windows.h.
#include "http_client.h"
#include "json.h"

#include "error.h"
#include "exception.h"

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "shared.h"        // ticks_ms
#include "version.h"       // APP_VERSION
#include "logger.h"

// Endpoint that receives error reports as JSON.
#define ERROR_ENDPOINT_URL "https://master.cod2x.me/api/telemetry/cod2x-errors"
//#define ERROR_ENDPOINT_URL "http://localhost:8080/api/telemetry/cod2x-errors"

#define ERROR_REPEAT_MS (24ULL * 60 * 60 * 1000)

// Max time a fatal error waits for the report to be sent, the process exits right after it.
#define ERROR_FATAL_WAIT_MS 3000


// Sends the JSON on a background thread and waits for it at most waitMs (0 = returns immediately).
// delivered is optional, it is set when the master server accepts the report.
static void error_post(const std::string& json, int waitMs, std::shared_ptr<std::atomic<bool>> delivered = nullptr) {
    auto sent = std::make_shared<std::promise<void>>();
    std::future<void> finished = sent->get_future();

    std::thread([json, sent, delivered]() {
        HttpClient client;
        client.postJson(
            ERROR_ENDPOINT_URL,
            json.c_str(),
            [delivered](const HttpClient::Response& res) {
                if (res.status >= 200 && res.status < 300) {
                    logger_add("[ERROR] Report sent successfully (HTTP %d)", res.status);
                    if (delivered)
                        *delivered = true;
                } else {
                    logger_add("[ERROR] Report rejected by endpoint (HTTP %d)", res.status);
                }
            },
            [](const std::string& err) {
                logger_add("[ERROR] Failed to send report: %s", err.c_str());
            },
            5000);

        // Drive the client until the request finishes (connection closed) or the timeout elapses.
        client.poll_max(7000);

        sent->set_value();
    }).detach();

    if (waitMs > 0)
        finished.wait_for(std::chrono::milliseconds(waitMs));
}


// Builds the report, stack can be empty
static std::string error_buildReport(const char* kind, const char* message, const char* stack) {
    ErrorPlatformInfo platform = error_getPlatformInfo();

    char loggerBuf[8192] = {0};
    logger_get_recent(loggerBuf, sizeof(loggerBuf));

    std::string json = "{";
    json += "\"version\":\"" + json_escape_string(APP_VERSION) + "\",";
    json += "\"type\":\"" + platform.type + "\",";
    json += "\"installId\":\"" + json_escape_string(platform.installId) + "\",";
    json += "\"hwid2\":\"" + json_escape_string(platform.hwid2) + "\",";
    json += "\"errorInfo\":{\"kind\":\"" + std::string(kind) + "\",";
    json += "\"message\":\"" + json_escape_string(message ? message : "") + "\",";
    json_appendLines(json, "stack", stack);
    json += "},";
    json += "\"systemInfo\":" + exception_getSystemInfoJson() + ",";
    json_appendLines(json, "loggerLines", loggerBuf);
    json += ",";
    json_appendLines(json, "logLines", platform.logLines.c_str());
    json += "}";
    return json;
}


void error_reportFatal(const char* message) {
    #if DEBUG && !ERROR_TEST
        return;
    #endif

    // Skips this function
    char stack[8192] = {0};
    exception_captureCallerStack(stack, sizeof(stack), 1);

    error_post(error_buildReport(ERROR_KIND_FATAL, message, stack), ERROR_FATAL_WAIT_MS);
}


void error_report(const char* kind, const std::string& message) {
    #if DEBUG && !ERROR_TEST
        return;
    #endif

    struct Report {
        uint64_t sentAt;
        std::shared_ptr<std::atomic<bool>> delivered;
    };
    static std::map<std::string, Report> reports;

    uint64_t now = ticks_ms();
    auto report = reports.find(message);
    if (report != reports.end() && *report->second.delivered && now - report->second.sentAt < ERROR_REPEAT_MS)
        return;

    auto delivered = std::make_shared<std::atomic<bool>>(false);
    reports[message] = { now, delivered };
    error_post(error_buildReport(kind, message.c_str(), ""), 0, delivered);
}
