#ifndef SHARED_EXCEPTION_H
#define SHARED_EXCEPTION_H

#include <stddef.h>
#include <functional>
#include <string>

// Delivery of crash reports to the master server (POST /api/telemetry/cod2x-exceptions).
// Each platform catches the crash and builds the report, the report is saved as a pending file
// and deleted once delivered, so it survives the end of the process.

#define EXCEPTION_PENDING_SUFFIX ".pending.json"

class HttpClient;

// error describes why the report was not delivered
typedef std::function<void(bool delivered, const std::string& error)> ExceptionSendCallback;

// Starts sending the report, the caller polls the client. pendingPath is deleted when the report was
// delivered or permanently rejected. onDone is optional.
void exception_postReport(HttpClient& client, const std::string& json, const std::string& pendingPath, int timeoutMs, ExceptionSendCallback onDone);

// Sends the report and waits for the result.
void exception_sendReport(const std::string& json, const std::string& pendingPath, int maxWaitMs);

// Sends the reports "<directory>/<filePrefix>*.pending.json" left by earlier runs, oldest first.
// markResent adds "resent":true, for a platform where a report is normally sent right after the crash.
// Blocks while sending, call it on a background thread.
void exception_resendPending(const char* directory, const char* filePrefix, bool markResent);

// Implemented by each platform, also used for error reports:

// Writes the stack of the calling thread as text, one frame per line. Used for reports that are not crashes.
size_t exception_captureCallerStack(char* out, size_t outSize, int framesToSkip);
// Short system info as a JSON object, it does not need a crash to be collected.
std::string exception_getSystemInfoJson();

#endif
