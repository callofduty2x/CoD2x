// http_client.h pulls in mongoose -> winsock2.h, which must be included before windows.h.
#include "http_client.h"
#include "json.h"

#include "exception.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <string>
#include <vector>

#include "logger.h"

#define EXCEPTION_ENDPOINT_URL "https://master.cod2x.me/api/telemetry/cod2x-exceptions"
//#define EXCEPTION_ENDPOINT_URL "http://localhost:8080/api/telemetry/cod2x-exceptions"

// Reports older than this are deleted instead of being sent again.
#define EXCEPTION_PENDING_MAX_AGE_DAYS 14
#define EXCEPTION_PENDING_MAX_SIZE     (8 * 1024 * 1024)
#define EXCEPTION_RESEND_MAX_REPORTS   10
#define EXCEPTION_RESEND_WAIT_MS       15000


// A rejected report (4xx) will never be accepted, other failures are worth retrying later.
static bool exception_isPermanentHttpFailure(int status) {
    return status >= 400 && status < 500 && status != 408 && status != 429;
}


// "HTTP <status>" with the short message of a JSON error body ("error":"..." or "message":"...")
static std::string exception_formatHttpError(int status, const std::string& body) {
    char message[128] = {0};
    if (!body.empty() && body[0] == '{') {
        if (!json_get_str(body.c_str(), "$.error", message, sizeof(message)))
            json_get_str(body.c_str(), "$.message", message, sizeof(message));
    }

    std::string out = "HTTP " + std::to_string(status);
    if (message[0])
        out += std::string(": ") + message;
    return out;
}


void exception_postReport(HttpClient& client, const std::string& json, const std::string& pendingPath, int timeoutMs, ExceptionSendCallback onDone) {
    client.postJson(
        EXCEPTION_ENDPOINT_URL,
        json.c_str(),
        [pendingPath, onDone](const HttpClient::Response& res) {
            bool delivered = res.status >= 200 && res.status < 300;
            if (delivered || exception_isPermanentHttpFailure(res.status))
                remove(pendingPath.c_str());

            std::string error;
            if (delivered) {
                logger_add("[EXCEPTION] Report sent successfully (HTTP %d)", res.status);
            } else {
                error = exception_formatHttpError(res.status, res.body);
                logger_add("[EXCEPTION] Report rejected by endpoint: %s", error.c_str());
            }
            if (onDone)
                onDone(delivered, error);
        },
        [onDone](const std::string& error) {
            logger_add("[EXCEPTION] Failed to send report: %s", error.c_str());
            if (onDone)
                onDone(false, error);
        },
        timeoutMs);
}


void exception_sendReport(const std::string& json, const std::string& pendingPath, int maxWaitMs) {
    HttpClient client;
    exception_postReport(client, json, pendingPath, maxWaitMs, nullptr);
    client.poll_max(maxWaitMs);
}


static bool exception_readFile(const char* path, size_t size, std::string& out) {
    FILE* file = fopen(path, "rb");
    if (!file)
        return false;
    out.resize(size);
    bool ok = fread(&out[0], 1, size, file) == size;
    fclose(file);
    return ok;
}


// Lists the pending reports sorted by name, the timestamp in the name sorts chronologically
static std::vector<std::string> exception_listPending(const char* directory, const char* filePrefix) {
    static const char suffix[] = EXCEPTION_PENDING_SUFFIX;
    size_t prefixLen = strlen(filePrefix);
    size_t suffixLen = sizeof(suffix) - 1;

    std::vector<std::string> names;
    DIR* dir = opendir(directory);
    if (!dir)
        return names;

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        const char* name = ent->d_name;
        size_t len = strlen(name);
        if (len >= prefixLen + suffixLen && strncmp(name, filePrefix, prefixLen) == 0 && strcmp(name + len - suffixLen, suffix) == 0)
            names.push_back(name);
    }
    closedir(dir);

    std::sort(names.begin(), names.end());
    return names;
}


void exception_resendPending(const char* directory, const char* filePrefix, bool markResent) {
    std::vector<std::string> names = exception_listPending(directory, filePrefix);

    time_t now = time(NULL);
    int sent = 0;
    for (size_t i = 0; i < names.size() && sent < EXCEPTION_RESEND_MAX_REPORTS; i++) {
        std::string path = std::string(directory) + "/" + names[i];

        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            continue;

        // Too old to be useful
        if (now > st.st_mtime && now - st.st_mtime > EXCEPTION_PENDING_MAX_AGE_DAYS * 86400) {
            remove(path.c_str());
            continue;
        }

        std::string json;
        if (st.st_size <= 0 || st.st_size > EXCEPTION_PENDING_MAX_SIZE ||
            !exception_readFile(path.c_str(), (size_t)st.st_size, json) || json[0] != '{') {
            remove(path.c_str());
            continue;
        }
        if (markResent)
            json.insert(1, "\"resent\":true,");

        logger_add("[EXCEPTION] Sending pending report %s", names[i].c_str());
        exception_sendReport(json, path, EXCEPTION_RESEND_WAIT_MS);
        sent++;
    }
}
