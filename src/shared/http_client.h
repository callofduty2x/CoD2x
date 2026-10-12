#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "mongoose/mongoose.h"
#include <functional>
#include <string>
#include <map>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <algorithm>

#undef poll

/**
 * A simple HTTP client using the Mongoose library.
 * Supports GET and POST requests with custom headers and timeouts.
 * Connection is closed after each request.
 * Call poll() periodically to process events.
 */
class HttpClient {
  public:
    // Response object
    struct Response {
        int status = 0;
        std::map<std::string, std::string> headers;
        std::string body;
    };
    using Callback = std::function<void(const Response&)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    using DownloadCallback = std::function<void(const char* data, size_t length, size_t downloaded, size_t total)>;
    using UploadCallback = std::function<void(size_t uploaded, size_t total, size_t bytes_per_second)>; // Cleaner callback for upload progress
    using ReadCallback = std::function<size_t(char* buffer, size_t maxLen, size_t offset)>;             // Fills buffer with next chunk at 'offset', returns bytes read

    // Headers used in every request
    std::vector<std::string> headers = {};


    HttpClient() {
        mg_log_set(MG_LL_NONE);
        mg_mgr_init(&mgr);
    }

    ~HttpClient() {
        mg_mgr_free(&mgr);
    }

    void poll(int wait_time_ms = 0) {
        mg_mgr_poll(&mgr, wait_time_ms);
    }

    // Poll until no active connections or max_time_ms reached
    void poll_max(int max_time_ms) {
        auto start_time = mg_millis();
        while (mg_millis() - start_time < (uint64_t)max_time_ms) {
            mg_mgr_poll(&mgr, 10); // Poll with a small wait time to avoid busy-waiting
            if (mgr.conns == nullptr) {
                break; // Exit if no active connections
            }
        }
    }

    // Basic GET
    void get(const char* url,
             Callback onDone,
             ErrorCallback onError = nullptr,
             int timeout_ms = 5000) {
        request("GET", url, "", 0, "", onDone, onError, timeout_ms);
    }

    // GET with headers
    void get(const char* url,
             const char* headers,
             Callback onDone,
             ErrorCallback onError = nullptr,
             int timeout_ms = 5000) {
        request("GET", url, "", 0, headers, onDone, onError, timeout_ms);
    }

    // Basic POST
    void post(const char* url,
              const char* body,
              Callback onDone,
              ErrorCallback onError = nullptr,
              int timeout_ms = 5000) {
        request("POST", url, body, strlen(body), "", onDone, onError, timeout_ms);
    }

    // POST with headers
    void post(const char* url,
              const char* body,
              const char* headers,
              Callback onDone,
              ErrorCallback onError = nullptr,
              int timeout_ms = 5000) {
        request("POST", url, body, strlen(body), headers, onDone, onError, timeout_ms);
    }

    // JSON POST convenience
    void postJson(const char* url,
                  const char* json,
                  Callback onDone,
                  ErrorCallback onError = nullptr,
                  int timeout_ms = 5000) {
        post(url, json, "Content-Type: application/json", onDone, onError, timeout_ms);
    }

    // Download file with chunked processing and progress
    void downloadFile(const char* url,
                      DownloadCallback onDownload,
                      Callback onDone,
                      ErrorCallback onError = nullptr,
                      int timeout_ms = 60000,
                      int connect_timeout_ms = 10000)
    {
        // Validate URL to be correct
        if (!is_valid_url(url)) {
            if (onError)
                onError("Invalid URL");
            return;
        }

        auto* ctx = new RequestContext{};
        ctx->url = url ? url : "";
        ctx->method = "GET";
        ctx->isDownload = true;
        // Combine global headers
        ctx->headers.clear();
        for (const auto& h : this->headers) {
            ctx->headers += h;
            ctx->headers += "\r\n";
        }
        ctx->onDone = std::move(onDone);
        ctx->onDownload = std::move(onDownload);
        ctx->onError = std::move(onError);

        if (timeout_ms <= 0)
            connect_timeout_ms = 0;
        ctx->timeout_ms = timeout_ms;
        ctx->timeout_connect_ms = connect_timeout_ms;

        // For streaming downloads, still use HTTP connection but intercept body data
        struct mg_connection* c = mg_http_connect(&mgr, ctx->url.c_str(), ev_handler, ctx);
        if (!c) {
            report_error(nullptr, ctx, "Failed to connect");
            delete ctx;
        }
    }

    // Upload file content using streaming chunks with progress reporting and bandwidth control
    // New API that avoids allocating large memory by reading chunks via callback
    void upload_chunks(const char* url,
                       size_t content_length,
                       ReadCallback onReadChunk,
                       UploadCallback onProgress,
                       Callback onDone,
                       ErrorCallback onError = nullptr,
                       int timeout_ms = 60000,
                       int connect_timeout_ms = 10000,
                       size_t bandwidth_limit = 0) // 0 = unlimited, otherwise bytes/sec
    {
        // Validate URL to be correct
        if (!is_valid_url(url)) {
            if (onError)
                onError("Invalid URL");
            return;
        }

        // Validate required callback
        if (!onReadChunk) {
            if (onError)
                onError("onReadChunk callback is required");
            return;
        }

        auto* ctx = new RequestContext{};
        ctx->url = url ? url : "";
        ctx->method = "POST";
        ctx->isUpload = true;
        ctx->upload_total = content_length;
        ctx->upload_sent = 0;
        ctx->upload_bandwidth_limit = bandwidth_limit;
        ctx->upload_chunk_max = 65536;

        // Combine global headers
        ctx->headers.clear();
        for (const auto& h : this->headers) {
            ctx->headers += h;
            ctx->headers += "\r\n";
        }

        ctx->onReadChunk = std::move(onReadChunk);
        ctx->onUpload = std::move(onProgress);
        ctx->onDone = std::move(onDone);
        ctx->onError = std::move(onError);

        if (timeout_ms <= 0)
            connect_timeout_ms = 0;
        ctx->timeout_ms = timeout_ms;
        ctx->timeout_connect_ms = connect_timeout_ms;

        struct mg_connection* c = mg_http_connect(&mgr, ctx->url.c_str(), ev_handler, ctx);
        if (!c) {
            report_error(nullptr, ctx, "Failed to connect");
            delete ctx;
        }
    }

    // Backwards-compatible API: implemented via upload_chunks with a memory-backed reader
    void upload(const char* url,
                const char* content,
                size_t content_length,
                UploadCallback onProgress,
                Callback onDone,
                ErrorCallback onError = nullptr,
                int timeout_ms = 60000,
                int connect_timeout_ms = 10000,
                size_t bandwidth_limit = 0) // 0 = unlimited, otherwise bytes/sec
    {
        // Adapter that reads from the provided memory without extra allocation
        ReadCallback reader = [content, content_length](char* buffer, size_t maxLen, size_t offset) -> size_t {
            if (!content || offset >= content_length)
                return 0;
            size_t to_copy = maxLen;
            size_t remaining = content_length - offset;
            if (to_copy > remaining)
                to_copy = remaining;
            std::memcpy(buffer, content + offset, to_copy);
            return to_copy;
        };

        upload_chunks(url,
                      content_length,
                      std::move(reader),
                      std::move(onProgress),
                      std::move(onDone),
                      std::move(onError),
                      timeout_ms,
                      connect_timeout_ms,
                      bandwidth_limit);
    }

    /**
     * Sends an HTTP request with the specified parameters.
     *
     * @param method             The HTTP method to use (e.g., "GET", "POST").
     * @param url                The URL to which the request is sent.
     * @param data               The data to send with the request (for POST/PUT methods), or nullptr if not applicable.
     * @param headers            Additional HTTP headers to include in the request separated by \r\n (e.g., "Content-Type: application/json\r\nAccept: application/json"), or nullptr if none.
     * @param onDone             Callback function to be called upon successful completion of the request.
     * @param onError            Callback function to be called if an error occurs during the request.
     * @param timeout_ms         Timeout for the entire request in milliseconds.
     * @param connect_timeout_ms Timeout for the initial connection in milliseconds.
     *
     * If the connection cannot be established, the onError callback is invoked with an error message.
     */
    void request(const char* method, const char* url, const char* data, size_t data_length, const char* headers, Callback onDone, ErrorCallback onError, int timeout_ms = 60000, int connect_timeout_ms = 5000) {
        
        // Validate URL to be correct
        if (!is_valid_url(url)) {
            if (onError)
                onError("Invalid URL");
            return;
        }
        
        // Own all strings inside the context to avoid dangling pointers
        auto* ctx = new RequestContext{};
        ctx->url = url ? url : "";
        ctx->method = method ? method : "GET";
        // Combine global headers and per-request headers
        ctx->headers.clear();
        for (const auto& h : this->headers) {
            ctx->headers += h;
            ctx->headers += "\r\n";
        }
        if (headers && *headers) {
            ctx->headers += headers;
            ctx->headers += "\r\n";
        }
        ctx->data.resize(data_length);
        std::memcpy(ctx->data.data(), data, data_length);
        ctx->onDone = std::move(onDone);
        ctx->onError = std::move(onError);

        if (timeout_ms <= 0)
            connect_timeout_ms = 0;
        ctx->timeout_ms = timeout_ms;
        ctx->timeout_connect_ms = connect_timeout_ms;

        struct mg_connection* c = mg_http_connect(&mgr, ctx->url.c_str(), ev_handler, ctx);
        if (!c) {
            report_error(nullptr, ctx, "Failed to connect");
            delete ctx;
        }
    }

    static size_t url_encode(const char* s, size_t sl, char* buf, size_t len) {
        return mg_url_encode(s, sl, buf, len);
    }

    static int url_decode(const char* src, size_t src_len, char* dst, size_t dst_len, int is_form_url_encoded) {
        return mg_url_decode(src, src_len, dst, dst_len, is_form_url_encoded);
    }

    static bool is_valid_url(const char *url) {
        if (!url || *url == '\0')
            return false;

        const char *p = url;

        // Scheme: must start with a letter
        if (!isalpha((unsigned char)*p))
            return false;
        p++;

        // Valid scheme chars: letters, digits, '+', '-', '.'
        while (*p && (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.'))
            p++;

        // Must be followed by "://"
        if (strncmp(p, "://", 3) != 0)
            return false;
        p += 3;

        // Must have something after "://"
        if (*p == '\0')
            return false;

        // Check remaining characters for invalid ones
        // Reject control chars, spaces, quotes, backticks, <, >, backslashes
        while (*p) {
            unsigned char c = (unsigned char)*p;
            if (c <= 32 || c > 126) // Ignore non-printable characters
                return false;
            if (c == ' ' || c == '"' || c == '`' || c == '<' || c == '>' || c == '\\') // Invalid URL characters
                return false;
            p++;
        }

        return true;
    }

  private:


    struct RequestContext {
        std::string url;
        std::string method;
        std::string headers;
        std::vector<char> data;
        Callback onDone;
        ErrorCallback onError;
       
        uint64_t last_poll_time_ms = 0; // Timestamp of the last poll call
        uint64_t poll_interval_ms = 0; // Time interval between polls in milliseconds

        // Timeout tracking
        int timeout_ms = 0;                   // Overall timeout
        int timeout_connect_ms = 0;           // Timeout first for connection only
        uint64_t timeout_endtime = 0;         // Deadline for entire request
        uint64_t timeout_connect_endtime = 0; // Deadline for first connection
        bool connected = false;               // Track if connection is established
        bool error_occurred = false;          // Track if an error has occurred
        bool finished = false;                // Track if request is finished

        // Download-specific fields
        DownloadCallback onDownload;
        bool isDownload = false;
        size_t downloaded = 0;
        size_t total_size = 0;
        bool headers_received = false;
        int http_status = 0;
        size_t header_offset = 0;    // Track where headers end in the first buffer
        size_t last_buffer_size = 0; // Track last processed buffer size

        // Upload-specific fields
        bool isUpload = false;
        ReadCallback onReadChunk;
        UploadCallback onUpload;
        bool upload_headers_sent = false;
        bool upload_done = false;
        size_t upload_bandwidth_limit = 0; // Bytes per second (0 = unlimited)
        size_t upload_total = 0;
        size_t upload_sent = 0;
        size_t upload_chunk_size = 0;
        size_t upload_chunk_max = 65536;
        double upload_speed_ema = 0;
        size_t upload_speed = 0;
        size_t upload_speed_accumulated = 0;
        uint64_t upload_speed_start_ms = 0;
        uint64_t upload_limiter_start_ms = 0;
        size_t upload_limiter_sent = 0;
        uint64_t upload_last_progress_ms = 0; // Track last time we made progress for starvation detection
    };
    mg_mgr mgr;


    // Centralized error reporter to avoid duplicated error handling logic.
    static void report_error(struct mg_connection* c, RequestContext* ctx, const std::string& msg) {
        if (ctx) {
            ctx->error_occurred = true;
            if (ctx->onError)
                ctx->onError(msg);
        }
        if (c) c->is_closing = 1;
    }


    static void ev_handler(struct mg_connection* c, int ev, void* ev_data) {
        RequestContext* ctx = (RequestContext*)c->fn_data;

        // Connection created
        if (ev == MG_EV_OPEN) {
            uint64_t now = mg_millis();
            ctx->timeout_endtime = now + ctx->timeout_ms;
            ctx->timeout_connect_endtime = now + ctx->timeout_connect_ms;

        // Every frame
        } else if (ev == MG_EV_POLL) {
            // Check for timeout
            uint64_t now = mg_millis();
            if (!c->is_closing && !ctx->error_occurred) {
                // First: if a connect-specific timeout was configured, enforce it
                if (ctx->timeout_connect_ms > 0 && !ctx->connected && now > ctx->timeout_connect_endtime) {
                    ctx->error_occurred = true;
                    mg_error(c, "Connection timeout");
                }
                // Then: enforce the overall request timeout if configured
                else if (ctx->timeout_ms > 0 && now > ctx->timeout_endtime) {
                    ctx->error_occurred = true;
                    mg_error(c, "Timeout");
                }
            }

            // Measure time since last poll
            if (ctx->last_poll_time_ms > 0) {
                ctx->poll_interval_ms = now - ctx->last_poll_time_ms;
            }
            ctx->last_poll_time_ms = now;


            // Update upload bandwidth statistics
            if (ctx->isUpload && !ctx->upload_done && !ctx->error_occurred && !c->is_closing) {

                // Initialize bandwidth control timers/counters
                if (ctx->upload_speed_start_ms == 0) {
                    ctx->upload_speed_start_ms = now;
                    ctx->upload_limiter_start_ms = now;
                    ctx->upload_limiter_sent = 0;
                    ctx->upload_last_progress_ms = now;
                }

                uint64_t timeDiffMs = now - ctx->upload_speed_start_ms;
                if (timeDiffMs >= 100) {
                    // Compute instantaneous Bps from actual bytes flushed over the elapsed interval
                    double bps_inst = (ctx->upload_speed_accumulated * 1000.0) / (double) timeDiffMs;

                    // Exponential moving average for upload speed
                    ctx->upload_speed_ema = 0.2 * bps_inst + 0.8 * ctx->upload_speed_ema;
                    ctx->upload_speed = (size_t)(ctx->upload_speed_ema + 0.5);
                    ctx->upload_speed_accumulated = 0;
                    ctx->upload_speed_start_ms = now;
                }

                // If there is still data buffered in the socket send queue, don't queue more yet
                if (ctx->upload_done || ctx->upload_headers_sent == false)
                    return;

                // We are waiting for send, it means we might send too much data, decrease available budget
                if (c->send.len > 0) {
                    ctx->upload_chunk_max = ctx->upload_chunk_max - (ctx->upload_chunk_max * 0.01);
                    if (ctx->upload_chunk_max < 1024) {
                        ctx->upload_chunk_max = 1024; // Minimum 1KB chunk size
                    }
                    // If there is still data buffered in the socket send queue, don't queue more yet
                    return;

                } else {
                    // No send backlog, can increase chunk size (capped at 1MB)
                    ctx->upload_chunk_max = ctx->upload_chunk_max + (ctx->upload_chunk_max * 0.1);
                    if (ctx->upload_chunk_max > 1048576) {
                        ctx->upload_chunk_max = 1048576; // Maximum 1MB chunk size
                    }
                }

                // Compute chunk size based on bandwidth limit
                if (ctx->upload_bandwidth_limit > 0) {

                    // Sliding window limiter (~1s)
                    uint64_t elapsed_ms = now - ctx->upload_limiter_start_ms;
                    if (elapsed_ms >= 1000) {
                        ctx->upload_limiter_start_ms = now;
                        ctx->upload_limiter_sent = 0;
                        elapsed_ms = 0;
                    }

                    // Allowed bytes so far in this window
                    size_t allowed = (size_t)((ctx->upload_bandwidth_limit * elapsed_ms) / 1000);
                    if (ctx->upload_limiter_sent >= allowed) {
                        return; // Wait until more budget accrues
                    }
                    
                    // Determine how much we can send now
                    size_t remaining = ctx->upload_total - ctx->upload_sent;
                    size_t payload = remaining;
                    if (payload > ctx->upload_chunk_max) {
                        payload = ctx->upload_chunk_max;
                    }

                    // Cap payload to remaining budget
                    size_t remaining_budget = allowed - ctx->upload_limiter_sent;
                    if (payload > remaining_budget) {
                        payload = remaining_budget;
                    }

                    // Ensure minimum progress to prevent starvation (allow at least 512 bytes every 2 seconds)
                    uint64_t stall_duration_ms = now - ctx->upload_last_progress_ms;
                    if (payload == 0 && remaining > 0 && stall_duration_ms >= 2000) {
                        payload = (remaining > 512) ? 512 : remaining;
                        // Reset limiter window to allow forced progress
                        ctx->upload_limiter_start_ms = now;
                        ctx->upload_limiter_sent = 0;
                    }
                    
                    // If still no budget available and data remains, wait for the next window
                    if (payload == 0 && remaining > 0) {
                        return;
                    }
                    ctx->upload_chunk_size = payload;
                    
                } else {
                    // Unlimited bandwidth: use default chunk size
                    ctx->upload_chunk_size = ctx->upload_chunk_max;
                }


                // Send while there’s budget and data left
                if (ctx->upload_sent < ctx->upload_total) {
                    size_t remaining = ctx->upload_total - ctx->upload_sent;
                    size_t bytesToSend = ctx->upload_chunk_size;

                    if (remaining < bytesToSend) {
                        bytesToSend = remaining;
                    }

                    ctx->data.resize(bytesToSend);

                    // Read next chunk from callback
                    size_t chunk_data_len = ctx->onReadChunk(ctx->data.data(), bytesToSend, ctx->upload_sent);

                    if (chunk_data_len == 0) {
                        report_error(c, ctx, "Read callback returned 0 bytes");
                        return;
                    } else if (chunk_data_len > bytesToSend) {
                        report_error(c, ctx, "Read callback returned more bytes than requested");
                        return;
                    }

                    // Send chunk: size in hex + CRLF + data + CRLF
                    mg_printf(c, "%x\r\n", (unsigned int)chunk_data_len);
                    mg_send(c, ctx->data.data(), chunk_data_len);
                    mg_send(c, "\r\n", 2);

                    // Account bytes in the current window for limiter
                    if (ctx->upload_bandwidth_limit > 0) {
                        ctx->upload_limiter_sent += chunk_data_len;
                    }

                    ctx->upload_sent += chunk_data_len;
                    ctx->upload_last_progress_ms = now; // Track progress for starvation detection
                    
                    // Call progress callback
                    if (ctx->onUpload) {
                        ctx->onUpload(ctx->upload_sent, ctx->upload_total, ctx->upload_speed);
                    }
                } else {
                    // Send final chunk and mark upload as complete
                    mg_send(c, "0\r\n\r\n", 5);
                    ctx->upload_done = true;
                    
                    // Final progress callback at 100%
                    if (ctx->onUpload) {
                        ctx->onUpload(ctx->upload_total, ctx->upload_total, ctx->upload_speed);
                    }
                }

            }

        // TCP connection established
        } else if (ev == MG_EV_CONNECT) {
            // Mark connection as established
            ctx->connected = true;

            // Connected to server. Extract host name from URL
            struct mg_str host = mg_url_host(ctx->url.c_str());
            const char* uri = mg_url_uri(ctx->url.c_str());
            const size_t body_len = ctx->data.size();

            if (c->is_tls) {
                struct mg_tls_opts opts = {};
                opts.name = host;
                mg_tls_init(c, &opts);
            }

            std::string requestStr;
            requestStr.reserve(256 + ctx->headers.size());

            requestStr += ctx->method;
            requestStr += " ";
            requestStr += uri;
            requestStr += " HTTP/1.1\r\n";

            requestStr += "Host: ";
            requestStr.append(host.buf, host.len);
            requestStr += "\r\n";

            if (!ctx->headers.empty()) {
                requestStr += ctx->headers;
            }

            if (ctx->isDownload) {
                requestStr += "Connection: close\r\n\r\n";

            } else if (ctx->isUpload) {

                requestStr +=
                    "Transfer-Encoding: chunked\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "\r\n";

                ctx->upload_headers_sent = true;

            } else {
                requestStr += "Content-Length: ";
                requestStr += std::to_string(body_len);
                requestStr += "\r\n\r\n";

                if (body_len > 0) {
                    requestStr.append(ctx->data.data(), body_len);
                }
            }

            mg_send(c, requestStr.data(), requestStr.size());
        }

        // TLS handshake complete – no-op
        else if (ev == MG_EV_TLS_HS) {
        }

        // HTTP headers received
        else if (ev == MG_EV_HTTP_HDRS) {
            if (ctx->isDownload) {
                auto* hm = (struct mg_http_message*)ev_data;

                // Extract HTTP status
                ctx->http_status = mg_http_status(hm);

                // Check for HTTP errors
                if (ctx->http_status != 200) {
                    report_error(c, ctx, std::string("HTTP error ") + std::to_string(ctx->http_status));
                    return;
                }

                // Get Content-Length header to show progress
                for (int i = 0; i < MG_MAX_HTTP_HEADERS && hm->headers[i].name.len > 0; i++) {
                    std::string header_name(hm->headers[i].name.buf, hm->headers[i].name.len);
                    if (strcasecmp(header_name.c_str(), "Content-Length") == 0) {
                        std::string content_length(hm->headers[i].value.buf, hm->headers[i].value.len);
                        ctx->total_size = (size_t)std::stoull(content_length);
                        break;
                    }
                }

                // Calculate header offset: headers + double CRLF
                ctx->header_offset = hm->head.len;
                ctx->headers_received = true;
            }
        }

        // Data received (raw socket level) - for streaming downloads
        else if (ev == MG_EV_READ) {
            if (ctx->isDownload && ctx->headers_received) {
                struct mg_iobuf* io = &c->recv;
                if (io->len > ctx->header_offset) {
                    // Calculate body data (everything after headers)
                    const char* body_start = (const char*)io->buf + ctx->header_offset;
                    size_t body_size = io->len - ctx->header_offset;

                    if (body_size > 0) {
                        // Update total downloaded with this chunk
                        ctx->downloaded += body_size;

                        // Call streaming callback with body data
                        if (ctx->onDownload) {
                            ctx->onDownload(body_start, body_size, ctx->downloaded, ctx->total_size);
                        }

                        // Clear only the body portion, keep headers intact
                        mg_iobuf_del(io, ctx->header_offset, body_size);
                    }
                }
            }
        }

        // Data written to socket - track actual bytes sent for rate limiting
        else if (ev == MG_EV_WRITE) {
            // MG_EV_WRITE reports how many bytes were actually flushed to the socket
            if (ctx->isUpload && !ctx->error_occurred) {
                long bytes_written = *(long*)ev_data;
                ctx->upload_speed_accumulated += (size_t)bytes_written;
            }
        }

        // HTTP response received
        else if (ev == MG_EV_HTTP_MSG) {
            // Don't process HTTP message if an error has already occurred
            if (ctx->error_occurred) {
                return;
            }

            auto* hm = (struct mg_http_message*)ev_data;

            if (ctx->isDownload) {
                Response res;
                res.status = mg_http_status(hm);
                res.body = "";
                if (ctx->onDone)
                    ctx->onDone(res);

            } else {
                // For regular requests: create response with body and headers
                Response res;
                res.status = mg_http_status(hm);
                res.body.assign(hm->body.buf, hm->body.len);

                // parse headers
                for (int i = 0; i < MG_MAX_HTTP_HEADERS && hm->headers[i].name.len > 0; i++) {
                    res.headers[std::string(hm->headers[i].name.buf, hm->headers[i].name.len)] =
                        std::string(hm->headers[i].value.buf, hm->headers[i].value.len);
                }

                if (ctx->onDone)
                    ctx->onDone(res);
            }
            c->is_closing = 1;

            ctx->finished = true;
        }

        else if (ev == MG_EV_ERROR) {
            // Forward error through centralized reporter
            report_error(c, ctx, ev_data ? std::string((char*)ev_data) : std::string(""));
        }

        else if (ev == MG_EV_CLOSE) {

            if (!ctx->error_occurred && !ctx->finished) {
                // Connection closed unexpectedly
                // For uploads, this might happen if server closed after receiving data but before sending response
                if (ctx->isUpload && ctx->upload_done) {
                    // Upload completed but no response - treat as success
                    Response res;
                    res.status = 200;
                    if (ctx->onDone)
                        ctx->onDone(res);
                } else {
                    // Truly unexpected closure
                    report_error(c, ctx, "Connection closed unexpectedly");
                }
            }

            delete ctx;
        }
    }
};

#endif