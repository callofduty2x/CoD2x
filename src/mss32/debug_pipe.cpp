#include "debug_pipe.h"

/**
 * Debug-only control pipe for automated tests.
 *
 * The game listens on \\.\pipe\cod2x-<pid>. A test script connects and sends one request line at a time:
 *   <console command>    runs at once with Cmd_ExecuteString (does not wait behind a `wait` in the command buffer),
 *                        the reply is everything printed to the console while it runs
 *   !ping                "pong"
 *   !state               one line of key=value pairs (client state, server, map, g_cod2x, ...)
 *   !dvar <name>         value, type, flags, latched and default value
 *   !log <n>             the last n console lines (color codes removed)
 * Reply: zero or more lines, then the terminator line "\x1e" "ok" or "\x1e" "error <message>".
 *
 * Input for tests (aim, movement, keys, typing, clicks) are normal console commands of debug_input.cpp.
 *
 * The pipe is overlapped and polled from debug_pipe_frame() on the main thread with zero timeouts - no threads,
 * the game never waits for the test script. See tools/game_pipe.py and .claude/skills/cod2-game-control/SKILL.md.
 */

#if DEBUG

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared.h"
#include "../shared/cod2_cmd.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_net.h"
#include "../shared/cod2_client.h"

#define DEBUG_PIPE_TERMINATOR       '\x1e'          // first character of the terminator line, never in console output
#define DEBUG_PIPE_LOG_LINES        500             // console lines kept for !log
#define DEBUG_PIPE_LINE_SIZE        512             // longer console lines are cut
#define DEBUG_PIPE_REQUEST_SIZE     8192            // longest request line
#define DEBUG_PIPE_BUFFER           (64 * 1024)     // pipe buffers and the reply (longer replies are cut)

enum debugPipeState_e {
    DEBUG_PIPE_CLOSED,          // no pipe (not created or failed)
    DEBUG_PIPE_CONNECTING,      // waiting for a client (ConnectNamedPipe pending)
    DEBUG_PIPE_IDLE,            // client connected, nothing pending
    DEBUG_PIPE_READING,         // ReadFile pending
    DEBUG_PIPE_WRITING,         // WriteFile of the reply pending
};

static HANDLE debug_pipe_handle = INVALID_HANDLE_VALUE;
static HANDLE debug_pipe_event = NULL;
static OVERLAPPED debug_pipe_overlapped;
static enum debugPipeState_e debug_pipe_state = DEBUG_PIPE_CLOSED;

// Received bytes not processed yet (may hold more than one line)
static char debug_pipe_readBuffer[4096];
static char debug_pipe_request[DEBUG_PIPE_REQUEST_SIZE + sizeof(debug_pipe_readBuffer)]; // the start of a line + one read
static int debug_pipe_requestLen = 0;
static bool debug_pipe_requestOverflow = false;         // a too long line is being dropped until its newline
#define DEBUG_PIPE_TOO_LONG         '\x01'          // request line put in place of a dropped too long line

// Reply being built / sent
static char debug_pipe_reply[DEBUG_PIPE_BUFFER];
static int debug_pipe_replyLen = 0;
static int debug_pipe_replySent = 0;
static bool debug_pipe_replyTruncated = false;
static volatile bool debug_pipe_capturing = false;      // console output goes into the reply
static char debug_pipe_commandErrorText[256];           // set by debug_pipe_commandError while a console command runs

// Console lines for !log (ring buffer), filled from every thread
static char debug_pipe_log[DEBUG_PIPE_LOG_LINES][DEBUG_PIPE_LINE_SIZE];
static int debug_pipe_logNext = 0;                      // index of the slot written next
static int debug_pipe_logCount = 0;
static char debug_pipe_logPartial[DEBUG_PIPE_LINE_SIZE];// current line until its newline arrives
static int debug_pipe_logPartialLen = 0;
static volatile LONG debug_pipe_lock = 0;               // the main and the render thread both print


static void debug_pipe_lockEnter() {
    while (InterlockedCompareExchange(&debug_pipe_lock, 1, 0) != 0) {
        YieldProcessor();
    }
}

static void debug_pipe_lockLeave() {
    InterlockedExchange(&debug_pipe_lock, 0);
}


/** Appends text to the reply (lock held or main thread only). */
static void debug_pipe_replyAppend(const char* text, int len) {
    // Keep room for the terminator line
    int space = (int)sizeof(debug_pipe_reply) - 128 - debug_pipe_replyLen;
    if (len > space) {
        len = space > 0 ? space : 0;
        debug_pipe_replyTruncated = true;
    }
    memcpy(debug_pipe_reply + debug_pipe_replyLen, text, len);
    debug_pipe_replyLen += len;
}

static void debug_pipe_replyPrintf(const char* format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (len < 0) {
        return;
    }
    if (len >= (int)sizeof(buffer)) {
        len = sizeof(buffer) - 1;
    }
    debug_pipe_lockEnter();
    debug_pipe_replyAppend(buffer, len);
    debug_pipe_lockLeave();
}

/** Starts a new reply. */
static void debug_pipe_replyBegin() {
    debug_pipe_lockEnter();
    debug_pipe_replyLen = 0;
    debug_pipe_replySent = 0;
    debug_pipe_replyTruncated = false;
    debug_pipe_lockLeave();
}


/**
 * Adds cleaned console text to the !log ring buffer and, while a pipe command runs, to the reply (lock held). Only the main
 * thread runs the command - prints of other threads (render thread) meanwhile go only to the log.
 */
static void debug_pipe_addText(const char* text, int len) {
    if (debug_pipe_capturing && !thread_isNotMain()) {
        debug_pipe_replyAppend(text, len);
    }
    for (int i = 0; i < len; i++) {
        if (text[i] != '\n') {
            if (debug_pipe_logPartialLen < DEBUG_PIPE_LINE_SIZE - 1) {
                debug_pipe_logPartial[debug_pipe_logPartialLen++] = text[i];
            }
            continue;
        }
        // End of the line: store it in the ring buffer
        memcpy(debug_pipe_log[debug_pipe_logNext], debug_pipe_logPartial, debug_pipe_logPartialLen);
        debug_pipe_log[debug_pipe_logNext][debug_pipe_logPartialLen] = '\0';
        debug_pipe_logNext = (debug_pipe_logNext + 1) % DEBUG_PIPE_LOG_LINES;
        if (debug_pipe_logCount < DEBUG_PIPE_LOG_LINES) {
            debug_pipe_logCount++;
        }
        debug_pipe_logPartialLen = 0;
    }
}

/** Console tap for every printed text, on any thread: removes color codes and carriage returns, then adds it under the lock. */
static void debug_pipe_consoleText(const char* msg) {
    char clean[1024];
    int n = 0;
    auto flush = [&]() {
        debug_pipe_lockEnter();
        debug_pipe_addText(clean, n);
        debug_pipe_lockLeave();
        n = 0;
    };
    for (const char* m = msg; *m; m++) {
        if (Q_IsColorString(m)) {
            m++;
            continue;
        }
        if (*m == '\r' || *m == DEBUG_PIPE_TERMINATOR) {
            continue;
        }
        clean[n++] = *m;
        if (n == (int)sizeof(clean)) {
            flush();
        }
    }
    if (n > 0) {
        flush();
    }
}


/**
 * Marks the console command that runs from the pipe as failed: the reply ends with "error <msg>" instead of "ok".
 * For CoD2x debug commands (debug_input.cpp) - engine commands only print their errors. No effect outside a pipe command.
 */
void debug_pipe_commandError(const char* msg) {
    if (debug_pipe_capturing && !debug_pipe_commandErrorText[0]) {
        snprintf(debug_pipe_commandErrorText, sizeof(debug_pipe_commandErrorText), "%s", msg);
    }
}


/**
 * Prints a message to the console window, the in-game console and console_mp.log (logfile). Channel 4 goes only to
 * console_mp.log - e.g. "      dvar set <name> <value>" of Dvar_SetVariant.
 */
// 00431d10    void Com_PrintMessage(int32_t channel, char* msg)
static void Com_PrintMessage(int channel, const char* msg)
{
    // CoD2x: debug control pipe - everything printed (also the lines only written to console_mp.log) for !log and the reply
    debug_pipe_consoleText(msg);
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x00431d10, 2, PUSH(channel), PUSH(msg));
}


/** A value of the dvar as text, copied into buffer (Dvar_ValueToString returns a va() buffer). */
static const char* debug_pipe_dvarValue(dvar_t* dvar, dvarValue_t value, char* buffer, size_t size) {
    snprintf(buffer, size, "%s", Dvar_ValueToString(dvar, value));
    return buffer;
}

/** Value of the dvar by name as text, "" if it does not exist. */
static const char* debug_pipe_dvarValueByName(const char* name, char* buffer, size_t size) {
    dvar_t* dvar = Dvar_GetDvarByName(name);
    return dvar ? debug_pipe_dvarValue(dvar, dvar->value, buffer, size) : "";
}

static const char* debug_pipe_dvarTypeName(int type) {
    switch (type) {
        case DVAR_TYPE_BOOL:   return "bool";
        case DVAR_TYPE_FLOAT:  return "float";
        case DVAR_TYPE_VEC2:   return "vec2";
        case DVAR_TYPE_VEC3:   return "vec3";
        case DVAR_TYPE_VEC4:   return "vec4";
        case DVAR_TYPE_INT:    return "int";
        case DVAR_TYPE_ENUM:   return "enum";
        case DVAR_TYPE_STRING: return "string";
        case DVAR_TYPE_COLOR:  return "color";
        default:               return "unknown";
    }
}


/** !state - one line of key=value pairs. */
static void debug_pipe_cmdState() {
    char cl_ingame[64], mapname[128], g_cod2x[64], g_competitive[64], loc_language[64], server[64] = "";
    if (clientState >= CLIENT_STATE_CONNECTING) {
        snprintf(server, sizeof(server), "%s", NET_AdrToString(clc_serverAddress));
    }
    debug_pipe_replyPrintf("clientState=%d clientStateName=%s cl_ingame=%s server=%s mapname=%s g_cod2x=%s g_competitive=%s demo_isPlaying=%d loc_language=%s\n",
        (int)clientState, get_client_state_name(clientState),
        debug_pipe_dvarValueByName("cl_ingame", cl_ingame, sizeof(cl_ingame)),
        server,
        debug_pipe_dvarValueByName("mapname", mapname, sizeof(mapname)),
        debug_pipe_dvarValueByName("g_cod2x", g_cod2x, sizeof(g_cod2x)),
        debug_pipe_dvarValueByName("g_competitive", g_competitive, sizeof(g_competitive)),
        demo_isPlaying,
        debug_pipe_dvarValueByName("loc_language", loc_language, sizeof(loc_language)));
}


/** !dvar <name> - value, type, flags, latched and default value, one key=value per line. Returns the error or NULL. */
static const char* debug_pipe_cmdDvar(const char* name) {
    if (!*name) {
        return "usage: !dvar <name>";
    }
    dvar_t* dvar = Dvar_GetDvarByName(name);
    if (!dvar) {
        static char error[128];
        snprintf(error, sizeof(error), "dvar '%s' does not exist", name);
        return error;
    }

    static const struct { int flag; const char* name; } flagNames[] = {
        { DVAR_ARCHIVE, "ARCHIVE" }, { DVAR_USERINFO, "USERINFO" }, { DVAR_SERVERINFO, "SERVERINFO" },
        { DVAR_SYSTEMINFO, "SYSTEMINFO" }, { DVAR_NOWRITE, "NOWRITE" }, { DVAR_LATCH, "LATCH" }, { DVAR_ROM, "ROM" },
        { DVAR_CHEAT, "CHEAT" }, { DVAR_CONFIG, "CONFIG" }, { DVAR_SAVED, "SAVED" }, { DVAR_SCRIPTINFO, "SCRIPTINFO" },
        { DVAR_CHANGEABLE_RESET, "CHANGEABLE_RESET" }, { DVAR_RENDERER, "RENDERER" }, { DVAR_EXTERNAL, "EXTERNAL" },
        { DVAR_AUTOEXEC, "AUTOEXEC" },
    };
    char flags[256] = "";
    for (const auto& f : flagNames) {
        if (dvar->flags & f.flag) {
            snprintf(flags + strlen(flags), sizeof(flags) - strlen(flags), "%s%s", flags[0] ? "|" : "", f.name);
        }
    }

    char value[1024], latched[1024], def[1024];
    debug_pipe_replyPrintf("name=%s\nvalue=%s\ntype=%s\nflags=0x%04x %s\nlatched=%s\ndefault=%s\n",
        dvar->name,
        debug_pipe_dvarValue(dvar, dvar->value, value, sizeof(value)),
        debug_pipe_dvarTypeName(dvar->type),
        (unsigned int)dvar->flags, flags,
        debug_pipe_dvarValue(dvar, dvar->latchedValue, latched, sizeof(latched)),
        debug_pipe_dvarValue(dvar, dvar->defaultValue, def, sizeof(def)));
    return NULL;
}


/** !log <n> - the last n console lines, oldest first. Lines that do not fit into the reply are dropped from the oldest side. Returns the error or NULL. */
static const char* debug_pipe_cmdLog(const char* arg) {
    int count = *arg ? atoi(arg) : 50;
    if (count <= 0) {
        return "usage: !log <n>";
    }
    debug_pipe_lockEnter();
    if (count > debug_pipe_logCount) {
        count = debug_pipe_logCount;
    }
    // Keep the newest lines that fit (the reply keeps 128 bytes for its end)
    int space = (int)sizeof(debug_pipe_reply) - 128 - debug_pipe_replyLen;
    int fit = 0;
    for (int size = 0; fit < count; fit++) {
        size += (int)strlen(debug_pipe_log[(debug_pipe_logNext - 1 - fit + DEBUG_PIPE_LOG_LINES) % DEBUG_PIPE_LOG_LINES]) + 1;
        if (size > space) {
            break;
        }
    }
    count = fit;
    for (int i = count; i > 0; i--) {
        const char* line = debug_pipe_log[(debug_pipe_logNext - i + DEBUG_PIPE_LOG_LINES) % DEBUG_PIPE_LOG_LINES];
        debug_pipe_replyAppend(line, (int)strlen(line));
        debug_pipe_replyAppend("\n", 1);
    }
    debug_pipe_lockLeave();
    return NULL;
}


/** Ends the reply: newline after unfinished output, then the terminator line ("ok" if error is NULL). */
static void debug_pipe_replyFinish(const char* error) {
    debug_pipe_lockEnter();
    debug_pipe_capturing = false;
    if (debug_pipe_replyLen > 0 && debug_pipe_reply[debug_pipe_replyLen - 1] != '\n') {
        debug_pipe_reply[debug_pipe_replyLen++] = '\n';
    }
    // The 128 bytes kept free by debug_pipe_replyAppend
    int room = (int)sizeof(debug_pipe_reply) - debug_pipe_replyLen;
    int len = snprintf(debug_pipe_reply + debug_pipe_replyLen, room, "%s%c%s%.96s\n",
        debug_pipe_replyTruncated ? "(output truncated)\n" : "", DEBUG_PIPE_TERMINATOR, error ? "error " : "ok", error ? error : "");
    debug_pipe_replyLen += len < room ? len : room - 1;
    debug_pipe_lockLeave();
}


/** Runs one request line (trimmed in place) and builds the reply. */
static void debug_pipe_processRequest(char* line) {
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }

    debug_pipe_replyBegin();
    const char* error = NULL;

    if (line[0] == '\0') {
        error = "empty request";
    }
    else if (line[0] == DEBUG_PIPE_TOO_LONG) {
        error = "request line too long";
    }
    else if (line[0] == '!') {
        char* arg = line + strcspn(line, " ");
        if (*arg) {
            *arg++ = '\0';
            arg += strspn(arg, " ");
        }
        const char* name = line + 1;

        if (strcmp(name, "ping") == 0) {
            debug_pipe_replyPrintf("pong\n");
        } else if (strcmp(name, "state") == 0) {
            debug_pipe_cmdState();
        } else if (strcmp(name, "dvar") == 0) {
            error = debug_pipe_cmdDvar(arg);
        } else if (strcmp(name, "log") == 0) {
            error = debug_pipe_cmdLog(arg);
        } else {
            error = "unknown request (!ping, !state, !dvar <name>, !log <n>)";
        }
    }
    else {
        // Console command: run now, everything printed meanwhile is the reply. A Com_Error(ERR_DROP) inside jumps
        // back to Com_Frame - debug_pipe_frame() then finds debug_pipe_capturing still set and ends the reply with an error.
        debug_pipe_commandErrorText[0] = '\0';
        debug_pipe_capturing = true;
        Cmd_ExecuteString(line);
        debug_pipe_capturing = false;
        if (debug_pipe_commandErrorText[0]) {
            error = debug_pipe_commandErrorText;
        }
    }

    debug_pipe_replyFinish(error);
}


static void debug_pipe_resetOverlapped() {
    memset(&debug_pipe_overlapped, 0, sizeof(debug_pipe_overlapped));
    debug_pipe_overlapped.hEvent = debug_pipe_event;
}

/** Cancels the pending operation of the pipe (waits only for the cancellation). */
static void debug_pipe_cancelPending() {
    if (debug_pipe_state != DEBUG_PIPE_CLOSED && debug_pipe_state != DEBUG_PIPE_IDLE && !HasOverlappedIoCompleted(&debug_pipe_overlapped)) {
        CancelIo(debug_pipe_handle);
        DWORD n;
        GetOverlappedResult(debug_pipe_handle, &debug_pipe_overlapped, &n, TRUE);
    }
}


/** Starts waiting for a client. */
static void debug_pipe_listen() {
    debug_pipe_requestLen = 0;
    debug_pipe_requestOverflow = false;
    debug_pipe_replyBegin();
    debug_pipe_resetOverlapped();

    if (ConnectNamedPipe(debug_pipe_handle, &debug_pipe_overlapped)) {
        debug_pipe_state = DEBUG_PIPE_IDLE;
        return;
    }
    DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
        debug_pipe_state = DEBUG_PIPE_CONNECTING;
    } else if (err == ERROR_PIPE_CONNECTED) {
        debug_pipe_state = DEBUG_PIPE_IDLE;
    } else if (err == ERROR_NO_DATA) {
        // Client connected and closed already
        DisconnectNamedPipe(debug_pipe_handle);
        debug_pipe_state = DEBUG_PIPE_IDLE; // next frame: read fails -> listen again
    } else {
        Com_Printf("Control pipe: ConnectNamedPipe failed (%lu)\n", err);
        debug_pipe_state = DEBUG_PIPE_CLOSED;
    }
}


/** The client is gone - disconnect and listen again. */
static void debug_pipe_reconnect() {
    debug_pipe_cancelPending();
    DisconnectNamedPipe(debug_pipe_handle);
    debug_pipe_listen();
}


static void debug_pipe_startRead() {
    debug_pipe_resetOverlapped();
    if (!ReadFile(debug_pipe_handle, debug_pipe_readBuffer, sizeof(debug_pipe_readBuffer), NULL, &debug_pipe_overlapped) && GetLastError() != ERROR_IO_PENDING) {
        debug_pipe_reconnect();
        return;
    }
    debug_pipe_state = DEBUG_PIPE_READING;
}

static void debug_pipe_startWrite() {
    debug_pipe_resetOverlapped();
    if (!WriteFile(debug_pipe_handle, debug_pipe_reply + debug_pipe_replySent, debug_pipe_replyLen - debug_pipe_replySent, NULL, &debug_pipe_overlapped) && GetLastError() != ERROR_IO_PENDING) {
        debug_pipe_reconnect();
        return;
    }
    debug_pipe_state = DEBUG_PIPE_WRITING;
}


/**
 * Adds received bytes to the request buffer line by line. A too long line is dropped up to its newline and a
 * DEBUG_PIPE_TOO_LONG line takes its place, so its error reply comes in order. The buffer holds only the start of the
 * current line when a read completes (complete lines are processed before the next read).
 */
static void debug_pipe_receive(const char* data, int n) {
    int lineStart = 0;
    while (n > 0) {
        const char* newline = (const char*)memchr(data, '\n', n);
        int take = newline ? (int)(newline + 1 - data) : n;

        // A line (with its newline) longer than DEBUG_PIPE_REQUEST_SIZE is dropped - the buffer always has room for the rest
        if (!debug_pipe_requestOverflow && debug_pipe_requestLen - lineStart + take > DEBUG_PIPE_REQUEST_SIZE) {
            debug_pipe_requestLen = lineStart; // drop the part of the line already buffered
            debug_pipe_requestOverflow = true;
        }
        if (!debug_pipe_requestOverflow) {
            memcpy(debug_pipe_request + debug_pipe_requestLen, data, take);
            debug_pipe_requestLen += take;
        } else if (newline) {
            debug_pipe_request[debug_pipe_requestLen++] = DEBUG_PIPE_TOO_LONG;
            debug_pipe_request[debug_pipe_requestLen++] = '\n';
            debug_pipe_requestOverflow = false;
        }
        if (newline) {
            lineStart = debug_pipe_requestLen;
        }
        data += take;
        n -= take;
    }
}


/** Takes the next complete line out of the request buffer and runs it. Returns false if there is no complete line. */
static bool debug_pipe_processNextLine() {
    char* newline = (char*)memchr(debug_pipe_request, '\n', debug_pipe_requestLen);
    if (!newline) {
        return false;
    }

    static char line[DEBUG_PIPE_REQUEST_SIZE];
    int lineLen = (int)(newline - debug_pipe_request);
    memcpy(line, debug_pipe_request, lineLen);
    line[lineLen] = '\0';
    debug_pipe_requestLen -= lineLen + 1;
    memmove(debug_pipe_request, newline + 1, debug_pipe_requestLen);

    debug_pipe_processRequest(line);
    return true;
}


/** Called every frame on the main thread. Advances the pipe without waiting. */
void debug_pipe_frame() {
    if (debug_pipe_handle == INVALID_HANDLE_VALUE) {
        return;
    }

    // The last console command did not return (Com_Error jumped back to Com_Frame) - send what it printed
    if (debug_pipe_capturing) {
        debug_pipe_replyFinish("command aborted by Com_Error (see the output above)");
        debug_pipe_startWrite();
    }

    // A few steps per frame: read -> run -> write can finish in one frame when the client is fast. Limited, so a
    // client that sends without end does not keep the frame.
    for (int step = 0; step < 8 && debug_pipe_state != DEBUG_PIPE_CLOSED; step++) {

        // A pending operation (connect, read, write): wait for it, the client is gone if it failed
        DWORD n = 0;
        if (debug_pipe_state != DEBUG_PIPE_IDLE) {
            if (!HasOverlappedIoCompleted(&debug_pipe_overlapped)) {
                return;
            }
            if (!GetOverlappedResult(debug_pipe_handle, &debug_pipe_overlapped, &n, FALSE)) {
                debug_pipe_reconnect(); // ERROR_BROKEN_PIPE: the client closed the pipe
                return;
            }
        }

        switch (debug_pipe_state) {
            case DEBUG_PIPE_CONNECTING:
                debug_pipe_state = DEBUG_PIPE_IDLE;
                break;

            case DEBUG_PIPE_IDLE:
                if (debug_pipe_processNextLine()) {
                    debug_pipe_startWrite();
                } else {
                    debug_pipe_startRead();
                }
                break;

            case DEBUG_PIPE_READING:
                debug_pipe_receive(debug_pipe_readBuffer, (int)n);
                debug_pipe_state = DEBUG_PIPE_IDLE;
                break;

            case DEBUG_PIPE_WRITING:
                debug_pipe_replySent += n;
                if (debug_pipe_replySent < debug_pipe_replyLen) {
                    debug_pipe_startWrite();
                } else {
                    debug_pipe_state = DEBUG_PIPE_IDLE;
                }
                break;

            case DEBUG_PIPE_CLOSED:
                break;
        }
    }
}


/**
 * Called by the crash handler on the crashed thread, before the crash dialog: sends the client the output of the running
 * command and the terminator line "\x1e" "error crash: <text>" at once, so a test gets the crash instead of a timeout.
 * A client that is not waiting gets it as the reply of its next request. Crash context: no locks (the crashed thread may
 * hold one), no heap, waits at most 2 s for the write. The pipe is not served any more afterwards.
 */
void debug_pipe_crash(const char* text) {
    if (debug_pipe_handle == INVALID_HANDLE_VALUE || debug_pipe_state == DEBUG_PIPE_CLOSED || debug_pipe_state == DEBUG_PIPE_CONNECTING) {
        return;
    }
    HANDLE pipe = debug_pipe_handle;
    debug_pipe_state = DEBUG_PIPE_CLOSED;

    // Stop the read / write of the main thread (CancelIo cancels only the operations of the calling thread)
    typedef BOOL (WINAPI *CancelIoEx_t)(HANDLE, LPOVERLAPPED);
    CancelIoEx_t cancelIoEx = (CancelIoEx_t)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CancelIoEx");
    if (cancelIoEx) {
        cancelIoEx(pipe, NULL);
    }

    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent) {
        return;
    }
    // Writes and waits at most 2 s, returns the bytes written
    auto write = [&](const char* data, int len) -> DWORD {
        DWORD written = 0;
        ResetEvent(overlapped.hEvent);
        if (WriteFile(pipe, data, len, NULL, &overlapped) || GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(overlapped.hEvent, 2000) == WAIT_OBJECT_0) {
                GetOverlappedResult(pipe, &overlapped, &written, FALSE);
            } else if (cancelIoEx) {
                cancelIoEx(pipe, &overlapped);
            }
        }
        return written;
    };

    // Output of the command that was running (its reply is not sent yet), then the error
    if (debug_pipe_capturing && debug_pipe_replyLen > 0) {
        write(debug_pipe_reply, debug_pipe_replyLen);
        if (debug_pipe_reply[debug_pipe_replyLen - 1] != '\n') {
            write("\n", 1);
        }
    }
    char line[1024];
    int len = snprintf(line, sizeof(line), "%cerror crash: %.900s\n", DEBUG_PIPE_TERMINATOR, text);
    DWORD written = write(line, len);
    CloseHandle(overlapped.hEvent);
    logger_add("Control pipe: crash sent to the client (%lu of %d bytes of the error line)", written, len);
}


/** Called before hot reload - closes the pipe so the new DLL can create it again. */
void debug_pipe_unload() {
    if (debug_pipe_handle != INVALID_HANDLE_VALUE) {
        debug_pipe_cancelPending();
        DisconnectNamedPipe(debug_pipe_handle);
        CloseHandle(debug_pipe_handle);
        debug_pipe_handle = INVALID_HANDLE_VALUE;
    }
    if (debug_pipe_event) {
        CloseHandle(debug_pipe_event);
        debug_pipe_event = NULL;
    }
    debug_pipe_state = DEBUG_PIPE_CLOSED;
    debug_pipe_capturing = false;
}


/** Called once on game start (and after hot reload) after common initialization. Creates the pipe. */
void debug_pipe_init() {
    char name[64];
    snprintf(name, sizeof(name), "\\\\.\\pipe\\cod2x-%lu", GetCurrentProcessId());

    debug_pipe_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!debug_pipe_event) {
        Com_Printf("Control pipe: CreateEvent failed (%lu)\n", GetLastError());
        return;
    }

    debug_pipe_handle = CreateNamedPipeA(name,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
        1, DEBUG_PIPE_BUFFER, DEBUG_PIPE_BUFFER, 0, NULL);
    if (debug_pipe_handle == INVALID_HANDLE_VALUE) {
        Com_Printf("Control pipe: CreateNamedPipe %s failed (%lu)\n", name, GetLastError());
        CloseHandle(debug_pipe_event);
        debug_pipe_event = NULL;
        return;
    }

    Com_Printf("Control pipe: %s\n", name);

    debug_pipe_listen();
}


/** Called before the entry point is called. Installs memory patches. */
void debug_pipe_patch() {
    // Com_PrintMessage: every call site
    static const unsigned int sites[] = {
        0x0040ee7c,                                         // CL_ConnectionlessPacket
        0x00431f18,                                         // Com_Printf
        0x004370c9, 0x0043716b,                             // Dvar_SetVariant ("dvar set", channel 4)
        0x0043a6b8, 0x0043a7ad, 0x0043a7f6, 0x0043a801,     // Dvar_Dump
        0x0047681b, 0x00476831, 0x00476849, 0x0047685a,     // Scr_PrintSourcePos
        0x00476937, 0x004769de, 0x004769f6,                 // Scr_PrintPrevCodePos
        0x00476c80, 0x00476cb8, 0x00476cdf, 0x00476cfa,     // RuntimeErrorInternal
    };
    for (unsigned int site : sites) {
        patch_call(site, (unsigned int)Com_PrintMessage);
    }
}

#else

// Release build: no control pipe
void debug_pipe_frame() {}
void debug_pipe_unload() {}
void debug_pipe_init() {}
void debug_pipe_patch() {}

#endif
