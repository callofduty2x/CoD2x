#ifndef HTTP_DOWNLOAD_H
#define HTTP_DOWNLOAD_H

#include "http_client.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#define HTTP_DOWNLOAD_MAX_REDIRECTS     5
#define HTTP_DOWNLOAD_STALL_TIMEOUT_MS  30000   // no data received for this long

/**
 * Downloads one file, follows redirects and detects incomplete files.
 * download() blocks. start() runs it on a background thread: read the state until it is not Running, then join().
 *
 * magic: the file must start with these bytes, for hosts returning a html page with status 200 (NULL = any file)
 */
class HttpDownload {
  public:
    enum State { Idle, Running, Finished, Failed };

    bool download(const std::string& url, const std::string& path, const char* magic) {
        reset();
        return run(url, path, magic);
    }

    void start(const std::string& url, const std::string& path, const char* magic) {
        cancel();
        reset();
        thread = new std::thread(&HttpDownload::run, this, url, path, magic);
    }

    // Stops the download and waits for the thread, the file stays on the disk
    void cancel() {
        cancelled = true;
        join();
    }

    // Waits for the thread and returns to Idle
    void join() {
        if (thread) {
            thread->join();
            delete thread;  // kept as a pointer, a running thread must not be destroyed when the process exits
            thread = NULL;
        }
        state = Idle;
    }

    State getState() const { return state; }
    size_t getDownloaded() const { return downloaded; }
    size_t getTotal() const { return total; }         // 0 if not known yet
    const std::string& getError() const { return error; }   // only when Failed

    // The size matches Content-Length. Without Content-Length a cut connection also ends as Finished.
    bool isComplete() const { return total > 0 && downloaded == total; }

  private:
    std::thread* thread = NULL;
    std::atomic<State> state{Idle};
    std::atomic<bool> cancelled{false};
    std::atomic<size_t> downloaded{0};
    std::atomic<size_t> total{0};
    std::string error;

    void reset() {
        cancelled = false;
        downloaded = 0;
        total = 0;
        error.clear();
        state = Running;
    }

    // The Location header can be absolute, relative to the scheme, to the host or to the current path
    static std::string resolveRedirect(const std::string& url, const std::string& location) {
        // "://" in the query of a relative location does not make it absolute
        size_t scheme = location.find("://");
        if (scheme != std::string::npos && location.find_first_of("/?#") == scheme + 1)
            return location;

        size_t hostStart = url.find("://") + 3;
        size_t pathStart = url.find_first_of("/?#", hostStart);
        if (pathStart == std::string::npos)
            pathStart = url.size();

        if (location.compare(0, 2, "//") == 0)
            return url.substr(0, hostStart - 2) + location;

        if (!location.empty() && location[0] == '/')
            return url.substr(0, pathStart) + location;

        size_t pathEnd = url.rfind('/', url.find_first_of("?#", pathStart));
        if (pathEnd < hostStart)
            return url.substr(0, pathStart) + "/" + location;
        return url.substr(0, pathEnd + 1) + location;
    }

    static bool hasMagic(const std::string& path, const char* magic) {
        char buffer[16] = {0};
        size_t length = strlen(magic);

        FILE* f = fopen(path.c_str(), "rb");
        if (!f)
            return false;
        size_t read = fread(buffer, 1, length, f);
        fclose(f);
        return read == length && memcmp(buffer, magic, length) == 0;
    }

    // One request, sets the error on failure. Redirect is set if the file is somewhere else.
    void fetch(const std::string& url, FILE* f, std::string* redirect) {
        bool running = true;
        uint64_t lastData = mg_millis();

        // Must be the last local, it can still call the callbacks when it is destroyed
        HttpClient client;
        client.headers.push_back("User-Agent: CoD2x");
        client.onRedirect = [&](const std::string& location) {
            *redirect = location;
            running = false;
        };
        client.downloadFile(url.c_str(),
            [&](const char* data, size_t length, size_t, size_t size) {
                lastData = mg_millis();
                size_t written = fwrite(data, 1, length, f);
                downloaded += written;
                total = size;
                if (written != length) {
                    error = "Failed to write the file";
                    running = false;
                }
            },
            [&](const HttpClient::Response&) {
                running = false;
            },
            [&](const std::string& message) {
                // Stopping the client below also reports the closed connection, the first error is the real one
                if (error.empty())
                    error = message;
                running = false;
            },
            0, 0    // a big file can take very long, the stall check below is the timeout
        );

        while (running) {
            client.poll(50);

            if (cancelled) {
                error = "Cancelled";
                return;
            }
            if (mg_millis() - lastData > HTTP_DOWNLOAD_STALL_TIMEOUT_MS) {
                error = "Timeout";
                return;
            }
        }
    }

    bool run(std::string url, std::string path, const char* magic) {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) {
            error = "Failed to open '" + path + "' for writing";
            state = Failed;
            return false;
        }

        bool done = false;
        for (int i = 0; i <= HTTP_DOWNLOAD_MAX_REDIRECTS && !done && error.empty(); i++) {
            std::string redirect;
            fetch(url, f, &redirect);

            if (redirect.empty())
                done = true;
            else
                url = resolveRedirect(url, redirect);
        }
        if (fclose(f) != 0 && error.empty())
            error = "Failed to write the file";

        if (error.empty()) {
            if (!done)
                error = "Too many redirects";
            else if (downloaded == 0)
                error = "Received an empty file";
            else if (magic && !hasMagic(path, magic))
                error = "Unexpected file content";
        }

        state = error.empty() ? Finished : Failed;
        return error.empty();
    }
};

#endif
