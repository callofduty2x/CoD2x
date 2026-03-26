#ifndef SHARED_ALL_H
#define SHARED_ALL_H

#include <stdint.h> // uint8_t, uint32_t, etc.
#include <stdarg.h> // va_list, va_start, va_end
#include <stdlib.h> // atoi, etc.
#include <string.h> // strncpy, etc.

// CoD2x is currecntly supported on Windows and Linux
// Based on the platform, the appropriate code will be compiled
#if _WIN32 == 1
    #define COD2X_WIN32 1
    #define COD2X_LINUX 0
#else
    #define COD2X_WIN32 0
    #define COD2X_LINUX 1
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)



// Application-wide constants
#define APP_NAME "CoD2x"
#define APP_URL "http://cod2x.me/"


// Original servers
#define SERVER_ACTIVISION_AUTHORIZE_URI "cod2master.activision.com"
#define SERVER_ACTIVISION_AUTHORIZE_PORT 20700
#define SERVER_ACTIVISION_MASTER_URI "cod2master.activision.com"
#define SERVER_ACTIVISION_MASTER_PORT 20710

// CoD2x servers
#if 1
    #define SERVER_MASTER_URI "master.cod2x.me"
    #define SERVER_MASTER_PORT 20710
    #define SERVER_UPDATE_URI "master.cod2x.me"
    #define SERVER_UPDATE_PORT 20720
#else
    #define SERVER_MASTER_URI "127.0.0.1"
    #define SERVER_MASTER_PORT 20710
    #define SERVER_UPDATE_URI "127.0.0.1"
    #define SERVER_UPDATE_PORT 20720
#endif



// Game protocol version,  115=1.0, 116=1.1, 117=1.3, 118=1.3, 119=1.3.Miscrosoft
// CoD2x servers will have the new protocol number
// When joining a server, the client will use the original 118 protocol to be able to connect old servers
#define PROTOCOL_VERSION 120    // original 118




// Platform-specific macro to get the address
#if COD2X_WIN32
    #define ADDR(addr_win, addr_linux) (addr_win)
    // Choose between Windows (W) or Linux (L) code
    #define WL(win, linux) win
#endif
#if COD2X_LINUX
    #define ADDR(addr_win, addr_linux) (addr_linux)
    // Choose between Windows (W) or Linux (L) code
    #define WL(win, linux) linux
    #define __cdecl
#endif

#if DEBUG
    #define DEBUG_RELEASE(debug, release) debug
#else
    #define DEBUG_RELEASE(debug, release) release
    #define DEBUG 0
#endif


#include "version.h"
#include "patch.h"
#include "assembly.h"
#include "logger.h"

int version_compare(const char *v1, const char *v2, bool* firstIsPrerelease = nullptr, bool* secondIsPrerelease = nullptr);
void escape_string(char* buffer, size_t bufferSize, const void* data, size_t length);
uint16_t crc16_ccitt(const uint8_t* data, size_t length);
int base64_encode(const uint8_t* input, size_t len, char* output, size_t out_size);
int base64_decode(const char* input, uint8_t* output, size_t out_size);
uint64_t time_utc_ms(void);
uint64_t ticks_ms(void);
char* time_to_iso8601(uint64_t ms_epoch, char* buf, size_t buf_size);
#endif

