#ifndef SHARED_ERROR_H
#define SHARED_ERROR_H

#include <string>

// Error reports for the master server (POST /api/telemetry/cod2x-errors).
// Fatal errors are reported by error_reportFatal(), install failures of updates by error_report().

// Set ERROR_TEST to 1 to send reports also in debug builds, Windows then simulates a fatal error after the start.
#define ERROR_TEST 0

// errorInfo.kind, the master server groups and filters by it
#define ERROR_KIND_FATAL        "fatal"
#define ERROR_KIND_ZPAM_UPDATE  "zpam-update"
#define ERROR_KIND_COD2X_UPDATE "cod2x-update"

// Part of a report that differs per platform, implemented by error.cpp of each platform
struct ErrorPlatformInfo {
    std::string type;           // "win-client", "win-server" or "linux-server"
    std::string installId;      // empty on a server
    std::string hwid2;          // empty on a server
    std::string logLines;       // console output, empty if not available
};
ErrorPlatformInfo error_getPlatformInfo();

// Reports a fatal error with the stack of the caller. Waits a few seconds for the report to be sent,
// the caller usually ends the process right after it and the report would be lost.
void error_reportFatal(const char* message);

// Main thread: reports an error the process survives. A failing update is tried with every update
// check, so a delivered message is repeated only once a day, which still shows it keeps failing.
void error_report(const char* kind, const std::string& message);

#endif
