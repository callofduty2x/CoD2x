#include "server_browser.h"

#include <ctype.h>
#include <string.h>

#include "shared.h"
#include "../shared/cod2_client.h"
#include "../shared/cod2_dvars.h"


#define ui_netSource                  (*((dvar_t**)0x0196ffec))     // 0 = local, 1 = internet, 2 = favorites

#define localServersCount             (*((int*)0x0068a540))
#define globalServersCount            (*((int*)0x0068ef48))
#define favoriteServersCount          (*((int*)0x009619cc))

#define localServers                  ((uint8_t*)0x0068a544)
#define globalServers                 ((uint8_t*)0x0068ef4c)
#define favoriteServers               ((uint8_t*)0x009619d0)

#define SERVERINFO_SIZE               148   // sizeof(serverInfo_t)
#define SERVERINFO_HOSTNAME           42    // char hostname[32]
#define SERVERINFO_MAPNAME            74    // char mapname[32]
#define SERVERINFO_STRING_SIZE        32

#define SERVER_BROWSER_FILTER_MAX     64

dvar_t* ui_browserFilterText;


/**
 * Copy the string while removing ^X color / style codes and converting it to lower case.
 */
void server_browser_normalize(const char* src, int srcSize, char* dst, int dstSize) {
    int len = 0;
    for (int i = 0; i < srcSize && src[i] && len < dstSize - 1; i++) {
        if (src[i] == '^' && i + 1 < srcSize && src[i + 1] && src[i + 1] != '^') {
            i++; // skip the code character
            continue;
        }
        dst[len++] = (char)tolower((unsigned char)src[i]);
    }
    dst[len] = '\0';
}


/**
 * Returns the serverInfo_t structure of the server from the list selected by ui_netSource.
 */
uint8_t* server_browser_getServer(int source, int index) {
    if (source == 0 && index >= 0 && index < 128) {
        return localServers + index * SERVERINFO_SIZE;
    } else if (source == 1 && index >= 0 && index < globalServersCount) {
        return globalServers + index * SERVERINFO_SIZE;
    } else if (source == 2 && index >= 0 && index < 128) {
        return favoriteServers + index * SERVERINFO_SIZE;
    }
    return NULL;
}


/**
 * Replacement of the function that inserts the server into the server browser display list.
 * It is called from UI_BuildServerDisplayList for each server that passed all original browser filters.
 * The server is inserted only if its hostname or map name contains the text from ui_browserFilterText.
 */
void UI_BinaryServerInsertion(int serverIndex) {

    if (ui_browserFilterText->value.string[0] != '\0') {
        char filter[SERVER_BROWSER_FILTER_MAX];
        server_browser_normalize(ui_browserFilterText->value.string, sizeof(filter), filter, sizeof(filter));

        uint8_t* server = server_browser_getServer(ui_netSource->value.integer, serverIndex);

        if (filter[0] != '\0' && server != NULL) {
            char hostname[SERVERINFO_STRING_SIZE + 1];
            char mapname[SERVERINFO_STRING_SIZE + 1];
            server_browser_normalize((const char*)(server + SERVERINFO_HOSTNAME), SERVERINFO_STRING_SIZE, hostname, sizeof(hostname));
            server_browser_normalize((const char*)(server + SERVERINFO_MAPNAME), SERVERINFO_STRING_SIZE, mapname, sizeof(mapname));

            if (strstr(hostname, filter) == NULL && strstr(mapname, filter) == NULL) {
                return;
            }
        }
    }

    // Call the original function
    ((void (__cdecl*)(int))0x005364f0)(serverIndex);
}


/** Called every frame on frame start. */
void server_browser_frame() {

    // Search text changed, rebuild the server list so all servers are filtered again
    // Server browser is available only in main menu, so wait until the player is disconnected
    if (ui_browserFilterText->modified && ui_netSource != NULL && clientState == CLIENT_STATE_DISCONNECTED) {
        ui_browserFilterText->modified = false;

        int serversCount = 0;
        if (ui_netSource->value.integer == 0) {
            serversCount = localServersCount;
        } else if (ui_netSource->value.integer == 1) {
            serversCount = globalServersCount;
        } else if (ui_netSource->value.integer == 2) {
            serversCount = favoriteServersCount;
        }

        if (serversCount > 0) {
            int force = 1;
            ASM_CALL(RETURN_VOID, 0x005365c0, 0, EAX(force)); // UI_BuildServerDisplayList(force @ eax)
        }
    }
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void server_browser_init() {
    ui_browserFilterText = Dvar_RegisterString("ui_browserFilterText", "", (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
}

/** Called before the entry point is called. Used to patch the memory. */
void server_browser_patch() {
    // Filter servers in server browser by search text
    patch_call(0x00536a85, (unsigned int)UI_BinaryServerInsertion); // 00536a85  e866faffff  call 0x5364f0 (inside UI_BuildServerDisplayList)
}
