#pragma once
#include <functional>
#include <string>
#include <cstdint>
#include "mongoose/mongoose.h"
#undef poll


// Wrapper around a Mongoose WebSocket CLIENT
// - Replies to incoming PING with PONG
// - Auto-reconnect on errors/remote close (unless manually closed)
// - Poll-driven: call poll(ms) regularly

class WebSocketClient {
  public:
    using OnOpen = std::function<void()>;
    using OnMessage = std::function<void(const std::string&)>;
    using OnClose = std::function<void(bool isClosedByRemote, bool isFullyDisconnected)>;
    using OnError = std::function<void(const std::string&)>;


	/**
	 * Constructs a WsClient instance with optional reconnect and ping intervals.
	 * @param reconnect_delay_ms Delay in milliseconds before attempting to reconnect after a server disconnect. Default is 2000 ms.
	 * @param ping_interval_ms Interval in milliseconds between sending PING frames to keep the connection alive. Default is 15000 ms.
	 * @param pong_timeout_ms Max time to wait for a PONG after sending our PING. 0 = auto (ping_interval_ms / 2; disabled if ping is 0).
	 */
    WebSocketClient(std::string headers = "", unsigned reconnect_delay_ms = 2000, unsigned ping_interval_ms = 15000, unsigned pong_timeout_ms = 0) {
        m_headers = std::move(headers);
        m_reconnect_ms = reconnect_delay_ms;
        m_ping_interval_ms = ping_interval_ms;
        // Auto-timeout: half of ping interval if provided, else disabled
        m_pong_timeout_ms = pong_timeout_ms ? pong_timeout_ms : (ping_interval_ms ? ping_interval_ms / 2 : 0);

        mg_log_set(MG_LL_NONE);
        mg_mgr_init(&m_mgr);
    }

    ~WebSocketClient() {
        close(); // Manual close disables auto-reconnect
        mg_mgr_free(&m_mgr);
    }

    // Start (or replace) connection to ws:// or wss:// URL
    bool connect(const std::string& url) {
        m_url = url;
        m_disconnect = false;
        return try_connect_now();
    }

    // Drive networking. Call this from your main loop.
    void poll(int ms = 0) {
        const uint64_t now = mg_millis();

        // Auto-reconnect timer
        if (!m_conn && !m_disconnect && !m_url.empty() && now >= m_nextReconnect) {
            try_connect_now();
        }
        // Watchdog: if connection created but not established within reconnect timeout, force reconnect
        if (m_conn && !m_connected && !m_disconnect && now >= m_nextReconnect) {
            mg_error(m_conn, "Connect timeout"); // set is_closing and call error event handler
            m_closing = true;
        }

        // Periodic PING keepalive
        if (m_conn && m_connected && !m_closing && m_ping_interval_ms > 0 && now >= m_nextPing) {
            mg_ws_send(m_conn, "", 0, WEBSOCKET_OP_PING);
            m_nextPing = now + m_ping_interval_ms;
            m_waitingPong = (m_pong_timeout_ms > 0);
            if (m_waitingPong) m_pongDeadline = now + m_pong_timeout_ms;
        }

        // Watchdog: if no PONG within timeout, force reconnect
        if (m_conn && m_connected && m_waitingPong && m_pong_timeout_ms > 0 && now >= m_pongDeadline) {
            mg_error(m_conn, "ping timeout (no pong received within time limit)"); // set is_closing and call error event handler
            m_closing = true;
            m_waitingPong = false; // avoid repeated triggers before MG_EV_CLOSE
        }

        mg_mgr_poll(&m_mgr, ms);
    }

    // Send a TEXT message. Returns false if not currently connected.
    bool sendText(const std::string& text) {
        if (!m_conn || !m_connected)
            return false;
        mg_ws_send(m_conn, text.c_str(), text.size(), WEBSOCKET_OP_TEXT);
        return true;
    }

    // Close connection
    // Politely request close; Mongoose will progress shutdown on next poll
    // Disables auto-reconnect until connect(url) is called again
    void close() {
        m_disconnect = true;
        if (m_conn) {
            mg_ws_send(m_conn, "", 0, WEBSOCKET_OP_CLOSE);
            m_conn->is_closing = 1;
            m_closing = true;
        }
    }

    // Callbacks
    void onOpen(OnOpen cb) { m_onOpen = std::move(cb); }
    void onMessage(OnMessage cb) { m_onMessage = std::move(cb); }
    void onClose(OnClose cb) { m_onClose = std::move(cb); }
    void onError(OnError cb) { m_onError = std::move(cb); }

    // State
    bool isConnected() const { return m_connected; }
    bool isClosing() const { return m_closing; }
    bool isClosed() const { return !m_conn && !m_connected && !m_closing; }
    bool isDisconnected() const { return !m_conn && !m_connected && !m_closing && m_disconnect; }

  private:
    // Try immediate connection. On failure, schedule a retry.
    bool try_connect_now() {
        m_conn = mg_ws_connect(&m_mgr, m_url.c_str(), &WebSocketClient::s_ev, this, "%s", m_headers.c_str());
        m_nextReconnect = mg_millis() + m_reconnect_ms;
        if (!m_conn) {
            return false;
        }
        return true;
    }

    // Static event handler
    static void s_ev(mg_connection* c, int ev, void* ev_data) {
        auto* self = static_cast<WebSocketClient*>(c->fn_data);
        if (self)
            self->handle_event(c, ev, ev_data);
    }

    // Instance event handler
    void handle_event(mg_connection* c, int ev, void* ev_data) {
        switch (ev) {
        case MG_EV_WS_OPEN: {
            m_connected = true;
            m_waitingPong = false;
            m_disconnect = false;
            m_closing = false;

            if (m_ping_interval_ms > 0)
                m_nextPing = mg_millis() + m_ping_interval_ms;
            if (m_onOpen)
                m_onOpen();
            break;
        }

        case MG_EV_WS_MSG: {
            // Incoming WS data; deliver TEXT frames only
            auto* wm = static_cast<mg_ws_message*>(ev_data);
            const uint8_t opcode = (uint8_t)(wm->flags & 0x0F);
            if (opcode == WEBSOCKET_OP_TEXT && m_onMessage) {
                m_onMessage(std::string(wm->data.buf, wm->data.len));
            }
            break;
        }

        case MG_EV_WS_CTL: {
            // Control frames: reply to PING; echo CLOSE and start shutdown
            auto* wm = static_cast<mg_ws_message*>(ev_data);
            const uint8_t opcode = (uint8_t)(wm->flags & 0x0F);
            // Ping received from remote: reply with Pong
            if (opcode == WEBSOCKET_OP_PING) {
                mg_ws_send(c, wm->data.buf, wm->data.len, WEBSOCKET_OP_PONG);
            }
            // Pong received: clear watchdog wait
            else if (opcode == WEBSOCKET_OP_PONG) {
                m_waitingPong = false;
                m_lastPong = mg_millis();
            }
            // Close received from remote: reply and close connection
            else if (opcode == WEBSOCKET_OP_CLOSE) {
                mg_ws_send(c, wm->data.buf, wm->data.len, WEBSOCKET_OP_CLOSE);
                c->is_closing = 1;
                m_closing = true;
            }
            break;
        }

        // Connection closed
        // - by remote, or by us
        case MG_EV_CLOSE: {
            if (c == m_conn)
                m_conn = nullptr;
            if (m_connected && m_onClose)
                m_onClose(m_closing == false, m_disconnect); // isClosedByRemote, isFullyDisconnected
            m_connected = false;
            m_closing = false;
            m_waitingPong = false;

            // Plan reconnect unless a manual close was requested
            if (!m_disconnect && !m_url.empty()) {
                m_nextReconnect = mg_millis() + m_reconnect_ms;
            }
            break;
        }

        case MG_EV_ERROR: {
            const char* error_message = static_cast<const char*>(ev_data);

            // Connection/handshake error: schedule reconnect
            if (!m_disconnect && !m_url.empty()) {
                m_nextReconnect = mg_millis() + m_reconnect_ms;
            }
            if (m_onError)
                m_onError(error_message);
            break;
        }

        default:
            break;
        }
    }

    // State
    mg_mgr m_mgr{};
    mg_connection* m_conn{nullptr};
    bool m_connected{false};
    bool m_disconnect{false};
    bool m_closing{false};
    std::string m_url;

    // Timers & config
    unsigned m_reconnect_ms;
    unsigned m_ping_interval_ms;
    unsigned m_pong_timeout_ms{0};
    std::string m_headers;
    uint64_t m_nextReconnect{0};
    uint64_t m_nextPing{0};
    bool m_waitingPong{false};
    uint64_t m_pongDeadline{0};
    uint64_t m_lastPong{0};

    // Callbacks
    OnOpen m_onOpen;
    OnMessage m_onMessage;
    OnClose m_onClose;
    OnError m_onError;
};
